/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_AUDIO_X55_STREAM_H
#define HW_AUDIO_X55_STREAM_H

#include "hw/audio/x55_codec.h"
#include "hw/audio/x55_speaker.h"

#define X55_STREAM_PACKET 128
#define X55_STREAM_QUEUE 8

typedef enum X55StreamKind {
    X55_STREAM_NONE, X55_STREAM_SAMPLED, X55_STREAM_SI3,
} X55StreamKind;

typedef struct X55Stream {
    X55Speaker *speaker;
    uint8_t registers[X55_STREAM_PACKET];
    uint16_t command;
    X55StreamKind kind;
    uint64_t settle_at, release_at;
    unsigned length, queued, head;
    uint8_t owned[X55_STREAM_QUEUE][X55_STREAM_PACKET];
    uint8_t lengths[X55_STREAM_QUEUE];
    bool retained;
    uint8_t pending[X55_STREAM_PACKET];
    unsigned pending_size;
    bool replay;
    uint8_t replay_packet[X55_STREAM_PACKET];
    unsigned replay_size;
    uint64_t packets, accepted, retried, replaced, released;
    X55SI3 si3;
    X55IMA ima;
    uint32_t rate;
    uint8_t arguments[4];
    bool command_pending;
    X55Wave wave; /* Legacy v1 migration metadata only. */
    /* Legacy v1 migration slots only. No physical memory is attached. */
    int source_ram;
    size_t source_offset;
    unsigned received, emitted, partial_size;
    uint8_t partial[X55_IMA_BLOCK_MAX];
    bool sampled_active, sampled_failed;
    void (*event)(void *opaque, uint64_t tick, const char *phase,
                   const uint8_t *bytes, unsigned size, const char *outcome);
    void *opaque;
} X55Stream;

uint64_t x55_stream_deadline(const X55Stream *s);
void x55_stream_advance(X55Stream *s, uint64_t tick);
void x55_stream_write(X55Stream *s, uint64_t tick, unsigned offset,
                       uint8_t value);
void x55_stream_submit(X55Stream *s, uint64_t tick);
void x55_stream_command(X55Stream *s, uint64_t tick, uint16_t command);
void x55_stream_clear(X55Stream *s);

#endif
