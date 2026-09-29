/*
 * S6B33Bx-compatible RGB565 LCD controller.
 *
 * The compatible name describes qualified protocol behavior, not fitted-part
 * proof.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/s6b33bx.h"
#include "x55_lcd_common.h"
#include "trace/trace-hw_display.h"

#define S6B33BX_WIDTH 130
#define S6B33BX_HEIGHT 130

struct S6B33BxState {
    SSIPeripheral parent_obj;
    X55LCDCommon common;
    uint16_t gram[S6B33BX_WIDTH * S6B33BX_HEIGHT];
    uint16_t presented_gram[S6B33BX_WIDTH * S6B33BX_HEIGHT];

    uint8_t x;
    uint8_t y;
    uint8_t window_start_x;
    uint8_t window_start_y;
    uint8_t end_x;
    uint8_t end_y;
    uint32_t write_index;
    uint8_t pending_window_command;
    uint8_t pending_window_index;
    uint8_t pending_window_start;
    uint32_t command_count;
};

static void s6b33bx_reset_registers(void *opaque)
{
    S6B33BxState *s = opaque;

    s->x = 0;
    s->y = 0;
    s->window_start_x = 0;
    s->window_start_y = 0;
    s->end_x = S6B33BX_WIDTH - 1;
    s->end_y = S6B33BX_HEIGHT - 1;
    s->write_index = 0;
    s->pending_window_command = 0;
    s->pending_window_index = 0;
    s->pending_window_start = 0;
    s->command_count = 0;
}

static uint8_t s6b33bx_bound(uint8_t value)
{
    return MIN(value, S6B33BX_WIDTH - 1);
}

static void s6b33bx_window_parameter(S6B33BxState *s, uint8_t value)
{
    uint8_t coordinate = s6b33bx_bound(value);
    uint8_t *start = s->pending_window_command == 0x43 ?
                     &s->window_start_x : &s->window_start_y;
    uint8_t *end = s->pending_window_command == 0x43 ?
                   &s->end_x : &s->end_y;

    if (s->pending_window_index++ == 0) {
        s->pending_window_start = coordinate;
    } else {
        if (*start != s->pending_window_start ||
            *end != MAX(coordinate, s->pending_window_start)) {
            x55_lcd_sweep_flush(&s->common.sweep, x55_lcd_common_present,
                                &s->common, "fallback");
        }
        *start = s->pending_window_start;
        *end = MAX(coordinate, *start);
        s->pending_window_command = 0;
        s->pending_window_index = 0;
        s->pending_window_start = 0;
    }
    s->write_index = 0;
}

static void s6b33bx_command(S6B33BxState *s, uint8_t value)
{
    bool parameter = s->pending_window_command != 0;

    s->command_count++;
    if (parameter) {
        s6b33bx_window_parameter(s, value);
    } else if (value == 0x42 || value == 0x43) {
        s->pending_window_command = value;
        s->pending_window_index = 0;
    }
    trace_x55_color_lcd_command(TYPE_S6B33BX_COMPATIBLE, value, s->x, s->y);
}

static void s6b33bx_pixel(S6B33BxState *s, uint16_t value)
{
    unsigned width = s->end_x - s->window_start_x + 1;
    unsigned height = s->end_y - s->window_start_y + 1;
    unsigned count = width * height;
    unsigned offset = s->write_index < count ? s->write_index : 0;
    unsigned x = s->window_start_x + offset % width;
    unsigned y = s->window_start_y + offset / width;
    X55LCDSweepShape shape = {
        .x0 = s->window_start_x, .y0 = s->window_start_y,
        .x1 = s->end_x, .y1 = s->end_y, .windowed = true,
    };
    unsigned writes = x < S6B33BX_WIDTH && y < S6B33BX_HEIGHT;

    x55_lcd_sweep_begin(&s->common.sweep, &shape, x, y, 0,
                        x55_lcd_common_present, &s->common);
    if (x < S6B33BX_WIDTH && y < S6B33BX_HEIGHT) {
        s->gram[y * S6B33BX_WIDTH + x] = value;
        trace_x55_color_lcd_data(TYPE_S6B33BX_COMPATIBLE, value, x, y);
    }
    s->write_index = offset + 1 >= count ? 0 : offset + 1;
    s->x = s->window_start_x + s->write_index % width;
    s->y = s->window_start_y + s->write_index / width;
    x55_lcd_sweep_end(&s->common.sweep, s->x, s->y, 0, 2, writes,
                      s->write_index == 0,
                      x55_lcd_common_present, &s->common);
}

static uint32_t s6b33bx_transfer(SSIPeripheral *dev, uint32_t value)
{
    S6B33BxState *s = S6B33BX_COMPATIBLE(dev);
    X55LCDSSIFrame frame = x55_lcd_common_frame(&s->common, value);

    if (frame.accepted) {
        if (!frame.data && frame.bits == 8) {
            s6b33bx_command(s, frame.value);
        } else if (frame.data && frame.bits == 16) {
            s6b33bx_pixel(s, frame.value);
        }
    }
    return frame.mask;
}

static void s6b33bx_deselect(void *opaque)
{
    S6B33BxState *s = opaque;

    s->pending_window_command = 0;
    s->pending_window_index = 0;
    s->pending_window_start = 0;
}

static int s6b33bx_set_cs(SSIPeripheral *dev, bool level)
{
    S6B33BxState *s = S6B33BX_COMPATIBLE(dev);

    return x55_lcd_common_set_cs(&s->common, level);
}

static void s6b33bx_pixel_rgb888(void *opaque, unsigned x, unsigned y,
                                 uint8_t rgb[3])
{
    S6B33BxState *s = opaque;
    uint16_t pixel = s->presented_gram[y * S6B33BX_WIDTH + x];

    rgb[0] = (((pixel >> 11) & 31) << 3) | ((pixel >> 13) & 7);
    rgb[1] = (((pixel >> 5) & 63) << 2) | ((pixel >> 9) & 3);
    rgb[2] = ((pixel & 31) << 3) | ((pixel >> 2) & 7);
}

static void s6b33bx_present(void *opaque)
{
    S6B33BxState *s = opaque;

    memcpy(s->presented_gram, s->gram, sizeof(s->gram));
}

static const X55LCDCommonOps s6b33bx_common_ops = {
    .reset = s6b33bx_reset_registers,
    .present = s6b33bx_present,
    .deselect = s6b33bx_deselect,
    .pixel_rgb888 = s6b33bx_pixel_rgb888,
};

static void s6b33bx_realize(SSIPeripheral *dev, Error **errp)
{
    S6B33BxState *s = S6B33BX_COMPATIBLE(dev);

    x55_lcd_common_realize(&s->common, DEVICE(dev), s,
                           &s6b33bx_common_ops,
                           TYPE_S6B33BX_COMPATIBLE,
                           S6B33BX_WIDTH, S6B33BX_HEIGHT,
                           S6B33BX_WIDTH, S6B33BX_HEIGHT, 0, 0, errp);
}

static void s6b33bx_system_reset(DeviceState *dev)
{
    S6B33BxState *s = S6B33BX_COMPATIBLE(dev);

    x55_lcd_common_system_reset(&s->common);
}

static bool s6b33bx_get_reset(Object *obj, Error **errp)
{
    return S6B33BX_COMPATIBLE(obj)->common.reset_asserted;
}

static const Property s6b33bx_properties[] = {
    X55_LCD_PROP_PANEL_WIDTH(S6B33BxState, common),
    X55_LCD_PROP_PANEL_HEIGHT(S6B33BxState, common),
    X55_LCD_PROP_PANEL_ORIGIN_X(S6B33BxState, common),
    X55_LCD_PROP_PANEL_ORIGIN_Y(S6B33BxState, common),
    X55_LCD_PROP_PANEL_MIRROR_X(S6B33BxState, common),
    X55_LCD_PROP_PANEL_MIRROR_Y(S6B33BxState, common),
    X55_LCD_PROP_RESET_IDLE_LEVEL(S6B33BxState, common),
    X55_LCD_PROP_SCALE(S6B33BxState, common),
};

static void s6b33bx_init(Object *obj)
{
    S6B33BxState *s = S6B33BX_COMPATIBLE(obj);

    x55_lcd_common_init(&s->common, obj, s, &s6b33bx_common_ops,
                        TYPE_S6B33BX_COMPATIBLE, s6b33bx_get_reset);
}

static int s6b33bx_post_load(void *opaque, int version_id)
{
    S6B33BxState *s = opaque;

    if (s->common.frame_bits > 16) {
        return -EINVAL;
    }
    /* Redraw the restored presentation, without publishing a partial sweep. */
    s->common.redraw = true;
    s->common.force_update = true;
    return 0;
}

static const VMStateDescription vmstate_s6b33bx = {
    .name = "x55/s6b33bx",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s6b33bx_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, S6B33BxState),
        VMSTATE_STRUCT(common, S6B33BxState, 0, vmstate_x55_lcd_common,
                       X55LCDCommon),
        VMSTATE_UINT16_ARRAY(gram, S6B33BxState,
                             S6B33BX_WIDTH * S6B33BX_HEIGHT),
        VMSTATE_UINT16_ARRAY(presented_gram, S6B33BxState,
                             S6B33BX_WIDTH * S6B33BX_HEIGHT),
        VMSTATE_UINT8(x, S6B33BxState),
        VMSTATE_UINT8(y, S6B33BxState),
        VMSTATE_UINT8(window_start_x, S6B33BxState),
        VMSTATE_UINT8(window_start_y, S6B33BxState),
        VMSTATE_UINT8(end_x, S6B33BxState),
        VMSTATE_UINT8(end_y, S6B33BxState),
        VMSTATE_UINT8(pending_window_command, S6B33BxState),
        VMSTATE_UINT8(pending_window_index, S6B33BxState),
        VMSTATE_UINT8(pending_window_start, S6B33BxState),
        VMSTATE_UINT32(write_index, S6B33BxState),
        VMSTATE_UINT32(command_count, S6B33BxState),
        VMSTATE_END_OF_LIST()
    },
};

static void s6b33bx_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSIPeripheralClass *spc = SSI_PERIPHERAL_CLASS(oc);

    dc->vmsd = &vmstate_s6b33bx;
    spc->realize = s6b33bx_realize;
    spc->transfer = s6b33bx_transfer;
    spc->set_cs = s6b33bx_set_cs;
    spc->cs_polarity = SSI_CS_LOW;
    device_class_set_props(dc, s6b33bx_properties);
    device_class_set_legacy_reset(dc, s6b33bx_system_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->desc = "S6B33Bx-compatible RGB565 LCD controller";
}

static const TypeInfo s6b33bx_info = {
    .name = TYPE_S6B33BX_COMPATIBLE,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(S6B33BxState),
    .instance_init = s6b33bx_init,
    .class_init = s6b33bx_class_init,
};

static void s6b33bx_register_types(void)
{
    type_register_static(&s6b33bx_info);
}
type_init(s6b33bx_register_types)
