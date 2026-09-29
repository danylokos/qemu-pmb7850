/*
 * Philips PCF8813 102 x 68 monochrome LCD controller.
 *
 * The SSI payload API does not describe the master's word width.  The
 * optional frame-start GPIO supplies that controller configuration while the
 * standard SSI bus continues to carry the full-duplex word.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/display/pcf8813.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "x55_lcd_sweep.h"
#include "trace/trace-hw_display.h"

#define PCF_WIDTH 102
#define PCF_HEIGHT 68
#define PCF_BANKS 10
#define PCF_DDRAM_SIZE (PCF_WIDTH * PCF_BANKS)

#define PCF_PENDING_NONE           0
#define PCF_PENDING_DISPLAY_CONFIG 1
#define PCF_PENDING_DATA_LENGTH    2
#define PCF_PENDING_MAX_Y          3
#define PCF_PENDING_MAX_X          4
#define PCF_PENDING_START_ROW      5
#define PCF_PENDING_PARTIAL_MODE   6
#define PCF_PENDING_RAM_LINE       7
#define PCF_PENDING_RESERVED       8
#define PCF_PENDING_PARTIAL_MASK   9

enum {
    PCF_DISPLAY_BLANK,
    PCF_DISPLAY_ALL_ON,
    PCF_DISPLAY_NORMAL,
    PCF_DISPLAY_INVERSE,
};

struct PCF8813State {
    SSIPeripheral parent_obj;
    QemuConsole *console;
    qemu_irq serial_data_out;

    uint8_t ddram[PCF_DDRAM_SIZE];
    uint8_t presented_ddram[PCF_DDRAM_SIZE];
    uint8_t x, y, x_max, y_max;
    uint8_t power_down, vertical, extended, mirror_x, mirror_y;
    uint8_t display_mode;
    uint8_t prs, power_control, temperature, hv_stages, bias, vop;
    uint8_t data_order, bottom_row_swap, normal_mode, partial_mask;
    uint8_t initial_row, ram_start_line, pending_command;

    bool dc;
    bool serial_clock;
    bool serial_data;
    bool serial_data_output;
    bool status_requested;
    bool status_active;
    int8_t status_bit;
    uint8_t status_value;

    uint8_t stream_shift;
    uint8_t stream_bits;
    uint8_t frame_bits;
    bool frame_msb_first;
    bool frame_pending;
    bool frame_is_data;

    X55LCDSweep sweep;
    uint64_t frame_count;
    uint64_t frame_digest;
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
};

static uint64_t pcf_present(void *opaque, const X55LCDSweep *sweep);

static uint8_t pcf_reverse8(uint8_t value)
{
    return revbit8(value);
}

static bool pcf_valid_y(unsigned y)
{
    return y <= 8 || y == 10;
}

static unsigned pcf_bank(unsigned y)
{
    return y == 10 ? 9 : y;
}

static void pcf_reset_registers(PCF8813State *s)
{
    s->x = 0;
    s->y = 0;
    s->x_max = 101;
    s->y_max = 8;
    s->power_down = 1;
    s->vertical = 0;
    s->extended = 0;
    s->mirror_x = 0;
    s->mirror_y = 0;
    s->display_mode = PCF_DISPLAY_BLANK;
    s->prs = 0;
    s->power_control = 1;
    s->temperature = 0;
    s->hv_stages = 0;
    s->bias = 0;
    s->vop = 0;
    s->data_order = 0;
    s->bottom_row_swap = 0;
    s->normal_mode = 1;
    s->partial_mask = 0xff;
    s->initial_row = 0;
    s->ram_start_line = 0;
    s->pending_command = PCF_PENDING_NONE;
    s->status_requested = false;
    s->status_active = false;
    s->status_bit = -1;
    s->status_value = 0;
    s->stream_shift = 0;
    s->stream_bits = 0;
    s->frame_pending = false;
    x55_lcd_sweep_discard(&s->sweep);
    qemu_set_irq(s->serial_data_out, 1);
}

static void pcf_increment_address(PCF8813State *s)
{
    if (s->y == 10) {
        if (++s->x > s->x_max) {
            s->x = 0;
        }
    } else if (s->vertical) {
        if (++s->y > s->y_max) {
            s->y = 0;
            if (++s->x > s->x_max) {
                s->x = 0;
            }
        }
    } else if (++s->x > s->x_max) {
        s->x = 0;
        if (++s->y > s->y_max) {
            s->y = 0;
        }
    }
}

static void pcf_parameter(PCF8813State *s, uint8_t value)
{
    uint8_t pending = s->pending_command;

    s->pending_command = PCF_PENDING_NONE;
    switch (pending) {
    case PCF_PENDING_DISPLAY_CONFIG:
        if (!(value & 0xfc)) {
            s->data_order = extract32(value, 1, 1);
            s->bottom_row_swap = value & 1;
        }
        break;
    case PCF_PENDING_DATA_LENGTH:
    case PCF_PENDING_RESERVED:
        break;
    case PCF_PENDING_MAX_Y:
        if (value <= 8) {
            s->y_max = value;
            if (s->y != 10 && s->y > value) {
                s->y = 0;
            }
        }
        break;
    case PCF_PENDING_MAX_X:
        if (value <= 101) {
            s->x_max = value;
            if (s->x > value) {
                s->x = 0;
            }
        }
        break;
    case PCF_PENDING_START_ROW:
        if (value <= 66) {
            s->initial_row = value;
        }
        break;
    case PCF_PENDING_PARTIAL_MODE:
        if (value <= 1) {
            s->normal_mode = value;
        }
        break;
    case PCF_PENDING_RAM_LINE:
        if (value <= 66) {
            s->ram_start_line = value;
        }
        break;
    case PCF_PENDING_PARTIAL_MASK:
        s->partial_mask = value;
        break;
    default:
        break;
    }
    trace_pcf8813_command(value, s->extended, s->x, s->y, "parameter");
}

static void pcf_command(PCF8813State *s, uint8_t value)
{
    const char *name = "unknown";

    if (s->pending_command != PCF_PENDING_NONE) {
        pcf_parameter(s, value);
        return;
    }
    if (value == 0x00) {
        name = "nop";
    } else if ((value & 0xfc) == 0x18) {
        s->status_requested = true;
        name = "read-status";
    } else if ((value & 0xe0) == 0x20) {
        s->mirror_x = extract32(value, 4, 1);
        s->mirror_y = extract32(value, 3, 1);
        s->power_down = extract32(value, 2, 1);
        s->vertical = extract32(value, 1, 1);
        s->extended = value & 1;
        name = "function-set";
    } else if (!s->extended) {
        if (value == 0x08 || value == 0x09 ||
            value == 0x0c || value == 0x0d) {
            s->display_mode = (extract32(value, 2, 1) << 1) | (value & 1);
            name = "display-control";
        } else if (value == 0x10 || value == 0x11) {
            s->prs = value & 1;
            name = "vop-range";
        } else if (value == 0x12 || value == 0x13) {
            s->power_control = value & 1;
            name = "power-control";
        } else if ((value & 0xfe) == 0x16) {
            s->pending_command = PCF_PENDING_DISPLAY_CONFIG;
            name = "display-configuration";
        } else if ((value & 0xf0) == 0x40 && pcf_valid_y(value & 0xf)) {
            s->y = value & 0xf;
            name = "set-y";
        } else if ((value & 0xf8) == 0x50) {
            s->pending_command = PCF_PENDING_MAX_Y;
            name = "set-maximum-y";
        } else if ((value & 0xf8) == 0x60) {
            s->pending_command = PCF_PENDING_MAX_X;
            name = "set-maximum-x";
        } else if ((value & 0xf0) == 0x70) {
            s->pending_command = PCF_PENDING_DATA_LENGTH;
            name = "set-display-data-length";
        } else if (value >= 0x80 && value <= 0xe5) {
            s->x = value & 0x7f;
            name = "set-x";
        }
    } else if ((value & 0xfc) == 0x04) {
        s->temperature = value & 3;
        name = "temperature";
    } else if ((value & 0xfc) == 0x08) {
        s->hv_stages = value & 3;
        name = "hv-stages";
    } else if ((value & 0xf8) == 0x10) {
        s->bias = value & 7;
        name = "bias";
    } else if (value == 0x1c) {
        name = "disable-otp";
    } else if (value == 0x1e) {
        name = "module-calibration";
    } else if ((value & 0xf8) == 0x48) {
        s->pending_command = PCF_PENDING_START_ROW;
        name = "set-initial-row";
    } else if ((value & 0xf8) == 0x50) {
        s->pending_command = PCF_PENDING_PARTIAL_MODE;
        name = "set-partial-mode";
    } else if ((value & 0xf8) == 0x58) {
        s->pending_command = PCF_PENDING_RAM_LINE;
        name = "set-ram-line";
    } else if ((value & 0xf8) == 0x60) {
        s->pending_command = PCF_PENDING_RESERVED;
        name = "reserved-double-command";
    } else if ((value & 0xf8) == 0x68) {
        s->pending_command = PCF_PENDING_PARTIAL_MASK;
        name = "set-partial-mask";
    } else if (value == 0x71) {
        pcf_reset_registers(s);
        name = "software-reset";
    } else if (value & 0x80) {
        s->vop = value & 0x7f;
        name = "vop";
    }
    trace_pcf8813_command(value, s->extended, s->x, s->y, name);
}

static void pcf_byte(PCF8813State *s, bool is_data, uint8_t value)
{
    if (!is_data) {
        bool function = s->pending_command == PCF_PENDING_NONE &&
                        (value & 0xe0) == 0x20;
        bool conflict =
            (s->pending_command == PCF_PENDING_DISPLAY_CONFIG &&
             !(value & 0xfc) && ((value >> 1) & 1) != s->data_order) ||
            (s->pending_command == PCF_PENDING_MAX_X &&
             value <= 101 && value != s->x_max) ||
            (s->pending_command == PCF_PENDING_MAX_Y &&
             value <= 8 && value != s->y_max) ||
            (function && ((value >> 1) & 1) != s->vertical);
        bool redraw = function ||
            s->pending_command == PCF_PENDING_DISPLAY_CONFIG ||
            s->pending_command == PCF_PENDING_START_ROW ||
            s->pending_command == PCF_PENDING_PARTIAL_MODE ||
            s->pending_command == PCF_PENDING_RAM_LINE ||
            s->pending_command == PCF_PENDING_PARTIAL_MASK ||
            (s->pending_command == PCF_PENDING_NONE &&
             ((!s->extended && (value == 0x08 || value == 0x09 ||
                                value == 0x0c || value == 0x0d ||
                                value == 0x12 || value == 0x13)) ||
              (s->extended && value == 0x71)));

        if (conflict) {
            x55_lcd_sweep_flush(&s->sweep, pcf_present, s, "fallback");
        }
        pcf_command(s, value);
        if (redraw && s->console) {
            s->redraw = true;
            qemu_console_hw_update(s->console);
        }
        return;
    }
    unsigned x = s->x, y = s->y;
    unsigned last_bank = (s->panel_origin_y + s->panel_height - 1) / 8;
    X55LCDSweepShape shape = {
        .mode = s->vertical | (s->data_order << 1) |
                ((uint64_t)s->x_max << 8) | ((uint64_t)s->y_max << 16),
        .x1 = s->x_max, .y1 = last_bank,
    };

    x55_lcd_sweep_begin(&s->sweep, &shape, x, y, 0, pcf_present, s);
    if (s->x <= 101 && pcf_valid_y(s->y)) {
        unsigned offset = pcf_bank(s->y) * PCF_WIDTH + s->x;
        uint8_t stored = s->data_order ? pcf_reverse8(value) : value;

        if (s->y == 8) {
            stored &= 0x07;
        } else if (s->y == 10) {
            stored &= 0x01;
        }
        s->ddram[offset] = stored;
        trace_pcf8813_data(stored, s->x, s->y);
    }
    pcf_increment_address(s);
    x55_lcd_sweep_end(&s->sweep, s->x, s->y, 0, 1,
                      x <= 101 && pcf_valid_y(y),
                      x == s->x_max && y == last_bank, pcf_present, s);
}

static void pcf_consume(PCF8813State *s, uint16_t value, unsigned bits,
                        bool msb_first, bool is_data)
{
    for (unsigned i = 0; i < bits; i++) {
        unsigned bit = msb_first ? bits - 1 - i : i;

        s->stream_shift = (s->stream_shift << 1) | extract32(value, bit, 1);
        if (++s->stream_bits == 8) {
            pcf_byte(s, is_data, s->stream_shift);
            s->stream_shift = 0;
            s->stream_bits = 0;
        }
    }
}

static int pcf_ram_pixel(PCF8813State *s, unsigned x, unsigned row)
{
    if (row == 67) {
        return s->presented_ddram[9 * PCF_WIDTH + x] & 1;
    }
    return extract32(s->presented_ddram[(row >> 3) * PCF_WIDTH + x],
                     row & 7, 1);
}

static bool pcf_visible_pixel(PCF8813State *s, unsigned x, unsigned y)
{
    unsigned panel_x = x + s->panel_origin_x;
    unsigned panel_y = y + s->panel_origin_y;
    unsigned controller_x = s->panel_mirror_x ? 101 - panel_x : panel_x;
    unsigned controller_y = s->panel_mirror_y ? 67 - panel_y : panel_y;
    unsigned source_x = s->mirror_x ? 101 - controller_x : controller_x;
    unsigned logical_y = s->mirror_y ? 67 - controller_y : controller_y;
    unsigned source_y;
    bool pixel;

    if (source_x >= PCF_WIDTH || logical_y >= PCF_HEIGHT) {
        return false;
    }
    if (logical_y == 67) {
        source_y = 67;
    } else {
        int mapped;

        if (!s->normal_mode &&
            !(s->partial_mask & (1u << (logical_y >> 3)))) {
            return false;
        }
        mapped = (int)logical_y - s->initial_row + s->ram_start_line;
        while (mapped < 0) {
            mapped += 67;
        }
        source_y = mapped % 67;
    }
    pixel = pcf_ram_pixel(s, source_x, source_y);
    if (s->power_down || !s->power_control ||
        s->display_mode == PCF_DISPLAY_BLANK) {
        return false;
    }
    if (s->display_mode == PCF_DISPLAY_ALL_ON) {
        return true;
    }
    return s->display_mode == PCF_DISPLAY_INVERSE ? !pixel : pixel;
}

static uint64_t pcf_digest(PCF8813State *s)
{
    static const uint8_t off[3] = { 202, 214, 184 };
    static const uint8_t on[3] = { 30, 39, 32 };
    uint64_t hash = UINT64_C(14695981039346656037);

    for (unsigned y = 0; y < s->panel_height; y++) {
        for (unsigned x = 0; x < s->panel_width; x++) {
            const uint8_t *rgb = pcf_visible_pixel(s, x, y) ? on : off;

            for (unsigned c = 0; c < 3; c++) {
                hash ^= rgb[c];
                hash *= UINT64_C(1099511628211);
            }
        }
    }
    return hash;
}

bool pcf8813_render_rgb888(DeviceState *dev, uint8_t *frame,
                           size_t frame_size, unsigned *width,
                           unsigned *height)
{
    static const uint8_t off[3] = { 202, 214, 184 };
    static const uint8_t on[3] = { 30, 39, 32 };
    PCF8813State *s;
    size_t required;

    if (!dev || !object_dynamic_cast(OBJECT(dev), TYPE_PCF8813)) {
        return false;
    }
    s = PCF8813(dev);
    required = (size_t)s->panel_width * s->panel_height * 3;
    if (!frame || frame_size != required) {
        return false;
    }
    for (unsigned y = 0; y < s->panel_height; y++) {
        for (unsigned x = 0; x < s->panel_width; x++) {
            const uint8_t *rgb = pcf_visible_pixel(s, x, y) ? on : off;
            size_t offset = ((size_t)y * s->panel_width + x) * 3;

            memcpy(frame + offset, rgb, 3);
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

static uint64_t pcf_present(void *opaque, const X55LCDSweep *sweep)
{
    PCF8813State *s = opaque;

    memcpy(s->presented_ddram, s->ddram, sizeof(s->presented_ddram));
    s->frame_count = sweep->frames;
    s->frame_digest = pcf_digest(s);
    s->force_update = true;
    s->redraw = true;
    qemu_console_hw_update(s->console);
    return s->frame_digest;
}

static uint32_t pcf_transfer(SSIPeripheral *dev, uint32_t value)
{
    PCF8813State *s = PCF8813(dev);
    unsigned bits = s->frame_pending ? s->frame_bits : 8;
    bool msb_first = s->frame_pending ? s->frame_msb_first : true;
    uint32_t mask = bits == 16 ? 0xffff : (1u << bits) - 1;

    if (bits >= 2 && bits <= 16) {
        pcf_consume(s, value & mask, bits, msb_first,
                    s->frame_pending ? s->frame_is_data : s->dc);
    }
    s->frame_pending = false;
    return mask;
}

static int pcf_set_cs(SSIPeripheral *dev, bool level)
{
    PCF8813State *s = PCF8813(dev);

    if (level) {
        x55_lcd_sweep_close(&s->sweep);
        s->stream_shift = 0;
        s->stream_bits = 0;
        s->frame_pending = false;
        s->status_requested = false;
        s->status_active = false;
        qemu_set_irq(s->serial_data_out, 1);
    }
    trace_pcf8813_select(!level);
    return 0;
}

static void pcf_dc(void *opaque, int n, int level)
{
    PCF8813State *s = opaque;
    s->dc = !!level;
}

static void pcf_frame_start(void *opaque, int n, int level)
{
    PCF8813State *s = opaque;
    unsigned bits = level & 0x1f;

    s->frame_bits = bits;
    s->frame_msb_first = !!(level & 0x100);
    s->frame_is_data = s->dc;
    s->frame_pending = bits >= 2 && bits <= 16;
}

static void pcf_serial_data(void *opaque, int n, int level)
{
    PCF8813State *s = opaque;
    s->serial_data = !!level;
}

static void pcf_drive_status(PCF8813State *s)
{
    if (s->status_active && s->status_bit >= 0) {
        qemu_set_irq(s->serial_data_out,
                     extract32(s->status_value, s->status_bit, 1));
    } else {
        qemu_set_irq(s->serial_data_out, 1);
    }
}

static void pcf_serial_direction(void *opaque, int n, int level)
{
    PCF8813State *s = opaque;
    bool output = !!level;

    if (s->serial_data_output == output) {
        return;
    }
    s->serial_data_output = output;
    if (!output && s->status_requested) {
        s->status_requested = false;
        s->status_active = true;
        s->status_bit = 7;
        s->status_value = !s->power_down && s->power_control ? 0x40 : 0;
        pcf_drive_status(s);
        trace_pcf8813_status(s->status_value);
    } else if (output) {
        s->status_active = false;
        s->status_bit = -1;
        pcf_drive_status(s);
    }
}

static void pcf_serial_clock(void *opaque, int n, int level)
{
    PCF8813State *s = opaque;
    bool clock = !!level;
    bool rising = clock && !s->serial_clock;

    if (clock == s->serial_clock) {
        return;
    }
    s->serial_clock = clock;
    if (!SSI_PERIPHERAL(s)->cs) {
        if (rising && s->serial_data_output) {
            s->stream_shift = (s->stream_shift << 1) | s->serial_data;
            if (++s->stream_bits == 8) {
                pcf_byte(s, s->dc, s->stream_shift);
                s->stream_shift = 0;
                s->stream_bits = 0;
            }
        } else if (!rising && !s->serial_data_output && s->status_active) {
            if (--s->status_bit < 0) {
                s->status_active = false;
            }
            pcf_drive_status(s);
        }
    }
}

static bool pcf_update_display(void *opaque)
{
    PCF8813State *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->console);
    uint8_t *base = surface_data(surface);
    int stride = surface_stride(surface);
    bool changed = s->force_update;

    if (!s->redraw || surface_bits_per_pixel(surface) != 32) {
        return true;
    }
    for (unsigned y = 0; y < s->panel_height; y++) {
        for (unsigned sy = 0; sy < s->scale; sy++) {
            uint32_t *line = (uint32_t *)(base +
                (y * s->scale + sy) * stride);

            for (unsigned x = 0; x < s->panel_width; x++) {
                bool on = pcf_visible_pixel(s, x, y);
                uint32_t pixel = on ? rgb_to_pixel32(30, 39, 32) :
                                      rgb_to_pixel32(202, 214, 184);

                for (unsigned sx = 0; sx < s->scale; sx++) {
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
        qemu_console_update(s->console, 0, 0,
                            s->panel_width * s->scale,
                            s->panel_height * s->scale);
    }
    return true;
}

static void pcf_invalidate_display(void *opaque)
{
    PCF8813State *s = opaque;
    s->redraw = true;
    s->force_update = true;
}

static const GraphicHwOps pcf_display_ops = {
    .invalidate = pcf_invalidate_display,
    .gfx_update = pcf_update_display,
};

static void pcf_realize(SSIPeripheral *dev, Error **errp)
{
    PCF8813State *s = PCF8813(dev);
    DeviceState *qdev = DEVICE(dev);

    if (!s->panel_width || s->panel_width > PCF_WIDTH ||
        !s->panel_height || s->panel_height > PCF_HEIGHT ||
        s->panel_origin_x + s->panel_width > PCF_WIDTH ||
        s->panel_origin_y + s->panel_height > PCF_HEIGHT ||
        !s->scale || s->scale > 16) {
        error_setg(errp, "invalid PCF8813 panel geometry or scale");
        return;
    }
    qdev_init_gpio_in_named(qdev, pcf_dc, "dc", 1);
    qdev_init_gpio_in_named(qdev, pcf_serial_clock, "serial-clock", 1);
    qdev_init_gpio_in_named(qdev, pcf_serial_data, "serial-data", 1);
    qdev_init_gpio_in_named(qdev, pcf_serial_direction,
                            "serial-data-direction", 1);
    qdev_init_gpio_in_named(qdev, pcf_frame_start, "frame-start", 1);
    qdev_init_gpio_out_named(qdev, &s->serial_data_out,
                             "serial-data-out", 1);
    s->console = qemu_graphic_console_create(qdev, 0, &pcf_display_ops, s);
    qemu_console_resize(s->console, s->panel_width * s->scale,
                        s->panel_height * s->scale);
    pcf_reset_registers(s);
}

static void pcf_system_reset(DeviceState *dev)
{
    pcf_reset_registers(PCF8813(dev));
}

static uint64_t pcf_memory_digest(PCF8813State *s)
{
    uint64_t hash = UINT64_C(14695981039346656037);

    for (unsigned i = 0; i < sizeof(s->ddram); i++) {
        hash ^= s->ddram[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void pcf_get_memory_digest(Object *obj, Visitor *v,
                                  const char *name, void *opaque,
                                  Error **errp)
{
    uint64_t value = pcf_memory_digest(PCF8813(obj));

    visit_type_uint64(v, name, &value, errp);
}

static void pcf_get_presentation(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    uint64_t digest = pcf_digest(PCF8813(obj));

    visit_type_uint64(v, name, &digest, errp);
}

static const Property pcf_properties[] = {
    DEFINE_PROP_UINT32("panel-width", PCF8813State, panel_width, PCF_WIDTH),
    DEFINE_PROP_UINT32("panel-height", PCF8813State, panel_height, PCF_HEIGHT),
    DEFINE_PROP_UINT32("panel-origin-x", PCF8813State, panel_origin_x, 0),
    DEFINE_PROP_UINT32("panel-origin-y", PCF8813State, panel_origin_y, 0),
    DEFINE_PROP_BOOL("panel-mirror-x", PCF8813State, panel_mirror_x, false),
    DEFINE_PROP_BOOL("panel-mirror-y", PCF8813State, panel_mirror_y, false),
    DEFINE_PROP_UINT32("scale", PCF8813State, scale, 1),
};

static void pcf_init(Object *obj)
{
    PCF8813State *s = PCF8813(obj);

    x55_lcd_sweep_init(&s->sweep, "pcf8813");
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
                        pcf_get_memory_digest, NULL, NULL, NULL);
    object_property_add(obj, "presentation-digest", "uint64",
                        pcf_get_presentation, NULL, NULL, NULL);
}

static int pcf_post_load(void *opaque, int version_id)
{
    PCF8813State *s = opaque;

    if (s->stream_bits > 8 || s->frame_bits > 16) {
        return -EINVAL;
    }
    /* Redraw the restored presentation, without publishing a partial sweep. */
    s->redraw = true;
    s->force_update = true;
    return 0;
}

static const VMStateDescription vmstate_pcf = {
    .name = "x55/pcf8813",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pcf_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, PCF8813State),
        VMSTATE_STRUCT(sweep, PCF8813State, 0, vmstate_x55_lcd_sweep,
                       X55LCDSweep),
        VMSTATE_BUFFER(ddram, PCF8813State),
        VMSTATE_BUFFER(presented_ddram, PCF8813State),
        VMSTATE_UINT8(x, PCF8813State),
        VMSTATE_UINT8(y, PCF8813State),
        VMSTATE_UINT8(x_max, PCF8813State),
        VMSTATE_UINT8(y_max, PCF8813State),
        VMSTATE_UINT8(power_down, PCF8813State),
        VMSTATE_UINT8(vertical, PCF8813State),
        VMSTATE_UINT8(extended, PCF8813State),
        VMSTATE_UINT8(mirror_x, PCF8813State),
        VMSTATE_UINT8(mirror_y, PCF8813State),
        VMSTATE_UINT8(display_mode, PCF8813State),
        VMSTATE_UINT8(prs, PCF8813State),
        VMSTATE_UINT8(power_control, PCF8813State),
        VMSTATE_UINT8(temperature, PCF8813State),
        VMSTATE_UINT8(hv_stages, PCF8813State),
        VMSTATE_UINT8(bias, PCF8813State),
        VMSTATE_UINT8(vop, PCF8813State),
        VMSTATE_UINT8(data_order, PCF8813State),
        VMSTATE_UINT8(bottom_row_swap, PCF8813State),
        VMSTATE_UINT8(normal_mode, PCF8813State),
        VMSTATE_UINT8(partial_mask, PCF8813State),
        VMSTATE_UINT8(initial_row, PCF8813State),
        VMSTATE_UINT8(ram_start_line, PCF8813State),
        VMSTATE_UINT8(pending_command, PCF8813State),
        VMSTATE_UINT8(status_value, PCF8813State),
        VMSTATE_UINT8(stream_shift, PCF8813State),
        VMSTATE_UINT8(stream_bits, PCF8813State),
        VMSTATE_UINT8(frame_bits, PCF8813State),
        VMSTATE_INT8(status_bit, PCF8813State),
        VMSTATE_BOOL(dc, PCF8813State),
        VMSTATE_BOOL(serial_clock, PCF8813State),
        VMSTATE_BOOL(serial_data, PCF8813State),
        VMSTATE_BOOL(serial_data_output, PCF8813State),
        VMSTATE_BOOL(status_requested, PCF8813State),
        VMSTATE_BOOL(status_active, PCF8813State),
        VMSTATE_BOOL(frame_msb_first, PCF8813State),
        VMSTATE_BOOL(frame_pending, PCF8813State),
        VMSTATE_BOOL(frame_is_data, PCF8813State),
        VMSTATE_UINT64(frame_count, PCF8813State),
        VMSTATE_UINT64(frame_digest, PCF8813State),
        VMSTATE_END_OF_LIST()
    },
};

static void pcf_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *spc = SSI_PERIPHERAL_CLASS(klass);

    dc->vmsd = &vmstate_pcf;
    spc->realize = pcf_realize;
    spc->transfer = pcf_transfer;
    spc->set_cs = pcf_set_cs;
    spc->cs_polarity = SSI_CS_LOW;
    device_class_set_props(dc, pcf_properties);
    device_class_set_legacy_reset(dc, pcf_system_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo pcf_info = {
    .name = TYPE_PCF8813,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(PCF8813State),
    .instance_align = __alignof__(PCF8813State),
    .instance_init = pcf_init,
    .class_init = pcf_class_init,
};

static void pcf_register_types(void)
{
    type_register_static(&pcf_info);
}

type_init(pcf_register_types)
