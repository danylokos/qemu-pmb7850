/*
 * QOM composition of the qualified X55 digital stream and terminal speaker.
 * Host playback consumes only final PCM; no firmware resource/global lookup.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "migration/qemu-file-types.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/audio.h"
#include "exec/icount.h"
#include "hw/core/qdev-clock.h"
#include "system/reset.h"
#include "hw/audio/x55_audio.h"
#include "hw/audio/x55_stream.h"
#include "trace.h"

struct X55AudioState {
    DeviceState parent_obj;
    Clock *clock;
    QEMUTimer timer;
    X55Speaker speaker;
    X55Stream stream;
    X55AudioContextFn context;
    void *context_opaque;
    bool instruction_timing;
    GChecksum *hash;
    uint64_t resets;
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    uint64_t delivery_dropped;
};

static void audio_backend_ready(void *opaque, int available)
{
    /* The terminal speaker is the clocked producer, never the backend. */
}

static bool audio_backend_open(X55AudioState *s)
{
    const struct audsettings settings = {
        .freq = 48000, .nchannels = 1, .fmt = AUDIO_FORMAT_S16,
        .big_endian = false,
    };

    if (!s->audio_be) {
        return true; /* Headless model qualification needs no host device. */
    }
    s->voice = audio_be_open_out(s->audio_be, NULL, "x55-speaker", s,
                                  audio_backend_ready, &settings);
    if (!s->voice) {
        return false;
    }
    audio_be_set_active_out(s->audio_be, s->voice, true);
    return true;
}

static void audio_backend_reset(X55AudioState *s)
{
    if (s->voice) {
        /* Fini/Init is a discontinuity; a normal partial drain never closes. */
        audio_be_close_out(s->audio_be, s->voice);
        s->voice = NULL;
        if (!audio_backend_open(s)) {
            s->delivery_dropped++;
        }
    }
}

static X55AudioContext audio_context(X55AudioState *s, bool sync)
{
    return s->context(s->context_opaque, sync);
}

static void audio_arm(X55AudioState *s)
{
    uint64_t next = MIN(x55_speaker_deadline(&s->speaker),
                        x55_stream_deadline(&s->stream));

    if (next == UINT64_MAX) {
        timer_del(&s->timer);
    } else {
        uint64_t now = audio_context(s, false).tick;
        uint64_t delta = next > now ? next - now : 0;
        uint64_t ns = s->instruction_timing ? icount_to_ns(delta) :
                      clock_ticks_to_ns(s->clock, delta);

        timer_mod_ns(&s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
    }
}

static void audio_advance(X55AudioState *s)
{
    uint64_t tick = audio_context(s, true).tick;

    x55_stream_advance(&s->stream, tick);
    x55_speaker_advance(&s->speaker, tick);
    audio_arm(s);
}

static void audio_timer(void *opaque)
{
    audio_advance(opaque);
}

static void audio_output(void *opaque, uint64_t tick,
                           const int16_t *pcm, size_t frames)
{
    X55AudioState *s = opaque;
    uint8_t bytes[X55_SPEAKER_BLOCK * 2];

    for (unsigned i = 0; i < frames; i++) {
        stw_le_p(bytes + i * 2, pcm[i]);
    }
    g_checksum_update(s->hash, bytes, frames * 2);
    if (s->voice && audio_be_write(s->audio_be, s->voice, bytes,
                                    frames * 2) != frames * 2) {
        s->delivery_dropped += frames;
        audio_backend_reset(s);
    }
    if (trace_event_get_state_backends(TRACE_X55_AUDIO_PCM)) {
        X55AudioContext ctx = audio_context(s, false);
        g_autofree char *hex = g_malloc(frames * 4 + 1);

        for (unsigned i = 0; i < frames * 2; i++) {
            snprintf(hex + 2 * i, 3, "%02x", bytes[i]);
        }
        trace_x55_audio_pcm(tick, ctx.icount, ctx.pc, s->speaker.blocks + 1,
                             frames, hex);
    }
}

static void audio_reset_output(void *opaque, uint64_t tick, bool overflow)
{
    X55AudioState *s = opaque;
    X55AudioContext ctx = audio_context(s, false);

    s->resets++;
    audio_backend_reset(s);
    trace_x55_audio_reset(tick, ctx.icount, ctx.pc,
                           overflow ? "overflow" : "cancel", s->resets);
}

static void audio_event(void *opaque, uint64_t tick, const char *phase,
                          const uint8_t *bytes, unsigned size,
                          const char *outcome)
{
    X55AudioState *s = opaque;
    X55Stream *stream = &s->stream;
    X55AudioContext ctx = audio_context(s, false);
    char hex[X55_STREAM_PACKET * 2 + 1] = "";

    if (trace_event_get_state_backends(TRACE_X55_AUDIO_PACKET)) {
        for (unsigned i = 0; i < size; i++) {
            snprintf(hex + 2 * i, 3, "%02x", bytes[i]);
        }
        trace_x55_audio_packet(tick, ctx.icount, ctx.pc, phase,
                                stream->command, stream->kind,
                                stream->packets, stream->queued, outcome, hex);
    }

}

uint8_t x55_audio_read(DeviceState *dev, hwaddr addr)
{
    X55AudioState *s = X55_AUDIO(dev);

    audio_advance(s);
    if (addr >= 0xeb80 && addr < 0xec00) {
        return s->stream.registers[addr - 0xeb80];
    }
    if (addr >= 0xe838 && addr <= 0xe83b) {
        return s->stream.arguments[addr - 0xe838];
    }
    assert(addr == 0xe836 || addr == 0xe837);
    return s->stream.command >> (8 * (addr - 0xe836));
}

void x55_audio_write(DeviceState *dev, hwaddr addr, uint64_t value,
                      unsigned size)
{
    X55AudioState *s = X55_AUDIO(dev);
    uint64_t tick;

    audio_advance(s);
    tick = s->speaker.tick;
    if (addr >= 0xeb80 && addr + size <= 0xec00) {
        for (unsigned i = 0; i < size; i++) {
            x55_stream_write(&s->stream, tick, addr + i - 0xeb80,
                              value >> (8 * i));
        }
    } else if (addr >= 0xe838 && addr + size <= 0xe83c) {
        for (unsigned i = 0; i < size; i++) {
            s->stream.arguments[addr + i - 0xe838] = value >> (8 * i);
        }
    } else if (addr == 0xef3a) {
        if (value & 4) {
            x55_stream_submit(&s->stream, tick);
        }
    } else {
        uint8_t raw[2];

        assert(addr >= 0xe836 && addr + size <= 0xe838);
        stw_le_p(raw, s->stream.command);
        for (unsigned i = 0; i < size; i++) {
            raw[addr + i - 0xe836] = value >> (8 * i);
        }
        x55_stream_command(&s->stream, tick, lduw_le_p(raw));
    }
    audio_arm(s);
}

void x55_audio_ringer(DeviceState *dev, uint64_t tick, bool high)
{
    X55AudioState *s = X55_AUDIO(dev);
    X55AudioContext ctx = audio_context(s, false);

    x55_stream_advance(&s->stream, tick);
    x55_speaker_edge(&s->speaker, tick, high);
    trace_x55_audio_ringer(tick, ctx.icount, ctx.pc, high);
    audio_arm(s);
}

void x55_audio_configure(DeviceState *dev, X55AudioContextFn context,
                          void *opaque, bool instruction_timing)
{
    X55AudioState *s = X55_AUDIO(dev);

    assert(!dev->realized);
    s->context = context;
    s->context_opaque = opaque;
    s->instruction_timing = instruction_timing;
}

static void audio_hold(Object *obj, ResetType type)
{
    X55AudioState *s = X55_AUDIO(obj);

    timer_del(&s->timer);
    audio_backend_reset(s);
    x55_stream_clear(&s->stream);
    x55_speaker_clear(&s->speaker, 0);
    s->speaker.frames = s->speaker.blocks = s->speaker.clipped = 0;
    s->speaker.overflows = s->resets = 0;
    g_checksum_reset(s->hash);
    s->delivery_dropped = 0;
}

static void audio_realize(DeviceState *dev, Error **errp)
{
    X55AudioState *s = X55_AUDIO(dev);

    if (!s->context || !clock_has_source(s->clock)) {
        error_setg(errp, "X55 audio requires board timing and sysclk");
        return;
    }
    s->speaker.clock_hz = clock_get_hz(s->clock);
    device_cold_reset(dev);
    if (!audio_backend_open(s)) {
        error_setg(errp, "cannot open X55 fixed-format audio backend");
        return;
    }
    /* This board-composed terminal has no bus parent to propagate resets. */
    qemu_register_resettable(OBJECT(dev));
}

static void audio_unrealize(DeviceState *dev)
{
    X55AudioState *s = X55_AUDIO(dev);

    qemu_unregister_resettable(OBJECT(dev));
    timer_del(&s->timer);
    audio_be_close_out(s->audio_be, s->voice);
    s->voice = NULL;
}

static char *audio_hash(Object *obj, Error **errp)
{
    X55AudioState *s = X55_AUDIO(obj);
    g_autoptr(GChecksum) copy = g_checksum_copy(s->hash);

    return g_strdup(g_checksum_get_string(copy));
}

static void audio_init(Object *obj)
{
    X55AudioState *s = X55_AUDIO(obj);

    s->clock = qdev_init_clock_in(DEVICE(obj), "sysclk", NULL, NULL, 0);
    s->hash = g_checksum_new(G_CHECKSUM_SHA256);
    s->speaker.output = audio_output;
    s->speaker.reset = audio_reset_output;
    s->speaker.opaque = s;
    s->stream.speaker = &s->speaker;
    s->stream.event = audio_event;
    s->stream.opaque = s;
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, audio_timer, s);
    object_property_add_str(obj, "pcm-sha256", audio_hash, NULL);
    object_property_add_uint64_ptr(obj, "pcm-frames", &s->speaker.frames,
                                    OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "stream-accepted", &s->stream.accepted,
                                    OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "stream-retried", &s->stream.retried,
                                    OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "output-resets", &s->resets,
                                    OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "delivery-dropped",
                                  &s->delivery_dropped,
                                  OBJ_PROP_FLAG_READ);
}

static void audio_finalize(Object *obj)
{
    X55AudioState *s = X55_AUDIO(obj);

    timer_del(&s->timer);
    x55_speaker_clear(&s->speaker, 0);
    g_checksum_free(s->hash);
}

/* Save logical PCM queue contents, not host pointers or unused ring slots. */
static int audio_queue_get(QEMUFile *f, void *opaque, size_t size,
                            const VMStateField *field)
{
    X55PCMQueue *q = opaque;
    uint32_t count = qemu_get_be32(f);
    uint32_t rate = qemu_get_be32(f);
    uint32_t phase = qemu_get_be32(f);
    int16_t last = qemu_get_be16(f);
    uint8_t held = qemu_get_byte(f);

    if (count > X55_SPEAKER_LIMIT || held > 1 ||
        ((count || held) && (!rate || rate > 192000))) {
        return -EINVAL;
    }
    g_free(q->data);
    *q = (X55PCMQueue) { .count = count, .rate = rate, .phase = phase,
                         .last = last, .held = held };
    if (count || held) {
        q->data = g_new0(int16_t, X55_SPEAKER_LIMIT);
    }
    for (unsigned i = 0; i < count; i++) {
        q->data[i] = qemu_get_be16(f);
    }
    return 0;
}

static int audio_queue_put(QEMUFile *f, void *opaque, size_t size,
                            const VMStateField *field, JSONWriter *vmdesc)
{
    X55PCMQueue *q = opaque;

    qemu_put_be32(f, q->count);
    qemu_put_be32(f, q->rate);
    qemu_put_be32(f, q->phase);
    qemu_put_be16(f, q->last);
    qemu_put_byte(f, q->held);
    for (unsigned i = 0; i < q->count; i++) {
        qemu_put_be16(f, q->data[(q->head + i) % X55_SPEAKER_LIMIT]);
    }
    return 0;
}

static const VMStateInfo vmstate_audio_queue = {
    .name = "x55/pcm-queue",
    .get = audio_queue_get,
    .put = audio_queue_put,
};

static int audio_size_get(QEMUFile *f, void *opaque, size_t size,
                           const VMStateField *field)
{
    uint64_t value = qemu_get_be64(f);

    if (value > SIZE_MAX) {
        return -EINVAL;
    }
    *(size_t *)opaque = value;
    return 0;
}

static int audio_size_put(QEMUFile *f, void *opaque, size_t size,
                           const VMStateField *field, JSONWriter *vmdesc)
{
    qemu_put_be64(f, *(size_t *)opaque);
    return 0;
}

static const VMStateInfo vmstate_audio_size = {
    .name = "x55/size",
    .get = audio_size_get,
    .put = audio_size_put,
};

static const VMStateDescription vmstate_audio_voice = {
    .name = "x55/si3-voice",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(phase, X55SI3Voice),
        VMSTATE_UINT32(increment, X55SI3Voice),
        VMSTATE_UINT32(release, X55SI3Voice),
        VMSTATE_UINT32(gain, X55SI3Voice),
        VMSTATE_BOOL(active, X55SI3Voice),
        VMSTATE_PARTIAL_BUFFER(parameters, X55SI3Voice, 14),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_audio_si3 = {
    .name = "x55/si3",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(voices, X55SI3, X55_SI3_VOICES, 0,
                             vmstate_audio_voice, X55SI3Voice),
        VMSTATE_UINT64(remainder, X55SI3),
        VMSTATE_UINT64(quanta, X55SI3),
        VMSTATE_UINT64(stops, X55SI3),
        VMSTATE_UINT64(malformed, X55SI3),
        VMSTATE_UINT64_ARRAY(starts, X55SI3, 2),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_audio_wave = {
    .name = "x55/wave",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SINGLE(file_size, X55Wave, 0, vmstate_audio_size, size_t),
        VMSTATE_SINGLE(data_offset, X55Wave, 0, vmstate_audio_size, size_t),
        VMSTATE_UINT32(data_size, X55Wave),
        VMSTATE_UINT32(frames, X55Wave),
        VMSTATE_UINT32(rate, X55Wave),
        VMSTATE_UINT16(channels, X55Wave),
        VMSTATE_UINT16(block_size, X55Wave),
        VMSTATE_UINT16(block_frames, X55Wave),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_audio_speaker = {
    .name = "x55/speaker",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SINGLE(input[0], X55Speaker, 0, vmstate_audio_queue,
                       X55PCMQueue),
        VMSTATE_SINGLE(input[1], X55Speaker, 0, vmstate_audio_queue,
                       X55PCMQueue),
        VMSTATE_UINT32(fraction, X55Speaker),
        VMSTATE_UINT32(used, X55Speaker),
        VMSTATE_UINT64(tick, X55Speaker),
        VMSTATE_UINT64(ringer_end, X55Speaker),
        VMSTATE_UINT64(frames, X55Speaker),
        VMSTATE_UINT64(blocks, X55Speaker),
        VMSTATE_UINT64(clipped, X55Speaker),
        VMSTATE_UINT64(overflows, X55Speaker),
        VMSTATE_BOOL(ringer, X55Speaker),
        VMSTATE_BOOL(ringer_high, X55Speaker),
        VMSTATE_INT16_ARRAY(block, X55Speaker, X55_SPEAKER_BLOCK),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_audio_stream = {
    .name = "x55/stream",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(si3, X55Stream, 0, vmstate_audio_si3, X55SI3),
        VMSTATE_STRUCT(wave, X55Stream, 0, vmstate_audio_wave, X55Wave),
        VMSTATE_SINGLE(source_offset, X55Stream, 0, vmstate_audio_size, size_t),
        VMSTATE_PARTIAL_BUFFER(registers, X55Stream, 48),
        VMSTATE_BUFFER(lengths, X55Stream),
        VMSTATE_PARTIAL_BUFFER(pending, X55Stream, 48),
        VMSTATE_PARTIAL_BUFFER(replay_packet, X55Stream, 48),
        VMSTATE_BUFFER(partial, X55Stream),
        VMSTATE_PARTIAL_BUFFER(owned[0], X55Stream, 48),
        VMSTATE_PARTIAL_BUFFER(owned[1], X55Stream, 48),
        VMSTATE_PARTIAL_BUFFER(owned[2], X55Stream, 48),
        VMSTATE_PARTIAL_BUFFER(owned[3], X55Stream, 48),
        VMSTATE_PARTIAL_BUFFER(owned[4], X55Stream, 48),
        VMSTATE_PARTIAL_BUFFER(owned[5], X55Stream, 48),
        VMSTATE_PARTIAL_BUFFER(owned[6], X55Stream, 48),
        VMSTATE_PARTIAL_BUFFER(owned[7], X55Stream, 48),
        VMSTATE_UINT16(command, X55Stream),
        VMSTATE_UINT32(kind, X55Stream),
        VMSTATE_UINT32(length, X55Stream),
        VMSTATE_UINT32(queued, X55Stream),
        VMSTATE_UINT32(head, X55Stream),
        VMSTATE_UINT32(pending_size, X55Stream),
        VMSTATE_UINT32(replay_size, X55Stream),
        VMSTATE_UINT32(received, X55Stream),
        VMSTATE_UINT32(emitted, X55Stream),
        VMSTATE_UINT32(partial_size, X55Stream),
        VMSTATE_UINT64(settle_at, X55Stream),
        VMSTATE_UINT64(release_at, X55Stream),
        VMSTATE_UINT64(packets, X55Stream),
        VMSTATE_UINT64(accepted, X55Stream),
        VMSTATE_UINT64(retried, X55Stream),
        VMSTATE_UINT64(replaced, X55Stream),
        VMSTATE_UINT64(released, X55Stream),
        VMSTATE_BOOL(retained, X55Stream),
        VMSTATE_BOOL(replay, X55Stream),
        VMSTATE_BOOL(sampled_active, X55Stream),
        VMSTATE_BOOL(sampled_failed, X55Stream),
        VMSTATE_INT32(source_ram, X55Stream),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_audio_voice_extra = {
    .name = "x55/si3-voice-extra", .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER_START_MIDDLE(parameters, X55SI3Voice, 14),
        VMSTATE_UINT8(parameter_length, X55SI3Voice),
        VMSTATE_END_OF_LIST()
    },
};
static const VMStateDescription vmstate_audio_stream_extra = {
    .name = "x55/stream-extra", .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(si3.voices, X55Stream, X55_SI3_VOICES, 0,
                             vmstate_audio_voice_extra, X55SI3Voice),
        VMSTATE_UINT64(si3.controls, X55Stream),
        VMSTATE_BUFFER(si3.last_control, X55Stream),
        VMSTATE_BUFFER_START_MIDDLE(registers, X55Stream, 48),
        VMSTATE_BUFFER_START_MIDDLE(pending, X55Stream, 48),
        VMSTATE_BUFFER_START_MIDDLE(replay_packet, X55Stream, 48),
        VMSTATE_BUFFER_START_MIDDLE(owned[0], X55Stream, 48),
        VMSTATE_BUFFER_START_MIDDLE(owned[1], X55Stream, 48),
        VMSTATE_BUFFER_START_MIDDLE(owned[2], X55Stream, 48),
        VMSTATE_BUFFER_START_MIDDLE(owned[3], X55Stream, 48),
        VMSTATE_BUFFER_START_MIDDLE(owned[4], X55Stream, 48),
        VMSTATE_BUFFER_START_MIDDLE(owned[5], X55Stream, 48),
        VMSTATE_BUFFER_START_MIDDLE(owned[6], X55Stream, 48),
        VMSTATE_BUFFER_START_MIDDLE(owned[7], X55Stream, 48),
        VMSTATE_END_OF_LIST()
    },
};

static int audio_post_load(void *opaque, int version_id)
{
    X55AudioState *s = opaque;
    X55Stream *st = &s->stream;

    if (s->speaker.used > X55_SPEAKER_BLOCK ||
        s->speaker.fraction >= s->speaker.clock_hz ||
        st->kind > X55_STREAM_SI3 || st->length > X55_STREAM_PACKET ||
        st->queued > X55_STREAM_QUEUE || st->head >= X55_STREAM_QUEUE ||
        st->pending_size > X55_STREAM_PACKET ||
        st->replay_size > X55_STREAM_PACKET ||
        st->partial_size > X55_IMA_BLOCK_MAX ||
        st->source_ram < -1) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < X55_STREAM_QUEUE; i++) {
        if (st->lengths[i] > X55_STREAM_PACKET) {
            return -EINVAL;
        }
    }
    if (version_id < 3 && st->kind == X55_STREAM_SI3) {
        error_report("active legacy SI3 state requires the old decoder profile");
        return -EINVAL;
    }
    if (version_id < 2) {
        if (st->sampled_active || (st->kind == X55_STREAM_SAMPLED && st->partial_size)) {
            error_report("active legacy sampled audio cannot be restored without firmware configuration");
            return -EINVAL;
        }
        st->kind = st->kind == X55_STREAM_SAMPLED ? X55_STREAM_NONE : st->kind;
    }
    if (st->ima.index < 0 || st->ima.index > 88 ||
        st->ima.predictor < -32768 || st->ima.predictor > 32767 ||
        (st->rate && st->rate != 8000 && st->rate != 16000)) {
        return -EINVAL;
    }
    /*
     * Destination realization owns callbacks, RAM pointers and the host voice.
     * The PCM hash describes this host session and starts afresh on restore.
     */
    audio_backend_reset(s);
    g_checksum_reset(s->hash);
    return 0;
}

static const VMStateDescription vmstate_audio = {
    .name = "x55/audio",
    .version_id = 3,
    .minimum_version_id = 1,
    .post_load = audio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(speaker, X55AudioState, 0, vmstate_audio_speaker,
                       X55Speaker),
        VMSTATE_STRUCT(stream, X55AudioState, 0, vmstate_audio_stream,
                       X55Stream),
        VMSTATE_TIMER(timer, X55AudioState),
        VMSTATE_UINT64(resets, X55AudioState),
        VMSTATE_UINT32_V(stream.rate, X55AudioState, 2),
        VMSTATE_INT32_V(stream.ima.predictor, X55AudioState, 2),
        VMSTATE_INT32_V(stream.ima.index, X55AudioState, 2),
        VMSTATE_BOOL_V(stream.ima.have_header, X55AudioState, 2),
        VMSTATE_BUFFER_V(stream.arguments, X55AudioState, 2),
        VMSTATE_BOOL_V(stream.command_pending, X55AudioState, 2),
        VMSTATE_STRUCT(stream, X55AudioState, 3, vmstate_audio_stream_extra,
                         X55Stream),
        VMSTATE_END_OF_LIST()
    },
};

static void audio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    static const Property properties[] = {
        DEFINE_AUDIO_PROPERTIES(X55AudioState, audio_be),
    };

    dc->vmsd = &vmstate_audio;
    dc->desc = "Qualified X55 digital audio (calibrated speaker)";
    dc->realize = audio_realize;
    dc->unrealize = audio_unrealize;
    dc->user_creatable = false;
    device_class_set_props(dc, properties);
    rc->phases.hold = audio_hold;
}

static const TypeInfo audio_info = {
    .name = TYPE_X55_AUDIO,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(X55AudioState),
    .instance_init = audio_init,
    .instance_finalize = audio_finalize,
    .class_init = audio_class_init,
};

static void audio_register(void)
{
    type_register_static(&audio_info);
}
type_init(audio_register)
