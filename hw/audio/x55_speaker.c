/*
 * Deterministic terminal speaker, separate from transport and audio backend.
 * Linear reconstruction, ringer +/-6553 and 50-ms tail are emulator policy.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/audio/x55_speaker.h"

static void queue_clear(X55PCMQueue *q)
{
    g_free(q->data);
    memset(q, 0, sizeof(*q));
}

static void queue_interval(X55PCMQueue *q, int16_t next)
{
    while (q->phase < X55_SPEAKER_RATE) {
        int64_t delta = (int32_t)next - q->last;
        int16_t value = q->last + delta * q->phase / X55_SPEAKER_RATE;

        assert(q->count < X55_SPEAKER_LIMIT);
        q->data[(q->head + q->count++) % X55_SPEAKER_LIMIT] = value;
        q->phase += q->rate;
    }
    q->phase -= X55_SPEAKER_RATE;
    q->last = next;
}

static bool speaker_ready(const X55Speaker *s)
{
    if (s->ringer) {
        return true;
    }
    for (unsigned i = 0; i < X55_SPEAKER_SOURCES; i++) {
        if (s->input[i].count) {
            return true;
        }
    }
    return false;
}

static bool speaker_open(const X55Speaker *s)
{
    if (speaker_ready(s)) {
        return true;
    }
    for (unsigned i = 0; i < X55_SPEAKER_SOURCES; i++) {
        if (s->input[i].held) {
            return true;
        }
    }
    return false;
}

static void speaker_flush(X55Speaker *s)
{
    if (!s->used) {
        return;
    }
    if (s->output) {
        s->output(s->opaque, s->tick, s->block, s->used);
    }
    s->frames += s->used;
    s->blocks++;
    s->used = 0;
}

static uint64_t speaker_sample_deadline(const X55Speaker *s)
{
    uint64_t delta;

    if (!speaker_ready(s) || !s->clock_hz) {
        return UINT64_MAX;
    }
    delta = DIV_ROUND_UP((uint64_t)s->clock_hz - s->fraction,
                        X55_SPEAKER_RATE);
    if (s->ringer && s->ringer_end > s->tick) {
        delta = MIN(delta, s->ringer_end - s->tick);
    }
    return s->tick + delta;
}

/* Coalesce wakeups, preserving sample advancement and observable edges. */
uint64_t x55_speaker_deadline(const X55Speaker *s)
{
    uint64_t frames = X55_SPEAKER_BLOCK - s->used;
    uint64_t delta;

    if (!speaker_ready(s) || !s->clock_hz) {
        return UINT64_MAX;
    }
    for (unsigned i = 0; i < X55_SPEAKER_SOURCES; i++) {
        if (s->input[i].count) {
            frames = MIN(frames, s->input[i].count);
        }
    }
    delta = DIV_ROUND_UP(frames * s->clock_hz - s->fraction,
                        X55_SPEAKER_RATE);
    if (s->ringer && s->ringer_end > s->tick) {
        delta = MIN(delta, s->ringer_end - s->tick);
    }
    return s->tick + delta;
}

void x55_speaker_advance(X55Speaker *s, uint64_t tick)
{
    assert(tick >= s->tick && s->clock_hz);
    while (s->tick < tick && speaker_ready(s)) {
        uint64_t next = MIN(tick, speaker_sample_deadline(s));
        uint64_t fraction = s->fraction +
                            (next - s->tick) * X55_SPEAKER_RATE;

        s->tick = next;
        while (fraction >= s->clock_hz && speaker_ready(s)) {
            int32_t mix = s->ringer ? (s->ringer_high ? 6553 : -6553) : 0;

            fraction -= s->clock_hz;
            for (unsigned i = 0; i < X55_SPEAKER_SOURCES; i++) {
                X55PCMQueue *q = &s->input[i];

                if (q->count) {
                    mix += q->data[q->head];
                    q->head = (q->head + 1) % X55_SPEAKER_LIMIT;
                    q->count--;
                }
            }
            s->clipped += mix < -32768 || mix > 32767;
            s->block[s->used++] = CLAMP(mix, -32768, 32767);
            if (s->used == X55_SPEAKER_BLOCK) {
                speaker_flush(s);
            }
        }
        s->fraction = fraction % s->clock_hz;
        if (s->ringer && s->tick >= s->ringer_end) {
            s->ringer = false;
        }
        if (!speaker_open(s)) {
            speaker_flush(s);
        }
    }
    /* Starvation discards idle time, not the fractional sample phase. */
    s->tick = tick;
}

void x55_speaker_cancel(X55Speaker *s, unsigned source, uint64_t tick,
                        bool overflow)
{
    assert(source < X55_SPEAKER_SOURCES);
    x55_speaker_advance(s, tick);
    queue_clear(&s->input[source]);
    s->used = 0;
    s->fraction = 0;
    s->overflows += overflow;
    if (s->reset) {
        s->reset(s->opaque, tick, overflow);
    }
}

int x55_speaker_push(X55Speaker *s, unsigned source, uint64_t tick,
                     uint32_t rate, unsigned channels,
                     const int16_t *pcm, size_t samples)
{
    X55PCMQueue *q;
    uint64_t intervals, pending;

    if (source >= X55_SPEAKER_SOURCES || !rate || rate > 192000 ||
        channels < 1 || channels > 2 || !pcm || !samples ||
        samples % channels) {
        return -1;
    }
    q = &s->input[source];
    if (q->held && q->rate != rate) {
        return -1;
    }
    x55_speaker_advance(s, tick);
    intervals = samples / channels + q->held;
    if (intervals > UINT64_MAX / X55_SPEAKER_RATE) {
        pending = UINT64_MAX;
    } else {
        uint64_t span = intervals * X55_SPEAKER_RATE;
        pending = span > q->phase ? DIV_ROUND_UP(span - q->phase, rate) : 0;
    }
    /* Include the last, deferred interval in the same bounded capacity. */
    if (pending > X55_SPEAKER_LIMIT - q->count) {
        x55_speaker_cancel(s, source, tick, true);
        return 1;
    }
    if (!q->data) {
        q->data = g_new(int16_t, X55_SPEAKER_LIMIT);
    }
    q->rate = rate;
    for (size_t i = 0; i < samples; i += channels) {
        int16_t value = channels == 1 ? pcm[i] :
                        ((int32_t)pcm[i] + pcm[i + 1]) / 2;

        if (q->held) {
            queue_interval(q, value);
        } else {
            q->last = value;
            q->held = true;
        }
    }
    return 0;
}

void x55_speaker_drain(X55Speaker *s, unsigned source, uint64_t tick)
{
    X55PCMQueue *q = &s->input[source];

    assert(source < X55_SPEAKER_SOURCES);
    x55_speaker_advance(s, tick);
    if (q->held) {
        queue_interval(q, q->last);
        q->held = false;
        q->rate = q->phase = 0;
    }
    if (!speaker_open(s)) {
        speaker_flush(s);
    }
}

void x55_speaker_edge(X55Speaker *s, uint64_t tick, bool high)
{
    x55_speaker_advance(s, tick);
    s->ringer = true;
    s->ringer_high = high;
    s->ringer_end = tick + s->clock_hz / 20;
}

void x55_speaker_clear(X55Speaker *s, uint64_t tick)
{
    for (unsigned i = 0; i < X55_SPEAKER_SOURCES; i++) {
        queue_clear(&s->input[i]);
    }
    s->tick = tick;
    s->fraction = s->used = 0;
    s->ringer = false;
    s->ringer_end = 0;
}
