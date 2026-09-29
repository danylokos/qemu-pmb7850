/*
 * Synthetic board for PMB7850 subsystem tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "exec/icount.h"
#include "hw/block/am29lv.h"
#include "hw/block/m58lw064d.h"
#include "hw/block/w30.h"
#include "hw/c166/pmb7850/pmb7850.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/c166/siemens_x55/keypad.h"
#include "hw/c166/siemens_x55/twi.h"
#include "hw/display/hm17.h"
#include "hw/display/pcf8833.h"
#include "hw/display/s6b33bx.h"
#include "hw/mem/siemens_x55_ram.h"
#include "system/blockdev.h"
#include "system/memory.h"
#include "system/runstate.h"

#define TYPE_PMB7850_TEST_MACHINE MACHINE_TYPE_NAME("pmb7850-test")
#define PMB7850_TEST_RAM_SIZE (16 * MiB)
#define PMB7850_TEST_MMIO_BASE 0x00eff0
#define PMB7850_TEST_SYSCLK 26000000

typedef struct PMB7850TestMachineState {
    MachineState parent_obj;
    PMB7850State soc;
    DeviceState *flash;
    DeviceState *external_ram;
    DeviceState *keypad;
    DeviceState *display;
    DeviceState *twi;
    MemoryRegion test_mmio;
    Clock *sysclk;
    char *flash_type;
    char *ram_type;
    char *keypad_profile;
    char *display_type;
    char *twi_profile;
} PMB7850TestMachineState;

OBJECT_DECLARE_SIMPLE_TYPE(PMB7850TestMachineState, PMB7850_TEST_MACHINE)

static const SiemensKeypadButton color_keypad_buttons[] = {
    { "soft-left", 0x001e, 0x01, Q_KEY_CODE_F1, Q_KEY_CODE_LEFT },
    { "power", 0x000d, 0x0c, Q_KEY_CODE_ESC, Q_KEY_CODE_POWER },
    { "1", 0x0107, 0x31, Q_KEY_CODE_1, Q_KEY_CODE_KP_1 },
};

static const SiemensKeypadConfig color_keypad = {
    .idle_result = 0xff0f,
    .matrix_idle_result = 0x000f,
    .buttons = color_keypad_buttons,
    .button_count = ARRAY_SIZE(color_keypad_buttons),
    .matrix_select_mask = 0x003d,
    .release_activity_on_last_key_up = true,
};

static uint64_t pmb7850_test_mmio_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    return 0;
}

static void pmb7850_test_mmio_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    PMB7850TestMachineState *s = opaque;

    if (offset == 0) {
        qemu_system_shutdown_request_with_code(
            value ? SHUTDOWN_CAUSE_GUEST_PANIC :
                    SHUTDOWN_CAUSE_GUEST_SHUTDOWN,
            value & 0xffff);
    } else if (offset == 2 && (value & 0x8000)) {
        qemu_set_irq(qdev_get_gpio_in_named(DEVICE(&s->soc), "ext-irq",
                                            value & 0x7f), 1);
    } else if (offset == 4 && s->soc.instruction_timing &&
               s->soc.cpu.env.idle) {
        /*
         * Synthetic late-delivery fixture, never present on handset boards.
         * Present a controlled idle elapsed interval to the real PMB timer
         * callback without relying on host sleep/jitter or changing icount.
         * The debugger supplies idle state; no guest instruction is retired.
         */
        s->soc.last_guest_icount = s->soc.cpu.env.guest_icount;
        s->soc.last_sync_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                             icount_to_ns(value);
        timer_del(&s->soc.peripheral_timer);
        s->soc.peripheral_timer.cb(s->soc.peripheral_timer.opaque);
    }
}

static const MemoryRegionOps pmb7850_test_mmio_ops = {
    .read = pmb7850_test_mmio_read,
    .write = pmb7850_test_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 2,
};

static char *pmb7850_test_get_flash_type(Object *obj, Error **errp)
{
    PMB7850TestMachineState *s = PMB7850_TEST_MACHINE(obj);

    return g_strdup(s->flash_type);
}

static void pmb7850_test_set_flash_type(Object *obj, const char *value,
                                        Error **errp)
{
    PMB7850TestMachineState *s = PMB7850_TEST_MACHINE(obj);

    if (value && value[0] && strcmp(value, TYPE_M58LW064D) &&
        strcmp(value, TYPE_M58_COMPATIBLE_32MBIT) &&
        strcmp(value, TYPE_AM29LV640MH) &&
        strcmp(value, TYPE_AM29LV128MH) &&
        strcmp(value, TYPE_W30_64MBIT_TOP) &&
        strcmp(value, TYPE_W30_128MBIT_TOP)) {
        error_setg(errp, "unsupported flash-type '%s'", value);
        return;
    }
    g_free(s->flash_type);
    s->flash_type = value && value[0] ? g_strdup(value) : NULL;
}

static char *pmb7850_test_get_ram_type(Object *obj, Error **errp)
{
    PMB7850TestMachineState *s = PMB7850_TEST_MACHINE(obj);

    return g_strdup(s->ram_type);
}

static void pmb7850_test_set_ram_type(Object *obj, const char *value,
                                      Error **errp)
{
    PMB7850TestMachineState *s = PMB7850_TEST_MACHINE(obj);

    if (value && value[0] && strcmp(value, TYPE_SIEMENS_SRAM_256KX16) &&
        strcmp(value, TYPE_SIEMENS_SRAM_512KX16) &&
        strcmp(value, TYPE_SIEMENS_PSRAM_1024KX16)) {
        error_setg(errp, "unsupported ram-type '%s'", value);
        return;
    }
    g_free(s->ram_type);
    s->ram_type = value && value[0] ? g_strdup(value) : NULL;
}

static char *pmb7850_test_get_keypad_profile(Object *obj, Error **errp)
{
    PMB7850TestMachineState *s = PMB7850_TEST_MACHINE(obj);

    return g_strdup(s->keypad_profile);
}

static void pmb7850_test_set_keypad_profile(Object *obj, const char *value,
                                             Error **errp)
{
    PMB7850TestMachineState *s = PMB7850_TEST_MACHINE(obj);

    if (value && value[0] && strcmp(value, "color")) {
        error_setg(errp, "unsupported keypad-profile '%s'", value);
        return;
    }
    g_free(s->keypad_profile);
    s->keypad_profile = value && value[0] ? g_strdup(value) : NULL;
}

static char *pmb7850_test_get_display_type(Object *obj, Error **errp)
{
    return g_strdup(PMB7850_TEST_MACHINE(obj)->display_type);
}

static void pmb7850_test_set_display_type(Object *obj, const char *value,
                                          Error **errp)
{
    PMB7850TestMachineState *s = PMB7850_TEST_MACHINE(obj);

    if (value && value[0] && strcmp(value, TYPE_HM17CM4096) &&
        strcmp(value, TYPE_HM17CM256) &&
        strcmp(value, TYPE_S6B33BX_COMPATIBLE) &&
        strcmp(value, TYPE_PCF8833_COMPATIBLE)) {
        error_setg(errp, "unsupported display-type '%s'", value);
        return;
    }
    g_free(s->display_type);
    s->display_type = value && value[0] ? g_strdup(value) : NULL;
}

static char *pmb7850_test_get_twi_profile(Object *obj, Error **errp)
{
    return g_strdup(PMB7850_TEST_MACHINE(obj)->twi_profile);
}

static void pmb7850_test_set_twi_profile(Object *obj, const char *value,
                                         Error **errp)
{
    PMB7850TestMachineState *s = PMB7850_TEST_MACHINE(obj);

    if (value && value[0] && strcmp(value, "mc60") &&
        strcmp(value, "cf62")) {
        error_setg(errp, "unsupported twi-profile '%s'", value);
        return;
    }
    g_free(s->twi_profile);
    s->twi_profile = value && value[0] ? g_strdup(value) : NULL;
}

static void pmb7850_test_init(MachineState *machine)
{
    PMB7850TestMachineState *s = PMB7850_TEST_MACHINE(machine);
    DeviceState *soc;
    MemoryRegion *boot_memory = machine->ram;
    MemoryRegion *cs0_memory = machine->ram;
    MemoryRegion *cs2_memory = machine->ram;

    /*
     * Instantiate CPU-containing SoCs only after accelerator selection.
     * Creating this child from the machine instance initializer is too early
     * for TCG to allocate its per-CPU translation/plugin state.
     */
    object_initialize_child(OBJECT(machine), "soc", &s->soc,
                            TYPE_PMB7850_SOC);
    s->sysclk = clock_new(OBJECT(machine), "sysclk");
    clock_set_hz(s->sysclk, PMB7850_TEST_SYSCLK);
    soc = DEVICE(&s->soc);

    if (s->flash_type) {
        DriveInfo *dinfo = drive_get(IF_PFLASH, 0, 0);

        s->flash = qdev_new(s->flash_type);
        object_property_add_child(OBJECT(machine), "flash",
                                  OBJECT(s->flash));
        object_unref(OBJECT(s->flash));
        if (dinfo) {
            qdev_prop_set_drive(s->flash, "drive",
                                blk_by_legacy_dinfo(dinfo));
        }
        if (object_property_find(OBJECT(s->flash), "instruction-timing")) {
            qdev_connect_clock_in(s->flash, "sysclk", s->sysclk);
        }
        if (!sysbus_realize(SYS_BUS_DEVICE(s->flash), &error_fatal)) {
            return;
        }
        boot_memory = sysbus_mmio_get_region(
            SYS_BUS_DEVICE(s->flash),
            (!strcmp(s->flash_type, TYPE_AM29LV128MH) ||
             !strcmp(s->flash_type, TYPE_W30_128MBIT_TOP)) ? 1 : 0);
        cs0_memory = sysbus_mmio_get_region(SYS_BUS_DEVICE(s->flash), 1);
    }
    if (s->ram_type) {
        s->external_ram = qdev_new(s->ram_type);
        object_property_add_child(OBJECT(machine), "external-ram",
                                  OBJECT(s->external_ram));
        object_unref(OBJECT(s->external_ram));
        if (!sysbus_realize(SYS_BUS_DEVICE(s->external_ram), &error_fatal)) {
            return;
        }
        cs2_memory = siemens_x55_ram_get_memory(s->external_ram);
    }

    object_property_set_link(OBJECT(soc), "memory",
                             OBJECT(get_system_memory()), &error_abort);
    object_property_set_link(OBJECT(soc), "boot-memory",
                             OBJECT(boot_memory), &error_abort);
    object_property_set_link(OBJECT(soc), "cs0-memory",
                             OBJECT(cs0_memory), &error_abort);
    for (unsigned i = 1; i < 5; i++) {
        g_autofree char *name = g_strdup_printf("cs%u-memory", i);
        MemoryRegion *memory = i == 2 ? cs2_memory : machine->ram;

        /* Synthetic alias for CPU-executed NOR mutation/cache regressions. */
        if (i == 3 && s->flash) {
            memory = cs0_memory;
        }

        object_property_set_link(OBJECT(soc), name, OBJECT(memory),
                                 &error_abort);
    }
    for (unsigned i = 1; i <= 6; i++) {
        g_autofree char *name = g_strdup_printf("xbus%u-memory", i);
        object_property_set_link(OBJECT(soc), name, OBJECT(machine->ram),
                                 &error_abort);
    }
    qdev_connect_clock_in(soc, "sysclk", s->sysclk);
    if (!sysbus_realize(SYS_BUS_DEVICE(soc), &error_fatal)) {
        return;
    }
    if (s->display_type) {
        s->display = qdev_new(s->display_type);
        object_property_add_child(OBJECT(machine), "display",
                                  OBJECT(s->display));
        object_unref(OBJECT(s->display));
        if (!strcmp(s->display_type, TYPE_S6B33BX_COMPATIBLE)) {
            qdev_prop_set_uint32(s->display, "panel-width", 130);
            qdev_prop_set_uint32(s->display, "panel-height", 130);
        } else {
            qdev_prop_set_uint32(s->display, "panel-width", 101);
            qdev_prop_set_uint32(s->display, "panel-height", 80);
            qdev_prop_set_uint32(s->display, "panel-origin-x",
                !strcmp(s->display_type, TYPE_HM17CM4096) ? 24 :
                !strcmp(s->display_type, TYPE_HM17CM256) ? 13 : 1);
            qdev_prop_set_bit(s->display, "panel-mirror-x", true);
        }
        if (!qdev_realize(s->display, BUS(s->soc.ssc0), &error_fatal)) {
            return;
        }
        qdev_connect_gpio_out_named(soc, "p3-out", 11,
            qdev_get_gpio_in_named(s->display, SSI_GPIO_CS, 0));
        qdev_connect_gpio_out_named(soc, "p7-out", 6,
            qdev_get_gpio_in_named(s->display, "dc", 0));
        qdev_connect_gpio_out_named(soc, "ssc0-frame-start", 0,
            qdev_get_gpio_in_named(s->display, "frame-start", 0));
        qdev_connect_gpio_out_named(soc, "capcom-out", 0,
            qdev_get_gpio_in_named(s->display, "reset-level", 0));
        qdev_connect_gpio_out_named(soc, "capcom-driven", 0,
            qdev_get_gpio_in_named(s->display, "reset-driven", 0));
    }
    if (s->twi_profile) {
        static const uint8_t zero_registers[56];
        SiemensTWIConfig config = {
            .model = "register-file",
            .address = 0x31,
            .registers = zero_registers,
            .register_count = !strcmp(s->twi_profile, "mc60") ? 32 : 56,
        };

        s->twi = qdev_new(TYPE_SIEMENS_TWI);
        object_property_add_child(OBJECT(machine), "twi", OBJECT(s->twi));
        object_unref(OBJECT(s->twi));
        if (!siemens_twi_configure(s->twi, &config, &error_fatal) ||
            !qdev_realize(s->twi, NULL, &error_fatal)) {
            return;
        }
        qdev_connect_gpio_out_named(soc, "p6-state", 0,
            qdev_get_gpio_in_named(s->twi, "port-state", 0));
        qdev_connect_gpio_out_named(s->twi, "scl-in", 0,
            qdev_get_gpio_in_named(soc, "p6-in", 12));
        qdev_connect_gpio_out_named(s->twi, "sda-in", 0,
            qdev_get_gpio_in_named(soc, "p6-in", 13));
    }
    if (s->keypad_profile) {
        s->keypad = qdev_new(TYPE_SIEMENS_KEYPAD);
        object_property_add_child(OBJECT(machine), "keypad",
                                  OBJECT(s->keypad));
        object_unref(OBJECT(s->keypad));
        if (!siemens_keypad_configure(s->keypad, &color_keypad,
                                      &error_fatal)) {
            return;
        }
        qdev_connect_gpio_out_named(s->keypad, "activity", 0,
            qdev_get_gpio_in_named(soc, "ext-irq", 0x37));
        qdev_connect_gpio_out_named(soc, "keypad-matrix-select", 0,
            qdev_get_gpio_in_named(s->keypad, "matrix-select", 0));
        if (!sysbus_realize(SYS_BUS_DEVICE(s->keypad), &error_fatal)) {
            return;
        }
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->keypad), 0, 0xef1a, 40);
    }

    memory_region_init_io(&s->test_mmio, OBJECT(machine),
                          &pmb7850_test_mmio_ops, s,
                          "pmb7850-test-mmio", 6);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        PMB7850_TEST_MMIO_BASE,
                                        &s->test_mmio, 100);

    if (machine->kernel_filename &&
        load_image_mr(machine->kernel_filename, machine->ram) < 0) {
        error_report("could not load raw kernel '%s'",
                     machine->kernel_filename);
        exit(EXIT_FAILURE);
    }
}

static void pmb7850_test_machine_finalize(Object *obj)
{
    PMB7850TestMachineState *s = PMB7850_TEST_MACHINE(obj);

    g_free(s->flash_type);
    g_free(s->ram_type);
    g_free(s->keypad_profile);
    g_free(s->display_type);
    g_free(s->twi_profile);
}

static void pmb7850_test_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Synthetic PMB7850 SoC test machine";
    mc->init = pmb7850_test_init;
    mc->default_cpu_type = TYPE_C166S_V1_CPU;
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
    mc->default_ram_size = PMB7850_TEST_RAM_SIZE;
    mc->default_ram_id = "pmb7850-test.ram";
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
    object_class_property_add_str(oc, "flash-type",
                                  pmb7850_test_get_flash_type,
                                  pmb7850_test_set_flash_type);
    object_class_property_add_str(oc, "ram-type",
                                  pmb7850_test_get_ram_type,
                                  pmb7850_test_set_ram_type);
    object_class_property_add_str(oc, "keypad-profile",
                                  pmb7850_test_get_keypad_profile,
                                  pmb7850_test_set_keypad_profile);
    object_class_property_add_str(oc, "display-type",
                                  pmb7850_test_get_display_type,
                                  pmb7850_test_set_display_type);
    object_class_property_add_str(oc, "twi-profile",
                                  pmb7850_test_get_twi_profile,
                                  pmb7850_test_set_twi_profile);
    object_class_property_set_description(
        oc, "flash-type", "Attach a supported NOR flash for qtests");
    object_class_property_set_description(
        oc, "ram-type", "Attach a supported external RAM for qtests");
    object_class_property_set_description(
        oc, "keypad-profile", "Attach a supported keypad for qtests");
    object_class_property_set_description(
        oc, "display-type", "Attach a supported color display for qtests");
    object_class_property_set_description(
        oc, "twi-profile", "Attach a supported GPIO TWI responder for qtests");
}

static const TypeInfo pmb7850_test_machine_type = {
    .name = TYPE_PMB7850_TEST_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(PMB7850TestMachineState),
    .instance_align = __alignof__(PMB7850TestMachineState),
    .instance_finalize = pmb7850_test_machine_finalize,
    .class_init = pmb7850_test_machine_class_init,
};

static void pmb7850_test_machine_register_types(void)
{
    type_register_static(&pmb7850_test_machine_type);
}

type_init(pmb7850_test_machine_register_types)
