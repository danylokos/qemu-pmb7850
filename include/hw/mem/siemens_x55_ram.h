/*
 * Siemens x55 external SRAM and PSRAM devices.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MEM_SIEMENS_X55_RAM_H
#define HW_MEM_SIEMENS_X55_RAM_H

#include "hw/core/sysbus.h"

#define TYPE_SIEMENS_SRAM_256KX16  "sram-256kx16"
#define TYPE_SIEMENS_SRAM_512KX16  "sram-512kx16"
#define TYPE_SIEMENS_PSRAM_1024KX16 "psram-1024kx16"

MemoryRegion *siemens_x55_ram_get_memory(DeviceState *dev);
uint64_t siemens_x55_ram_get_size(DeviceState *dev);

#endif
