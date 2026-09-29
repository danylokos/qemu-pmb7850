/*
 * AMD AM29LV uniform-sector NOR flash family.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_BLOCK_AM29LV_H
#define HW_BLOCK_AM29LV_H

#include "qemu/units.h"

#define TYPE_AM29LV640MH  "am29lv640mh"
#define TYPE_AM29LV128MH  "am29lv128mh"

#define AM29LV640MH_SIZE  (8 * MiB)
#define AM29LV128MH_SIZE  (16 * MiB)
#define AM29LV_SECTOR_SIZE (64 * KiB)
#define AM29LV_SECSI_SIZE 256

#endif
