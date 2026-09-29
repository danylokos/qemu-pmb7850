/* Private storage/views for the x55 NOR command engines.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_BLOCK_X55_NOR_H
#define HW_BLOCK_X55_NOR_H

#include "system/memory.h"

/* Largest supported geometry: 16 MiB with 64 KiB status granularity. */
#define X55_NOR_MAX_OVERLAYS 256

typedef struct X55NORStorage {
    MemoryRegion backing;
    MemoryRegion array;
    MemoryRegion commands;
    MemoryRegion overlays[X55_NOR_MAX_OVERLAYS];
    MemoryRegion *native;
    Object *owner;
    uint32_t granularity;
    unsigned count;
    bool (*array_byte)(Object *, MemoryRegion *, uint32_t);
} X55NORStorage;

bool x55_nor_init(X55NORStorage *s, MemoryRegion *native, Object *owner,
                  const MemoryRegionOps *ops, void *opaque, const char *name,
                  uint32_t size, uint32_t granularity,
                  bool (*array_byte)(Object *, MemoryRegion *, uint32_t),
                  Error **errp);
void x55_nor_update(X55NORStorage *s);
void x55_nor_flush(X55NORStorage *s, hwaddr offset, hwaddr size);

#endif
