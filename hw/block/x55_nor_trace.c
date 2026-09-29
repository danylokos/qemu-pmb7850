/*
 * Physical NOR observation; logical persistent-record formats belong to hosts.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "hw/block/x55_nor_trace.h"
#include "trace.h"

void x55_nor_trace_configure(Object *obj, unsigned chip_index,
                            X55NORTraceContextFn context, void *opaque)
{
    X55NORTraceClass *nc = X55_NOR_TRACE_GET_CLASS(obj);
    X55NORTraceState *state = nc->state(obj);

    state->chip_index = chip_index;
    state->context = context;
    state->opaque = opaque;
}

void x55_nor_trace_read(MemoryRegion *view, uint32_t offset,
                       uint32_t guest_addr, uint32_t value, unsigned size,
                       X55NORTraceContext context, const char *source)
{
    Object *obj = memory_region_owner(view);
    X55NORTraceClass *nc;
    X55NORTraceState *state;
    unsigned consumed = 0;

    if (!trace_event_get_state_backends(TRACE_X55_NOR_READ) ||
        !object_dynamic_cast(obj, TYPE_X55_NOR_TRACE)) {
        return;
    }
    nc = X55_NOR_TRACE_GET_CLASS(obj);
    state = nc->state(obj);
    assert(size && size <= 4);
    while (consumed < size) {
        bool array = nc->array_byte(obj, view, offset + consumed);
        unsigned count = 1;
        uint32_t data;

        while (consumed + count < size &&
               nc->array_byte(obj, view, offset + consumed + count) == array) {
            count++;
        }
        data = extract32(value, 8 * consumed, 8 * count);
        trace_x55_nor_read(context.tick, context.icount, context.pc,
                           state->chip_index, offset + consumed,
                           guest_addr + consumed, data, count, array, source);
        consumed += count;
    }
}

static X55NORTraceContext nor_context(X55NORTraceState *state)
{
    return state->context ? state->context(state->opaque) :
                            (X55NORTraceContext) { 0 };
}

void x55_nor_trace_program(X55NORTraceState *state, uint32_t offset,
                          const uint8_t *bytes, unsigned size)
{
    X55NORTraceContext context;

    if (!trace_event_get_state_backends(TRACE_X55_NOR_MUTATION)) {
        return;
    }
    assert(size && size <= 8);
    context = nor_context(state);
    trace_x55_nor_mutation(context.tick, context.icount, context.pc,
                           state->chip_index, offset, size, "program",
                           ldn_le_p(bytes, size));
}

void x55_nor_trace_erase(X55NORTraceState *state, uint32_t offset,
                        uint32_t size)
{
    X55NORTraceContext context;

    if (!trace_event_get_state_backends(TRACE_X55_NOR_MUTATION)) {
        return;
    }
    context = nor_context(state);
    trace_x55_nor_mutation(context.tick, context.icount, context.pc,
                           state->chip_index, offset, size, "erase", 0);
}

void x55_nor_trace_mapping(Object *obj, uint32_t offset, uint32_t size,
                           bool enabled)
{
    X55NORTraceState *state;
    X55NORTraceContext context;

    if (!trace_event_get_state_backends(TRACE_X55_NOR_MAPPING)) {
        return;
    }
    state = X55_NOR_TRACE_GET_CLASS(obj)->state(obj);
    context = nor_context(state);
    trace_x55_nor_mapping(context.tick, context.icount, context.pc,
                          state->chip_index, offset, size, enabled);
}

void x55_nor_trace_flush(Object *obj, uint32_t offset, uint32_t size)
{
    X55NORTraceState *state;
    X55NORTraceContext context;

    if (!trace_event_get_state_backends(TRACE_X55_NOR_FLUSH)) {
        return;
    }
    state = X55_NOR_TRACE_GET_CLASS(obj)->state(obj);
    context = nor_context(state);
    trace_x55_nor_flush(context.tick, context.icount, context.pc,
                        state->chip_index, offset, size);
}

static const TypeInfo nor_trace_type = {
    .name = TYPE_X55_NOR_TRACE,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(X55NORTraceClass),
};

static void nor_trace_register_types(void)
{
    type_register_static(&nor_trace_type);
}

type_init(nor_trace_register_types)
