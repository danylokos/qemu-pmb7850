/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "common.h"

static const SiemensKeypadConfig keypad = {
    .idle_result = 0xff0f, .matrix_idle_result = 0x000f,
    .buttons = siemens_x55_common_keys,
    .button_count = SIEMENS_X55_COMMON_KEY_COUNT,
    .matrix_select_mask = 0x003d,
    .release_activity_on_last_key_up = true,
};

const SiemensX55BoardDescriptor siemens_x55_device_mc60 = {
    .name = "mc60", .description = "Siemens MC60 (PMB7850 / C166S V1)",
    .sysclk_hz = 26000000,
    .audio_xbus_v1 = true,
    .irq55_sources = SIEMENS_X55_IRQ55_KEYPAD_ACTIVITY,
    .flash = { X55_FLASH_AM29(TYPE_AM29LV128MH, AM29LV128MH_SIZE, 1, 0,
                              8 * MiB, 256 * KiB) },
    .flash_count = 1,
    .ram = X55_RAM("siemens-mc60.psram", TYPE_SIEMENS_PSRAM_1024KX16,
                   2 * MiB, 1),
    X55_RESET_SEED, .xbus_device_id = 0x1203, .xbus_irq = 0x50, X55_SERIAL,
    /*
     * Match CEMU's unmodeled SSC receive input for SW13 camera polling.
     * Calibration, not a SAA8130 register model or a proven MRST pull-up.
     */
    .ssc0_unselected_rx_high = true,
    .port_inputs = { { 7, 0x1400, 0x0400 }, { 8, 0x4000, 0x4000 },
                     { 3, 0x0002, 0 } }, .port_input_count = 3,
    .display = X55_LCD(TYPE_HM17CM4096, 8, 13, -1, 24, 101, 80, true),
    .keypad = &keypad, .twi = { true, 32 },
    .battery = &siemens_x55_common_battery[0],
};
