/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "common.h"

static const SiemensKeypadConfig keypad = X55_KEYPAD_COLOR(
    siemens_x55_m55_keys, SIEMENS_X55_M55_KEY_COUNT);

const SiemensX55BoardDescriptor siemens_x55_device_s55 = {
    .name = "s55", .description = "Siemens S55 (PMB7850 / C166S V1)",
    .sysclk_hz = 26000000, .irq55_sources = SIEMENS_X55_IRQ55_DEFAULT,
    .flash = {
        X55_FLASH(TYPE_W30_64MBIT_TOP, W30_64MBIT_TOP_SIZE, 0, 0),
        { .name = "flash-secondary",
          .default_type = TYPE_M58_COMPATIBLE_32MBIT,
          .permitted_types = { TYPE_M58_COMPATIBLE_32MBIT },
          .size = 4 * MiB, .boot_region = 0, .cs_region = 1, .cs = 1 },
    },
    .flash_count = 2,
    .ram = X55_RAM("siemens-s55.sram", TYPE_SIEMENS_SRAM_512KX16,
                   1 * MiB, 2),
    X55_RESET_SEED, .xbus_device_id = 0x1202, .xbus_irq = 0x50, X55_SERIAL,
    .port_inputs = { { 7, 0x3c00, 0x2800 }, { 6, 0x0020, 0x0020 } },
    .port_input_count = 2,
    .display = X55_LCD(TYPE_HM17CM256, 7, 6, -1, 13, 101, 80, true),
    .keypad = &keypad,
    .battery = &siemens_x55_common_battery[1],
};
