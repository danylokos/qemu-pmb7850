/*
 * Siemens x55 external SRAM and PSRAM devices.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/core/qdev-properties.h"
#include "hw/mem/siemens_x55_ram.h"

#define TYPE_SIEMENS_X55_RAM "siemens-x55-ram"
OBJECT_DECLARE_TYPE(SiemensX55RAMState, SiemensX55RAMClass, SIEMENS_X55_RAM)

typedef struct SiemensX55RAMGeometry {
    const char *description;
    uint32_t size;
} SiemensX55RAMGeometry;

struct SiemensX55RAMState {
    SysBusDevice parent_obj;
    MemoryRegion memory;
    char *region_name;
};

struct SiemensX55RAMClass {
    SysBusDeviceClass parent_class;
    const SiemensX55RAMGeometry *geometry;
};

static const SiemensX55RAMGeometry sram_256kx16_geometry = {
    .description = "Siemens x55 256K x16 external SRAM",
    .size = 512 * KiB,
};

static const SiemensX55RAMGeometry sram_512kx16_geometry = {
    .description = "Siemens x55 512K x16 external SRAM",
    .size = 1 * MiB,
};

static const SiemensX55RAMGeometry psram_1024kx16_geometry = {
    .description = "Siemens x55 1024K x16 external PSRAM",
    .size = 2 * MiB,
};

static void siemens_x55_ram_realize(DeviceState *dev, Error **errp)
{
    SiemensX55RAMState *s = SIEMENS_X55_RAM(dev);
    SiemensX55RAMClass *xrc = SIEMENS_X55_RAM_GET_CLASS(dev);
    const char *name = s->region_name ?: object_get_typename(OBJECT(dev));

    if (!memory_region_init_ram(&s->memory, OBJECT(s), name,
                                xrc->geometry->size, errp)) {
        return;
    }
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->memory);
}

MemoryRegion *siemens_x55_ram_get_memory(DeviceState *dev)
{
    return &SIEMENS_X55_RAM(dev)->memory;
}

uint64_t siemens_x55_ram_get_size(DeviceState *dev)
{
    SiemensX55RAMClass *xrc = SIEMENS_X55_RAM_GET_CLASS(dev);

    return xrc->geometry->size;
}

static const Property siemens_x55_ram_properties[] = {
    DEFINE_PROP_STRING("region-name", SiemensX55RAMState, region_name),
};

static void siemens_x55_ram_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = siemens_x55_ram_realize;
    device_class_set_props(dc, siemens_x55_ram_properties);
}

static void siemens_x55_ram_concrete_class_init(ObjectClass *oc,
                                                 const void *data)
{
    SiemensX55RAMClass *xrc = SIEMENS_X55_RAM_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    xrc->geometry = data;
    dc->desc = xrc->geometry->description;
}

static const TypeInfo siemens_x55_ram_type = {
    .name = TYPE_SIEMENS_X55_RAM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SiemensX55RAMState),
    .instance_align = __alignof__(SiemensX55RAMState),
    .class_size = sizeof(SiemensX55RAMClass),
    .class_init = siemens_x55_ram_class_init,
    .abstract = true,
};

static const TypeInfo siemens_x55_ram_concrete_types[] = {
    {
        .name = TYPE_SIEMENS_SRAM_256KX16,
        .parent = TYPE_SIEMENS_X55_RAM,
        .class_init = siemens_x55_ram_concrete_class_init,
        .class_data = &sram_256kx16_geometry,
    }, {
        .name = TYPE_SIEMENS_SRAM_512KX16,
        .parent = TYPE_SIEMENS_X55_RAM,
        .class_init = siemens_x55_ram_concrete_class_init,
        .class_data = &sram_512kx16_geometry,
    }, {
        .name = TYPE_SIEMENS_PSRAM_1024KX16,
        .parent = TYPE_SIEMENS_X55_RAM,
        .class_init = siemens_x55_ram_concrete_class_init,
        .class_data = &psram_1024kx16_geometry,
    },
};

static void siemens_x55_ram_register_types(void)
{
    type_register_static(&siemens_x55_ram_type);
    type_register_static_array(siemens_x55_ram_concrete_types,
                               ARRAY_SIZE(siemens_x55_ram_concrete_types));
}

type_init(siemens_x55_ram_register_types)
