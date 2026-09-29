/* Shared storage, deliberately independent of NOR command/timing policy.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/block/x55_nor_view.h"
#include "x55_nor.h"
#include "hw/block/x55_nor_trace.h"

bool x55_nor_init(X55NORStorage *s, MemoryRegion *native, Object *owner,
                  const MemoryRegionOps *ops, void *opaque, const char *name,
                  uint32_t size, uint32_t granularity,
                  bool (*array_byte)(Object *, MemoryRegion *, uint32_t),
                  Error **errp)
{
    g_autofree char *view_name = g_strdup_printf("%s.view", name);
    g_autofree char *array_name = g_strdup_printf("%s.array", name);
    g_autofree char *command_name = g_strdup_printf("%s.commands", name);

    assert(granularity && size % granularity == 0);
    assert(size / granularity <= X55_NOR_MAX_OVERLAYS);
    /* Keep the original allocation name and owner: these identify migrated RAM. */
    if (!memory_region_init_rom_device(&s->backing, owner, ops, opaque,
                                       name, size, errp)) {
        return false;
    }
    s->native = native;
    s->owner = owner;
    s->granularity = granularity;
    s->count = size / granularity;
    s->array_byte = array_byte;
    memory_region_init(native, owner, view_name, size);
    memory_region_init_alias(&s->array, owner, array_name, &s->backing, 0, size);
    memory_region_add_subregion(native, 0, &s->array);
    memory_region_init_io(&s->commands, owner, ops, opaque, command_name, size);
    for (unsigned i = 0; i < s->count; i++) {
        g_autofree char *overlay_name = g_strdup_printf("%s.status-%u", name, i);
        uint32_t offset = i * granularity;

        /* Alias offsets preserve physical-chip addresses in callbacks/traces. */
        memory_region_init_alias(&s->overlays[i], owner, overlay_name,
                                 &s->commands, offset, granularity);
        memory_region_set_enabled(&s->overlays[i], false);
        memory_region_add_subregion_overlap(native, offset, &s->overlays[i], 1);
    }
    return true;
}

void x55_nor_update(X55NORStorage *s)
{
    memory_region_transaction_begin();
    for (unsigned i = 0; i < s->count; i++) {
        uint32_t offset = i * s->granularity;
        bool enabled = !s->array_byte(s->owner, s->native, offset);

        if (s->overlays[i].enabled != enabled) {
            x55_nor_trace_mapping(s->owner, offset, s->granularity, enabled);
            memory_region_set_enabled(&s->overlays[i], enabled);
        }
    }
    memory_region_transaction_commit();
}

void x55_nor_flush(X55NORStorage *s, hwaddr offset, hwaddr size)
{
    /* Always ROMD, including while overlays handle reads. No topology changes. */
    memory_region_flush_rom_device(&s->backing, offset, size);
    x55_nor_trace_flush(s->owner, offset, size);
}

bool x55_nor_is_native_view(MemoryRegion *view)
{
    Object *owner = memory_region_owner(view);

    return object_dynamic_cast(owner, TYPE_X55_NOR_VIEW) &&
           X55_NOR_VIEW_GET_CLASS(owner)->native_view(owner) == view;
}

static const TypeInfo nor_view_type = {
    .name = TYPE_X55_NOR_VIEW,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(X55NORViewClass),
};

static void nor_view_register_types(void)
{
    type_register_static(&nor_view_type);
}

type_init(nor_view_register_types)
