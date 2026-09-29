/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Terminal PCM transport: partial writes, reset, reconnect and backpressure. */
#include "qemu/osdep.h"
#include <gio/gunixfdlist.h>
#include "libqtest.h"
#include "ui/dbus-display1.h"

typedef struct Listener {
    GDBusConnection *conn;
    guint registration;
    unsigned init, fini, writes, bytes;
    bool enabled, stall;
    GPtrArray *held;
    GByteArray *pcm;
} Listener;

static GDBusConnection *from_fd(int fd)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GSocket) socket = g_socket_new_from_fd(fd, &err);
    g_autoptr(GSocketConnection) stream = NULL;
    GDBusConnection *conn;

    g_assert_no_error(err);
    stream = g_socket_connection_factory_create_connection(socket);
    conn = g_dbus_connection_new_sync(G_IO_STREAM(stream), NULL,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING, NULL, NULL, &err);
    g_assert_no_error(err);
    return conn;
}

static gpointer connect_thread(gpointer fd)
{
    return from_fd(GPOINTER_TO_INT(fd));
}

static void pump(void)
{
    while (g_main_context_iteration(NULL, false)) {
    }
}

static void method(GDBusConnection *conn, const char *sender, const char *path,
                   const char *iface, const char *name, GVariant *args,
                   GDBusMethodInvocation *invocation, gpointer opaque)
{
    Listener *l = opaque;

    if (!strcmp(name, "Init")) {
        guint64 id;
        guchar bits, channels;
        gboolean sign, floating, be;
        guint rate, frame, second;

        g_variant_get(args, "(tybbuyuub)", &id, &bits, &sign, &floating,
                      &rate, &channels, &frame, &second, &be);
        g_assert_cmpuint(bits, ==, 16);
        g_assert_true(sign);
        g_assert_false(floating);
        g_assert_false(be);
        g_assert_cmpuint(rate, ==, 48000);
        g_assert_cmpuint(channels, ==, 1);
        g_assert_cmpuint(frame, ==, 2);
        g_assert_cmpuint(second, ==, 96000);
        l->init++;
    } else if (!strcmp(name, "Fini")) {
        l->fini++;
        l->enabled = false;
    } else if (!strcmp(name, "SetEnabled")) {
        guint64 id;
        gboolean enabled;

        g_variant_get(args, "(tb)", &id, &enabled);
        l->enabled = enabled;
    } else if (!strcmp(name, "Write")) {
        g_autoptr(GVariant) data = g_variant_get_child_value(args, 1);
        gsize size;
        const uint8_t *bytes = g_variant_get_fixed_array(data, &size, 1);

        g_assert_true(l->enabled);
        g_assert_cmpuint(size, >, 0);
        g_assert_cmpuint(size % 2, ==, 0);
        g_assert_cmpuint(size, <=, 512);
        l->writes++;
        l->bytes += size;
        g_byte_array_append(l->pcm, bytes, size);
        if (l->stall) {
            g_ptr_array_add(l->held, g_object_ref(invocation));
            return;
        }
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *property(GDBusConnection *conn, const char *sender,
                          const char *path, const char *iface,
                          const char *name, GError **err, gpointer opaque)
{
    return g_variant_new_strv(NULL, 0);
}

static const GDBusInterfaceVTable vtable = {
    .method_call = method, .get_property = property,
};

static void attach(GDBusConnection *control, Listener *l)
{
    int pair[2];
    g_autoptr(GError) err = NULL;
    g_autoptr(GUnixFDList) fds = g_unix_fd_list_new();
    g_autoptr(GVariant) reply = NULL;
    GThread *thread;
    int handle;
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    memset(l, 0, sizeof(*l));
    l->held = g_ptr_array_new_with_free_func(g_object_unref);
    l->pcm = g_byte_array_new();
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), ==, 0);
    handle = g_unix_fd_list_append(fds, pair[1], &err);
    g_assert_no_error(err);
    close(pair[1]);
    thread = g_thread_new("audio-listener", connect_thread,
                          GINT_TO_POINTER(pair[0]));
    reply = g_dbus_connection_call_with_unix_fd_list_sync(control, NULL,
        "/org/qemu/Display1/Audio", "org.qemu.Display1.Audio",
        "RegisterOutListener", g_variant_new("(h)", handle), NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, fds, NULL, NULL, &err);
    g_assert_no_error(err);
    l->conn = g_thread_join(thread);
    l->registration = g_dbus_connection_register_object(l->conn,
        "/org/qemu/Display1/AudioOutListener",
        qemu_dbus_display1_audio_out_listener_interface_info(), &vtable,
        l, NULL, &err);
    g_assert_no_error(err);
    g_dbus_connection_start_message_processing(l->conn);
    while ((!l->init || !l->enabled) && g_get_monotonic_time() < deadline) {
        pump();
        g_usleep(1000);
    }
    g_assert_cmpuint(l->init, ==, 1);
    g_assert_true(l->enabled);
}

static void detach(Listener *l)
{
    g_dbus_connection_close_sync(l->conn, NULL, NULL);
    g_dbus_connection_unregister_object(l->conn, l->registration);
    g_clear_object(&l->conn);
    g_ptr_array_unref(l->held);
    g_byte_array_unref(l->pcm);
}

static void wait_bytes(Listener *l, unsigned bytes)
{
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    while (l->bytes < bytes && g_get_monotonic_time() < deadline) {
        pump();
        g_usleep(1000);
    }
    g_assert_cmpuint(l->bytes, ==, bytes);
}

static void sampled_tail(QTestState *qts)
{
    /* Nine delivered frames followed by a header-only final block. */
    static const uint8_t packet[] = {
        0xbc,0xad,0,0,0,0,0,0,0x11,0x11,0x11,0x11
    };
    static const uint8_t final[] = {0xbc,0xad,0,0,0,0,0,0};

    qtest_writew(qts, 0xfe1a, 9);
    qtest_writew(qts, 0xff16, 0x400);
    qtest_writel(qts, 0x10000, 0xfe0d00cc);
    qtest_writew(qts, 0xe836, 0x14);
    qtest_writew(qts, 0xe838, 1);
    qtest_writew(qts, 0xef3a, 4);
    qtest_memwrite(qts, 0xeb80, packet, sizeof(packet));
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    g_usleep(10000);
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    qtest_memwrite(qts, 0xeb80, final, sizeof(final));
    qtest_writew(qts, 0xe836, 0x15);
    qtest_writew(qts, 0xef3a, 4);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    g_usleep(10000);
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
}

static void test_direct(void)
{
    QTestState *qts = qtest_init(
        "-M siemens-c55,audiodev=audio "
        "-accel tcg -S -icount shift=0,align=off,sleep=off "
        "-device loader,addr=0x10000,data=0xfe0d00cc,data-len=4 "
        "-device loader,addr=0x10000,cpu-num=0 "
        "-audiodev dbus,id=audio,direct-pcm=on "
        "-display dbus,p2p=on,gl=off,audiodev=audio");
    int pair[2];
    g_autoptr(GDBusConnection) control = NULL;
    Listener l;
    gint64 deadline;
    unsigned resets;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), ==, 0);
    qtest_qmp_add_client(qts, "@dbus-display", pair[1]);
    control = from_fd(pair[0]);
    g_dbus_connection_start_message_processing(control);
    attach(control, &l);
    sampled_tail(qts);
    wait_bytes(&l, 120);
    g_assert_cmpuint(l.writes, ==, 1); /* No backend padding to 256 frames. */
    g_autofree char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                       l.pcm->data, l.pcm->len);
    g_assert_cmpstr(hash, ==,
        "babe03f70ab2a317a036510c12b988c5a92dad90ce9386621cff3d4e91594aeb");
    resets = l.init;
    qtest_writew(qts, 0xe836, 0x17);
    qtest_writew(qts, 0xef3a, 4);
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (l.init == resets && g_get_monotonic_time() < deadline) {
        pump();
        g_usleep(1000);
    }
    g_assert_cmpuint(l.init, ==, resets + 1);
    g_assert_cmpuint(l.fini, ==, resets);
    detach(&l);
    /* Let QEMU's peer-closed handler remove the old registration. */
    g_usleep(20000);
    attach(control, &l);
    /* A new listener never replays old PCM. */
    g_assert_cmpuint(l.bytes, ==, 0);
    sampled_tail(qts);
    wait_bytes(&l, 120);
    l.stall = true;
    sampled_tail(qts);
    wait_bytes(&l, 240);
    deadline = g_get_monotonic_time() + 7 * G_TIME_SPAN_SECOND;
    while (!g_dbus_connection_is_closed(l.conn) &&
           g_get_monotonic_time() < deadline) {
        pump();
        g_usleep(1000);
    }
    g_assert_true(g_dbus_connection_is_closed(l.conn));
    g_assert_cmpuint(l.held->len, ==, 1);
    detach(&l);
    g_clear_object(&control);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("audio/dbus-direct", test_direct);
    return g_test_run();
}
