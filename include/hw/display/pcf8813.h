/*
 * Philips PCF8813 monochrome LCD controller.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_PCF8813_H
#define HW_DISPLAY_PCF8813_H

#include "hw/ssi/ssi.h"

#define TYPE_PCF8813 "pcf8813"
OBJECT_DECLARE_SIMPLE_TYPE(PCF8813State, PCF8813)

bool pcf8813_render_rgb888(DeviceState *dev, uint8_t *frame,
                           size_t frame_size, unsigned *width,
                           unsigned *height);

#endif
