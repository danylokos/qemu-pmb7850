/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "common.h"

static const SiemensKeypadConfig keypad = X55_KEYPAD_COLOR(
    siemens_x55_color_keys, SIEMENS_X55_COLOR_KEY_COUNT);

const SiemensX55BoardDescriptor siemens_x55_device_a62 = {
    .name = "a62", .description = "Siemens A62 (PMB7850 / C166S V1)",
    .sysclk_hz = 26000000,
    .audio_xbus_v1 = true,
    .irq55_sources = SIEMENS_X55_IRQ55_KEYPAD_ACTIVITY,
    .flash = { X55_FLASH_AM29(TYPE_AM29LV640MH, AM29LV640MH_SIZE, 1, 0,
                              0, 0) },
    .flash_count = 1,
    .ram = X55_RAM("siemens-a62.psram", TYPE_SIEMENS_PSRAM_1024KX16,
                   2 * MiB, 1),
    X55_RESET_SEED, .xbus_device_id = 0x1203, .xbus_irq = 0x50, X55_SERIAL,
    .port_inputs = { { 7, 0x14a0, 0x0420 }, { 8, 0x4000, 0x4000 },
                     { 3, 0x0400, 0x0400 } }, .port_input_count = 3,
    .display = X55_LCD(TYPE_HM17CM4096, 8, 13, -1, 24, 101, 80, true),
    .keypad = &keypad,
    .battery = &siemens_x55_common_battery[0],
};
