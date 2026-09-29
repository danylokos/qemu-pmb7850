/*
 * Private capability for PMB7850 executable NOR chip-select aliases.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_BLOCK_X55_NOR_VIEW_H
#define HW_BLOCK_X55_NOR_VIEW_H

#include "qom/object.h"
#include "system/memory.h"

#define TYPE_X55_NOR_VIEW "x55-nor-view"
typedef struct X55NORViewClass X55NORViewClass;
DECLARE_CLASS_CHECKERS(X55NORViewClass, X55_NOR_VIEW, TYPE_X55_NOR_VIEW)

struct X55NORViewClass {
    InterfaceClass parent_class;
    /* Complete native view: array backing plus all command/status overlays. */
    MemoryRegion *(*native_view)(Object *obj);
};

bool x55_nor_is_native_view(MemoryRegion *view);

#endif
