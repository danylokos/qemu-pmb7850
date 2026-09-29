/*
 * Intel W30-family top-parameter NOR flash.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_BLOCK_W30_H
#define HW_BLOCK_W30_H

#include "qemu/units.h"

#define TYPE_W30_64MBIT_TOP "w30-64mbit-top"
#define TYPE_W30_128MBIT_TOP "w30-128mbit-top"

#define W30_64MBIT_TOP_SIZE (8 * MiB)
#define W30_128MBIT_TOP_SIZE (16 * MiB)
#define W30_PARTITION_SIZE   (512 * KiB)
#define W30_MAIN_BLOCK_SIZE  (64 * KiB)
#define W30_PARAMETER_SIZE   (8 * KiB)
#define W30_FACTORY_UID_SIZE 8

#endif
