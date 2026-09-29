/*
 * Shared Siemens x55 battery and keypad tables.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "common.h"

/* C55 SW24 percentage-bucket calibration, not a physical battery model. */
static const SiemensBatteryPoint common_battery_curve[] = {
    { 0, 3649 }, { 1, 3659 }, { 2, 3669 }, { 3, 3679 }, { 4, 3689 },
    { 5, 3699 }, { 10, 3754 }, { 15, 3774 }, { 20, 3789 },
    { 25, 3804 }, { 30, 3814 }, { 35, 3824 }, { 40, 3834 },
    { 45, 3849 }, { 50, 3859 }, { 55, 3879 }, { 60, 3894 },
    { 65, 3919 }, { 70, 3939 }, { 75, 3969 }, { 80, 3999 },
    { 85, 4029 }, { 90, 4059 }, { 95, 4104 }, { 100, 4200 },
};

#define BATTERY_ROUTE(word_) { \
    .curve = common_battery_curve, \
    .curve_count = ARRAY_SIZE(common_battery_curve), \
    .adc_raw_high = -1628, .adc_mv_high = 4178, \
    .adc_raw_low = 4499, .adc_mv_low = 3177, \
    .battery_channel = 1, .reference_channel = 3, \
    .adc_result_word = (word_), \
}

const SiemensBatteryConfig siemens_x55_common_battery[2] = {
    BATTERY_ROUTE(0), BATTERY_ROUTE(1),
};
#undef BATTERY_ROUTE

#define KEY(name_, raw_, logical_) \
    { name_, raw_, logical_, Q_KEY_CODE_UNMAPPED, Q_KEY_CODE_UNMAPPED }

const SiemensKeypadButton siemens_x55_common_keys[] = {
    KEY("0", 0x020b, 0x30), KEY("1", 0x0107, 0x31),
    KEY("2", 0x010b, 0x32), KEY("3", 0x010d, 0x33),
    KEY("4", 0x0087, 0x34), KEY("5", 0x008b, 0x35),
    KEY("6", 0x008d, 0x36), KEY("7", 0x0047, 0x37),
    KEY("8", 0x004b, 0x38), KEY("9", 0x004d, 0x39),
    KEY("star", 0x0207, 0x2a), KEY("hash", 0x020d, 0x23),
    KEY("up", 0x008e, 0x3b), KEY("down", 0x004e, 0x3c),
    KEY("soft-left", 0x010e, 0x01), KEY("soft-right", 0x020e, 0x04),
    KEY("send", 0x0017, 0x0b), KEY("power", 0x000d, 0x0c),
};

const SiemensKeypadButton siemens_x55_color_keys[] = {
    KEY("0", 0x020b, 0x30), KEY("1", 0x0107, 0x31),
    KEY("2", 0x010b, 0x32), KEY("3", 0x010d, 0x33),
    KEY("4", 0x0087, 0x34), KEY("5", 0x008b, 0x35),
    KEY("6", 0x008d, 0x36), KEY("7", 0x0047, 0x37),
    KEY("8", 0x004b, 0x38), KEY("9", 0x004d, 0x39),
    KEY("star", 0x0207, 0x2a), KEY("hash", 0x020d, 0x23),
    KEY("up", 0x008e, 0x3b), KEY("down", 0x004e, 0x3c),
    KEY("left", 0x010e, 0x3d), KEY("right", 0x020e, 0x3e),
    KEY("soft-left", 0x001e, 0x01), KEY("soft-right", 0x001b, 0x04),
    KEY("send", 0x0017, 0x0b), KEY("power", 0x000d, 0x0c),
};

const SiemensKeypadButton siemens_x55_m55_keys[] = {
    KEY("0", 0x001b, 0x30), KEY("1", 0x0107, 0x31),
    KEY("2", 0x010b, 0x32), KEY("3", 0x010d, 0x33),
    KEY("4", 0x0087, 0x34), KEY("5", 0x008b, 0x35),
    KEY("6", 0x008d, 0x36), KEY("7", 0x0047, 0x37),
    KEY("8", 0x004b, 0x38), KEY("9", 0x004d, 0x39),
    KEY("star", 0x0017, 0x2a), KEY("hash", 0x001d, 0x23),
    KEY("up", 0x010e, 0x3b), KEY("down", 0x001e, 0x3c),
    KEY("left", 0x020b, 0x3d), KEY("right", 0x004e, 0x3e),
    KEY("soft-left", 0x0207, 0x01), KEY("soft-right", 0x008e, 0x04),
    KEY("send", 0x020e, 0x0b), KEY("power", 0x000d, 0x0c),
};

const uint8_t siemens_x55_common_matrix[] = {
    0xfc, 0xfd, 0xf9, 0xf5, 0xed, 0xdd,
};
