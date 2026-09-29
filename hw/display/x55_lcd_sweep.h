/*
 * Address-driven host presentation policy, independent of LCD bus framing.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_X55_LCD_SWEEP_H
#define HW_DISPLAY_X55_LCD_SWEEP_H

typedef struct X55LCDSweepShape {
    uint64_t mode;
    unsigned x0, y0, x1, y1;
    bool windowed;
    bool software_rows;
} X55LCDSweepShape;

typedef struct X55LCDSweep {
    const char *model;
    X55LCDSweepShape shape;
    bool active;
    bool row_end_known;
    unsigned row_end;
    unsigned origin_x, origin_y;
    unsigned expected_x, expected_y;
    unsigned last_x, last_y;
    unsigned packing;
    uint64_t bytes, writes;
    uint64_t frames;
    uint64_t transactions;
    uint64_t transaction_bytes;
    uint64_t transaction_first_frame;
    unsigned transaction_x, transaction_y;
} X55LCDSweep;

/*
 * Called before a conflicting write/control, or after a terminal write.
 * Copy live storage to presentation here; the tracker is still inspectable.
 */
typedef uint64_t (*X55LCDPresent)(void *opaque, const X55LCDSweep *sweep);

void x55_lcd_sweep_init(X55LCDSweep *sweep, const char *model);
void x55_lcd_sweep_discard(X55LCDSweep *sweep);
void x55_lcd_sweep_flush(X55LCDSweep *sweep, X55LCDPresent present,
                         void *opaque, const char *reason);
void x55_lcd_sweep_begin(X55LCDSweep *sweep, const X55LCDSweepShape *shape,
                         unsigned x, unsigned y, unsigned packing,
                         X55LCDPresent present, void *opaque);
void x55_lcd_sweep_end(X55LCDSweep *sweep, unsigned next_x, unsigned next_y,
                       unsigned packing, unsigned bytes, unsigned writes,
                       bool terminal, X55LCDPresent present, void *opaque);
/* Close only the raw CS transaction; never publish or discard a held sweep. */
void x55_lcd_sweep_close(X55LCDSweep *sweep);

extern const VMStateDescription vmstate_x55_lcd_sweep;

#endif
