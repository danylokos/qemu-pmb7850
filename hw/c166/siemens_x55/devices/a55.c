/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "common.h"

static const SiemensKeypadConfig keypad = X55_KEYPAD_COMMON;

const SiemensX55BoardDescriptor siemens_x55_device_a55 = {
    .name = "a55", .description = "Siemens A55 (PMB7850 / C166S V1)",
    .sysclk_hz = 26000000, .irq55_sources = SIEMENS_X55_IRQ55_DEFAULT,
    .flash = { X55_FLASH(TYPE_M58LW064D, M58LW064D_SIZE, 0, 0) },
    .flash_count = 1,
    .ram = X55_RAM("siemens-a55.sram", TYPE_SIEMENS_SRAM_256KX16,
                   512 * KiB, 2),
    X55_RESET_SEED, .xbus_device_id = 0x1202, .xbus_irq = 0x50,
    .audio_ringer_present = true, .audio_ringer_channel = 2,
    .audio_xbus_v1 = true,
    .port_inputs = { { 7, 0x3400, 0x1400 } }, .port_input_count = 1,
    .display = X55_LCD_MONO(TYPE_PCF8813, 7, 6, -1, 1, 101, 64),
    .keypad = &keypad,
    .battery = &siemens_x55_common_battery[1],
};
