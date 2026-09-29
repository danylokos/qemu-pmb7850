/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/audio/x55_stream.h"

static const uint8_t note[] = {
    1, 0x91, 0x10, 0x18, 0x14, 0x73, 0x14, 0x14,
    0x0a, 0x4d, 0x72, 0x52, 0xf8, 0xff, 0xa0, 0xa0, 0xcc, 0xc1,
};
static const uint8_t sampled[] = { 0xbc, 0xad, 0, 0, 0, 0, 0, 0, 0x11, 0x11 };

static void send_packet(X55Stream *s, uint64_t tick,
                         const uint8_t *p, unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        x55_stream_write(s, tick, i, p[i]);
    }
}

static void command(X55Stream *s, uint64_t tick, uint16_t value)
{
    x55_stream_command(s, tick, value);
    x55_stream_submit(s, tick);
}

static void test_ordering(void)
{
    X55Speaker speaker = { .clock_hz = 26000000 };
    X55Stream s = { .speaker = &speaker };
    uint8_t zero[48] = { 0 };

    command(&s, 0, 0);
    send_packet(&s, 0, zero, 48);
    x55_stream_advance(&s, 32);
    g_assert_cmpuint(s.command, ==, 0);
    g_assert_cmpuint(s.packets, ==, 0);
    command(&s, 32, 0x16);
    g_assert_cmpuint(s.command, ==, 0x8016);
    send_packet(&s, 175, note, sizeof(note));
    x55_stream_advance(&s, 206);
    g_assert_cmphex(lduw_le_p(s.registers), ==, 0x9101);
    x55_stream_advance(&s, 207);
    g_assert_cmphex(lduw_le_p(s.registers), ==, 0x5555);
    g_assert_cmpuint(s.accepted, ==, 1);
    g_assert_cmpuint(speaker.input[0].count, ==, 216);
    g_assert_cmpuint(s.queued, ==, 1);
    x55_stream_advance(&s, 208);
    g_assert_cmpuint(s.queued, ==, 0);
    command(&s, 208, 0x17);
    send_packet(&s, 208, note, sizeof(note));
    x55_stream_advance(&s, 240);
    g_assert_true(s.retained);
    g_assert_cmphex(lduw_le_p(s.registers), ==, 0x9101);
    x55_stream_advance(&s, 1200240);
    command(&s, 1200240, 0x16);
    g_assert_false(s.retained);
    g_assert_cmpuint(s.accepted, ==, 2);
    g_assert_cmpuint(speaker.input[0].count, ==, 216);
    command(&s, 1200241, 0x20);
    g_assert_cmpint(s.kind, ==, X55_STREAM_SI3);
    send_packet(&s, 1200241, note, sizeof(note));
    x55_stream_advance(&s, 1200273);
    g_assert_cmpuint(s.si3.starts[1], ==, 2); /* New submission, even if identical. */
    command(&s, 1200274, 0x17);
    send_packet(&s, 1200274, sampled, sizeof(sampled));
    x55_stream_advance(&s, 1200306);
    send_packet(&s, 1200306, note, sizeof(note));
    x55_stream_advance(&s, 1200338);
    g_assert_cmpuint(s.replaced, ==, 1);
    g_assert_cmpmem(s.pending, s.pending_size, note, sizeof(note));
    command(&s, 1200338, 0x16);
    x55_stream_advance(&s, 1200339);
    /* Saturated ownership is a diagnostic state, not a firmware assumption. */
    s.queued = 8;
    s.release_at = 0;
    send_packet(&s, 1200340, note, sizeof(note));
    x55_stream_advance(&s, 1200372);
    g_assert_cmphex(lduw_le_p(s.registers), ==, 0xaaaa);
    g_assert_cmpuint(s.retried, ==, 1);
    command(&s, 1200372, 0x17);
    g_assert_cmpuint(s.queued, ==, 0);
    g_assert_false(s.si3.voices[0].active);
    x55_speaker_clear(&speaker, 0);
}


static void test_configured(void)
{
    for (unsigned selector = 1; selector <= 3; selector++) {
        X55Speaker speaker = { .clock_hz = 26000000 };
        X55Stream s = { .speaker = &speaker };
        send_packet(&s, 0, sampled, sizeof(sampled));
        x55_stream_advance(&s, 32);
        x55_stream_command(&s, 33, 0x14);
        g_assert_cmphex(s.command, ==, 0x14);
        g_assert_cmpuint(s.accepted, ==, 0);
        stw_le_p(s.arguments, selector);
        x55_stream_submit(&s, 34);
        g_assert_cmphex(s.command, ==, 0x8014);
        g_assert_cmpuint(s.accepted, ==, 1);
        g_assert_cmpuint(s.rate, ==, selector == 1 ? 8000 : selector == 2 ? 16000 : 0);
        if (selector < 3) {
            g_assert_cmpuint(s.emitted, ==, 5);
            send_packet(&s, 35, sampled, sizeof(sampled));
            x55_stream_advance(&s, 67);
            g_assert_cmpuint(s.emitted, ==, 10);
            const uint8_t tail[] = {0xbc,0xad,0xff,0xff,0x11,0x11};
            send_packet(&s, 68, tail, sizeof(tail));
            command(&s, 69, 0x15);
            g_assert_cmpint(s.kind, ==, X55_STREAM_NONE);
            g_assert_cmpuint(speaker.input[0].count, ==, selector == 1 ? 84 : 42);
        } else {
            g_assert_cmpuint(s.emitted, ==, 0);
        }
        x55_stream_clear(&s);
        x55_speaker_clear(&speaker, 0);
    }
}

static void test_packet_codec(void)
{
    X55IMA s = {0};
    int16_t pcm[64];
    g_assert_cmpint(x55_ima_packet(&s, sampled, sizeof(sampled), pcm, 64), ==, 5);
    for (int i = 0; i < 5; i++) {
        g_assert_cmpint(pcm[i], ==, i);
    }
    const uint8_t cross[] = {0xbc,0xad,1,0,0x11,0x11,100,0,0,0,0x99,0x99};
    g_assert_cmpint(x55_ima_packet(&s, cross, sizeof(cross), pcm, 64), ==, 9);
    g_assert_cmpint(pcm[0], ==, 5);
    g_assert_cmpint(pcm[3], ==, 8);
    g_assert_cmpint(pcm[4], ==, 100);
    g_assert_cmpint(pcm[8], ==, 96);
    X55IMA before = s;
    uint8_t bad[sizeof(cross)];
    memcpy(bad, cross, sizeof(bad));
    bad[8] = 89;
    g_assert_cmpint(x55_ima_packet(&s, bad, sizeof(bad), pcm, 64), ==, -1);
    g_assert_cmpmem(&s, sizeof(s), &before, sizeof(before));
    g_assert_cmpint(x55_ima_packet(&s, cross, sizeof(cross), pcm, 8), ==, -1);
    g_assert_cmpmem(&s, sizeof(s), &before, sizeof(before));
    bad[8] = 0;
    bad[2] = 0xfe;
    g_assert_cmpint(x55_ima_packet(&s, bad, sizeof(bad), pcm, 64), ==, -1);
    memset(&s, 0, sizeof(s));
    g_assert_cmpint(x55_ima_packet(&s, cross, sizeof(cross), pcm, 64), ==, -1);
}

static void test_full_packet(void)
{
    X55Speaker speaker = { .clock_hz = 26000000 };
    X55Stream s = { .speaker = &speaker };
    uint8_t packet[128];
    for (unsigned i = 0; i < 128; i += 2) { packet[i] = 0xcc; packet[i+1] = 0xc1; }
    command(&s, 0, 0x16);
    for (unsigned i = 0; i < 128; i++) {
        x55_stream_write(&s, i, i, packet[i]);
    }
    x55_stream_advance(&s, 158);
    g_assert_cmpuint(s.accepted, ==, 0);
    x55_stream_advance(&s, 159);
    g_assert_cmpuint(s.accepted, ==, 1);
    g_assert_cmpuint(s.lengths[0], ==, 128);
    g_assert_cmpuint(s.si3.quanta, ==, 64);
    g_assert_cmpuint(s.si3.malformed, ==, 0);
    g_assert_cmpmem(s.owned[0], 128, packet, 128);
    g_assert_cmphex(lduw_le_p(s.registers), ==, 0x5555);
    x55_speaker_clear(&speaker, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/x55/stream/ordering", test_ordering);
    g_test_add_func("/x55/stream/configured", test_configured);
    g_test_add_func("/x55/stream/codec", test_packet_codec);
    g_test_add_func("/x55/stream/full-packet", test_full_packet);
    return g_test_run();
}
