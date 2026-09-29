/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "common.h"

static const SiemensKeypadButton keys[] = {
    { "0", 0x020b, 0x30, Q_KEY_CODE_0, Q_KEY_CODE_KP_0 },
    { "1", 0x0107, 0x31, Q_KEY_CODE_1, Q_KEY_CODE_KP_1 },
    { "2", 0x010b, 0x32, Q_KEY_CODE_2, Q_KEY_CODE_KP_2 },
    { "3", 0x010d, 0x33, Q_KEY_CODE_3, Q_KEY_CODE_KP_3 },
    { "4", 0x0087, 0x34, Q_KEY_CODE_4, Q_KEY_CODE_KP_4 },
    { "5", 0x008b, 0x35, Q_KEY_CODE_5, Q_KEY_CODE_KP_5 },
    { "6", 0x008d, 0x36, Q_KEY_CODE_6, Q_KEY_CODE_KP_6 },
    { "7", 0x0047, 0x37, Q_KEY_CODE_7, Q_KEY_CODE_KP_7 },
    { "8", 0x004b, 0x38, Q_KEY_CODE_8, Q_KEY_CODE_KP_8 },
    { "9", 0x004d, 0x39, Q_KEY_CODE_9, Q_KEY_CODE_KP_9 },
    { "star", 0x0207, 0x2a, Q_KEY_CODE_ASTERISK,
      Q_KEY_CODE_KP_MULTIPLY },
    { "hash", 0x020d, 0x23, Q_KEY_CODE_UNMAPPED, Q_KEY_CODE_UNMAPPED },
    { "up", 0x008e, 0x3b, Q_KEY_CODE_UP, Q_KEY_CODE_HOME },
    { "down", 0x004e, 0x3c, Q_KEY_CODE_DOWN, Q_KEY_CODE_END },
    { "soft-left", 0x010e, 0x01, Q_KEY_CODE_F1, Q_KEY_CODE_LEFT },
    { "soft-right", 0x020e, 0x04, Q_KEY_CODE_F2, Q_KEY_CODE_RIGHT },
    { "send", 0x0017, 0x0b, Q_KEY_CODE_RET, Q_KEY_CODE_KP_ENTER },
    { "power", 0x000d, 0x0c, Q_KEY_CODE_ESC, Q_KEY_CODE_POWER },
};

static const SiemensKeypadConfig keypad = {
    .scan_command = 0x003d,
    .idle_result = 0x000f,
    .matrix_idle_result = 0x000f, .buttons = keys,
    .button_count = ARRAY_SIZE(keys),
    .matrix_commands = siemens_x55_common_matrix,
    .matrix_command_count = 6,
};

const SiemensX55BoardDescriptor siemens_x55_device_c55 = {
    .name = "c55", .description = "Siemens C55 (PMB7850 / C166S V1)",
    .sysclk_hz = 26000000, .irq55_sources = SIEMENS_X55_IRQ55_DEFAULT,
    .flash = { X55_FLASH(TYPE_M58LW064D, M58LW064D_SIZE, 0, 0) },
    .flash_count = 1,
    .ram = X55_RAM("siemens-c55.sram", TYPE_SIEMENS_SRAM_256KX16,
                   512 * KiB, 2),
    X55_RESET_SEED, .xbus_device_id = 0x1202, .xbus_irq = 0x50, X55_SERIAL,
    .audio_ringer_present = true, .audio_ringer_channel = 2,
    .audio_xbus_v1 = true,
    .display = X55_LCD_MONO(TYPE_PCF8813, 7, 6, -1, 1, 101, 64),
    .keypad = &keypad,
    .battery = &siemens_x55_common_battery[1],
};
