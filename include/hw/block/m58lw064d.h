/*
 * ST M58LW064D 64-Mbit NOR flash.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_BLOCK_M58LW064D_H
#define HW_BLOCK_M58LW064D_H

#include "qemu/units.h"

#define TYPE_M58LW064D "m58lw064d"
#define TYPE_M58_COMPATIBLE_32MBIT "m58-compatible-32mbit"

#define M58LW064D_SIZE               (8 * MiB)
#define M58_COMPATIBLE_32MBIT_SIZE   (4 * MiB)
#define M58_UNIFORM_BLOCK_SIZE       (128 * KiB)
#define M58LW064D_BLOCKS             64
#define M58_COMPATIBLE_32MBIT_BLOCKS 32

#endif
