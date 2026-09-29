/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_AUDIO_X55_SPEAKER_H
#define HW_AUDIO_X55_SPEAKER_H

#define X55_SPEAKER_RATE 48000
#define X55_SPEAKER_BLOCK 256
#define X55_SPEAKER_LIMIT (8 * X55_SPEAKER_RATE)
#define X55_SPEAKER_SOURCES 2

typedef struct X55PCMQueue {
    int16_t *data;
    size_t head, count;
    uint32_t rate, phase;
    int16_t last;
    bool held;
} X55PCMQueue;

typedef struct X55Speaker {
    X55PCMQueue input[X55_SPEAKER_SOURCES];
    uint32_t clock_hz;
    uint32_t fraction;
    uint64_t tick;
    uint64_t ringer_end;
    bool ringer;
    bool ringer_high;
    int16_t block[X55_SPEAKER_BLOCK];
    unsigned used;
    uint64_t frames, blocks, clipped, overflows;
    void (*output)(void *opaque, uint64_t tick, const int16_t *pcm, size_t n);
    void (*reset)(void *opaque, uint64_t tick, bool overflow);
    void *opaque;
} X55Speaker;

void x55_speaker_advance(X55Speaker *s, uint64_t tick);
/* 0 accepted, 1 overflow/reset, -1 invalid format; sample count interleaved. */
int x55_speaker_push(X55Speaker *s, unsigned source, uint64_t tick,
                     uint32_t rate, unsigned channels,
                     const int16_t *pcm, size_t samples);
void x55_speaker_drain(X55Speaker *s, unsigned source, uint64_t tick);
void x55_speaker_cancel(X55Speaker *s, unsigned source, uint64_t tick,
                        bool overflow);
void x55_speaker_edge(X55Speaker *s, uint64_t tick, bool high);
/* UINT64_MAX when no samples are ready; open starvation keeps partial state. */
uint64_t x55_speaker_deadline(const X55Speaker *s);
/* Preserve callbacks/clock, discard all queued output and reset time. */
void x55_speaker_clear(X55Speaker *s, uint64_t tick);

#endif
