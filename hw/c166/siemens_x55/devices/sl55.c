/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/display/pcf8833.h"
#include "common.h"

#define KEY(name_, raw_, logical_) \
    { name_, raw_, logical_, Q_KEY_CODE_UNMAPPED, Q_KEY_CODE_UNMAPPED }

static const SiemensKeypadButton keys[] = {
    KEY("0", 0x004d, 0x30), KEY("1", 0x001e, 0x31),
    KEY("2", 0x004e, 0x32), KEY("3", 0x008e, 0x33),
    KEY("4", 0x0017, 0x34), KEY("5", 0x0047, 0x35),
    KEY("6", 0x0087, 0x36), KEY("7", 0x001b, 0x37),
    KEY("8", 0x004b, 0x38), KEY("9", 0x008b, 0x39),
    KEY("star", 0x001d, 0x2a), KEY("hash", 0x008d, 0x23),
    KEY("up", 0x010e, 0x3b), KEY("down", 0x010d, 0x3c),
    KEY("left", 0x0107, 0x3d), KEY("right", 0x010b, 0x3e),
    KEY("soft-left", 0x0007, 0x01), KEY("soft-right", 0x000b, 0x04),
    KEY("send", 0x000e, 0x0b), KEY("power", 0x000d, 0x0c),
};
static const uint8_t matrix[] = { 0xfe, 0xfb, 0xf7, 0xef, 0xdf };
static const SiemensKeypadConfig keypad = {
    .scan_command = 0x003d,
    .idle_result = 0xff0f,
    .matrix_idle_result = 0x000f, .buttons = keys,
    .button_count = ARRAY_SIZE(keys), .matrix_commands = matrix,
    .matrix_command_count = ARRAY_SIZE(matrix), .matrix_select_mask = 0x003d,
    .release_activity_on_last_key_up = true,
};

const SiemensX55BoardDescriptor siemens_x55_device_sl55 = {
    .name = "sl55", .description = "Siemens SL55 (PMB7850 / C166S V1)",
    .sysclk_hz = 26000000, .irq55_sources = SIEMENS_X55_IRQ55_DEFAULT,
    .flash = {
        X55_FLASH(TYPE_W30_64MBIT_TOP, W30_64MBIT_TOP_SIZE, 0, 0),
        { .name = "flash-secondary",
          .default_type = TYPE_M58_COMPATIBLE_32MBIT,
          .permitted_types = { TYPE_M58_COMPATIBLE_32MBIT },
          .size = 4 * MiB, .boot_region = 0, .cs_region = 1, .cs = 1 },
    },
    .flash_count = 2,
    .ram = X55_RAM("siemens-sl55.sram", TYPE_SIEMENS_SRAM_512KX16,
                   1 * MiB, 2),
    X55_RESET_SEED, .xbus_device_id = 0x1203, .xbus_irq = 0x50, X55_SERIAL,
    .port_inputs = { { 6, 0x0008, 0x0008 }, { 7, 0x0200, 0x0200 } },
    .port_input_count = 2,
    .display = X55_LCD(TYPE_PCF8833_COMPATIBLE, 7, 6, 0, 1, 101, 80, true),
    .keypad = &keypad,
    .battery = &siemens_x55_common_battery[0],
};
