/*
 * PCF8833-compatible four-wire RGB444 LCD controller.
 *
 * The compatible name describes qualified protocol behavior, not fitted-part
 * proof.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_PCF8833_H
#define HW_DISPLAY_PCF8833_H

#include "hw/ssi/ssi.h"

#define TYPE_PCF8833_COMPATIBLE "pcf8833-compatible"
OBJECT_DECLARE_SIMPLE_TYPE(PCF8833State, PCF8833_COMPATIBLE)

#endif
