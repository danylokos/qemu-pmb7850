/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "common.h"

static const SiemensKeypadConfig keypad = X55_KEYPAD_COLOR(
    siemens_x55_m55_keys, SIEMENS_X55_M55_KEY_COUNT);

const SiemensX55BoardDescriptor siemens_x55_device_m55 = {
    .name = "m55", .description = "Siemens M55 (PMB7850 / C166S V1)",
    .sysclk_hz = 26000000, .irq55_sources = SIEMENS_X55_IRQ55_DEFAULT,
    .flash = { X55_FLASH_AM29(TYPE_AM29LV128MH, AM29LV128MH_SIZE, 1, 0,
                              8 * MiB, 256 * KiB) },
    .flash_count = 1,
    .ram = X55_RAM("siemens-m55.psram", TYPE_SIEMENS_PSRAM_1024KX16,
                   2 * MiB, 1),
    X55_RESET_SEED, .xbus_device_id = 0x1203, .xbus_irq = 0x50, X55_SERIAL,
    .audio_ringer_present = true, .audio_ringer_channel = 2,
    .audio_xbus_v1 = true,
    .port_inputs = { { 7, 0x3000, 0x2000 }, { 6, 0x0020, 0 } },
    .port_input_count = 2,
    .display = X55_LCD(TYPE_HM17CM4096, 7, 6, 0, 24, 101, 80, true),
    .keypad = &keypad,
    .battery = &siemens_x55_common_battery[0],
};
