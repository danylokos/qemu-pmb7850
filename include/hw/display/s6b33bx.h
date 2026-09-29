/*
 * S6B33Bx-compatible RGB565 LCD controller.
 *
 * The compatible name describes qualified protocol behavior, not fitted-part
 * proof.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_S6B33BX_H
#define HW_DISPLAY_S6B33BX_H

#include "hw/ssi/ssi.h"

#define TYPE_S6B33BX_COMPATIBLE "s6b33bx-compatible"
OBJECT_DECLARE_SIMPLE_TYPE(S6B33BxState, S6B33BX_COMPATIBLE)

#endif
