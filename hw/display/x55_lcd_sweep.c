/*
 * Address-driven host presentation policy, independent of LCD bus framing.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "exec/icount.h"
#include "hw/core/cpu.h"
#include "x55_lcd_sweep.h"
#include "trace/trace-hw_display.h"

static uint64_t sweep_icount(void)
{
    return icount_enabled() ? icount_get_raw() : 0;
}

static uint32_t sweep_pc(void)
{
    CPUClass *cc = current_cpu ? CPU_GET_CLASS(current_cpu) : NULL;

    return cc && cc->get_pc ? cc->get_pc(current_cpu) : 0;
}

void x55_lcd_sweep_init(X55LCDSweep *s, const char *model)
{
    memset(s, 0, sizeof(*s));
    s->model = model;
}

void x55_lcd_sweep_discard(X55LCDSweep *s)
{
    s->active = false;
    s->bytes = s->writes = 0;
    s->packing = 0;
    s->transaction_bytes = 0;
}

void x55_lcd_sweep_flush(X55LCDSweep *s, X55LCDPresent present,
                         void *opaque, const char *reason)
{
    if (s->active && s->writes) {
        uint64_t digest;
        unsigned x0 = s->shape.x0, y0 = s->shape.y0;
        unsigned x1 = s->shape.x1;

        /* Shared trace coordinates describe bounds, not observed endpoints. */
        if (s->shape.software_rows) {
            x0 = s->origin_x;
            y0 = s->origin_y;
            x1 = s->row_end_known ? s->row_end : s->last_x;
        }

        s->frames++;
        digest = present(opaque, s);
        trace_x55_lcd_sweep_frame(sweep_icount(), sweep_pc(), s->model,
                                 s->frames, s->bytes, s->writes,
                                 (uint64_t)x0 << 48 | (uint64_t)y0 << 32 |
                                 (uint64_t)x1 << 16 | s->shape.y1,
                                 reason, digest);
    }
    s->active = false;
    s->bytes = s->writes = 0;
    s->packing = 0;
}

static bool same_shape(const X55LCDSweepShape *a, const X55LCDSweepShape *b)
{
    return a->mode == b->mode && a->x0 == b->x0 && a->y0 == b->y0 &&
           a->x1 == b->x1 && a->y1 == b->y1 &&
           a->windowed == b->windowed && a->software_rows == b->software_rows;
}

void x55_lcd_sweep_begin(X55LCDSweep *s, const X55LCDSweepShape *shape,
                         unsigned x, unsigned y, unsigned packing,
                         X55LCDPresent present, void *opaque)
{
    const char *boundary = NULL;

    if (!s->transaction_bytes) {
        s->transaction_x = x;
        s->transaction_y = y;
        s->transaction_first_frame = s->frames;
    }
    if (s->active) {
        if (!same_shape(&s->shape, shape) || packing != s->packing) {
            boundary = "fallback";
        } else if (!packing && s->writes &&
                   x == s->origin_x && y == s->origin_y) {
            boundary = "origin_restart";
        } else if (x != s->expected_x || y != s->expected_y) {
            /*
             * A firmware-addressed next row establishes its span from the
             * preceding cursor, not from byte counts or chip selection.
             */
            if (shape->software_rows && !packing && x == s->origin_x &&
                y == s->last_y + 1 && s->last_x >= s->origin_x &&
                (!s->row_end_known || s->row_end == s->last_x)) {
                s->row_end = s->last_x;
                s->row_end_known = true;
            } else {
                boundary = "fallback";
            }
        }
    }
    if (boundary) {
        x55_lcd_sweep_flush(s, present, opaque, boundary);
    }
    if (!s->active) {
        s->shape = *shape;
        s->active = true;
        s->row_end_known = false;
        s->origin_x = x;
        s->origin_y = y;
        s->bytes = s->writes = 0;
    }
    s->last_x = x;
    s->last_y = y;
}

void x55_lcd_sweep_end(X55LCDSweep *s, unsigned next_x, unsigned next_y,
                       unsigned packing, unsigned bytes, unsigned writes,
                       bool terminal, X55LCDPresent present, void *opaque)
{
    s->bytes += bytes;
    s->writes += writes;
    s->transaction_bytes += bytes;
    s->expected_x = next_x;
    s->expected_y = next_y;
    s->packing = packing;
    if (s->shape.software_rows && s->row_end_known &&
        s->last_x == s->row_end && s->last_y == s->shape.y1) {
        terminal = true;
    }
    if (terminal && !packing) {
        x55_lcd_sweep_flush(s, present, opaque,
                            s->shape.windowed ? "window_wrap" :
                                                "viewport_complete");
    }
}

void x55_lcd_sweep_close(X55LCDSweep *s)
{
    if (!s->transaction_bytes) {
        return;
    }
    s->transactions++;
    trace_x55_lcd_sweep_transaction(sweep_icount(), sweep_pc(),
                                   s->model, s->transactions,
                                   s->transaction_bytes,
                                   s->transaction_x, s->transaction_y,
                                   s->frames != s->transaction_first_frame ?
                                   "committed" : "held");
    s->transaction_bytes = 0;
}

const VMStateDescription vmstate_x55_lcd_sweep = {
    .name = "x55/lcd-sweep",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(shape.mode, X55LCDSweep),
        VMSTATE_UINT64(bytes, X55LCDSweep),
        VMSTATE_UINT64(writes, X55LCDSweep),
        VMSTATE_UINT64(frames, X55LCDSweep),
        VMSTATE_UINT64(transactions, X55LCDSweep),
        VMSTATE_UINT64(transaction_bytes, X55LCDSweep),
        VMSTATE_UINT64(transaction_first_frame, X55LCDSweep),
        VMSTATE_UINT32(shape.x0, X55LCDSweep),
        VMSTATE_UINT32(shape.y0, X55LCDSweep),
        VMSTATE_UINT32(shape.x1, X55LCDSweep),
        VMSTATE_UINT32(shape.y1, X55LCDSweep),
        VMSTATE_UINT32(row_end, X55LCDSweep),
        VMSTATE_UINT32(origin_x, X55LCDSweep),
        VMSTATE_UINT32(origin_y, X55LCDSweep),
        VMSTATE_UINT32(expected_x, X55LCDSweep),
        VMSTATE_UINT32(expected_y, X55LCDSweep),
        VMSTATE_UINT32(last_x, X55LCDSweep),
        VMSTATE_UINT32(last_y, X55LCDSweep),
        VMSTATE_UINT32(packing, X55LCDSweep),
        VMSTATE_UINT32(transaction_x, X55LCDSweep),
        VMSTATE_UINT32(transaction_y, X55LCDSweep),
        VMSTATE_BOOL(shape.windowed, X55LCDSweep),
        VMSTATE_BOOL(shape.software_rows, X55LCDSweep),
        VMSTATE_BOOL(active, X55LCDSweep),
        VMSTATE_BOOL(row_end_known, X55LCDSweep),
        VMSTATE_END_OF_LIST()
    },
};
