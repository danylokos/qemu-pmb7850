/*
 * Ordered Siemens x55 handset descriptor registry.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/c166/siemens_x55/devices.h"

const SiemensX55BoardDescriptor *const siemens_x55_devices[] = {
    &siemens_x55_device_a52, &siemens_x55_device_a55,
    &siemens_x55_device_a60, &siemens_x55_device_a62,
    &siemens_x55_device_a65, &siemens_x55_device_c55,
    &siemens_x55_device_c60, &siemens_x55_device_cf62,
    &siemens_x55_device_m55, &siemens_x55_device_mc60,
    &siemens_x55_device_s55, &siemens_x55_device_sl55,
};

const size_t siemens_x55_device_count = ARRAY_SIZE(siemens_x55_devices);
