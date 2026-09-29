/*
 * Qualified EB80/E836 digital stream contract, unknown physical consumer.
 * Pure guest-tick transport, independent of QEMU address-space aliases.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/audio/x55_stream.h"

static void stream_event(X55Stream *s, uint64_t tick, const char *phase,
                          const uint8_t *bytes, unsigned size,
                          const char *outcome)
{
    if (s->event) {
        s->event(s->opaque, tick, phase, bytes, size, outcome);
    }
}

static const char *sampled_packet(X55Stream *s, uint64_t tick,
                                  const uint8_t *bytes, unsigned size)
{
    int16_t pcm[X55_STREAM_PACKET * 2];
    int count, pushed;

    if (!s->rate) {
        return "unsupported-rate";
    }
    if (s->sampled_failed) {
        return "failed";
    }
    count = x55_ima_packet(&s->ima, bytes, size, pcm, ARRAY_SIZE(pcm));
    if (count < 0) {
        s->sampled_failed = true;
        return "malformed-envelope";
    }
    s->received += size - 4;
    s->emitted += count;
    pushed = x55_speaker_push(s->speaker, 0, tick, s->rate, 1, pcm, count);
    return pushed < 0 ? "invalid-output" : pushed > 0 ? "output-overflow" : "ok";
}

static const char *decode_packet(X55Stream *s, uint64_t tick,
                                 const uint8_t *bytes, unsigned size)
{
    if (s->kind == X55_STREAM_SAMPLED) {
        return sampled_packet(s, tick, bytes, size);
    }
    int16_t pcm[X55_SI3_PACKET_MAX];
    int count = x55_si3_packet(&s->si3, s->speaker->clock_hz, bytes, size,
                               pcm, ARRAY_SIZE(pcm));
    int pushed;

    if (count < 0) {
        return "malformed";
    }
    if (!count) {
        return "no-pcm";
    }
    pushed = x55_speaker_push(s->speaker, 0, tick, X55_SI3_RATE, 1, pcm, count);
    return pushed == 0 ? "pcm" : pushed > 0 ? "output-overflow" :
                                            "invalid-output";
}

static void accept_packet(X55Stream *s, uint64_t tick,
                           const uint8_t *bytes, unsigned size,
                           bool classified)
{
    const char *outcome;
    unsigned slot;

    if (s->queued == X55_STREAM_QUEUE) {
        stw_le_p(s->registers, 0xaaaa);
        s->retried++;
        stream_event(s, tick, "retry", bytes, size, "queue-full");
        return;
    }
    slot = (s->head + s->queued) % X55_STREAM_QUEUE;
    memcpy(s->owned[slot], bytes, size);
    bytes = s->owned[slot];
    s->lengths[slot] = size;
    s->queued++;
    s->accepted++;
    s->release_at = tick + 1;
    outcome = decode_packet(s, tick, bytes, size);
    stw_le_p(s->registers, 0x5555);
    stream_event(s, tick, "accepted", bytes, size, outcome);
}

static void codec_clear(X55Stream *s)
{
    memset(&s->si3, 0, sizeof(s->si3));
    memset(&s->wave, 0, sizeof(s->wave));
    memset(&s->ima, 0, sizeof(s->ima));
    s->rate = 0;
    s->sampled_active = s->sampled_failed = false;
    s->source_ram = -1;
    s->source_offset = s->received = s->emitted = s->partial_size = 0;
}

/* Retire decoder ownership, not speaker data or transport acknowledgements. */
static void sampled_retire(X55Stream *s)
{
    codec_clear(s);
    s->kind = X55_STREAM_NONE;
    s->replay = false;
    s->replay_size = 0;
}

uint64_t x55_stream_deadline(const X55Stream *s)
{
    return MIN(s->settle_at ? s->settle_at : UINT64_MAX,
               s->release_at ? s->release_at : UINT64_MAX);
}

void x55_stream_advance(X55Stream *s, uint64_t tick)
{
    while (x55_stream_deadline(s) <= tick) {
        uint64_t when = x55_stream_deadline(s);

        if (s->release_at && s->release_at == when) {
            stream_event(s, when, "released", s->owned[s->head],
                          s->lengths[s->head], "done");
            s->head = (s->head + 1) % X55_STREAM_QUEUE;
            s->queued--;
            s->released++;
            s->release_at = s->queued ? when + 1 : 0;
        }
        if (s->settle_at && s->settle_at == when) {
            bool nonzero = false;

            s->settle_at = 0;
            for (unsigned i = 0; i < s->length; i++) {
                nonzero |= s->registers[i] != 0;
            }
            if (!nonzero) {
                s->length = 0;
                continue;
            }
            s->packets++;
            if (s->kind != X55_STREAM_NONE) {
                accept_packet(s, when, s->registers, s->length, false);
            } else {
                memcpy(s->pending, s->registers, s->length);
                s->pending_size = s->length;
                s->retained = true;
                stream_event(s, when, "retained", s->pending,
                              s->pending_size, "unclassified");
            }
            s->length = 0;
        }
    }
}

void x55_stream_write(X55Stream *s, uint64_t tick, unsigned offset,
                       uint8_t value)
{
    assert(offset < X55_STREAM_PACKET);
    x55_stream_advance(s, tick);
    s->registers[offset] = value;
    /*
     * Settlement before classification may be a firmware interrupt mid-write.
     * The unclaimed packet stays writable until the command owns it.
     */
    if (offset && s->retained && !s->length) {
        s->pending[offset] = value;
        s->pending_size = MAX(s->pending_size, offset + 1);
        stream_event(s, tick, "retained", s->pending, s->pending_size,
                     "continued-write");
        return;
    }
    if (!offset) {
        if (s->retained) {
            s->replaced++;
            s->retained = false;
            stream_event(s, tick, "replaced", s->pending,
                          s->pending_size, "new-base-write");
        }
        s->length = 0;
        s->settle_at = 0;
        s->length = 1;
    }
    if (s->length) {
        s->length = MAX(s->length, offset + 1);
        s->settle_at = tick + 32;
    }
}


void x55_stream_command(X55Stream *s, uint64_t tick, uint16_t command)
{
    x55_stream_advance(s, tick);
    s->command = command;
    s->command_pending = !(command & 0x8000) && command >= 0x10 && command <= 0x23;
}

void x55_stream_submit(X55Stream *s, uint64_t tick)
{
    if (!s->command_pending) {
        return;
    }
    uint16_t command = s->command;
    s->command_pending = false;
    /* Stop commits the completed, still-settling payload before retiring it. */
    if (command == 0x15 && s->length) {
        s->settle_at = tick;
    }
    x55_stream_advance(s, tick);
    if (command & 0x8000 || command < 0x10 || command > 0x23) {
        s->command = command;
        return;
    }
    s->command = command | 0x8000;
    stream_event(s, tick, "command", s->arguments, sizeof(s->arguments), "ack");
    if (command == 0x14 || command == 0x16) {
        x55_speaker_cancel(s->speaker, 0, tick, false);
        codec_clear(s);
        s->kind = command == 0x14 ? X55_STREAM_SAMPLED : X55_STREAM_SI3;
        if (command == 0x14) {
            unsigned selector = lduw_le_p(s->arguments);
            s->rate = selector == 1 ? 8000 : selector == 2 ? 16000 : 0;
            s->sampled_active = s->rate != 0;
        }
        s->replay = false;
        if (s->retained) {
            accept_packet(s, tick, s->pending, s->pending_size, true);
            s->retained = false;
        }
    } else if (command == 0x15) {
        stream_event(s, tick, "drain", NULL, 0,
                     s->sampled_failed ? "failed" : "delivered-only");
        x55_speaker_drain(s->speaker, 0, tick);
        if (s->kind == X55_STREAM_SAMPLED) {
            sampled_retire(s);
        }
    } else if (command == 0x17) {
        x55_speaker_cancel(s->speaker, 0, tick, false);
        codec_clear(s);
        s->kind = X55_STREAM_NONE;
        s->settle_at = s->release_at = 0;
        s->length = s->queued = s->head = 0;
        s->retained = s->replay = false;
    }
}

void x55_stream_clear(X55Stream *s)
{
    X55Stream keep = { .speaker = s->speaker, .event = s->event,
                      .opaque = s->opaque, .source_ram = -1 };
    *s = keep;
}
