/*
 * Controller-neutral support for Siemens x55 color LCDs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "x55_lcd_common.h"
#include "ui/pixel_ops.h"
#include "trace/trace-hw_display.h"

static void x55_lcd_common_pixel(X55LCDCommon *common,
                                 unsigned x, unsigned y, uint8_t rgb[3])
{
    if (common->panel_mirror_x) {
        x = common->panel_width - 1 - x;
    }
    if (common->panel_mirror_y) {
        y = common->panel_height - 1 - y;
    }
    x += common->panel_origin_x;
    y += common->panel_origin_y;
    if (x >= common->controller_width || y >= common->controller_height) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        return;
    }
    common->ops->pixel_rgb888(common->opaque, x, y, rgb);
}

static uint64_t x55_lcd_common_digest(X55LCDCommon *common)
{
    uint64_t hash = UINT64_C(14695981039346656037);

    for (unsigned y = 0; y < common->panel_height; y++) {
        for (unsigned x = 0; x < common->panel_width; x++) {
            uint8_t rgb[3];

            x55_lcd_common_pixel(common, x, y, rgb);
            for (unsigned c = 0; c < 3; c++) {
                hash ^= rgb[c];
                hash *= UINT64_C(1099511628211);
            }
        }
    }
    return hash;
}

void x55_lcd_common_refresh(X55LCDCommon *common)
{
    common->redraw = true;
    if (common->console) {
        qemu_console_hw_update(common->console);
    }
}

uint64_t x55_lcd_common_present(void *opaque, const X55LCDSweep *sweep)
{
    X55LCDCommon *common = opaque;

    common->ops->present(common->opaque);
    common->frame_count = sweep->frames;
    common->frame_digest = x55_lcd_common_digest(common);
    common->force_update = true;
    x55_lcd_common_refresh(common);
    return common->frame_digest;
}

int x55_lcd_common_set_cs(X55LCDCommon *common, bool level)
{
    if (level) {
        x55_lcd_sweep_close(&common->sweep);
        common->frame_pending = false;
        if (common->ops->deselect) {
            common->ops->deselect(common->opaque);
        }
    }
    return 0;
}

static void x55_lcd_common_dc(void *opaque, int n, int level)
{
    X55LCDCommon *common = opaque;

    common->dc = !!level;
}

static void x55_lcd_common_frame_start(void *opaque, int n, int level)
{
    X55LCDCommon *common = opaque;
    unsigned bits = level & 0x1f;

    common->frame_bits = bits;
    common->frame_msb_first = !!(level & 0x100);
    common->frame_is_data = common->dc;
    common->frame_pending = bits >= 2 && bits <= 16;
}

void x55_lcd_common_reset(X55LCDCommon *common)
{
    common->frame_pending = false;
    x55_lcd_sweep_discard(&common->sweep);
    common->ops->reset(common->opaque);
    x55_lcd_common_refresh(common);
}

static void x55_lcd_common_resolve_reset(X55LCDCommon *common)
{
    bool level = common->reset_driven ? common->reset_level :
                                       common->reset_idle_level;
    bool asserted = !level;

    if (asserted != common->reset_asserted) {
        common->reset_asserted = asserted;
        if (asserted) {
            x55_lcd_common_reset(common);
        }
        trace_x55_color_lcd_reset(common->model_name, asserted,
                                  common->reset_driven, level);
        x55_lcd_common_refresh(common);
    }
}

static void x55_lcd_common_reset_level(void *opaque, int n, int level)
{
    X55LCDCommon *common = opaque;

    common->reset_level = !!level;
    x55_lcd_common_resolve_reset(common);
}

static void x55_lcd_common_reset_driven(void *opaque, int n, int level)
{
    X55LCDCommon *common = opaque;

    common->reset_driven = !!level;
    x55_lcd_common_resolve_reset(common);
}

static bool x55_lcd_common_update(void *opaque)
{
    X55LCDCommon *common = opaque;
    DisplaySurface *surface = qemu_console_surface(common->console);
    uint8_t *base = surface_data(surface);
    int stride = surface_stride(surface);
    bool changed = common->force_update;

    if (!common->redraw || surface_bits_per_pixel(surface) != 32) {
        return true;
    }
    for (unsigned y = 0; y < common->panel_height; y++) {
        for (unsigned sy = 0; sy < common->scale; sy++) {
            uint32_t *line = (uint32_t *)(base +
                (y * common->scale + sy) * stride);

            for (unsigned x = 0; x < common->panel_width; x++) {
                uint8_t rgb[3];

                x55_lcd_common_pixel(common, x, y, rgb);
                for (unsigned sx = 0; sx < common->scale; sx++) {
                    uint32_t pixel = rgb_to_pixel32(rgb[0], rgb[1], rgb[2]);

                    changed |= line[x * common->scale + sx] != pixel;
                    line[x * common->scale + sx] = pixel;
                }
            }
        }
    }
    common->redraw = false;
    common->force_update = false;
    if (changed) {
        common->display_updates++;
        qemu_console_update(common->console, 0, 0,
                            common->panel_width * common->scale,
                            common->panel_height * common->scale);
    }
    return true;
}

static void x55_lcd_common_invalidate(void *opaque)
{
    X55LCDCommon *common = opaque;

    common->redraw = true;
    common->force_update = true;
}

static const GraphicHwOps x55_lcd_common_ops = {
    .invalidate = x55_lcd_common_invalidate,
    .gfx_update = x55_lcd_common_update,
};

static void x55_lcd_common_get_presentation(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    uint64_t digest = x55_lcd_common_digest(opaque);

    visit_type_uint64(v, name, &digest, errp);
}

void x55_lcd_common_init(X55LCDCommon *common, Object *obj, void *opaque,
                         const X55LCDCommonOps *ops, const char *model_name,
                         bool (*get_reset)(Object *, Error **))
{
    common->opaque = opaque;
    common->ops = ops;
    common->model_name = model_name;
    x55_lcd_sweep_init(&common->sweep, model_name);
    object_property_add_uint64_ptr(obj, "frame-count", &common->frame_count,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "display-update-count",
                                   &common->display_updates,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "frame-digest", &common->frame_digest,
                                   OBJ_PROP_FLAG_READ);
    object_property_add(obj, "presentation-digest", "uint64",
                        x55_lcd_common_get_presentation, NULL, NULL, common);
    object_property_add_bool(obj, "reset-asserted", get_reset, NULL);
}

void x55_lcd_common_realize(X55LCDCommon *common, DeviceState *device,
                            void *opaque, const X55LCDCommonOps *ops,
                            const char *model_name,
                            unsigned controller_width,
                            unsigned controller_height,
                            unsigned default_panel_width,
                            unsigned default_panel_height,
                            unsigned default_panel_origin_x,
                            unsigned default_panel_origin_y,
                            Error **errp)
{
    assert(common->opaque == opaque);
    assert(common->ops == ops);
    assert(common->model_name == model_name);
    common->controller_width = controller_width;
    common->controller_height = controller_height;
    if (!common->panel_width) {
        common->panel_width = default_panel_width;
    }
    if (!common->panel_height) {
        common->panel_height = default_panel_height;
    }
    if (!common->panel_origin_x) {
        common->panel_origin_x = default_panel_origin_x;
    }
    if (!common->panel_origin_y) {
        common->panel_origin_y = default_panel_origin_y;
    }
    if (!common->panel_width || !common->panel_height ||
        common->panel_origin_x + common->panel_width > controller_width ||
        common->panel_origin_y + common->panel_height > controller_height ||
        !common->scale || common->scale > 16) {
        error_setg(errp, "invalid %s panel geometry or scale", model_name);
        return;
    }
    qdev_init_gpio_in_named_with_opaque(
        device, x55_lcd_common_dc, common, "dc", 1);
    qdev_init_gpio_in_named_with_opaque(
        device, x55_lcd_common_frame_start, common, "frame-start", 1);
    qdev_init_gpio_in_named_with_opaque(
        device, x55_lcd_common_reset_level, common, "reset-level", 1);
    qdev_init_gpio_in_named_with_opaque(
        device, x55_lcd_common_reset_driven, common, "reset-driven", 1);
    common->console = qemu_graphic_console_create(
        device, 0, &x55_lcd_common_ops, common);
    qemu_console_resize(common->console,
                        common->panel_width * common->scale,
                        common->panel_height * common->scale);
    x55_lcd_common_reset(common);
    x55_lcd_common_resolve_reset(common);
}

void x55_lcd_common_system_reset(X55LCDCommon *common)
{
    x55_lcd_common_reset(common);
    x55_lcd_common_resolve_reset(common);
}

X55LCDSSIFrame x55_lcd_common_frame(X55LCDCommon *common, uint32_t value)
{
    unsigned bits = common->frame_pending ? common->frame_bits : 8;
    bool msb = common->frame_pending ? common->frame_msb_first : true;
    X55LCDSSIFrame frame = {
        .bits = bits,
        .data = common->frame_pending ? common->frame_is_data : common->dc,
        .accepted = !common->reset_asserted && msb,
        .value = value,
        .mask = bits == 16 ? 0xffff : (1u << bits) - 1,
    };

    common->frame_pending = false;
    return frame;
}

const VMStateDescription vmstate_x55_lcd_common = {
    .name = "x55/lcd-common",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(sweep, X55LCDCommon, 0, vmstate_x55_lcd_sweep,
                       X55LCDSweep),
        VMSTATE_BOOL(dc, X55LCDCommon),
        VMSTATE_BOOL(frame_msb_first, X55LCDCommon),
        VMSTATE_BOOL(frame_pending, X55LCDCommon),
        VMSTATE_BOOL(frame_is_data, X55LCDCommon),
        VMSTATE_BOOL(reset_level, X55LCDCommon),
        VMSTATE_BOOL(reset_driven, X55LCDCommon),
        VMSTATE_BOOL(reset_asserted, X55LCDCommon),
        VMSTATE_UINT8(frame_bits, X55LCDCommon),
        VMSTATE_UINT64(frame_count, X55LCDCommon),
        VMSTATE_UINT64(frame_digest, X55LCDCommon),
        VMSTATE_END_OF_LIST()
    },
};
