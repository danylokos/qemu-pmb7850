/*
 * Repeated Siemens x55 handset descriptor fragments.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_C166_SIEMENS_X55_DEVICES_COMMON_H
#define HW_C166_SIEMENS_X55_DEVICES_COMMON_H

#include "qemu/units.h"
#include "hw/block/am29lv.h"
#include "hw/block/m58lw064d.h"
#include "hw/block/w30.h"
#include "hw/c166/siemens_x55/devices.h"
#include "hw/display/hm17.h"
#include "hw/display/pcf8813.h"
#include "hw/mem/siemens_x55_ram.h"

extern const SiemensBatteryConfig siemens_x55_common_battery[2];
extern const SiemensKeypadButton siemens_x55_common_keys[];
extern const SiemensKeypadButton siemens_x55_color_keys[];
extern const SiemensKeypadButton siemens_x55_m55_keys[];
extern const uint8_t siemens_x55_common_matrix[];

#define SIEMENS_X55_COMMON_KEY_COUNT 18
#define SIEMENS_X55_COLOR_KEY_COUNT 20
#define SIEMENS_X55_M55_KEY_COUNT 20
#define SIEMENS_X55_COMMON_MATRIX_COUNT 6

#define X55_KEYPAD_COMMON { \
    .scan_command = 0x003d, \
    .idle_result = 0x000f, .matrix_idle_result = 0x000f, \
    .buttons = siemens_x55_common_keys, \
    .button_count = SIEMENS_X55_COMMON_KEY_COUNT, \
    .matrix_commands = siemens_x55_common_matrix, \
    .matrix_command_count = SIEMENS_X55_COMMON_MATRIX_COUNT }

#define X55_KEYPAD_COLOR(keys_, count_) { \
    .idle_result = 0xff0f, \
    .matrix_idle_result = 0x000f, .buttons = (keys_), \
    .button_count = (count_), .matrix_select_mask = 0x003d, \
    .release_activity_on_last_key_up = true }

#define X55_FLASH(type_, size_, boot_, cs_) { \
    .name = "flash", .default_type = (type_), \
    .permitted_types = { (type_) }, .size = (size_), \
    .boot_region = (boot_), .cs_region = 1, .cs = (cs_) }

#define X55_FLASH_AM29(type_, size_, boot_, cs_, protect_, protect_size_) { \
    .name = "flash", .default_type = (type_), \
    .permitted_types = { (type_) }, .size = (size_), \
    .boot_region = (boot_), .cs_region = 1, .cs = (cs_), \
    .protected_start = (protect_), .protected_size = (protect_size_), \
    .secsi_factory_locked = true }

#define X55_FLASH_ALT(type_, alt_, size_, boot_, cs_) { \
    .name = "flash", .default_type = (type_), \
    .permitted_types = { (type_), (alt_) }, .size = (size_), \
    .boot_region = (boot_), .cs_region = 1, .cs = (cs_), \
    .protected_start = 8 * MiB, .protected_size = 256 * KiB, \
    .secsi_factory_locked = true }

#define X55_RAM(name_, type_, size_, cs_) \
    { .name = (name_), .type = (type_), .size = (size_), .cs = (cs_) }

#define X55_RESET_SEED \
    .reset_seeds = { { 0xf13c, 0x0002 } }, .reset_seed_count = 1

#define X55_SERIAL \
    .serial = { .available = true, .port = 7, .bit = 3, .idle_high = true }

#define X55_LCD(type_, dcport_, dcbit_, reset_, origin_, w_, h_, mirror_x_) { \
    .type = (type_), .dc_port = (dcport_), .dc_bit = (dcbit_), \
    .reset_capcom_channel = (reset_), .reset_idle_high = true, \
    .panel_width = (w_), .panel_height = (h_), \
    .panel_origin_x = (origin_), .panel_mirror_x = (mirror_x_) }

#define X55_LCD_MONO(type_, dcport_, dcbit_, reset_, origin_, w_, h_) { \
    .type = (type_), .dc_port = (dcport_), .dc_bit = (dcbit_), \
    .reset_capcom_channel = (reset_), .reset_idle_high = true, \
    .panel_width = (w_), .panel_height = (h_), \
    .panel_origin_x = (origin_), .panel_mirror_x = true, \
    .panel_mirror_y = true }

#endif
