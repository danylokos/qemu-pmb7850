/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/display/s6b33bx.h"
#include "common.h"

static const SiemensKeypadConfig keypad = {
    .idle_result = 0xff0f, .matrix_idle_result = 0x000f,
    .buttons = siemens_x55_color_keys,
    .button_count = SIEMENS_X55_COLOR_KEY_COUNT,
    .matrix_select_mask = 0x003d,
    .release_activity_on_last_key_up = true,
};

const SiemensX55BoardDescriptor siemens_x55_device_cf62 = {
    .name = "cf62", .description = "Siemens CF62 (PMB7850 / C166S V1)",
    .sysclk_hz = 26000000,
    .audio_xbus_v1 = true,
    .irq55_sources = SIEMENS_X55_IRQ55_KEYPAD_ACTIVITY,
    .flash = { X55_FLASH_ALT(TYPE_AM29LV128MH, TYPE_W30_128MBIT_TOP,
                             16 * MiB, 1, 0) },
    .flash_count = 1,
    .ram = X55_RAM("siemens-cf62.psram", TYPE_SIEMENS_PSRAM_1024KX16,
                   2 * MiB, 1),
    X55_RESET_SEED, .xbus_device_id = 0x1203, .xbus_irq = 0x50, X55_SERIAL,
    .port_inputs = { { 7, 0x1000, 0 }, { 8, 0x4000, 0x4000 },
                     { 3, 0x0402, 0x0402 } }, .port_input_count = 3,
    .display = X55_LCD(TYPE_S6B33BX_COMPATIBLE, 8, 13, -1, 0, 130, 130,
                       false),
    .keypad = &keypad, .twi = { true, 56 },
    .battery = &siemens_x55_common_battery[0],
};
