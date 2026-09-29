/*
 * Siemens handset keypad scanner.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_C166_SIEMENS_KEYPAD_H
#define HW_C166_SIEMENS_KEYPAD_H

#include "hw/core/sysbus.h"
#include "qapi/qapi-types-ui.h"

#define TYPE_SIEMENS_KEYPAD "siemens-keypad"
OBJECT_DECLARE_SIMPLE_TYPE(SiemensKeypadState, SIEMENS_KEYPAD)

typedef struct SiemensKeypadButton {
    const char *name;
    uint16_t raw_code;
    uint8_t logical_code;
    QKeyCode qcode;
    QKeyCode alias_qcode;
} SiemensKeypadButton;

#define SIEMENS_KEYPAD_MAX_MATRIX_COMMANDS 8

typedef struct SiemensKeypadConfig {
    uint16_t scan_command;
    uint16_t idle_result;
    uint16_t matrix_idle_result;
    const SiemensKeypadButton *buttons;
    size_t button_count;
    const uint8_t *matrix_commands;
    size_t matrix_command_count;
    uint16_t matrix_select_mask;
    bool release_activity_on_last_key_up;
} SiemensKeypadConfig;

bool siemens_keypad_configure(DeviceState *dev,
                              const SiemensKeypadConfig *config,
                              Error **errp);
const SiemensKeypadButton *siemens_keypad_buttons(DeviceState *dev,
                                                  size_t *count);
bool siemens_keypad_set_ui_button(DeviceState *dev, unsigned button,
                                  bool down);
void siemens_keypad_release_ui_buttons(DeviceState *dev);
uint32_t siemens_keypad_sampled_buttons(DeviceState *dev);
uint32_t siemens_keypad_ui_buttons(DeviceState *dev);

#endif
