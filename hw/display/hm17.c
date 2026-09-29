/*
 * Hynix HM17CM4096 and HM17CM256 LCD controllers.
 *
 * The controllers share the command-bank and serial framing architecture,
 * but expose immutable GRAM geometry and pixel packing per concrete QOM type.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/hm17.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "x55_lcd_sweep.h"
#include "trace/trace-hw_display.h"

#define HM17_MAX_WIDTH 128
#define HM17_MAX_HEIGHT 162

typedef enum HM17Model {
    HM17_MODEL_4096,
    HM17_MODEL_256,
} HM17Model;

typedef struct HM17Class {
    SSIPeripheralClass parent_class;
    HM17Model model;
    const char *model_name;
    unsigned width;
    unsigned height;
    unsigned graphic_height;
} HM17Class;

typedef struct HM17State {
    SSIPeripheral parent_obj;
    QemuConsole *console;

    uint16_t gram[HM17_MAX_WIDTH * HM17_MAX_HEIGHT];
    uint16_t presented_gram[HM17_MAX_WIDTH * HM17_MAX_HEIGHT];
    uint8_t palette[3][16];
    uint8_t x, y, window_start_x, window_start_y, end_x, end_y;
    uint8_t start_line, n_line, line_start, line_end;
    uint8_t re, scan_start, duty, boost, bias, electric_volume;
    bool display_on, all_on, reverse, common_shift;
    bool n_line_enable, swap, reflect;
    bool window, aim, increment_x, increment_y;
    bool amp_on, halt, dc_on, discharge;
    bool monochrome, fixed_pwm, clock_select, word_length_16;
    bool color_256, high_speed_write, absolute_12bit;
    bool palette_upper, line_reverse, blink_type, glsb;
    uint8_t extension_cs, oscillator_rf, oscillator_ffl;
    uint8_t dummy_y, pwm_control;
    uint8_t pack[3], pack_count;

    bool dc;
    uint8_t frame_bits;
    bool frame_msb_first, frame_pending, frame_is_data;
    uint8_t stream_shift, stream_bits;
    X55LCDSweep sweep;
    uint64_t frame_count, frame_digest;

    bool reset_level, reset_driven, reset_idle_level, reset_asserted;
    bool redraw;
    bool force_update;
    uint64_t display_updates;
    uint32_t panel_width, panel_height, panel_origin_x, panel_origin_y;
    bool panel_mirror_x, panel_mirror_y;
    uint32_t scale;
} HM17State;

#define TYPE_HM17_BASE "hm17-base"
#define TYPE_HM17 TYPE_HM17_BASE
OBJECT_DECLARE_TYPE(HM17State, HM17Class, HM17)

static uint64_t hm17_present(void *opaque, const X55LCDSweep *sweep);

static const HM17Class *hm17_class(HM17State *s)
{
    return HM17_GET_CLASS(s);
}

static void hm17_reset_registers(HM17State *s)
{
    const HM17Class *hc = hm17_class(s);
    static const uint8_t palette4096[16] = {
        0, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31,
    };
    static const uint8_t palette256[8] = { 0, 5, 10, 14, 17, 21, 26, 31 };

    s->x = s->y = s->window_start_x = s->window_start_y = 0;
    s->end_x = hc->model == HM17_MODEL_4096 ? 0xff : hc->width - 1;
    s->end_y = hc->height - 1;
    s->start_line = s->n_line = s->scan_start = s->re = 0;
    s->line_start = 0;
    s->line_end = hc->height - 1;
    s->duty = s->boost = s->bias = s->electric_volume = 0;
    s->display_on = s->all_on = s->reverse = s->common_shift = false;
    s->n_line_enable = s->swap = s->reflect = false;
    s->window = s->aim = s->increment_x = s->increment_y = false;
    s->amp_on = s->halt = s->dc_on = s->discharge = false;
    s->monochrome = s->fixed_pwm = s->clock_select = false;
    s->word_length_16 = s->color_256 = false;
    s->high_speed_write = s->absolute_12bit = false;
    s->palette_upper = s->line_reverse = s->blink_type = s->glsb = false;
    s->extension_cs = s->oscillator_rf = s->oscillator_ffl = 0;
    s->dummy_y = s->pwm_control = 0;
    s->pack_count = s->stream_shift = s->stream_bits = 0;
    s->frame_pending = false;
    x55_lcd_sweep_discard(&s->sweep);
    for (unsigned c = 0; c < 3; c++) {
        memset(s->palette[c], 0, sizeof(s->palette[c]));
        memcpy(s->palette[c], hc->model == HM17_MODEL_4096 ?
               palette4096 : palette256,
               hc->model == HM17_MODEL_4096 ? sizeof(palette4096) :
                                               sizeof(palette256));
    }
}

static void hm17_increment(HM17State *s)
{
    const HM17Class *hc = hm17_class(s);
    unsigned xmax = s->window ? s->end_x :
                    (hc->model == HM17_MODEL_4096 ? 0xff : hc->width - 1);
    unsigned ymax = s->window ? s->end_y : hc->height - 1;
    unsigned xstart = s->window ? s->window_start_x : 0;
    unsigned ystart = s->window ? s->window_start_y : 0;

    if (s->increment_x) {
        if (s->x >= xmax) {
            s->x = xstart;
            if (s->increment_y) {
                s->y = s->y >= ymax ? ystart : s->y + 1;
            }
        } else {
            s->x++;
        }
    } else if (s->increment_y) {
        s->y = s->y >= ymax ? ystart : s->y + 1;
    }
}

static unsigned hm17_set_pixel(HM17State *s, unsigned x, unsigned y,
                           uint16_t pixel)
{
    const HM17Class *hc = hm17_class(s);

    if (x >= hc->width || y >= hc->height) {
        return 0;
    }
    if (s->reflect) {
        x = hc->width - 1 - x;
    }
    s->gram[y * HM17_MAX_WIDTH + x] = pixel;
    trace_hm17_data(hc->model_name, pixel, x, y);
    return 1;
}

static uint16_t rgb444(unsigned r, unsigned g, unsigned b)
{
    return ((r & 15) << 8) | ((g & 15) << 4) | (b & 15);
}

static void hm17_data(HM17State *s, uint8_t value)
{
    const HM17Class *hc = hm17_class(s);

    if (s->word_length_16) {
        return;
    }
    unsigned x = s->x, y = s->y;
    unsigned density = hc->model == HM17_MODEL_256 || s->color_256 ? 1 : 2;
    X55LCDSweepShape shape = {
        .mode = s->increment_x | (s->increment_y << 1) | (s->reflect << 2) |
                (s->swap << 3) | (s->color_256 << 4) |
                (s->high_speed_write << 5) | (s->absolute_12bit << 6) |
                (s->glsb << 7) | (s->fixed_pwm << 8),
        .x0 = s->window ? s->window_start_x : 0,
        .y0 = s->window ? s->window_start_y : s->panel_origin_y,
        .x1 = s->window ? s->end_x :
              hc->model == HM17_MODEL_256 ? hc->width - 1 :
              s->high_speed_write ? 0xff :
              (s->panel_origin_x + s->panel_width) * density - 1,
        .y1 = s->window ? s->end_y :
              s->panel_origin_y + s->panel_height - 1,
        .windowed = s->window,
        .software_rows = !s->window && s->increment_x && !s->increment_y,
    };
    unsigned writes = 0;

    x55_lcd_sweep_begin(&s->sweep, &shape, x, y, s->pack_count,
                        hm17_present, s);
    if (hc->model == HM17_MODEL_256) {
        uint8_t pixel = s->reflect != s->swap ? revbit8(value) : value;

        writes += hm17_set_pixel(s, s->x, s->y, pixel);
    } else if (s->color_256) {
        writes += hm17_set_pixel(s, s->x, s->y,
                       rgb444((((value >> 5) & 7) << 1) | 1,
                              (((value >> 2) & 7) << 1) | 1,
                              ((value & 3) << 2) | 3));
    } else if (s->high_speed_write) {
        s->pack[s->pack_count++] = value;
        if (s->pack_count == 3) {
            unsigned group = s->x / 3;

            writes += hm17_set_pixel(s, group * 2, s->y,
                           rgb444(s->pack[0] >> 4, s->pack[0],
                                  s->pack[1] >> 4));
            writes += hm17_set_pixel(s, group * 2 + 1, s->y,
                           rgb444(s->pack[1], s->pack[2] >> 4, s->pack[2]));
            s->pack_count = 0;
        }
    } else {
        s->pack[s->pack_count++] = value;
        if (s->pack_count == 2) {
            uint16_t pixel;

            if (s->absolute_12bit) {
                pixel = rgb444(s->pack[0], s->pack[1] >> 4, s->pack[1]);
            } else {
                pixel = rgb444(s->pack[0] >> 4,
                               ((s->pack[0] & 7) << 1) |
                               (s->pack[1] >> 7),
                               (s->pack[1] >> 1) & 15);
            }
            writes += hm17_set_pixel(s, s->x / 2, s->y, pixel);
            s->pack_count = 0;
        }
    }
    hm17_increment(s);
    bool terminal = s->window ?
                    s->x == s->window_start_x && s->y == s->window_start_y :
                    x == shape.x1 && y == shape.y1;

    x55_lcd_sweep_end(&s->sweep, s->x, s->y, s->pack_count, 1, writes,
                      terminal, hm17_present, s);
}

static bool hm17_palette_target(HM17State *s, unsigned address,
                                unsigned *channel, unsigned *index,
                                bool *upper)
{
    if (s->re == 1 && address <= 0x0d) {
        *channel = 0; *index = address >> 1; *upper = address & 1; return true;
    }
    if (s->re == 2 && address <= 1) {
        *channel = 0; *index = 7; *upper = address & 1; return true;
    }
    if (s->re == 2 && address <= 0x0d) {
        *channel = 1; *index = (address - 2) >> 1;
        *upper = address & 1; return true;
    }
    if (s->re == 3 && address <= 3) {
        *channel = 1; *index = 6 + (address >> 1);
        *upper = address & 1; return true;
    }
    if (s->re == 3 && address <= 0x0d) {
        *channel = 2; *index = (address - 4) >> 1;
        *upper = address & 1; return true;
    }
    if (s->re == 4 && address <= 5) {
        *channel = 2; *index = 5 + (address >> 1);
        *upper = address & 1; return true;
    }
    return false;
}

static void hm17_command(HM17State *s, uint8_t value)
{
    const HM17Class *hc = hm17_class(s);
    unsigned address = value & 15;
    unsigned channel, index;
    bool upper;
    unsigned group = value >> 4;
    bool bank_select = (value & 0xf8) == 0xf0;
    bool redraw = !bank_select &&
                  !((s->re == 0 || s->re == 5) && group < 4);
    bool conflict = false;

    if (!bank_select && s->re == 0) {
        if (group == 0xa) {
            conflict = !!(value & 8) != s->window ||
                       !!(value & 2) != s->increment_y ||
                       !!(value & 1) != s->increment_x;
        } else if (group == 9) {
            conflict = !!(value & 2) != s->swap ||
                       !!(value & 1) != s->reflect;
        } else if (s->window && group < 4) {
            unsigned start = group < 2 ? s->window_start_x : s->window_start_y;

            conflict = ((start >> (4 * (group & 1))) & 15) != address;
        }
    } else if (!bank_select && s->re == 4) {
        if (group == 8) {
            conflict = !!(value & 8) != s->fixed_pwm ||
                       !!(value & 4) != (hc->model == HM17_MODEL_4096 ?
                                         s->color_256 : s->glsb);
        } else if (group == 9) {
            conflict = !!(value & 1) != s->word_length_16 ||
                       (hc->model == HM17_MODEL_4096 &&
                        (!!(value & 8) != s->high_speed_write ||
                         !!(value & 4) != s->absolute_12bit));
        }
    } else if (!bank_select && s->re == 5 && s->window && group < 4) {
        unsigned end = group < 2 ? s->end_x : s->end_y;

        conflict = ((end >> (4 * (group & 1))) & 15) != address;
    }
    if (conflict) {
        x55_lcd_sweep_flush(&s->sweep, hm17_present, s, "fallback");
    }

    if ((value & 0xf8) == 0xf0) {
        s->re = value & 7;
        goto out;
    }
    if (hm17_palette_target(s, value >> 4, &channel, &index, &upper)) {
        if (hc->model == HM17_MODEL_4096 && s->palette_upper) {
            index += 8;
        }
        if (upper) {
            s->palette[channel][index] =
                (s->palette[channel][index] & 0x0f) | ((value & 1) << 4);
        } else {
            s->palette[channel][index] =
                (s->palette[channel][index] & 0x10) | address;
        }
        goto out;
    }
    if (s->re == 0) {
        switch (value >> 4) {
        case 0x0:
            s->x = (s->x & 0xf0) | address;
            s->window_start_x = s->x;
            s->pack_count = 0;
            break;
        case 0x1:
            s->x = (s->x & 0x0f) | (address << 4);
            s->window_start_x = s->x;
            s->pack_count = 0;
            break;
        case 0x2:
            s->y = (s->y & 0xf0) | address;
            s->window_start_y = s->y;
            s->pack_count = 0;
            break;
        case 0x3:
            s->y = (s->y & 0x0f) | (address << 4);
            s->window_start_y = s->y;
            s->pack_count = 0;
            break;
        case 0x4:
            s->start_line = (s->start_line & 0xf0) | address;
            break;
        case 0x5:
            s->start_line = (s->start_line & 0x0f) | (address << 4);
            break;
        case 0x6:
            s->n_line = (s->n_line & 0xf0) | address;
            break;
        case 0x7:
            s->n_line = (s->n_line & 0x0f) | (address << 4);
            break;
        case 0x8:
            s->common_shift = value & 8;
            s->monochrome = value & 4;
            s->all_on = value & 2;
            s->display_on = value & 1;
            break;
        case 0x9:
            s->reverse = value & 8;
            s->n_line_enable = value & 4;
            s->swap = value & 2;
            s->reflect = value & 1;
            break;
        case 0xa:
            s->window = value & 8;
            s->aim = value & 4;
            s->increment_y = value & 2;
            s->increment_x = value & 1;
            break;
        case 0xb:
            s->amp_on = value & 8;
            s->halt = value & 4;
            s->dc_on = value & 2;
            if (value & 1) {
                hm17_reset_registers(s);
            }
            break;
        case 0xc:
            s->duty = address;
            break;
        case 0xd:
            s->boost = value & 7;
            break;
        case 0xe:
            s->bias = value & 7;
            break;
        }
    } else if (s->re == 4) {
        switch (value >> 4) {
        case 0x6:
            s->scan_start = address;
            break;
        case 0x7:
            s->extension_cs = address;
            break;
        case 0x8:
            s->fixed_pwm = value & 8;
            if (hc->model == HM17_MODEL_4096) {
                s->color_256 = value & 4;
            } else {
                s->glsb = value & 4;
            }
            break;
        case 0x9:
            if (hc->model == HM17_MODEL_4096) {
                s->high_speed_write = value & 8;
                s->absolute_12bit = value & 4;
            }
            s->clock_select = value & 2;
            s->word_length_16 = value & 1;
            s->pack_count = 0;
            break;
        case 0xa:
            s->electric_volume = (s->electric_volume & 0x70) | address;
            break;
        case 0xb:
            s->electric_volume = (s->electric_volume & 0x0f) |
                                 ((value & 7) << 4);
            break;
        case 0xd:
            s->oscillator_rf = (value >> 2) & 3;
            s->oscillator_ffl = value & 3;
            break;
        case 0xe:
            s->discharge = value & 1;
            break;
        }
    } else if (s->re == 5) {
        switch (value >> 4) {
        case 0x0:
            s->end_x = (s->end_x & 0xf0) | address;
            break;
        case 0x1:
            s->end_x = (s->end_x & 0x0f) | (address << 4);
            break;
        case 0x2:
            s->end_y = (s->end_y & 0xf0) | address;
            break;
        case 0x3:
            s->end_y = (s->end_y & 0x0f) | (address << 4);
            break;
        case 0x4:
            s->line_start = (s->line_start & 0xf0) | address;
            break;
        case 0x5:
            s->line_start = (s->line_start & 0x0f) | (address << 4);
            break;
        case 0x6:
            s->line_end = (s->line_end & 0xf0) | address;
            break;
        case 0x7:
            s->line_end = (s->line_end & 0x0f) | (address << 4);
            break;
        case 0x8:
            s->blink_type = value & 2;
            s->line_reverse = value & 1;
            break;
        case 0x9:
            if (hc->model == HM17_MODEL_4096) {
                s->palette_upper = value & 1;
            } else {
                s->dummy_y = address;
            }
            break;
        case 0xa:
            s->pwm_control = address;
            break;
        }
    }
out:
    trace_hm17_command(hc->model_name, value, s->re, s->x, s->y);
    if (redraw && s->console) {
        s->redraw = true;
        qemu_console_hw_update(s->console);
    }
}

static void hm17_byte(HM17State *s, bool is_data, uint8_t value)
{
    if (s->reset_asserted) {
        return;
    }
    if (is_data) {
        hm17_data(s, value);
    } else {
        hm17_command(s, value);
    }
}

static void hm17_consume(HM17State *s, uint16_t value, unsigned bits,
                         bool msb_first, bool is_data)
{
    for (unsigned i = 0; i < bits; i++) {
        unsigned bit = msb_first ? bits - 1 - i : i;

        s->stream_shift = (s->stream_shift << 1) | extract32(value, bit, 1);
        if (++s->stream_bits == 8) {
            hm17_byte(s, is_data, s->stream_shift);
            s->stream_shift = s->stream_bits = 0;
        }
    }
}

static unsigned hm17_scan_offset(uint8_t scan)
{
    static const uint8_t offsets[16] = {
        0, 1, 9, 14, 17, 25, 33, 41, 49, 57, 65, 73, 122, 130, 138, 146,
    };
    return offsets[scan & 15];
}

static uint8_t hm17_scaled(unsigned value, unsigned maximum)
{
    return (value * 255 + maximum / 2) / maximum;
}

static void hm17_pixel(HM17State *s, unsigned x, unsigned y, uint8_t rgb[3])
{
    const HM17Class *hc = hm17_class(s);
    unsigned sx, sy, line;
    uint16_t pixel;

    if (s->panel_mirror_x) {
        x = s->panel_width - 1 - x;
    }
    if (s->panel_mirror_y) {
        y = s->panel_height - 1 - y;
    }
    sx = s->panel_origin_x + x;
    line = s->panel_origin_y + y;
    if (s->common_shift) {
        line = hc->graphic_height - 1 - line;
    }
    sy = (s->start_line +
          (hc->model == HM17_MODEL_4096 ? hm17_scan_offset(s->scan_start) :
                                          15 * (s->scan_start & 15)) + line) %
         hc->graphic_height;
    pixel = sx < hc->width && sy < hc->height ?
            s->presented_gram[sy * HM17_MAX_WIDTH + sx] : 0;
    if (s->reset_asserted || !s->display_on || s->halt) {
        rgb[0] = rgb[1] = rgb[2] = 0;
    } else if (s->all_on) {
        rgb[0] = rgb[1] = rgb[2] = 255;
    } else if (hc->model == HM17_MODEL_4096) {
        unsigned nibble[3] = {
            (pixel >> 8) & 15, (pixel >> 4) & 15, pixel & 15,
        };

        for (unsigned c = 0; c < 3; c++) {
            rgb[c] = s->monochrome ? (nibble[c] & 8 ? 255 : 0) :
                     s->fixed_pwm ? hm17_scaled(c == 2 ? nibble[c] >> 2 :
                                                        nibble[c] >> 1,
                                                c == 2 ? 3 : 7) :
                     hm17_scaled(s->palette[c][nibble[c]], 31);
        }
    } else {
        unsigned v[3] = { (pixel >> 5) & 7, (pixel >> 2) & 7, pixel & 3 };
        unsigned bi = s->fixed_pwm ?
                      (v[2] == 0 ? 0 : v[2] == 3 ? 7 : (v[2] << 1) | s->glsb) :
                      (v[2] << 1) | s->glsb;

        if (s->monochrome) {
            rgb[0] = rgb[1] = rgb[2] = pixel ? 255 : 0;
        } else if (s->fixed_pwm) {
            rgb[0] = hm17_scaled(v[0], 7);
            rgb[1] = hm17_scaled(v[1], 7);
            rgb[2] = hm17_scaled(bi, 7);
        } else {
            rgb[0] = hm17_scaled(s->palette[0][v[0]], 31);
            rgb[1] = hm17_scaled(s->palette[1][v[1]], 31);
            rgb[2] = hm17_scaled(s->palette[2][bi], 31);
        }
    }
    if (s->reverse) {
        rgb[0] = 255 - rgb[0]; rgb[1] = 255 - rgb[1]; rgb[2] = 255 - rgb[2];
    }
}

static uint64_t hm17_digest(HM17State *s)
{
    uint64_t hash = UINT64_C(14695981039346656037);

    for (unsigned y = 0; y < s->panel_height; y++) {
        for (unsigned x = 0; x < s->panel_width; x++) {
            uint8_t rgb[3];

            hm17_pixel(s, x, y, rgb);
            for (unsigned c = 0; c < 3; c++) {
                hash ^= rgb[c]; hash *= UINT64_C(1099511628211);
            }
        }
    }
    return hash;
}

bool hm17_render_rgb888(DeviceState *dev, uint8_t *frame,
                        size_t frame_size, unsigned *width, unsigned *height)
{
    HM17State *s;
    size_t required;

    if (!dev || !object_dynamic_cast(OBJECT(dev), TYPE_HM17_BASE)) {
        return false;
    }
    s = HM17(dev);
    required = (size_t)s->panel_width * s->panel_height * 3;
    if (!frame || frame_size != required) {
        return false;
    }
    for (unsigned y = 0; y < s->panel_height; y++) {
        for (unsigned x = 0; x < s->panel_width; x++) {
            hm17_pixel(s, x, y, frame + ((size_t)y * s->panel_width + x) * 3);
        }
    }
    if (width) {
        *width = s->panel_width;
    }
    if (height) {
        *height = s->panel_height;
    }
    return true;
}

static uint64_t hm17_present(void *opaque, const X55LCDSweep *sweep)
{
    HM17State *s = opaque;

    memcpy(s->presented_gram, s->gram, sizeof(s->presented_gram));
    s->frame_count = sweep->frames;
    s->frame_digest = hm17_digest(s);
    s->force_update = true;
    s->redraw = true;
    qemu_console_hw_update(s->console);
    return s->frame_digest;
}

static uint32_t hm17_transfer(SSIPeripheral *dev, uint32_t value)
{
    HM17State *s = HM17(dev);
    unsigned bits = s->frame_pending ? s->frame_bits : 8;
    bool msb = s->frame_pending ? s->frame_msb_first : true;
    bool data = s->frame_pending ? s->frame_is_data : !s->dc;
    uint32_t mask = bits == 16 ? 0xffff : (1u << bits) - 1;

    if (bits >= 2 && bits <= 16) {
        hm17_consume(s, value & mask, bits, msb, data);
    }
    s->frame_pending = false;
    return mask;
}

static int hm17_set_cs(SSIPeripheral *dev, bool level)
{
    HM17State *s = HM17(dev);

    if (level) {
        x55_lcd_sweep_close(&s->sweep);
        s->stream_shift = s->stream_bits = s->pack_count = 0;
        s->frame_pending = false;
    }
    return 0;
}

static void hm17_dc(void *opaque, int n, int level)
{
    HM17State *s = opaque;
    s->dc = !!level;
}

static void hm17_frame_start(void *opaque, int n, int level)
{
    HM17State *s = opaque;
    unsigned bits = level & 0x1f;

    s->frame_bits = bits;
    s->frame_msb_first = !!(level & 0x100);
    s->frame_is_data = !s->dc;
    s->frame_pending = bits >= 2 && bits <= 16;
}

static void hm17_resolve_reset(HM17State *s)
{
    bool level = s->reset_driven ? s->reset_level : s->reset_idle_level;
    bool asserted = !level;

    if (asserted != s->reset_asserted) {
        s->reset_asserted = asserted;
        if (asserted) {
            hm17_reset_registers(s);
        }
        trace_hm17_reset(hm17_class(s)->model_name, asserted,
                         s->reset_driven, level);
    }
}

static void hm17_reset_level(void *opaque, int n, int level)
{
    HM17State *s = opaque;
    s->reset_level = !!level; hm17_resolve_reset(s);
}

static void hm17_reset_driven(void *opaque, int n, int level)
{
    HM17State *s = opaque;
    s->reset_driven = !!level; hm17_resolve_reset(s);
}

static bool hm17_update_display(void *opaque)
{
    HM17State *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->console);
    uint8_t *base = surface_data(surface);
    int stride = surface_stride(surface);
    bool changed = s->force_update;

    if (!s->redraw || surface_bits_per_pixel(surface) != 32) {
        return true;
    }
    for (unsigned y = 0; y < s->panel_height; y++) {
        for (unsigned sy = 0; sy < s->scale; sy++) {
            uint32_t *line = (uint32_t *)(base + (y * s->scale + sy) * stride);
            for (unsigned x = 0; x < s->panel_width; x++) {
                uint8_t rgb[3];
                hm17_pixel(s, x, y, rgb);
                for (unsigned sx = 0; sx < s->scale; sx++) {
                    uint32_t pixel = rgb_to_pixel32(rgb[0], rgb[1], rgb[2]);

                    changed |= line[x * s->scale + sx] != pixel;
                    line[x * s->scale + sx] = pixel;
                }
            }
        }
    }
    s->redraw = false;
    s->force_update = false;
    if (changed) {
        s->display_updates++;
        qemu_console_update(s->console, 0, 0, s->panel_width * s->scale,
                            s->panel_height * s->scale);
    }
    return true;
}

static void hm17_invalidate(void *opaque)
{
    ((HM17State *)opaque)->redraw = true;
    ((HM17State *)opaque)->force_update = true;
}
static const GraphicHwOps hm17_display_ops = {
    .invalidate = hm17_invalidate, .gfx_update = hm17_update_display,
};

static void hm17_realize(SSIPeripheral *dev, Error **errp)
{
    HM17State *s = HM17(dev);
    const HM17Class *hc = hm17_class(s);
    DeviceState *qdev = DEVICE(dev);

    if (!s->panel_width || !s->panel_height ||
        s->panel_origin_x + s->panel_width > hc->width ||
        s->panel_origin_y + s->panel_height > hc->graphic_height ||
        !s->scale || s->scale > 16) {
        error_setg(errp, "invalid %s panel geometry or scale", hc->model_name);
        return;
    }
    qdev_init_gpio_in_named(qdev, hm17_dc, "dc", 1);
    qdev_init_gpio_in_named(qdev, hm17_frame_start, "frame-start", 1);
    qdev_init_gpio_in_named(qdev, hm17_reset_level, "reset-level", 1);
    qdev_init_gpio_in_named(qdev, hm17_reset_driven, "reset-driven", 1);
    s->console = qemu_graphic_console_create(qdev, 0, &hm17_display_ops, s);
    qemu_console_resize(s->console, s->panel_width * s->scale,
                        s->panel_height * s->scale);
    hm17_reset_registers(s);
    hm17_resolve_reset(s);
}

static void hm17_system_reset(DeviceState *dev)
{
    HM17State *s = HM17(dev);
    hm17_reset_registers(s);
    hm17_resolve_reset(s);
}

static uint64_t hm17_memory_digest(HM17State *s)
{
    const HM17Class *hc = hm17_class(s);
    uint64_t hash = UINT64_C(14695981039346656037);

    for (unsigned y = 0; y < hc->height; y++) {
        for (unsigned x = 0; x < hc->width; x++) {
            uint16_t value = s->gram[y * HM17_MAX_WIDTH + x];

            hash ^= value & 0xff;
            hash *= UINT64_C(1099511628211);
            hash ^= value >> 8;
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

static void hm17_get_memory_digest(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    uint64_t value = hm17_memory_digest(HM17(obj));

    visit_type_uint64(v, name, &value, errp);
}

static const Property hm17_properties[] = {
    DEFINE_PROP_UINT32("panel-width", HM17State, panel_width, 101),
    DEFINE_PROP_UINT32("panel-height", HM17State, panel_height, 80),
    DEFINE_PROP_UINT32("panel-origin-x", HM17State, panel_origin_x, 0),
    DEFINE_PROP_UINT32("panel-origin-y", HM17State, panel_origin_y, 0),
    DEFINE_PROP_BOOL("panel-mirror-x", HM17State, panel_mirror_x, false),
    DEFINE_PROP_BOOL("panel-mirror-y", HM17State, panel_mirror_y, false),
    DEFINE_PROP_BOOL("reset-idle-level", HM17State, reset_idle_level, true),
    DEFINE_PROP_UINT32("scale", HM17State, scale, 1),
};

static void hm17_get_presentation(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    uint64_t digest = hm17_digest(HM17(obj));

    visit_type_uint64(v, name, &digest, errp);
}

static bool hm17_get_reset(Object *obj, Error **errp);

static void hm17_init(Object *obj)
{
    HM17State *s = HM17(obj);
    x55_lcd_sweep_init(&s->sweep, hm17_class(s)->model_name);
    object_property_add_uint64_ptr(obj, "frame-count", &s->frame_count,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "display-update-count",
                                   &s->display_updates, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "frame-digest", &s->frame_digest,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "transaction-count",
                                   &s->sweep.transactions,
                                   OBJ_PROP_FLAG_READ);
    object_property_add(obj, "memory-digest", "uint64",
                        hm17_get_memory_digest, NULL, NULL, NULL);
    object_property_add(obj, "presentation-digest", "uint64",
                        hm17_get_presentation, NULL, NULL, NULL);
    object_property_add_bool(obj, "reset-asserted", hm17_get_reset, NULL);
}

static bool hm17_get_reset(Object *obj, Error **errp)
{
    return HM17(obj)->reset_asserted;
}

static int hm17_base_post_load(void *opaque, int version_id)
{
    HM17State *s = opaque;

    if (s->pack_count > 2 || s->stream_bits > 8 || s->frame_bits > 16) {
        return -EINVAL;
    }
    /* Redraw the restored presentation, without publishing a partial sweep. */
    s->redraw = true;
    s->force_update = true;
    return 0;
}

static const VMStateDescription vmstate_hm17_base = {
    .name = "x55/hm17",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = hm17_base_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, HM17State),
        VMSTATE_STRUCT(sweep, HM17State, 0, vmstate_x55_lcd_sweep,
                       X55LCDSweep),
        VMSTATE_UINT16_ARRAY(gram, HM17State, HM17_MAX_WIDTH * HM17_MAX_HEIGHT),
        VMSTATE_UINT16_ARRAY(presented_gram, HM17State,
                             HM17_MAX_WIDTH * HM17_MAX_HEIGHT),
        VMSTATE_UINT8_2DARRAY(palette, HM17State, 3, 16),
        VMSTATE_BUFFER(pack, HM17State),
        VMSTATE_UINT8(x, HM17State),
        VMSTATE_UINT8(y, HM17State),
        VMSTATE_UINT8(window_start_x, HM17State),
        VMSTATE_UINT8(window_start_y, HM17State),
        VMSTATE_UINT8(end_x, HM17State),
        VMSTATE_UINT8(end_y, HM17State),
        VMSTATE_UINT8(start_line, HM17State),
        VMSTATE_UINT8(n_line, HM17State),
        VMSTATE_UINT8(line_start, HM17State),
        VMSTATE_UINT8(line_end, HM17State),
        VMSTATE_UINT8(re, HM17State),
        VMSTATE_UINT8(scan_start, HM17State),
        VMSTATE_UINT8(duty, HM17State),
        VMSTATE_UINT8(boost, HM17State),
        VMSTATE_UINT8(bias, HM17State),
        VMSTATE_UINT8(electric_volume, HM17State),
        VMSTATE_UINT8(extension_cs, HM17State),
        VMSTATE_UINT8(oscillator_rf, HM17State),
        VMSTATE_UINT8(oscillator_ffl, HM17State),
        VMSTATE_UINT8(dummy_y, HM17State),
        VMSTATE_UINT8(pwm_control, HM17State),
        VMSTATE_UINT8(pack_count, HM17State),
        VMSTATE_UINT8(frame_bits, HM17State),
        VMSTATE_UINT8(stream_shift, HM17State),
        VMSTATE_UINT8(stream_bits, HM17State),
        VMSTATE_BOOL(display_on, HM17State),
        VMSTATE_BOOL(all_on, HM17State),
        VMSTATE_BOOL(reverse, HM17State),
        VMSTATE_BOOL(common_shift, HM17State),
        VMSTATE_BOOL(n_line_enable, HM17State),
        VMSTATE_BOOL(swap, HM17State),
        VMSTATE_BOOL(reflect, HM17State),
        VMSTATE_BOOL(window, HM17State),
        VMSTATE_BOOL(aim, HM17State),
        VMSTATE_BOOL(increment_x, HM17State),
        VMSTATE_BOOL(increment_y, HM17State),
        VMSTATE_BOOL(amp_on, HM17State),
        VMSTATE_BOOL(halt, HM17State),
        VMSTATE_BOOL(dc_on, HM17State),
        VMSTATE_BOOL(discharge, HM17State),
        VMSTATE_BOOL(monochrome, HM17State),
        VMSTATE_BOOL(fixed_pwm, HM17State),
        VMSTATE_BOOL(clock_select, HM17State),
        VMSTATE_BOOL(word_length_16, HM17State),
        VMSTATE_BOOL(color_256, HM17State),
        VMSTATE_BOOL(high_speed_write, HM17State),
        VMSTATE_BOOL(absolute_12bit, HM17State),
        VMSTATE_BOOL(palette_upper, HM17State),
        VMSTATE_BOOL(line_reverse, HM17State),
        VMSTATE_BOOL(blink_type, HM17State),
        VMSTATE_BOOL(glsb, HM17State),
        VMSTATE_BOOL(dc, HM17State),
        VMSTATE_BOOL(frame_msb_first, HM17State),
        VMSTATE_BOOL(frame_pending, HM17State),
        VMSTATE_BOOL(frame_is_data, HM17State),
        VMSTATE_BOOL(reset_level, HM17State),
        VMSTATE_BOOL(reset_driven, HM17State),
        VMSTATE_BOOL(reset_asserted, HM17State),
        VMSTATE_UINT64(frame_count, HM17State),
        VMSTATE_UINT64(frame_digest, HM17State),
        VMSTATE_END_OF_LIST()
    },
};

static void hm17_base_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSIPeripheralClass *spc = SSI_PERIPHERAL_CLASS(oc);

    dc->vmsd = &vmstate_hm17_base;
    spc->realize = hm17_realize;
    spc->transfer = hm17_transfer;
    spc->set_cs = hm17_set_cs;
    spc->cs_polarity = SSI_CS_LOW;
    device_class_set_props(dc, hm17_properties);
    device_class_set_legacy_reset(dc, hm17_system_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static void hm17_4096_class_init(ObjectClass *oc, const void *data)
{
    HM17Class *hc = HM17_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);
    hc->model = HM17_MODEL_4096; hc->model_name = TYPE_HM17CM4096;
    hc->width = 128; hc->height = hc->graphic_height = 162;
    dc->desc = "Hynix HM17CM4096 RGB444 LCD controller";
}

static void hm17_256_class_init(ObjectClass *oc, const void *data)
{
    HM17Class *hc = HM17_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);
    hc->model = HM17_MODEL_256; hc->model_name = TYPE_HM17CM256;
    hc->width = 128; hc->height = 82; hc->graphic_height = 80;
    dc->desc = "Hynix HM17CM256 RGB332 LCD controller";
}

static const TypeInfo hm17_types[] = {
    {
        .name = TYPE_HM17_BASE,
        .parent = TYPE_SSI_PERIPHERAL,
        .instance_size = sizeof(HM17State),
        .instance_align = __alignof__(HM17State),
        .class_size = sizeof(HM17Class),
        .instance_init = hm17_init,
        .class_init = hm17_base_class_init,
        .abstract = true,
    }, {
        .name = TYPE_HM17CM4096,
        .parent = TYPE_HM17_BASE,
        .class_init = hm17_4096_class_init,
    }, {
        .name = TYPE_HM17CM256,
        .parent = TYPE_HM17_BASE,
        .class_init = hm17_256_class_init,
    },
};

static void hm17_register_types(void)
{
    type_register_static_array(hm17_types, ARRAY_SIZE(hm17_types));
}
type_init(hm17_register_types)
