/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/audio/x55_codec.h"

static void make_wave(uint8_t *bytes, unsigned data_size, unsigned frames)
{
    memset(bytes, 0, 60 + data_size);
    memcpy(bytes, "RIFF", 4);
    stl_le_p(bytes + 4, 52 + data_size);
    memcpy(bytes + 8, "WAVEfmt ", 8);
    stl_le_p(bytes + 16, 20);
    stw_le_p(bytes + 20, 0x11);
    stw_le_p(bytes + 22, 1);
    stl_le_p(bytes + 24, 8000);
    stl_le_p(bytes + 28, 7111);
    stw_le_p(bytes + 32, 8);
    stw_le_p(bytes + 34, 4);
    stw_le_p(bytes + 36, 2);
    stw_le_p(bytes + 38, 9);
    memcpy(bytes + 40, "fact", 4);
    stl_le_p(bytes + 44, 4);
    stl_le_p(bytes + 48, frames);
    memcpy(bytes + 52, "data", 4);
    stl_le_p(bytes + 56, data_size);
    memset(bytes + 64, 0x11, MIN(data_size - 4, 4));
}

static void test_wave(void)
{
    uint8_t bytes[96];
    X55Wave wave;
    int16_t samples[32];

    make_wave(bytes, 8, 9);
    g_assert_true(x55_wave_parse(bytes, 68, &wave));
    g_assert_cmpuint(wave.data_offset, ==, 60);
    g_assert_cmpint(x55_ima_decode(&wave, bytes + 60, 8, samples, 32), ==, 9);
    for (int i = 0; i < 9; i++) {
        g_assert_cmpint(samples[i], ==, i);
    }
    g_assert_cmpint(x55_ima_decode(&wave, bytes + 60, 8, samples, 8), ==, -1);
    for (size_t size = 0; size < 68; size++) {
        g_assert_false(x55_wave_parse(bytes, size, &wave));
    }
    make_wave(bytes, 12, 10);
    g_assert_true(x55_wave_parse(bytes, 72, &wave));
    g_assert_cmpint(x55_ima_decode(&wave, bytes + 68, 4, samples, 32), ==, 1);
    bytes[71] = 1;
    g_assert_cmpint(x55_ima_decode(&wave, bytes + 68, 4, samples, 32), ==, -1);
    bytes[71] = 0;
    bytes[70] = 89;
    g_assert_cmpint(x55_ima_decode(&wave, bytes + 68, 4, samples, 32), ==, -1);
    static const struct { unsigned offset, value; } invalid[] = {
        { 20, 1 }, { 22, 0 }, { 22, 3 }, { 24, 0 }, { 32, 0 },
        { 32, 7 }, { 34, 3 }, { 36, 1 }, { 36, 3 }, { 38, 10 },
        { 48, 0 }, { 48, 10 }, { 56, 7 },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        make_wave(bytes, 8, 9);
        stw_le_p(bytes + invalid[i].offset, invalid[i].value);
        g_assert_false(x55_wave_parse(bytes, 68, &wave));
    }
    make_wave(bytes, 8, 9);
    memcpy(bytes + 68, bytes + 40, 12); /* duplicate fact */
    stl_le_p(bytes + 4, 72);
    g_assert_false(x55_wave_parse(bytes, 80, &wave));
    make_wave(bytes, 8, 9);
    memcpy(bytes + 68, "JUNK", 4);
    stl_le_p(bytes + 72, 1);
    bytes[76] = 0x99;
    bytes[77] = 0;
    stl_le_p(bytes + 4, 70);
    g_assert_true(x55_wave_parse(bytes, sizeof(bytes), &wave));
    g_assert_cmpuint(wave.file_size, ==, 78);
}

static void test_stereo(void)
{
    X55Wave wave = { .channels = 2, .block_size = 16, .block_frames = 9 };
    uint8_t block[] = { 100, 0, 0, 0, 156, 255, 0, 0,
                        0x11, 0x11, 0x11, 0x11, 0x99, 0x99, 0x99, 0x99 };
    int16_t samples[18];

    g_assert_cmpint(x55_ima_decode(&wave, block, 16, samples, 18), ==, 18);
    for (int i = 0; i < 9; i++) {
        g_assert_cmpint(samples[2 * i], ==, 100 + i);
        g_assert_cmpint(samples[2 * i + 1], ==, -100 - i);
    }
    g_assert_cmpint(x55_ima_decode(&wave, block, 8, samples, 18), ==, 2);
    g_assert_cmpint(x55_ima_decode(&wave, block, 12, samples, 18), ==, -1);
}

static void hash_samples(GChecksum *hash, const int16_t *samples, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        uint8_t encoded[2];
        stw_le_p(encoded, samples[i]);
        g_checksum_update(hash, encoded, 2);
    }
}

/* Frozen CEMU reference: all 89 indices x 16 codes, both saturation rails. */
static void test_ima_reference(void)
{
    X55Wave wave = { .channels = 1, .block_size = 8, .block_frames = 9 };
    g_autoptr(GChecksum) hash = g_checksum_new(G_CHECKSUM_SHA256);

    for (unsigned index = 0; index < 89; index++) {
        for (unsigned code = 0; code < 16; code++) {
            uint8_t block[8];
            int16_t samples[9];

            stw_le_p(block, code & 1 ? -32760 : 32760);
            block[2] = index;
            block[3] = 0;
            memset(block + 4, code * 17, 4);
            g_assert_cmpint(x55_ima_decode(&wave, block, 8, samples, 9), ==, 9);
            hash_samples(hash, samples, 9);
        }
    }
    g_assert_cmpstr(g_checksum_get_string(hash), ==,
        "5bda18d19a61d5b5b1525eceff94473e9c1ce6a61e716cc823688a62c6971187");
}

static const uint8_t note[] = {
    0x01, 0x91, 0x10, 0x18, 0x14, 0x73, 0x14, 0x14,
    0x0a, 0x4d, 0x72, 0x52, 0xf8, 0xff, 0xa0, 0xa0,
};
static const uint8_t wait_op[] = { 0xcc, 0xc1 };
static const uint8_t stop_op[] = { 1, 0x81 };

static int packet(X55SI3 *state, GChecksum *hash,
                   const uint8_t *bytes, size_t size)
{
    int16_t pcm[X55_SI3_PACKET_MAX];
    uint8_t count[4];
    int n = x55_si3_packet_profile(state, 26000000, bytes, size,
                           pcm, ARRAY_SIZE(pcm), false);

    if (n >= 0) {
        stl_le_p(count, n);
        g_checksum_update(hash, count, 4);
        hash_samples(hash, pcm, n);
    }
    return n;
}

static void test_si3_reference(void)
{
    static const char *const hashes[] = {
        "da6902ca1abf6219c80cb4f287e91c819a7d14d7cb5c158e7c2c1679f5b5b317",
        "034fb02af8553175105513e5a0aa01ac42e0a0d2dce392860ae40f27ca171356",
        "da6902ca1abf6219c80cb4f287e91c819a7d14d7cb5c158e7c2c1679f5b5b317",
        "6605e7840fe3085029bb3ea7556162f2f38858d71ded522aad5ad7d2b4417326",
    };

    /* Four historical vectors; malformed atomicity is checked separately. */
    for (unsigned variant = 0; variant < ARRAY_SIZE(hashes); variant++) {
        X55SI3 state = { 0 };
        g_autoptr(GChecksum) hash = g_checksum_new(G_CHECKSUM_SHA256);
        uint8_t start[16];
        unsigned waits = 4;

        memcpy(start, note, 16);
        if (variant == 2) {
            start[1] = 0x11;
        }
        if (variant == 3) {
            for (unsigned slot = 0; slot < 16; slot++) {
                start[0] = 1 + slot * 16;
                start[3] = 255 - slot;
                g_assert_cmpint(packet(&state, hash, start, 16), ==, 0);
            }
            waits = 20;
        } else {
            g_assert_cmpint(packet(&state, hash, start, 16), ==, 0);
        }
        if (variant == 1) {
            g_assert_cmpint(packet(&state, hash, wait_op, 2), ==, 73);
            g_assert_cmpint(packet(&state, hash, stop_op, 2), ==, 0);
            waits = 16;
        }
        for (unsigned i = 0; i < waits; i++) {
            g_assert_cmpint(packet(&state, hash, wait_op, 2), >=, 73);
        }
        g_assert_cmpstr(g_checksum_get_string(hash), ==, hashes[variant]);
    }
}

static void test_si3_recovered(void)
{
    X55SI3 state = {0}, before;
    uint8_t start[26] = {1, 0x91, 0, 0x9f, 0xfc, 0xff};
    int16_t pcm[X55_SI3_PACKET_MAX];
    g_assert_cmpint(x55_si3_packet(&state, 26000000, start, 26, pcm, ARRAY_SIZE(pcm)), ==, 0);
    g_assert_cmpuint(state.voices[0].gain, ==, 31 * 57);
    g_assert_cmpuint(state.voices[0].parameter_length, ==, 24);
    uint64_t expected = (((uint64_t)1023 * 32768 * 190 % 64000000) << 32) / 64000000;
    g_assert_cmpuint(state.voices[0].increment, ==, expected);
    uint8_t control[] = {1, 0xa2, 0xcc, 0xc1, 0xff, 0xff};
    g_assert_cmpint(x55_si3_packet(&state, 26000000, control, 6, pcm, ARRAY_SIZE(pcm)), ==, 0);
    g_assert_cmpuint(state.controls, ==, 1);
    g_assert_cmpmem(state.last_control, 6, control, 6);
    before = state;
    uint8_t bad[] = {0xcc, 0xc1, 1, 0x91};
    g_assert_cmpint(x55_si3_packet(&state, 26000000, bad, 4, pcm, ARRAY_SIZE(pcm)), ==, -1);
    before.malformed++;
    g_assert_cmpmem(&state, sizeof(state), &before, sizeof(before));
    for (unsigned kind = 0; kind < 8; kind++) {
        start[3] = (kind << 5) | 7;
        int n = x55_si3_packet(&state, 26000000, start, kind < 4 ? 16 : 26, pcm, ARRAY_SIZE(pcm));
        g_assert_cmpint(n, ==, kind == 2 || kind == 3 ? -1 : 0);
    }
    uint8_t waits[128];
    for (unsigned i = 0; i < 128; i += 2) { waits[i] = 0xcc; waits[i+1] = 0xc1; }
    state.remainder = 25999999;
    before = state;
    g_assert_cmpint(x55_si3_packet(&state, 26000000, waits, 128, pcm, 4726), ==, -1);
    before.malformed++;
    g_assert_cmpmem(&state, sizeof(state), &before, sizeof(before));
    g_assert_cmpint(x55_si3_packet(&state, 26000000, waits, 128, pcm, 4727), ==, 4727);
    for (unsigned octave = 0; octave < 16; octave++) {
        stw_le_p(start + 4, (460 << 6) | (octave << 2)); start[3] = 7;
        g_assert_cmpint(x55_si3_packet(&state, 26000000, start, 16, pcm, ARRAY_SIZE(pcm)), ==, 0);
        expected = (((uint64_t)460 * (1u << octave) * 190 % 64000000) << 32) / 64000000;
        g_assert_cmpuint(state.voices[0].increment, ==, expected);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/x55/wave/bounds", test_wave);
    g_test_add_func("/x55/ima/stereo", test_stereo);
    g_test_add_func("/x55/ima/reference", test_ima_reference);
    g_test_add_func("/x55/si3/reference", test_si3_reference);
    g_test_add_func("/x55/si3/recovered", test_si3_recovered);
    return g_test_run();
}
