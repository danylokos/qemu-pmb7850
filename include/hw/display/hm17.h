/*
 * Hynix HM17 family LCD controllers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_HM17_H
#define HW_DISPLAY_HM17_H

#include "hw/ssi/ssi.h"

#define TYPE_HM17CM4096 "hm17cm4096"
#define TYPE_HM17CM256  "hm17cm256"

bool hm17_render_rgb888(DeviceState *dev, uint8_t *frame,
                        size_t frame_size, unsigned *width,
                        unsigned *height);

#endif
