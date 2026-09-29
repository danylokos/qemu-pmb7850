/*
 * Synthetic machine for C166 CPU tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "target/c166/cpu.h"
#include "qom/object.h"
#include "trace/trace-hw_c166.h"

#define TYPE_C166_TEST_MACHINE MACHINE_TYPE_NAME("c166-test")
#define C166_TEST_RAM_SIZE (16 * MiB)
#define C166_TEST_MMIO_BASE 0x00e000

typedef struct C166TestMachineState {
    MachineState parent_obj;
    C166CPU *cpu;
    MemoryRegion test_mmio;
    MemoryRegion bit_mmio;
    uint16_t read_sequence;
    uint16_t bit_value;
} C166TestMachineState;

OBJECT_DECLARE_SIMPLE_TYPE(C166TestMachineState, C166_TEST_MACHINE)

static uint64_t c166_test_mmio_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    C166TestMachineState *s = opaque;
    uint16_t value = offset == 4 ? ++s->read_sequence : 0;

    if (offset == 6) {
        value = s->bit_value;
        s->read_sequence++;
    }
    trace_c166_test_mmio_read(offset, value, size);
    return value;
}

static void c166_test_mmio_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    C166TestMachineState *s = opaque;

    trace_c166_test_mmio(offset, value);
    if (offset == 0) {
        qemu_system_shutdown_request_with_code(
            value ? SHUTDOWN_CAUSE_GUEST_PANIC :
                    SHUTDOWN_CAUSE_GUEST_SHUTDOWN,
            value & 0xffff);
        return;
    }
    if (offset == 2) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(s->cpu), 0), value & 0x8000 ?
                     0x8000 | (((value >> 8) & 0xf) << 8) |
                     (value & 0x7f) : 0);
        return;
    }
    if (offset == 6) {
        s->bit_value = value;
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "c166-test: invalid MMIO write offset=%" HWADDR_PRIx
                  " value=%" PRIx64 "\n", offset, value);
}

static const MemoryRegionOps c166_test_mmio_ops = {
    .read = c166_test_mmio_read,
    .write = c166_test_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 2,
        .max_access_size = 2,
    },
};

static void c166_test_init(MachineState *machine)
{
    C166TestMachineState *s = C166_TEST_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();

    memory_region_add_subregion(sysmem, 0, machine->ram);
    memory_region_init_io(&s->test_mmio, OBJECT(machine),
                          &c166_test_mmio_ops, s, "c166-test-mmio", 8);
    memory_region_add_subregion_overlap(sysmem, C166_TEST_MMIO_BASE,
                                        &s->test_mmio, 1);
    /* Synthetic, writable bit operand with a counted read side effect. */
    memory_region_init_alias(&s->bit_mmio, OBJECT(machine),
                             "c166-test-bit-mmio", &s->test_mmio, 6, 2);
    memory_region_add_subregion_overlap(sysmem, 0xf13c, &s->bit_mmio, 1);

    s->cpu = C166_CPU(cpu_create(machine->cpu_type));

    if (machine->kernel_filename) {
        long size = load_image_targphys(machine->kernel_filename, 0,
                                        C166_TEST_RAM_SIZE, NULL);
        if (size < 0) {
            error_report("could not load raw kernel '%s'",
                         machine->kernel_filename);
            exit(EXIT_FAILURE);
        }
    }
}

static void c166_test_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Synthetic C166S CPU test machine";
    mc->init = c166_test_init;
    mc->default_cpu_type = TYPE_C166S_V1_CPU;
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
    mc->default_ram_size = C166_TEST_RAM_SIZE;
    mc->default_ram_id = "c166-test.ram";
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

static const TypeInfo c166_test_machine_type = {
    .name = TYPE_C166_TEST_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(C166TestMachineState),
    .class_init = c166_test_machine_class_init,
};

static void c166_test_machine_register_types(void)
{
    type_register_static(&c166_test_machine_type);
}

type_init(c166_test_machine_register_types)
