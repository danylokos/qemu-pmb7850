/*
 * Siemens x55 battery and charger input.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_C166_SIEMENS_BATTERY_H
#define HW_C166_SIEMENS_BATTERY_H

#include "hw/core/qdev.h"

#define TYPE_SIEMENS_X55_BATTERY "siemens-x55-battery"
OBJECT_DECLARE_SIMPLE_TYPE(SiemensX55BatteryState, SIEMENS_X55_BATTERY)

typedef struct SiemensBatteryPoint {
    uint8_t level;
    uint16_t millivolts;
} SiemensBatteryPoint;

typedef struct SiemensBatteryConfig {
    const SiemensBatteryPoint *curve;
    size_t curve_count;
    int16_t adc_raw_high;
    uint16_t adc_mv_high;
    int16_t adc_raw_low;
    uint16_t adc_mv_low;
    uint8_t battery_channel;
    uint8_t reference_channel;
    uint8_t adc_result_word;
} SiemensBatteryConfig;

bool siemens_battery_configure(DeviceState *dev,
                               const SiemensBatteryConfig *config,
                               Error **errp);
uint32_t siemens_battery_level(DeviceState *dev);
bool siemens_battery_charging(DeviceState *dev);
bool siemens_battery_set_level(DeviceState *dev, uint32_t level,
                               Error **errp);
void siemens_battery_set_charging(DeviceState *dev, bool charging);

#endif
