/*
 * QEMU DBus audio
 *
 * Copyright (c) 2021 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/dbus.h"
#include "qom/object.h"

#ifdef G_OS_UNIX
#include <gio/gunixfdlist.h>
#endif

#include "ui/dbus.h"
#include "ui/dbus-display.h"
#include "ui/dbus-display1.h"

#include "qemu/audio.h"
#include "audio_int.h"
#include "trace.h"

#define DBUS_DISPLAY1_AUDIO_PATH DBUS_DISPLAY1_ROOT "/Audio"

#define DBUS_DEFAULT_AUDIO_NSAMPLES 480

#define TYPE_AUDIO_DBUS "audio-dbus"
OBJECT_DECLARE_SIMPLE_TYPE(AudioDbus, AUDIO_DBUS)

static AudioBackendClass *audio_dbus_parent_class;
typedef struct DBusDirectVoice DBusDirectVoice;

struct AudioDbus {
    AudioMixengBackend parent_obj;

    GDBusObjectManagerServer *server;
    bool p2p;
    GDBusObjectSkeleton *audio;
    QemuDBusDisplay1Audio *iface;
    GHashTable *out_listeners;
    GHashTable *in_listeners;
    DBusDirectVoice *direct;
};


typedef struct DBusVoiceOut {
    HWVoiceOut hw;
    bool enabled;
    RateCtl rate;

    void *buf;
    size_t buf_pos;
    size_t buf_size;

    bool has_volume;
    Volume volume;
} DBusVoiceOut;

struct DBusDirectVoice {
    SWVoiceOut sw;
    DBusVoiceOut out;
};

typedef struct DBusVoiceIn {
    HWVoiceIn hw;
    bool enabled;
    RateCtl rate;

    bool has_volume;
    Volume volume;
} DBusVoiceIn;

static void *dbus_get_buffer_out(HWVoiceOut *hw, size_t *size)
{
    DBusVoiceOut *vo = container_of(hw, DBusVoiceOut, hw);

    if (!vo->buf) {
        vo->buf_size = hw->samples * hw->info.bytes_per_frame;
        vo->buf = g_malloc(vo->buf_size);
        vo->buf_pos = 0;
    }

    *size = MIN(vo->buf_size - vo->buf_pos, *size);
    *size = audio_rate_get_bytes(&vo->rate, &hw->info, *size);

    return vo->buf + vo->buf_pos;

}

static size_t dbus_put_buffer_out(HWVoiceOut *hw, void *buf, size_t size)
{
    AudioDbus *da = AUDIO_DBUS(hw->s);
    DBusVoiceOut *vo = container_of(hw, DBusVoiceOut, hw);
    GHashTableIter iter;
    QemuDBusDisplay1AudioOutListener *listener = NULL;
    g_autoptr(GBytes) bytes = NULL;
    g_autoptr(GVariant) v_data = NULL;

    assert(buf == vo->buf + vo->buf_pos && vo->buf_pos + size <= vo->buf_size);
    vo->buf_pos += size;

    trace_dbus_audio_put_buffer_out(vo->buf_pos, vo->buf_size);

    if (vo->buf_pos < vo->buf_size) {
        return size;
    }

    bytes = g_bytes_new_take(g_steal_pointer(&vo->buf), vo->buf_size);
    v_data = g_variant_new_from_bytes(G_VARIANT_TYPE("ay"), bytes, TRUE);
    g_variant_ref_sink(v_data);

    g_hash_table_iter_init(&iter, da->out_listeners);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&listener)) {
        qemu_dbus_display1_audio_out_listener_call_write(
            listener,
            (uintptr_t)hw,
            v_data,
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }

    return size;
}

#if HOST_BIG_ENDIAN
#define AUDIO_HOST_BE TRUE
#else
#define AUDIO_HOST_BE FALSE
#endif

static void
dbus_init_out_listener(QemuDBusDisplay1AudioOutListener *listener,
                       HWVoiceOut *hw)
{
    qemu_dbus_display1_audio_out_listener_call_init(
        listener,
        (uintptr_t)hw,
        audio_format_bits(hw->info.af),
        audio_format_is_signed(hw->info.af),
        audio_format_is_float(hw->info.af),
        hw->info.freq,
        hw->info.nchannels,
        hw->info.bytes_per_frame,
        hw->info.bytes_per_second,
        hw->info.swap_endianness ? !AUDIO_HOST_BE : AUDIO_HOST_BE,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static guint
dbus_audio_get_nsamples(AudioDbus *da)
{
    AudiodevDBusOptions *opts = &AUDIO_MIXENG_BACKEND(da)->dev->u.dbus;

    if (opts->has_nsamples && opts->nsamples) {
        return opts->nsamples;
    } else {
        return DBUS_DEFAULT_AUDIO_NSAMPLES;
    }
}

static int
dbus_init_out(HWVoiceOut *hw, struct audsettings *as)
{
    AudioDbus *da = AUDIO_DBUS(hw->s);
    DBusVoiceOut *vo = container_of(hw, DBusVoiceOut, hw);
    GHashTableIter iter;
    QemuDBusDisplay1AudioOutListener *listener = NULL;

    audio_pcm_init_info(&hw->info, as);
    hw->samples = dbus_audio_get_nsamples(da);
    audio_rate_start(&vo->rate);

    g_hash_table_iter_init(&iter, da->out_listeners);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&listener)) {
        dbus_init_out_listener(listener, hw);
    }
    return 0;
}

static void
dbus_fini_out(HWVoiceOut *hw)
{
    AudioDbus *da = AUDIO_DBUS(hw->s);
    DBusVoiceOut *vo = container_of(hw, DBusVoiceOut, hw);
    GHashTableIter iter;
    QemuDBusDisplay1AudioOutListener *listener = NULL;

    g_hash_table_iter_init(&iter, da->out_listeners);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&listener)) {
        qemu_dbus_display1_audio_out_listener_call_fini(
            listener,
            (uintptr_t)hw,
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }

    g_clear_pointer(&vo->buf, g_free);
}

static void
dbus_enable_out(HWVoiceOut *hw, bool enable)
{
    AudioDbus *da = AUDIO_DBUS(hw->s);
    DBusVoiceOut *vo = container_of(hw, DBusVoiceOut, hw);
    GHashTableIter iter;
    QemuDBusDisplay1AudioOutListener *listener = NULL;

    vo->enabled = enable;
    if (enable) {
        audio_rate_start(&vo->rate);
    }

    g_hash_table_iter_init(&iter, da->out_listeners);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&listener)) {
        qemu_dbus_display1_audio_out_listener_call_set_enabled(
            listener, (uintptr_t)hw, enable,
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void
dbus_volume_out_listener(HWVoiceOut *hw,
                         QemuDBusDisplay1AudioOutListener *listener)
{
    DBusVoiceOut *vo = container_of(hw, DBusVoiceOut, hw);
    Volume *vol = &vo->volume;
    g_autoptr(GBytes) bytes = NULL;
    GVariant *v_vol = NULL;

    if (!vo->has_volume) {
        return;
    }

    assert(vol->channels < sizeof(vol->vol));
    bytes = g_bytes_new(vol->vol, vol->channels);
    v_vol = g_variant_new_from_bytes(G_VARIANT_TYPE("ay"), bytes, TRUE);
    qemu_dbus_display1_audio_out_listener_call_set_volume(
        listener, (uintptr_t)hw, vol->mute, v_vol,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
dbus_volume_out(HWVoiceOut *hw, Volume *vol)
{
    AudioDbus *da = AUDIO_DBUS(hw->s);
    DBusVoiceOut *vo = container_of(hw, DBusVoiceOut, hw);
    GHashTableIter iter;
    QemuDBusDisplay1AudioOutListener *listener = NULL;

    vo->has_volume = true;
    vo->volume = *vol;

    g_hash_table_iter_init(&iter, da->out_listeners);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&listener)) {
        dbus_volume_out_listener(hw, listener);
    }
}

static void
dbus_init_in_listener(QemuDBusDisplay1AudioInListener *listener, HWVoiceIn *hw)
{
    qemu_dbus_display1_audio_in_listener_call_init(
        listener,
        (uintptr_t)hw,
        audio_format_bits(hw->info.af),
        audio_format_is_signed(hw->info.af),
        audio_format_is_float(hw->info.af),
        hw->info.freq,
        hw->info.nchannels,
        hw->info.bytes_per_frame,
        hw->info.bytes_per_second,
        hw->info.swap_endianness ? !AUDIO_HOST_BE : AUDIO_HOST_BE,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static int
dbus_init_in(HWVoiceIn *hw, struct audsettings *as)
{
    AudioDbus *da = AUDIO_DBUS(hw->s);
    DBusVoiceIn *vo = container_of(hw, DBusVoiceIn, hw);
    GHashTableIter iter;
    QemuDBusDisplay1AudioInListener *listener = NULL;

    audio_pcm_init_info(&hw->info, as);
    hw->samples = dbus_audio_get_nsamples(da);
    audio_rate_start(&vo->rate);

    g_hash_table_iter_init(&iter, da->in_listeners);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&listener)) {
        dbus_init_in_listener(listener, hw);
    }
    return 0;
}

static void
dbus_fini_in(HWVoiceIn *hw)
{
    AudioDbus *da = AUDIO_DBUS(hw->s);
    GHashTableIter iter;
    QemuDBusDisplay1AudioInListener *listener = NULL;

    g_hash_table_iter_init(&iter, da->in_listeners);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&listener)) {
        qemu_dbus_display1_audio_in_listener_call_fini(
            listener,
            (uintptr_t)hw,
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void
dbus_volume_in_listener(HWVoiceIn *hw,
                         QemuDBusDisplay1AudioInListener *listener)
{
    DBusVoiceIn *vo = container_of(hw, DBusVoiceIn, hw);
    Volume *vol = &vo->volume;
    g_autoptr(GBytes) bytes = NULL;
    GVariant *v_vol = NULL;

    if (!vo->has_volume) {
        return;
    }

    assert(vol->channels < sizeof(vol->vol));
    bytes = g_bytes_new(vol->vol, vol->channels);
    v_vol = g_variant_new_from_bytes(G_VARIANT_TYPE("ay"), bytes, TRUE);
    qemu_dbus_display1_audio_in_listener_call_set_volume(
        listener, (uintptr_t)hw, vol->mute, v_vol,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
dbus_volume_in(HWVoiceIn *hw, Volume *vol)
{
    AudioDbus *da = AUDIO_DBUS(hw->s);
    DBusVoiceIn *vo = container_of(hw, DBusVoiceIn, hw);
    GHashTableIter iter;
    QemuDBusDisplay1AudioInListener *listener = NULL;

    vo->has_volume = true;
    vo->volume = *vol;

    g_hash_table_iter_init(&iter, da->in_listeners);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&listener)) {
        dbus_volume_in_listener(hw, listener);
    }
}

static size_t
dbus_read(HWVoiceIn *hw, void *buf, size_t size)
{
    AudioDbus *da = AUDIO_DBUS(hw->s);
    /* DBusVoiceIn *vo = container_of(hw, DBusVoiceIn, hw); */
    GHashTableIter iter;
    QemuDBusDisplay1AudioInListener *listener = NULL;

    trace_dbus_audio_read(size);

    /* size = audio_rate_get_bytes(&vo->rate, &hw->info, size); */

    g_hash_table_iter_init(&iter, da->in_listeners);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&listener)) {
        g_autoptr(GVariant) v_data = NULL;
        const char *data;
        gsize n = 0;

        if (qemu_dbus_display1_audio_in_listener_call_read_sync(
                listener,
                (uintptr_t)hw,
                size,
                G_DBUS_CALL_FLAGS_NONE, -1,
                &v_data, NULL, NULL)) {
            data = g_variant_get_fixed_array(v_data, &n, 1);
            g_warn_if_fail(n <= size);
            size = MIN(n, size);
            memcpy(buf, data, size);
            break;
        }
    }

    return size;
}

static void
dbus_enable_in(HWVoiceIn *hw, bool enable)
{
    AudioDbus *da = AUDIO_DBUS(hw->s);
    DBusVoiceIn *vo = container_of(hw, DBusVoiceIn, hw);
    GHashTableIter iter;
    QemuDBusDisplay1AudioInListener *listener = NULL;

    vo->enabled = enable;
    if (enable) {
        audio_rate_start(&vo->rate);
    }

    g_hash_table_iter_init(&iter, da->in_listeners);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&listener)) {
        qemu_dbus_display1_audio_in_listener_call_set_enabled(
            listener, (uintptr_t)hw, enable,
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

#define DBUS_DIRECT_MAX_PENDING (8 * 48000 * 2)

static bool dbus_direct(AudioBackend *be)
{
    return AUDIO_MIXENG_BACKEND(be)->dev->u.dbus.direct_pcm;
}

static SWVoiceOut *dbus_open_out(AudioBackend *be, SWVoiceOut *sw,
                                const char *name, void *opaque,
                                audio_callback_fn callback,
                                const struct audsettings *settings)
{
    AudioDbus *da = AUDIO_DBUS(be);
    DBusDirectVoice *voice;
    struct audsettings as = *settings;

    if (!dbus_direct(be)) {
        return audio_dbus_parent_class->open_out(be, sw, name, opaque,
                                                 callback, settings);
    }
    if (as.freq != 48000 || as.nchannels != 1 ||
        as.fmt != AUDIO_FORMAT_S16 || as.big_endian ||
        (da->direct && sw != &da->direct->sw)) {
        return NULL;
    }
    if (da->direct) {
        return sw;
    }
    voice = da->direct = g_new0(DBusDirectVoice, 1);
    voice->sw.hw = &voice->out.hw;
    voice->out.hw.s = AUDIO_MIXENG_BACKEND(be);
    dbus_init_out(&voice->out.hw, &as);
    return &voice->sw;
}

static void dbus_close_out(AudioBackend *be, SWVoiceOut *sw)
{
    AudioDbus *da = AUDIO_DBUS(be);

    if (!dbus_direct(be)) {
        audio_dbus_parent_class->close_out(be, sw);
        return;
    }
    assert(da->direct && sw == &da->direct->sw);
    dbus_fini_out(&da->direct->out.hw);
    g_clear_pointer(&da->direct, g_free);
}

static bool dbus_is_active_out(AudioBackend *be, SWVoiceOut *sw)
{
    return dbus_direct(be) ? AUDIO_DBUS(be)->direct->out.enabled :
           audio_dbus_parent_class->is_active_out(be, sw);
}

static void dbus_set_active_out(AudioBackend *be, SWVoiceOut *sw, bool on)
{
    if (dbus_direct(be)) {
        dbus_enable_out(&AUDIO_DBUS(be)->direct->out.hw, on);
    } else {
        audio_dbus_parent_class->set_active_out(be, sw, on);
    }
}

static void dbus_set_volume_out(AudioBackend *be, SWVoiceOut *sw, Volume *vol)
{
    if (dbus_direct(be)) {
        dbus_volume_out(&AUDIO_DBUS(be)->direct->out.hw, vol);
    } else {
        audio_dbus_parent_class->set_volume_out(be, sw, vol);
    }
}

static int dbus_buffer_size_out(AudioBackend *be, SWVoiceOut *sw)
{
    return dbus_direct(be) ? DBUS_DIRECT_MAX_PENDING :
           audio_dbus_parent_class->get_buffer_size_out(be, sw);
}

static void dbus_direct_write_done(GObject *object, GAsyncResult *result,
                                    gpointer opaque)
{
    guint pending = GPOINTER_TO_UINT(
        g_object_get_data(object, "direct-pending"));
    guint size = GPOINTER_TO_UINT(opaque);
    g_autoptr(GError) error = NULL;

    assert(pending >= size);
    g_object_set_data(object, "direct-pending",
                      GUINT_TO_POINTER(pending - size));
    if (!qemu_dbus_display1_audio_out_listener_call_write_finish(
            QEMU_DBUS_DISPLAY1_AUDIO_OUT_LISTENER(object), result, &error)) {
        /* A failed or stalled receiver cannot accumulate an unbounded FIFO. */
        g_dbus_connection_close(
            g_dbus_proxy_get_connection(G_DBUS_PROXY(object)),
            NULL, NULL, NULL);
    }
}

static size_t dbus_write(AudioBackend *be, SWVoiceOut *sw,
                          void *buf, size_t size)
{
    AudioDbus *da = AUDIO_DBUS(be);
    GHashTableIter iter;
    QemuDBusDisplay1AudioOutListener *listener;
    g_autoptr(GVariant) data = NULL;

    if (!dbus_direct(be)) {
        return audio_dbus_parent_class->write(be, sw, buf, size);
    }
    if (!size || !da->direct->out.enabled || (size & 1) ||
        size > DBUS_DIRECT_MAX_PENDING) {
        return 0;
    }
    data = g_variant_ref_sink(g_variant_new_fixed_array(
        G_VARIANT_TYPE_BYTE, buf, size, 1));
    g_hash_table_iter_init(&iter, da->out_listeners);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&listener)) {
        GObject *object = G_OBJECT(listener);
        guint pending = GPOINTER_TO_UINT(
            g_object_get_data(object, "direct-pending"));

        if (g_object_get_data(object, "direct-failed")) {
            continue;
        }
        if (size > DBUS_DIRECT_MAX_PENDING - pending) {
            g_object_set_data(object, "direct-failed", GUINT_TO_POINTER(1));
            g_dbus_connection_close(
                g_dbus_proxy_get_connection(G_DBUS_PROXY(listener)),
                NULL, NULL, NULL);
            continue;
        }
        g_object_set_data(object, "direct-pending",
                          GUINT_TO_POINTER(pending + size));
        qemu_dbus_display1_audio_out_listener_call_write(
            listener, (uintptr_t)&da->direct->out.hw, data,
            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, dbus_direct_write_done,
            GUINT_TO_POINTER(size));
    }
    return size;
}

static bool
audio_dbus_realize(AudioBackend *abe, Audiodev *dev, Error **errp)
{
    AudioDbus *da = AUDIO_DBUS(abe);

    if (!qemu_using_dbus_display(errp)) {
        qapi_free_Audiodev(dev);
        return false;
    }

    if (!audio_dbus_parent_class->realize(abe, dev, errp)) {
        return false;
    }

    da->out_listeners = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, g_object_unref);
    da->in_listeners = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_object_unref);
    return true;
}

static void
audio_dbus_finalize(Object *obj)
{
    AudioDbus *da = AUDIO_DBUS(obj);

    if (da->direct) {
        dbus_fini_out(&da->direct->out.hw);
        g_clear_pointer(&da->direct, g_free);
    }
    if (da->server) {
        g_dbus_object_manager_server_unexport(da->server,
                                              DBUS_DISPLAY1_AUDIO_PATH);
    }
    g_clear_object(&da->audio);
    g_clear_object(&da->iface);
    g_clear_pointer(&da->in_listeners, g_hash_table_unref);
    g_clear_pointer(&da->out_listeners, g_hash_table_unref);
    g_clear_object(&da->server);
}

static void
listener_out_vanished_cb(GDBusConnection *connection,
                         gboolean remote_peer_vanished,
                         GError *error,
                         AudioDbus *da)
{
    char *name = g_object_get_data(G_OBJECT(connection), "name");

    g_hash_table_remove(da->out_listeners, name);
}

static void
listener_in_vanished_cb(GDBusConnection *connection,
                        gboolean remote_peer_vanished,
                        GError *error,
                        AudioDbus *da)
{
    char *name = g_object_get_data(G_OBJECT(connection), "name");

    g_hash_table_remove(da->in_listeners, name);
}

static gboolean
dbus_audio_register_listener(AudioMixengBackend *s,
                             GDBusMethodInvocation *invocation,
#ifdef G_OS_UNIX
                             GUnixFDList *fd_list,
#endif
                             GVariant *arg_listener,
                             bool out)
{
    AudioDbus *da = AUDIO_DBUS(s);
    const char *sender =
        da->p2p ? "p2p" : g_dbus_method_invocation_get_sender(invocation);
    g_autoptr(GDBusConnection) listener_conn = NULL;
    g_autoptr(GError) err = NULL;
    g_autoptr(GSocket) socket = NULL;
    g_autoptr(GSocketConnection) socket_conn = NULL;
    g_autofree char *guid = g_dbus_generate_guid();
    GHashTable *listeners = out ? da->out_listeners : da->in_listeners;
    GObject *listener;
    int fd;

    trace_dbus_audio_register(sender, out ? "out" : "in");

    if (g_hash_table_contains(listeners, sender)) {
        g_dbus_method_invocation_return_error(invocation,
                                              DBUS_DISPLAY_ERROR,
                                              DBUS_DISPLAY_ERROR_INVALID,
                                              "`%s` is already registered!",
                                              sender);
        return DBUS_METHOD_INVOCATION_HANDLED;
    }

#ifdef G_OS_WIN32
    if (!dbus_win32_import_socket(invocation, arg_listener, &fd)) {
        return DBUS_METHOD_INVOCATION_HANDLED;
    }
#else
    fd = g_unix_fd_list_get(fd_list, g_variant_get_handle(arg_listener), &err);
    if (err) {
        g_dbus_method_invocation_return_error(invocation,
                                              DBUS_DISPLAY_ERROR,
                                              DBUS_DISPLAY_ERROR_FAILED,
                                              "Couldn't get peer fd: %s",
                                              err->message);
        return DBUS_METHOD_INVOCATION_HANDLED;
    }
#endif

    socket = g_socket_new_from_fd(fd, &err);
    if (err) {
        g_dbus_method_invocation_return_error(invocation,
                                              DBUS_DISPLAY_ERROR,
                                              DBUS_DISPLAY_ERROR_FAILED,
                                              "Couldn't make a socket: %s",
                                              err->message);
#ifdef G_OS_WIN32
        closesocket(fd);
#else
        close(fd);
#endif
        return DBUS_METHOD_INVOCATION_HANDLED;
    }
    socket_conn = g_socket_connection_factory_create_connection(socket);
    if (out) {
        qemu_dbus_display1_audio_complete_register_out_listener(
            da->iface, invocation
#ifdef G_OS_UNIX
            , NULL
#endif
            );
    } else {
        qemu_dbus_display1_audio_complete_register_in_listener(
            da->iface, invocation
#ifdef G_OS_UNIX
            , NULL
#endif
            );
    }

    GDBusConnectionFlags flags =
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER;
#ifdef WIN32
    flags |= G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_ALLOW_ANONYMOUS;
#endif

    listener_conn =
        g_dbus_connection_new_sync(
            G_IO_STREAM(socket_conn),
            guid,
            flags,
            NULL, NULL, &err);
    if (err) {
        error_report("Failed to setup peer connection: %s", err->message);
        return DBUS_METHOD_INVOCATION_HANDLED;
    }

    listener = out ?
        G_OBJECT(qemu_dbus_display1_audio_out_listener_proxy_new_sync(
            listener_conn,
            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
            NULL,
            "/org/qemu/Display1/AudioOutListener",
            NULL,
            &err)) :
        G_OBJECT(qemu_dbus_display1_audio_in_listener_proxy_new_sync(
            listener_conn,
            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
            NULL,
            "/org/qemu/Display1/AudioInListener",
            NULL,
            &err));
    if (!listener) {
        error_report("Failed to setup proxy: %s", err->message);
        return DBUS_METHOD_INVOCATION_HANDLED;
    }

    if (out) {
        HWVoiceOut *hw;

        if (da->direct) {
            DBusVoiceOut *vo = &da->direct->out;
            QemuDBusDisplay1AudioOutListener *l =
                QEMU_DBUS_DISPLAY1_AUDIO_OUT_LISTENER(listener);

            dbus_init_out_listener(l, &vo->hw);
            qemu_dbus_display1_audio_out_listener_call_set_enabled(
                l, (uintptr_t)&vo->hw, vo->enabled,
                G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        }

        QLIST_FOREACH(hw, &s->hw_head_out, entries) {
            DBusVoiceOut *vo = container_of(hw, DBusVoiceOut, hw);
            QemuDBusDisplay1AudioOutListener *l =
                QEMU_DBUS_DISPLAY1_AUDIO_OUT_LISTENER(listener);

            dbus_init_out_listener(l, hw);
            qemu_dbus_display1_audio_out_listener_call_set_enabled(
                l, (uintptr_t)hw, vo->enabled,
                G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        }
    } else {
        HWVoiceIn *hw;

        QLIST_FOREACH(hw, &s->hw_head_in, entries) {
            DBusVoiceIn *vo = container_of(hw, DBusVoiceIn, hw);
            QemuDBusDisplay1AudioInListener *l =
                QEMU_DBUS_DISPLAY1_AUDIO_IN_LISTENER(listener);

            dbus_init_in_listener(
                QEMU_DBUS_DISPLAY1_AUDIO_IN_LISTENER(listener), hw);
            qemu_dbus_display1_audio_in_listener_call_set_enabled(
                l, (uintptr_t)hw, vo->enabled,
                G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        }
    }

    g_object_set_data_full(G_OBJECT(listener_conn), "name",
                           g_strdup(sender), g_free);
    g_hash_table_insert(listeners, g_strdup(sender), listener);
    g_object_connect(listener_conn,
                     "signal::closed",
                     out ? listener_out_vanished_cb : listener_in_vanished_cb,
                     da,
                     NULL);

    return DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
dbus_audio_register_out_listener(AudioMixengBackend *s,
                                 GDBusMethodInvocation *invocation,
#ifdef G_OS_UNIX
                                 GUnixFDList *fd_list,
#endif
                                 GVariant *arg_listener)
{
    return dbus_audio_register_listener(s, invocation,
#ifdef G_OS_UNIX
                                        fd_list,
#endif
                                        arg_listener, true);

}

static gboolean
dbus_audio_register_in_listener(AudioMixengBackend *s,
                                GDBusMethodInvocation *invocation,
#ifdef G_OS_UNIX
                                GUnixFDList *fd_list,
#endif
                                GVariant *arg_listener)
{
    return dbus_audio_register_listener(s, invocation,
#ifdef G_OS_UNIX
                                        fd_list,
#endif
                                        arg_listener, false);
}

static bool
dbus_audio_set_server(AudioBackend *s,
                      GDBusObjectManagerServer *server,
                      bool p2p,
                      Error **errp)
{
    AudioDbus *da = AUDIO_DBUS(s);

    g_assert(!da->server);

    da->server = g_object_ref(server);
    da->p2p = p2p;

    da->audio = g_dbus_object_skeleton_new(DBUS_DISPLAY1_AUDIO_PATH);
    da->iface = qemu_dbus_display1_audio_skeleton_new();
    g_object_connect(da->iface,
                     "swapped-signal::handle-register-in-listener",
                     dbus_audio_register_in_listener, s,
                     "swapped-signal::handle-register-out-listener",
                     dbus_audio_register_out_listener, s,
                     NULL);
    qemu_dbus_display1_audio_set_nsamples(da->iface, dbus_audio_get_nsamples(da));

    g_dbus_object_skeleton_add_interface(G_DBUS_OBJECT_SKELETON(da->audio),
                                         G_DBUS_INTERFACE_SKELETON(da->iface));
    g_dbus_object_manager_server_export(da->server, da->audio);

    return true;
}

static void audio_dbus_class_init(ObjectClass *klass, const void *data)
{
    AudioBackendClass *b = AUDIO_BACKEND_CLASS(klass);
    AudioMixengBackendClass *k = AUDIO_MIXENG_BACKEND_CLASS(klass);

    audio_dbus_parent_class = AUDIO_BACKEND_CLASS(object_class_get_parent(klass));

    b->realize = audio_dbus_realize;
    b->set_dbus_server = dbus_audio_set_server;
    b->open_out = dbus_open_out;
    b->close_out = dbus_close_out;
    b->is_active_out = dbus_is_active_out;
    b->set_active_out = dbus_set_active_out;
    b->set_volume_out = dbus_set_volume_out;
    b->write = dbus_write;
    b->get_buffer_size_out = dbus_buffer_size_out;
    k->max_voices_out = INT_MAX;
    k->max_voices_in = INT_MAX;
    k->voice_size_out = sizeof(DBusVoiceOut);
    k->voice_size_in = sizeof(DBusVoiceIn);

    k->init_out = dbus_init_out;
    k->fini_out = dbus_fini_out;
    k->write = audio_generic_write;
    k->get_buffer_out = dbus_get_buffer_out;
    k->put_buffer_out = dbus_put_buffer_out;
    k->enable_out = dbus_enable_out;
    k->volume_out = dbus_volume_out;

    k->init_in = dbus_init_in;
    k->fini_in = dbus_fini_in;
    k->read = dbus_read;
    k->run_buffer_in = audio_generic_run_buffer_in;
    k->enable_in = dbus_enable_in;
    k->volume_in = dbus_volume_in;
}

static const TypeInfo audio_types[] = {
    {
        .name = TYPE_AUDIO_DBUS,
        .parent = TYPE_AUDIO_MIXENG_BACKEND,
        .instance_size = sizeof(AudioDbus),
        .instance_finalize = audio_dbus_finalize,
        .class_init = audio_dbus_class_init,
    },
};

DEFINE_TYPES(audio_types)

module_dep("ui-dbus")
module_obj(TYPE_AUDIO_DBUS)
