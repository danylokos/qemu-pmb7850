/*
 * Siemens x55 battery and charger input.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "exec/icount.h"
#include "hw/c166/pmb7850/pmb7850.h"
#include "hw/c166/siemens_x55/battery.h"
#include "hw/core/irq.h"
#include "system/cpus.h"
#include "trace/trace-hw_c166.h"

struct SiemensX55BatteryState {
    DeviceState parent_obj;

    SiemensBatteryConfig config;
    qemu_irq adc_raw[PMB7850_ADC_INPUTS];
    qemu_irq charging_out;
    uint32_t level;
    bool charging;
    bool configured;
};

static uint16_t battery_millivolts(SiemensX55BatteryState *s)
{
    const SiemensBatteryPoint *curve = s->config.curve;

    if (s->level <= curve[0].level) {
        return curve[0].millivolts;
    }
    for (unsigned i = 1; i < s->config.curve_count; i++) {
        const SiemensBatteryPoint *a = &curve[i - 1];
        const SiemensBatteryPoint *b = &curve[i];

        if (s->level <= b->level) {
            unsigned span = b->level - a->level;
            unsigned offset = s->level - a->level;
            int delta = b->millivolts - a->millivolts;

            return a->millivolts +
                   (delta * (int)offset + (int)span / 2) / (int)span;
        }
    }
    return curve[s->config.curve_count - 1].millivolts;
}

static int16_t battery_adc_raw(SiemensX55BatteryState *s)
{
    long mv = battery_millivolts(s);
    long raw_span = s->config.adc_raw_low - s->config.adc_raw_high;
    long mv_span = s->config.adc_mv_high - s->config.adc_mv_low;
    long numerator = (s->config.adc_mv_high - mv) * raw_span;
    /* Stay inside the requested firmware voltage bucket after inversion. */
    long rounded = numerator >= 0 ? (numerator + mv_span - 1) / mv_span :
                                    (numerator - mv_span / 2) / mv_span;

    return CLAMP(s->config.adc_raw_high + rounded, INT16_MIN, INT16_MAX);
}

static void battery_publish(SiemensX55BatteryState *s)
{
    uint32_t pc = 0;

    if (!s->configured) {
        return;
    }
    qemu_set_irq(s->adc_raw[s->config.battery_channel], battery_adc_raw(s));
    qemu_set_irq(s->adc_raw[s->config.reference_channel], 0);
    qemu_set_irq(s->charging_out, s->charging);
    if (current_cpu && CPU_GET_CLASS(current_cpu)->get_pc) {
        pc = CPU_GET_CLASS(current_cpu)->get_pc(current_cpu);
    }
    trace_siemens_battery_state(icount_enabled() ? icount_get_raw() : 0,
                                pc, s->level, s->charging,
                                battery_millivolts(s), battery_adc_raw(s));
}

bool siemens_battery_configure(DeviceState *dev,
                               const SiemensBatteryConfig *config,
                               Error **errp)
{
    SiemensX55BatteryState *s = SIEMENS_X55_BATTERY(dev);

    if (dev->realized) {
        error_setg(errp,
                   "battery calibration must be configured before realize");
        return false;
    }
    if (!config || !config->curve || config->curve_count < 2 ||
        config->battery_channel >= PMB7850_ADC_INPUTS ||
        config->reference_channel >= PMB7850_ADC_INPUTS ||
        config->battery_channel == config->reference_channel ||
        config->adc_result_word > 1 ||
        config->adc_raw_low <= config->adc_raw_high ||
        config->adc_mv_high <= config->adc_mv_low) {
        error_setg(errp, "invalid battery calibration");
        return false;
    }
    for (unsigned i = 1; i < config->curve_count; i++) {
        if (config->curve[i].level <= config->curve[i - 1].level) {
            error_setg(errp, "battery curve levels must increase");
            return false;
        }
    }
    s->config = *config;
    s->configured = true;
    return true;
}

uint32_t siemens_battery_level(DeviceState *dev)
{
    return SIEMENS_X55_BATTERY(dev)->level;
}

bool siemens_battery_charging(DeviceState *dev)
{
    return SIEMENS_X55_BATTERY(dev)->charging;
}

bool siemens_battery_set_level(DeviceState *dev, uint32_t level,
                               Error **errp)
{
    SiemensX55BatteryState *s = SIEMENS_X55_BATTERY(dev);

    if (level > 100) {
        error_setg(errp, "battery-level must be in the range 0..100");
        return false;
    }
    s->level = level;
    battery_publish(s);
    return true;
}

void siemens_battery_set_charging(DeviceState *dev, bool charging)
{
    SiemensX55BatteryState *s = SIEMENS_X55_BATTERY(dev);

    s->charging = charging;
    battery_publish(s);
}

static void battery_get_level(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    SiemensX55BatteryState *s = SIEMENS_X55_BATTERY(obj);
    uint32_t level = s->level;

    visit_type_uint32(v, name, &level, errp);
}

static void battery_set_level(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    uint32_t level;

    if (visit_type_uint32(v, name, &level, errp)) {
        siemens_battery_set_level(DEVICE(obj), level, errp);
    }
}

static bool battery_get_charging(Object *obj, Error **errp)
{
    return SIEMENS_X55_BATTERY(obj)->charging;
}

static void battery_set_charging(Object *obj, bool charging, Error **errp)
{
    siemens_battery_set_charging(DEVICE(obj), charging);
}

static void battery_realize(DeviceState *dev, Error **errp)
{
    SiemensX55BatteryState *s = SIEMENS_X55_BATTERY(dev);

    if (!s->configured) {
        error_setg(errp, "battery calibration is required");
        return;
    }
    battery_publish(s);
}

static void battery_init(Object *obj)
{
    SiemensX55BatteryState *s = SIEMENS_X55_BATTERY(obj);
    DeviceState *dev = DEVICE(obj);

    s->level = 100;
    qdev_init_gpio_out_named(dev, s->adc_raw, "adc-raw",
                             PMB7850_ADC_INPUTS);
    qdev_init_gpio_out_named(dev, &s->charging_out, "charging", 1);
    object_property_add(obj, "level", "uint32", battery_get_level,
                        battery_set_level, NULL, NULL);
    object_property_add_bool(obj, "charging", battery_get_charging,
                             battery_set_charging);
}

static int battery_post_load(void *opaque, int version_id)
{
    SiemensX55BatteryState *s = opaque;

    return s->level <= 100 ? 0 : -EINVAL;
}

static const VMStateDescription vmstate_battery = {
    .name = "x55/battery",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = battery_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(level, SiemensX55BatteryState),
        VMSTATE_BOOL(charging, SiemensX55BatteryState),
        VMSTATE_END_OF_LIST()
    },
};

static void battery_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->vmsd = &vmstate_battery;
    dc->desc = "Siemens x55 calibrated battery input";
    dc->realize = battery_realize;
}

static const TypeInfo battery_type = {
    .name = TYPE_SIEMENS_X55_BATTERY,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SiemensX55BatteryState),
    .instance_align = __alignof__(SiemensX55BatteryState),
    .instance_init = battery_init,
    .class_init = battery_class_init,
};

static void battery_register_types(void)
{
    type_register_static(&battery_type);
}

type_init(battery_register_types)
