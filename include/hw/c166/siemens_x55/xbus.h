/*
 * Siemens handset product/XBUS endpoint with unknown physical identity.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_C166_SIEMENS_XBUS_H
#define HW_C166_SIEMENS_XBUS_H

#include "hw/core/sysbus.h"

#define TYPE_SIEMENS_XBUS_ENDPOINT "siemens-xbus-endpoint"
OBJECT_DECLARE_SIMPLE_TYPE(SiemensXBusEndpointState,
                           SIEMENS_XBUS_ENDPOINT)

bool siemens_xbus_set_instruction_timing(DeviceState *dev, bool enabled,
                                         Error **errp);

#endif
