/*
 * Physical NOR observation; logical persistent-record formats belong to hosts.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_BLOCK_X55_NOR_TRACE_H
#define HW_BLOCK_X55_NOR_TRACE_H

#include "qom/object.h"
#include "system/memory.h"

#define TYPE_X55_NOR_TRACE "x55-nor-trace"
typedef struct X55NORTraceClass X55NORTraceClass;
DECLARE_CLASS_CHECKERS(X55NORTraceClass, X55_NOR_TRACE, TYPE_X55_NOR_TRACE)

typedef struct X55NORTraceContext {
    uint64_t tick;
    uint64_t icount;
    uint32_t pc;
} X55NORTraceContext;

typedef X55NORTraceContext (*X55NORTraceContextFn)(void *opaque);

typedef struct X55NORTraceState {
    unsigned chip_index;
    X55NORTraceContextFn context;
    void *opaque;
} X55NORTraceState;

struct X55NORTraceClass {
    InterfaceClass parent_class;
    X55NORTraceState *(*state)(Object *obj);
    /* Inspect classification without reading memory or advancing the device. */
    bool (*array_byte)(Object *obj, MemoryRegion *view, uint32_t offset);
};

void x55_nor_trace_configure(Object *obj, unsigned chip_index,
                            X55NORTraceContextFn context, void *opaque);
void x55_nor_trace_read(MemoryRegion *view, uint32_t offset,
                       uint32_t guest_addr, uint32_t value, unsigned size,
                       X55NORTraceContext context, const char *source);
void x55_nor_trace_program(X55NORTraceState *state, uint32_t offset,
                          const uint8_t *bytes, unsigned size);
void x55_nor_trace_erase(X55NORTraceState *state, uint32_t offset,
                        uint32_t size);

void x55_nor_trace_mapping(Object *obj, uint32_t offset, uint32_t size,
                           bool enabled);
void x55_nor_trace_flush(Object *obj, uint32_t offset, uint32_t size);

#endif
