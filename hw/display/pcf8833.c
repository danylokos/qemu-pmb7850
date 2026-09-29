/*
 * PCF8833-compatible four-wire RGB444 LCD controller.
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
#include "hw/display/pcf8833.h"
#include "x55_lcd_common.h"
#include "trace/trace-hw_display.h"

#define PCF8833_WIDTH 132
#define PCF8833_HEIGHT 132

struct PCF8833State {
    SSIPeripheral parent_obj;
    X55LCDCommon common;
    uint16_t gram[PCF8833_WIDTH * PCF8833_HEIGHT];
    uint16_t presented_gram[PCF8833_WIDTH * PCF8833_HEIGHT];

    uint8_t x;
    uint8_t y;
    uint8_t window_start_x;
    uint8_t window_start_y;
    uint8_t end_x;
    uint8_t end_y;
    uint8_t current_command;
    uint8_t parameter_index;
    uint8_t colmod;
    uint8_t madctl;
    bool display_on;
    bool reverse;
    bool sleep_out;
    bool partial_mode;
    bool normal_mode;
    bool idle_mode;
    bool tearing_on;
    bool booster_on;
    bool ram_write;
    uint8_t all_pixels;
    uint8_t contrast;
    uint8_t partial_start;
    uint8_t partial_end;
    uint8_t scroll_definition[3];
    uint8_t scroll_entry;
    uint8_t rgb_lut[20];
    uint8_t rgb_lut_count;
    uint32_t command_count;
    uint32_t unknown_commands;
};

static void pcf8833_reset_registers(void *opaque)
{
    PCF8833State *s = opaque;
    static const uint8_t default_lut[20] = {
        0, 2, 4, 6, 9, 11, 13, 15, 0, 2, 4, 6, 9, 11, 13, 15,
        0, 4, 11, 15,
    };

    s->x = 2;
    s->y = 2;
    s->window_start_x = 2;
    s->window_start_y = 2;
    s->end_x = 129;
    s->end_y = 129;
    s->current_command = 0;
    s->parameter_index = 0;
    s->colmod = 3;
    s->madctl = 0;
    s->display_on = true;
    s->reverse = false;
    s->sleep_out = false;
    s->partial_mode = false;
    s->normal_mode = true;
    s->idle_mode = false;
    s->tearing_on = false;
    s->booster_on = true;
    s->ram_write = false;
    s->all_pixels = 0;
    s->contrast = 0;
    s->partial_start = 0;
    s->partial_end = 31;
    s->scroll_definition[0] = 0;
    s->scroll_definition[1] = 130;
    s->scroll_definition[2] = 0;
    s->scroll_entry = 0;
    memcpy(s->rgb_lut, default_lut, sizeof(default_lut));
    s->rgb_lut_count = 0;
    s->command_count = 0;
    s->unknown_commands = 0;
}

static uint8_t pcf8833_bound(uint8_t value)
{
    return MIN(value, PCF8833_WIDTH - 1);
}

static void pcf8833_command(PCF8833State *s, uint8_t value)
{
    s->current_command = value;
    s->parameter_index = 0;
    s->ram_write = false;
    s->command_count++;
    switch (value) {
    case 0x01:
        x55_lcd_common_reset(&s->common);
        s->current_command = value;
        break;
    case 0x02:
        s->booster_on = false;
        break;
    case 0x03:
        s->booster_on = true;
        break;
    case 0x10:
        s->sleep_out = false;
        break;
    case 0x11:
        s->sleep_out = true;
        break;
    case 0x12:
        s->partial_mode = true;
        s->normal_mode = false;
        break;
    case 0x13:
        s->partial_mode = false;
        s->normal_mode = true;
        s->all_pixels = 0;
        break;
    case 0x20:
        s->reverse = false;
        break;
    case 0x21:
        s->reverse = true;
        break;
    case 0x22:
        s->all_pixels = 2;
        break;
    case 0x23:
        s->all_pixels = 1;
        break;
    case 0x28:
        s->display_on = false;
        break;
    case 0x29:
        s->display_on = true;
        break;
    case 0x2c:
        s->x = s->window_start_x;
        s->y = s->window_start_y;
        s->ram_write = true;
        break;
    case 0x34:
        s->tearing_on = false;
        break;
    case 0x35:
        s->tearing_on = true;
        break;
    case 0x38:
        s->idle_mode = false;
        break;
    case 0x39:
        s->idle_mode = true;
        break;
    case 0x00:
    case 0x25:
    case 0x2a:
    case 0x2b:
    case 0x2d:
    case 0x30:
    case 0x33:
    case 0x36:
    case 0x37:
    case 0x3a:
        break;
    default:
        s->unknown_commands++;
        break;
    }
    trace_x55_color_lcd_command(TYPE_PCF8833_COMPATIBLE, value, s->x, s->y);
    switch (value) {
    case 0x01:
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x28:
    case 0x29:
    case 0x38:
    case 0x39:
        x55_lcd_common_refresh(&s->common);
        break;
    }
}

static void pcf8833_parameter(PCF8833State *s, uint8_t value)
{
    unsigned index = s->parameter_index++;
    bool conflict = false;

    if (s->current_command == 0x2a && index < 2) {
        conflict = pcf8833_bound(value) !=
                   (index ? s->end_x : s->window_start_x);
    } else if (s->current_command == 0x2b && index < 2) {
        conflict = pcf8833_bound(value) !=
                   (index ? s->end_y : s->window_start_y);
    } else if (s->current_command == 0x36 && !index) {
        conflict = value != s->madctl;
    } else if (s->current_command == 0x3a && !index && (value & 7)) {
        conflict = (value & 7) != s->colmod;
    }
    if (conflict) {
        x55_lcd_sweep_flush(&s->common.sweep, x55_lcd_common_present,
                            &s->common, "fallback");
    }

    switch (s->current_command) {
    case 0x25:
        if (index == 0) {
            s->contrast = value & 0x7f;
        }
        break;
    case 0x2a:
        if (index == 0) {
            s->window_start_x = pcf8833_bound(value);
        } else if (index == 1) {
            s->end_x = pcf8833_bound(value);
        }
        break;
    case 0x2b:
        if (index == 0) {
            s->window_start_y = pcf8833_bound(value);
        } else if (index == 1) {
            s->end_y = pcf8833_bound(value);
        }
        break;
    case 0x2d:
        if (index < sizeof(s->rgb_lut)) {
            s->rgb_lut[index] = value & 15;
            s->rgb_lut_count = index + 1;
        }
        break;
    case 0x30:
        if (index == 0) {
            s->partial_start = pcf8833_bound(value);
        } else if (index == 1) {
            s->partial_end = pcf8833_bound(value);
        }
        break;
    case 0x33:
        if (index < 3) {
            s->scroll_definition[index] = value;
        }
        break;
    case 0x36:
        if (index == 0) {
            s->madctl = value;
        }
        break;
    case 0x37:
        if (index == 0) {
            s->scroll_entry = value;
        }
        break;
    case 0x3a:
        if (index == 0 && (value & 7)) {
            s->colmod = value & 7;
        }
        break;
    }
    if (s->current_command == 0x36 && !index) {
        x55_lcd_common_refresh(&s->common);
    }
}

static void pcf8833_increment(PCF8833State *s)
{
    if (s->madctl & 0x20) {
        if (s->y >= s->end_y) {
            s->y = s->window_start_y;
            s->x = s->x >= s->end_x ? s->window_start_x : s->x + 1;
        } else {
            s->y++;
        }
    } else if (s->x >= s->end_x) {
        s->x = s->window_start_x;
        s->y = s->y >= s->end_y ? s->window_start_y : s->y + 1;
    } else {
        s->x++;
    }
}

/*
 * SL55 SW20 sends a complete viewport while PASET includes an extra row.
 * Publish that contiguous horizontal sweep without changing the inclusive
 * controller window or cursor. Do not infer completion from CS or byte counts.
 */
static void pcf8833_present_complete_viewport(PCF8833State *s)
{
    X55LCDCommon *c = &s->common;
    X55LCDSweep *t = &c->sweep;
    unsigned next_y = c->panel_origin_y + c->panel_height;

    if (t->active && !t->packing && !(t->shape.mode & 0x20) &&
        t->origin_x == c->panel_origin_x &&
        t->origin_y == c->panel_origin_y &&
        t->shape.x0 == t->origin_x && t->shape.y0 == t->origin_y &&
        t->shape.x1 + 1 == c->panel_origin_x + c->panel_width &&
        t->shape.y1 >= next_y && t->expected_x == t->origin_x &&
        t->expected_y == next_y) {
        x55_lcd_sweep_flush(t, x55_lcd_common_present, c,
                            "viewport_complete");
    }
}

static void pcf8833_pixel(PCF8833State *s, uint16_t value)
{
    if (!s->ram_write || s->common.reset_asserted) {
        return;
    }
    X55LCDSweepShape shape = {
        .mode = s->madctl | (s->colmod << 8),
        .x0 = s->window_start_x, .y0 = s->window_start_y,
        .x1 = s->end_x, .y1 = s->end_y, .windowed = true,
    };
    unsigned writes = s->x < PCF8833_WIDTH && s->y < PCF8833_HEIGHT;

    x55_lcd_sweep_begin(&s->common.sweep, &shape, s->x, s->y, 0,
                        x55_lcd_common_present, &s->common);
    value &= 0x0fff;
    if (s->x < PCF8833_WIDTH && s->y < PCF8833_HEIGHT) {
        s->gram[s->y * PCF8833_WIDTH + s->x] = value;
        trace_x55_color_lcd_data(TYPE_PCF8833_COMPATIBLE, value, s->x, s->y);
    }
    pcf8833_increment(s);
    x55_lcd_sweep_end(&s->common.sweep, s->x, s->y, 0, 2, writes,
                      s->x == s->window_start_x && s->y == s->window_start_y,
                      x55_lcd_common_present, &s->common);
    pcf8833_present_complete_viewport(s);
}

static uint32_t pcf8833_transfer(SSIPeripheral *dev, uint32_t value)
{
    PCF8833State *s = PCF8833_COMPATIBLE(dev);
    X55LCDSSIFrame frame = x55_lcd_common_frame(&s->common, value);

    if (frame.accepted) {
        if (!frame.data && frame.bits == 8) {
            pcf8833_command(s, frame.value);
        } else if (frame.data && s->ram_write && frame.bits == 12 &&
                   s->colmod == 3) {
            pcf8833_pixel(s, frame.value);
        } else if (frame.data && !s->ram_write && frame.bits == 8) {
            pcf8833_parameter(s, frame.value);
        }
    }
    return frame.mask;
}

static int pcf8833_set_cs(SSIPeripheral *dev, bool level)
{
    PCF8833State *s = PCF8833_COMPATIBLE(dev);

    return x55_lcd_common_set_cs(&s->common, level);
}

static void pcf8833_pixel_rgb888(void *opaque, unsigned x, unsigned y,
                                 uint8_t rgb[3])
{
    PCF8833State *s = opaque;
    unsigned local_x = x - s->common.panel_origin_x;
    unsigned local_y = y - s->common.panel_origin_y;
    unsigned c[3];
    uint16_t pixel;

    if (s->madctl & 0x40) {
        local_x = s->common.panel_width - 1 - local_x;
    }
    if (s->madctl & 0x80) {
        local_y = s->common.panel_height - 1 - local_y;
    }
    x = local_x + s->common.panel_origin_x;
    y = local_y + s->common.panel_origin_y;
    pixel = s->presented_gram[y * PCF8833_WIDTH + x];
    c[0] = (pixel >> 8) & 15;
    c[1] = (pixel >> 4) & 15;
    c[2] = pixel & 15;
    if (s->common.reset_asserted || !s->sleep_out || !s->display_on ||
        s->all_pixels == 2) {
        c[0] = c[1] = c[2] = 0;
    } else if (s->all_pixels == 1) {
        c[0] = c[1] = c[2] = 15;
    } else if (s->idle_mode) {
        for (unsigned i = 0; i < 3; i++) {
            c[i] = c[i] & 8 ? 15 : 0;
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        if (s->reverse) {
            c[i] = 15 - c[i];
        }
        rgb[i] = c[i] * 17;
    }
}

static void pcf8833_present(void *opaque)
{
    PCF8833State *s = opaque;

    memcpy(s->presented_gram, s->gram, sizeof(s->gram));
}

static const X55LCDCommonOps pcf8833_common_ops = {
    .reset = pcf8833_reset_registers,
    .present = pcf8833_present,
    .pixel_rgb888 = pcf8833_pixel_rgb888,
};

static void pcf8833_realize(SSIPeripheral *dev, Error **errp)
{
    PCF8833State *s = PCF8833_COMPATIBLE(dev);

    x55_lcd_common_realize(&s->common, DEVICE(dev), s,
                           &pcf8833_common_ops,
                           TYPE_PCF8833_COMPATIBLE,
                           PCF8833_WIDTH, PCF8833_HEIGHT,
                           101, 80, 1, 0, errp);
}

static void pcf8833_system_reset(DeviceState *dev)
{
    PCF8833State *s = PCF8833_COMPATIBLE(dev);

    x55_lcd_common_system_reset(&s->common);
}

static bool pcf8833_get_reset(Object *obj, Error **errp)
{
    return PCF8833_COMPATIBLE(obj)->common.reset_asserted;
}

static const Property pcf8833_properties[] = {
    X55_LCD_PROP_PANEL_WIDTH(PCF8833State, common),
    X55_LCD_PROP_PANEL_HEIGHT(PCF8833State, common),
    X55_LCD_PROP_PANEL_ORIGIN_X(PCF8833State, common),
    X55_LCD_PROP_PANEL_ORIGIN_Y(PCF8833State, common),
    X55_LCD_PROP_PANEL_MIRROR_X(PCF8833State, common),
    X55_LCD_PROP_PANEL_MIRROR_Y(PCF8833State, common),
    X55_LCD_PROP_RESET_IDLE_LEVEL(PCF8833State, common),
    X55_LCD_PROP_SCALE(PCF8833State, common),
};

static void pcf8833_init(Object *obj)
{
    PCF8833State *s = PCF8833_COMPATIBLE(obj);

    x55_lcd_common_init(&s->common, obj, s, &pcf8833_common_ops,
                        TYPE_PCF8833_COMPATIBLE, pcf8833_get_reset);
}

static int pcf8833_post_load(void *opaque, int version_id)
{
    PCF8833State *s = opaque;

    if (s->common.frame_bits > 16 || s->rgb_lut_count > sizeof(s->rgb_lut)) {
        return -EINVAL;
    }
    /* Publish complete viewports held by older saves; retain partial sweeps. */
    pcf8833_present_complete_viewport(s);
    /* Redraw the restored presentation. */
    s->common.redraw = true;
    s->common.force_update = true;
    return 0;
}

static const VMStateDescription vmstate_pcf8833 = {
    .name = "x55/pcf8833",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pcf8833_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, PCF8833State),
        VMSTATE_STRUCT(common, PCF8833State, 0, vmstate_x55_lcd_common,
                       X55LCDCommon),
        VMSTATE_UINT16_ARRAY(gram, PCF8833State,
                             PCF8833_WIDTH * PCF8833_HEIGHT),
        VMSTATE_UINT16_ARRAY(presented_gram, PCF8833State,
                             PCF8833_WIDTH * PCF8833_HEIGHT),
        VMSTATE_UINT8(x, PCF8833State),
        VMSTATE_UINT8(y, PCF8833State),
        VMSTATE_UINT8(window_start_x, PCF8833State),
        VMSTATE_UINT8(window_start_y, PCF8833State),
        VMSTATE_UINT8(end_x, PCF8833State),
        VMSTATE_UINT8(end_y, PCF8833State),
        VMSTATE_UINT8(current_command, PCF8833State),
        VMSTATE_UINT8(parameter_index, PCF8833State),
        VMSTATE_UINT8(colmod, PCF8833State),
        VMSTATE_UINT8(madctl, PCF8833State),
        VMSTATE_UINT8(all_pixels, PCF8833State),
        VMSTATE_UINT8(contrast, PCF8833State),
        VMSTATE_UINT8(partial_start, PCF8833State),
        VMSTATE_UINT8(partial_end, PCF8833State),
        VMSTATE_UINT8(scroll_entry, PCF8833State),
        VMSTATE_UINT8(rgb_lut_count, PCF8833State),
        VMSTATE_BOOL(display_on, PCF8833State),
        VMSTATE_BOOL(reverse, PCF8833State),
        VMSTATE_BOOL(sleep_out, PCF8833State),
        VMSTATE_BOOL(partial_mode, PCF8833State),
        VMSTATE_BOOL(normal_mode, PCF8833State),
        VMSTATE_BOOL(idle_mode, PCF8833State),
        VMSTATE_BOOL(tearing_on, PCF8833State),
        VMSTATE_BOOL(booster_on, PCF8833State),
        VMSTATE_BOOL(ram_write, PCF8833State),
        VMSTATE_BUFFER(scroll_definition, PCF8833State),
        VMSTATE_BUFFER(rgb_lut, PCF8833State),
        VMSTATE_UINT32(command_count, PCF8833State),
        VMSTATE_UINT32(unknown_commands, PCF8833State),
        VMSTATE_END_OF_LIST()
    },
};

static void pcf8833_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSIPeripheralClass *spc = SSI_PERIPHERAL_CLASS(oc);

    dc->vmsd = &vmstate_pcf8833;
    spc->realize = pcf8833_realize;
    spc->transfer = pcf8833_transfer;
    spc->set_cs = pcf8833_set_cs;
    spc->cs_polarity = SSI_CS_LOW;
    device_class_set_props(dc, pcf8833_properties);
    device_class_set_legacy_reset(dc, pcf8833_system_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->desc = "PCF8833-compatible four-wire RGB444 LCD";
}

static const TypeInfo pcf8833_info = {
    .name = TYPE_PCF8833_COMPATIBLE,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(PCF8833State),
    .instance_init = pcf8833_init,
    .class_init = pcf8833_class_init,
};

static void pcf8833_register_types(void)
{
    type_register_static(&pcf8833_info);
}
type_init(pcf8833_register_types)
