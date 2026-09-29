/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_AUDIO_X55_AUDIO_H
#define HW_AUDIO_X55_AUDIO_H

#include "hw/core/qdev.h"
#include "system/memory.h"

#define TYPE_X55_AUDIO "x55-digital-audio"
OBJECT_DECLARE_SIMPLE_TYPE(X55AudioState, X55_AUDIO)

typedef struct X55AudioContext {
    uint64_t tick, icount;
    uint32_t pc;
} X55AudioContext;

/* Board clock adaptation; sync drains earlier SoC output events first. */
typedef X55AudioContext (*X55AudioContextFn)(void *opaque, bool sync);
void x55_audio_configure(DeviceState *dev, X55AudioContextFn context,
                          void *opaque, bool instruction_timing);
uint8_t x55_audio_read(DeviceState *dev, hwaddr addr);
void x55_audio_write(DeviceState *dev, hwaddr addr, uint64_t value,
                      unsigned size);
void x55_audio_ringer(DeviceState *dev, uint64_t tick, bool high);

#endif
