/*
 * Controller-neutral support for Siemens x55 color LCDs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_X55_LCD_COMMON_H
#define HW_DISPLAY_X55_LCD_COMMON_H

#include "hw/ssi/ssi.h"
#include "ui/console.h"
#include "x55_lcd_sweep.h"

typedef struct X55LCDCommonOps {
    void (*reset)(void *opaque);
    void (*present)(void *opaque);
    void (*deselect)(void *opaque);
    void (*pixel_rgb888)(void *opaque, unsigned x, unsigned y,
                         uint8_t rgb[3]);
} X55LCDCommonOps;

typedef struct X55LCDCommon {
    QemuConsole *console;
    void *opaque;
    const X55LCDCommonOps *ops;
    const char *model_name;
    unsigned controller_width;
    unsigned controller_height;
    X55LCDSweep sweep;

    bool dc;
    uint8_t frame_bits;
    bool frame_msb_first;
    bool frame_pending;
    bool frame_is_data;
    uint64_t frame_count;
    uint64_t frame_digest;
    bool reset_level;
    bool reset_driven;
    bool reset_idle_level;
    bool reset_asserted;
    bool redraw;
    bool force_update;
    uint64_t display_updates;
    uint32_t panel_width;
    uint32_t panel_height;
    uint32_t panel_origin_x;
    uint32_t panel_origin_y;
    bool panel_mirror_x;
    bool panel_mirror_y;
    uint32_t scale;
} X55LCDCommon;

typedef struct X55LCDSSIFrame {
    unsigned bits;
    bool data;
    bool accepted;
    uint32_t value;
    uint32_t mask;
} X55LCDSSIFrame;

#define X55_LCD_PROP_PANEL_WIDTH(_state, _field) \
    DEFINE_PROP_UINT32("panel-width", _state, _field.panel_width, 0)
#define X55_LCD_PROP_PANEL_HEIGHT(_state, _field) \
    DEFINE_PROP_UINT32("panel-height", _state, _field.panel_height, 0)
#define X55_LCD_PROP_PANEL_ORIGIN_X(_state, _field) \
    DEFINE_PROP_UINT32("panel-origin-x", _state, _field.panel_origin_x, 0)
#define X55_LCD_PROP_PANEL_ORIGIN_Y(_state, _field) \
    DEFINE_PROP_UINT32("panel-origin-y", _state, _field.panel_origin_y, 0)
#define X55_LCD_PROP_PANEL_MIRROR_X(_state, _field) \
    DEFINE_PROP_BOOL("panel-mirror-x", _state, _field.panel_mirror_x, false)
#define X55_LCD_PROP_PANEL_MIRROR_Y(_state, _field) \
    DEFINE_PROP_BOOL("panel-mirror-y", _state, _field.panel_mirror_y, false)
#define X55_LCD_PROP_RESET_IDLE_LEVEL(_state, _field) \
    DEFINE_PROP_BOOL("reset-idle-level", _state, \
                     _field.reset_idle_level, true)
#define X55_LCD_PROP_SCALE(_state, _field) \
    DEFINE_PROP_UINT32("scale", _state, _field.scale, 1)

void x55_lcd_common_init(X55LCDCommon *common, Object *obj, void *opaque,
                         const X55LCDCommonOps *ops, const char *model_name,
                         bool (*get_reset)(Object *, Error **));
void x55_lcd_common_realize(X55LCDCommon *common, DeviceState *device,
                            void *opaque, const X55LCDCommonOps *ops,
                            const char *model_name,
                            unsigned controller_width,
                            unsigned controller_height,
                            unsigned default_panel_width,
                            unsigned default_panel_height,
                            unsigned default_panel_origin_x,
                            unsigned default_panel_origin_y,
                            Error **errp);
void x55_lcd_common_system_reset(X55LCDCommon *common);
void x55_lcd_common_reset(X55LCDCommon *common);
X55LCDSSIFrame x55_lcd_common_frame(X55LCDCommon *common, uint32_t value);
int x55_lcd_common_set_cs(X55LCDCommon *common, bool level);
uint64_t x55_lcd_common_present(void *opaque, const X55LCDSweep *sweep);
void x55_lcd_common_refresh(X55LCDCommon *common);

extern const VMStateDescription vmstate_x55_lcd_common;

#endif
