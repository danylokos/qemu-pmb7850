/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/audio/x55_speaker.h"

typedef struct Capture {
    GArray *pcm, *events;
    unsigned calls, resets;
    uint64_t tick;
    bool overflow;
} Capture;

static void output(void *opaque, uint64_t tick, const int16_t *pcm, size_t n)
{
    Capture *c = opaque;

    g_assert_cmpuint(n, <=, 256);
    g_array_append_vals(c->pcm, pcm, n);
    g_array_append_val(c->events, tick);
    c->calls++;
    c->tick = tick;
}

static void reset(void *opaque, uint64_t tick, bool overflow)
{
    Capture *c = opaque;

    uint64_t event = tick | (1ULL << 63);
    g_array_append_val(c->events, event);
    c->resets++;
    c->tick = tick;
    c->overflow = overflow;
}

static void init(X55Speaker *s, Capture *c, uint32_t hz)
{
    *c = (Capture) { .pcm = g_array_new(false, false, sizeof(int16_t)),
                     .events = g_array_new(false, false, sizeof(uint64_t)) };
    *s = (X55Speaker) { .clock_hz = hz, .output = output,
                       .reset = reset, .opaque = c };
}

static void finish(X55Speaker *s, Capture *c)
{
    x55_speaker_clear(s, 0);
    g_array_unref(c->pcm);
    g_array_unref(c->events);
}

static void test_interpolation(void)
{
    static const uint32_t rates[] = { 8000, 16000, 44100, 48000, 96000 };
    const int16_t mono[] = { -300, -101, 0, 99, 301, 700 };
    const int16_t stereo[] = { 100, -99, 300, 100 };
    const int16_t expected[] = { 0, 66, 133, 200, 200, 200 };
    X55Speaker a, b;
    Capture ca, cb;

    init(&a, &ca, 48000);
    g_assert_cmpint(x55_speaker_push(&a, 0, 0, 16000, 2, stereo, 4), ==, 0);
    x55_speaker_drain(&a, 0, 0);
    x55_speaker_advance(&a, 100);
    g_assert_cmpmem(ca.pcm->data, ca.pcm->len * 2, expected, sizeof(expected));
    finish(&a, &ca);
    for (unsigned i = 0; i < ARRAY_SIZE(rates); i++) {
        init(&a, &ca, 26000000);
        init(&b, &cb, 26000000);
        g_assert_cmpint(x55_speaker_push(&a, 0, 0, rates[i], 1, mono, 6), ==, 0);
        for (unsigned n = 0; n < 6; n++) {
            g_assert_cmpint(x55_speaker_push(&b, 0, 0, rates[i], 1,
                                            mono + n, 1), ==, 0);
        }
        x55_speaker_drain(&a, 0, 0);
        x55_speaker_drain(&b, 0, 0);
        x55_speaker_advance(&a, 260000);
        /* One-tick and batched advancement must have identical PCM/context. */
        for (uint64_t tick = 1; tick <= 260000; tick++) {
            x55_speaker_advance(&b, tick);
        }
        g_assert_cmpuint(ca.pcm->len, ==, DIV_ROUND_UP(6 * 48000, rates[i]));
        g_assert_cmpmem(ca.pcm->data, ca.pcm->len * 2,
                        cb.pcm->data, cb.pcm->len * 2);
        g_assert_cmpuint(ca.tick, ==, cb.tick);
        finish(&a, &ca);
        finish(&b, &cb);
    }
}

static void test_lifecycle(void)
{
    X55Speaker s;
    Capture c;
    int16_t first = 10, second = 20;

    init(&s, &c, 48000);
    g_assert_cmpint(x55_speaker_push(&s, 0, 0, 48000, 1, &first, 1), ==, 0);
    g_assert_cmpuint(x55_speaker_deadline(&s), ==, UINT64_MAX);
    x55_speaker_advance(&s, 200);
    g_assert_cmpuint(s.fraction, ==, 0);
    g_assert_cmpuint(c.calls, ==, 0);
    g_assert_cmpint(x55_speaker_push(&s, 0, 200, 48000, 1, &second, 1), ==, 0);
    x55_speaker_advance(&s, 201);
    g_assert_cmpuint(s.used, ==, 1);
    x55_speaker_advance(&s, 400);
    g_assert_cmpuint(s.used, ==, 1);
    g_assert_cmpuint(c.calls, ==, 0);
    x55_speaker_drain(&s, 0, 400);
    x55_speaker_advance(&s, 401);
    g_assert_cmpuint(c.calls, ==, 1);
    g_assert_cmpuint(c.pcm->len, ==, 2);
    g_assert_cmpint(g_array_index(c.pcm, int16_t, 0), ==, 10);
    g_assert_cmpint(g_array_index(c.pcm, int16_t, 1), ==, 20);
    g_assert_cmpint(x55_speaker_push(&s, 0, 401, 16000, 1, &first, 1), ==, 0);
    g_assert_cmpint(x55_speaker_push(&s, 0, 401, 8000, 1, &first, 1), ==, -1);
    x55_speaker_cancel(&s, 0, 401, false);
    g_assert_cmpuint(c.resets, ==, 1);
    g_assert_false(s.input[0].held);
    g_assert_false(c.overflow);
    finish(&s, &c);
}

static void test_mix_bounds(void)
{
    X55Speaker s;
    Capture c;
    int16_t full[256];
    int16_t loud = 32767, quiet = -250;
    int16_t slow[9] = { 0 };

    init(&s, &c, 48000);
    for (unsigned i = 0; i < 256; i++) {
        full[i] = i;
    }
    g_assert_cmpint(x55_speaker_push(&s, 0, 0, 48000, 1, full, 256), ==, 0);
    x55_speaker_drain(&s, 0, 0);
    x55_speaker_advance(&s, 256);
    g_assert_cmpuint(c.calls, ==, 1);
    g_assert_cmpmem(c.pcm->data, c.pcm->len * 2, full, sizeof(full));
    g_assert_cmpint(x55_speaker_push(&s, 0, 256, 48000, 1, &loud, 1), ==, 0);
    g_assert_cmpint(x55_speaker_push(&s, 1, 256, 48000, 1, &quiet, 1), ==, 0);
    x55_speaker_drain(&s, 0, 256);
    x55_speaker_drain(&s, 1, 256);
    x55_speaker_edge(&s, 256, true);
    x55_speaker_advance(&s, 257);
    g_assert_cmpuint(s.clipped, ==, 1);
    x55_speaker_cancel(&s, 0, 257, false);
    g_assert_cmpuint(s.used, ==, 0);
    x55_speaker_clear(&s, 257);
    x55_speaker_edge(&s, 257, false);
    x55_speaker_advance(&s, 2657);
    g_assert_false(s.ringer);
    g_assert_cmpuint(c.pcm->len, ==, 2656);
    for (unsigned i = 256; i < c.pcm->len; i++) {
        g_assert_cmpint(g_array_index(c.pcm, int16_t, i), ==, -6553);
    }
    g_assert_cmpint(x55_speaker_push(&s, 0, 2657, 1, 1, slow, 8), ==, 0);
    g_assert_cmpuint(s.input[0].count, ==, X55_SPEAKER_LIMIT - 48000);
    g_assert_cmpint(x55_speaker_push(&s, 0, 2657, 1, 1, slow + 8, 1), ==, 1);
    g_assert_true(c.overflow);
    g_assert_cmpuint(c.resets, ==, 2);
    g_assert_cmpuint(s.overflows, ==, 1);
    g_assert_cmpuint(s.input[0].count, ==, 0);
    g_assert_false(s.input[0].held);
    finish(&s, &c);
}

static unsigned wakeups;

static void until(X55Speaker *s, bool batched, uint64_t end)
{
    if (!batched) {
        while (s->tick < end) {
            x55_speaker_advance(s, s->tick + 1);
        }
        return;
    }
    uint64_t deadline;
    while ((deadline = x55_speaker_deadline(s)) <= end) {
        g_assert_cmpuint(deadline, >, s->tick);
        x55_speaker_advance(s, deadline);
        wakeups++;
    }
    x55_speaker_advance(s, end);
}

static void schedule_scenario(Capture *capture, bool batched)
{
    X55Speaker s;
    int16_t data[1000];

    init(&s, capture, 26000000);
    for (unsigned i = 0; i < ARRAY_SIZE(data); i++) {
        data[i] = (i * 1973) % 65536 - 32768;
    }
    /* Open starvation, recovery, short drain, overlapping sources and edges. */
    x55_speaker_push(&s, 0, 0, 16000, 1, data, 1);
    until(&s, batched, 500000);
    x55_speaker_push(&s, 0, s.tick, 16000, 1, data + 1, 7);
    until(&s, batched, 900000);
    x55_speaker_drain(&s, 0, s.tick);
    until(&s, batched, 1000000);
    x55_speaker_push(&s, 0, s.tick, 44100, 1, data, 1000);
    x55_speaker_push(&s, 1, s.tick, 16000, 2, data, 600);
    x55_speaker_edge(&s, s.tick, true);
    until(&s, batched, 1000783);
    x55_speaker_edge(&s, s.tick, false);
    until(&s, batched, 1087543);
    x55_speaker_drain(&s, 0, s.tick);
    until(&s, batched, 1100000);
    x55_speaker_cancel(&s, 1, s.tick, false);
    until(&s, batched, 1200000);
    x55_speaker_push(&s, 1, s.tick, 8000, 1, data, 13);
    x55_speaker_drain(&s, 1, s.tick);
    until(&s, batched, 2700000);
    /* Stop/restart and fractional state at a clear/restore-style boundary. */
    x55_speaker_clear(&s, s.tick);
    x55_speaker_push(&s, 0, s.tick, 96000, 1, data, 17);
    x55_speaker_drain(&s, 0, s.tick);
    until(&s, batched, 3000000);
    x55_speaker_clear(&s, s.tick);
}

static void test_deadlines(void)
{
    Capture a, b;

    schedule_scenario(&a, false);
    schedule_scenario(&b, true);
    g_assert_cmpmem(a.pcm->data, a.pcm->len * 2, b.pcm->data, b.pcm->len * 2);
    g_assert_cmpmem(a.events->data, a.events->len * 8,
                   b.events->data, b.events->len * 8);
    g_assert_cmpuint(a.resets, ==, b.resets);
    g_assert_cmpuint(a.calls, ==, b.calls);
    g_assert_cmpuint(wakeups, <, 20);
    g_array_unref(a.pcm);
    g_array_unref(b.pcm);
    g_array_unref(a.events);
    g_array_unref(b.events);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/x55/speaker/interpolation", test_interpolation);
    g_test_add_func("/x55/speaker/lifecycle", test_lifecycle);
    g_test_add_func("/x55/speaker/mix-bounds", test_mix_bounds);
    g_test_add_func("/x55/speaker/deadlines", test_deadlines);
    return g_test_run();
}
