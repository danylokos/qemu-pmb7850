/*
 * X55 digital audio formats. No firmware addresses or host playback state.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_AUDIO_X55_CODEC_H
#define HW_AUDIO_X55_CODEC_H

#define X55_IMA_BLOCK_MAX 4096
#define X55_SI3_RATE 16000
#define X55_SI3_PACKET_MAX 4727
#define X55_SI3_VOICES 16

typedef struct X55Wave {
    size_t file_size;
    size_t data_offset;
    uint32_t data_size;
    uint32_t frames;
    uint32_t rate;
    uint16_t channels;
    uint16_t block_size;
    uint16_t block_frames;
} X55Wave;

/* Parse complete RIFF bounds, permitting unrelated bytes after the RIFF. */
bool x55_wave_parse(const uint8_t *bytes, size_t size, X55Wave *wave);
/* Returns interleaved sample count, or -1; includes the block predictors. */
int x55_ima_decode(const X55Wave *wave, const uint8_t *block, size_t size,
                   int16_t *samples, size_t capacity);

/* Firmware-delivered mono IMA state, independent of RIFF/file storage. */
typedef struct X55IMA {
    int32_t predictor, index;
    bool have_header;
} X55IMA;
int x55_ima_packet(X55IMA *state, const uint8_t *packet, size_t size,
                   int16_t *samples, size_t capacity);

typedef struct X55SI3Voice {
    uint32_t phase;
    uint32_t increment;
    unsigned release;
    unsigned gain;
    bool active;
    uint8_t parameter_length;
    uint8_t parameters[24]; /* Uninterpreted fields remain observable. */
} X55SI3Voice;

typedef struct X55SI3 {
    X55SI3Voice voices[X55_SI3_VOICES];
    uint64_t remainder;
    uint64_t quanta;
    uint64_t starts[2]; /* 11xx, 91xx, without naming the high bit. */
    uint64_t stops;
    uint64_t malformed;
    uint64_t controls;
    uint8_t last_control[6];
} X55SI3;

/*
 * Clock-dependent quantum: 120,000 board ticks, calibrated triangle/gain.
 * Malformed packets change only the diagnostic counter and emit no PCM.
 */
int x55_si3_packet(X55SI3 *state, uint32_t clock_hz,
                   const uint8_t *packet, size_t size,
                   int16_t *samples, size_t capacity);

int x55_si3_packet_profile(X55SI3 *state, uint32_t clock_hz,
    const uint8_t *packet, size_t size, int16_t *samples, size_t capacity,
    bool recovered);

#endif
