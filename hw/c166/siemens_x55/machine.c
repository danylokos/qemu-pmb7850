/*
 * Siemens x55 handset machine composition.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/c166/pmb7850/pmb7850.h"
#include "hw/block/x55_nor_trace.h"
#include "hw/audio/x55_audio.h"
#include "hw/c166/siemens_x55/twi.h"
#include "hw/c166/siemens_x55/machine.h"
#include "hw/c166/siemens_x55/xbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/pcf8813.h"
#include "hw/mem/siemens_x55_ram.h"
#include "system/blockdev.h"
#include "system/system.h"
#include "target/c166/cpu.h"
#include "trace/trace-hw_c166.h"

struct SiemensX55MachineState {
    MachineState parent_obj;
    PMB7850State soc;
    DeviceState *flash[SIEMENS_X55_MAX_FLASH_CHIPS];
    DeviceState *ram;
    DeviceState *xbus;
    DeviceState *audio;
    qemu_irq ringer;
    DeviceState *display;
    DeviceState *keypad;
    DeviceState *battery;
    DeviceState *twi;
    Clock *sysclk;
    char *audiodev;
    bool ringer_audio;
    bool xbus_audio;
    char *flash_type[SIEMENS_X55_MAX_FLASH_CHIPS];
    MemoryRegion folded_boot;
    MemoryRegion boot_alias[2];
    bool folded_boot_valid;
    uint32_t battery_level;
    bool battery_charging;
};

const SiemensX55BoardDescriptor *siemens_x55_board(MachineState *machine)
{
    SiemensX55MachineClass *xmc = SIEMENS_X55_MACHINE_GET_CLASS(machine);

    g_assert(xmc->board);
    return xmc->board;
}

static void siemens_x55_machine_class_set_board(
    ObjectClass *oc, const SiemensX55BoardDescriptor *board)
{
    SiemensX55MachineClass *xmc = SIEMENS_X55_MACHINE_CLASS(oc);
    MachineClass *mc = MACHINE_CLASS(oc);

    g_assert(board);
    g_assert(board->name);
    g_assert(board->description);
    g_assert(board->sysclk_hz);
    g_assert(board->flash_count > 0);
    g_assert(board->flash_count <= SIEMENS_X55_MAX_FLASH_CHIPS);
    xmc->board = board;
    mc->desc = board->description;
}

static X55AudioContext board_audio_context(void *opaque, bool sync)
{
    PMB7850State *soc = opaque;

    return (X55AudioContext) {
        .tick = sync ? pmb7850_timing_ticks(soc) :
                       pmb7850_peek_timing_ticks(soc),
        .icount = soc->cpu.env.guest_icount,
        .pc = c166_env_get_pc(&soc->cpu.env),
    };
}

static void board_ringer(void *opaque, int n, int level)
{
    SiemensX55MachineState *s = opaque;

    /* Reset establishes a pin level; it is not a carrier transition. */
    if (s->soc.capcom_edge_tick) {
        x55_audio_ringer(s->audio, s->soc.capcom_edge_tick, level);
    }
}

static DeviceState *new_child(MachineState *machine, const char *name,
                              const char *type)
{
    DeviceState *dev = qdev_new(type);
    object_property_add_child(OBJECT(machine), name, OBJECT(dev));
    object_unref(OBJECT(dev));
    return dev;
}

static bool flash_type_allowed(const SiemensX55FlashChipDescriptor *flash,
                               const char *type)
{
    for (unsigned i = 0; i < SIEMENS_X55_MAX_FLASH_TYPES; i++) {
        if (flash->permitted_types[i] &&
            !strcmp(flash->permitted_types[i], type)) {
            return true;
        }
    }
    return false;
}

static char *get_flash_type(Object *obj, Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    return g_strdup(s->flash_type[0]);
}

static char *get_audiodev(Object *obj, Error **errp)
{
    return g_strdup(SIEMENS_X55_MACHINE(obj)->audiodev);
}

static void set_audiodev(Object *obj, const char *value, Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);

    if (s->soc.parent_obj.parent_obj.realized) {
        error_setg(errp, "audiodev must be selected before board realization");
        return;
    }
    g_free(s->audiodev);
    s->audiodev = g_strdup(value);
}

static void get_audio_available(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    uint32_t value = s->audio && s->audiodev && s->audiodev[0];

    visit_type_uint32(v, name, &value, errp);
}

static bool get_ringer_audio(Object *obj, Error **errp)
{
    return SIEMENS_X55_MACHINE(obj)->ringer_audio;
}

static void set_ringer_audio(Object *obj, bool value, Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);

    if (s->xbus) {
        error_setg(errp, "ringer-audio must be selected before realization");
        return;
    }
    s->ringer_audio = value;
}

static bool get_xbus_audio(Object *obj, Error **errp)
{
    return SIEMENS_X55_MACHINE(obj)->xbus_audio;
}

static void set_xbus_audio(Object *obj, bool value, Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);

    if (s->xbus) {
        error_setg(errp, "xbus-audio must be selected before realization");
        return;
    }
    s->xbus_audio = value;
}

static void set_flash_type(Object *obj, const char *value, Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    g_free(s->flash_type[0]);
    s->flash_type[0] = g_strdup(value);
}

static char *get_secondary_flash_type(Object *obj, Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    return g_strdup(s->flash_type[1]);
}

static void set_secondary_flash_type(Object *obj, const char *value,
                                     Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    g_free(s->flash_type[1]);
    s->flash_type[1] = g_strdup(value);
}

/*
 * Host diagnostics join the native producer order, with no guest mutation.
 * Tokens exclude quoting/control characters and fit the host trace reader.
 */
static void set_host_service_event(Object *obj, const char *value, Error **errp)
{
    if (!value || strlen(value) >= 1536 ||
        (!g_str_has_prefix(value, "input_owner ") &&
         !g_str_has_prefix(value, "serial_history ") &&
         !g_str_has_prefix(value, "keypad_deferred_release "))) {
        error_setg(errp, "invalid host service event");
        return;
    }
    for (const char *p = value; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != ' ' && *p != '_' && *p != '-') {
            error_setg(errp, "invalid host service token");
            return;
        }
    }
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    trace_siemens_host_service(s->soc.cpu.env.guest_icount, value);
}

static void get_nor_trace_version(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    uint32_t value = 1;

    visit_type_uint32(v, name, &value, errp);
}

static void get_guest_instructions(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    uint64_t value = s->soc.cpu.env.guest_icount;
    visit_type_uint64(v, name, &value, errp);
}

static void get_snapshot_version(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    uint32_t value = 1;

    visit_type_uint32(v, name, &value, errp);
}

static void get_timing_ticks(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    uint64_t value = s->soc.sysclk ? pmb7850_timing_ticks(&s->soc) : 0;
    visit_type_uint64(v, name, &value, errp);
}

static void get_pc(Object *obj, Visitor *v, const char *name, void *opaque,
                   Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    uint32_t value = c166_env_get_pc(&s->soc.cpu.env);
    visit_type_uint32(v, name, &value, errp);
}

static void get_battery_level(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    uint32_t value = s->battery ? siemens_battery_level(s->battery) :
                                  s->battery_level;
    visit_type_uint32(v, name, &value, errp);
}

static void set_battery_level(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > 100) {
        error_setg(errp, "battery-level must be in the range 0..100");
        return;
    }
    s->battery_level = value;
    if (s->battery) {
        siemens_battery_set_level(s->battery, value, errp);
    }
}

static bool get_battery_charging(Object *obj, Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    return s->battery ? siemens_battery_charging(s->battery) :
                        s->battery_charging;
}

static void set_battery_charging(Object *obj, bool value, Error **errp)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    s->battery_charging = value;
    if (s->battery) {
        siemens_battery_set_charging(s->battery, value);
    }
}

static const char *port_input_name(unsigned port)
{
    switch (port) {
    case 3: return "p3-in";
    case 6: return "p6-in";
    case 7: return "p7-in";
    case 8: return "p8-in";
    default:
        g_assert_not_reached();
    }
}

static const char *port_output_name(unsigned port)
{
    switch (port) {
    case 6: return "p6-out";
    case 7: return "p7-out";
    case 8: return "p8-out";
    default:
        g_assert_not_reached();
    }
}

static void compose_folded_boot(SiemensX55MachineState *s,
                                const SiemensX55BoardDescriptor *board,
                                MemoryRegion **boot)
{
    if (board->flash_count == 1 && board->flash[0].size != 4 * MiB) {
        return;
    }
    memory_region_init(&s->folded_boot, OBJECT(s), "x55.folded-boot", 8 * MiB);
    if (board->flash_count == 1) {
        memory_region_init_alias(&s->boot_alias[0], OBJECT(s),
                                 "x55.boot-low", *boot, 0, 4 * MiB);
        memory_region_init_alias(&s->boot_alias[1], OBJECT(s),
                                 "x55.boot-fold", *boot, 0, 4 * MiB);
    } else {
        MemoryRegion *secondary = sysbus_mmio_get_region(
            SYS_BUS_DEVICE(s->flash[1]), board->flash[1].cs_region);
        memory_region_init_alias(&s->boot_alias[0], OBJECT(s),
                                 "x55.boot-primary", *boot, 0, 4 * MiB);
        memory_region_init_alias(&s->boot_alias[1], OBJECT(s),
                                 "x55.boot-secondary", secondary, 0, 4 * MiB);
    }
    memory_region_add_subregion(&s->folded_boot, 0, &s->boot_alias[0]);
    memory_region_add_subregion(&s->folded_boot, 4 * MiB, &s->boot_alias[1]);
    s->folded_boot_valid = true;
    *boot = &s->folded_boot;
}

static X55NORTraceContext board_flash_context(void *opaque)
{
    PMB7850State *soc = opaque;

    return (X55NORTraceContext) {
        .tick = pmb7850_peek_timing_ticks(soc),
        .icount = soc->cpu.env.guest_icount,
        .pc = c166_env_get_pc(&soc->cpu.env),
    };
}

static void board_init(MachineState *machine)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(machine);
    const SiemensX55BoardDescriptor *board = siemens_x55_board(machine);
    const bool ringer_audio = board->audio_ringer_present && s->ringer_audio;
    const bool xbus_audio = board->audio_xbus_v1 && s->xbus_audio;
    DeviceState *soc;
    MemoryRegion *boot;

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_PMB7850_SOC);
    soc = DEVICE(&s->soc);
    s->sysclk = clock_new(OBJECT(machine), "sysclk");
    clock_set_hz(s->sysclk, board->sysclk_hz);

    for (unsigned i = 0; i < board->flash_count; i++) {
        const SiemensX55FlashChipDescriptor *flash = &board->flash[i];
        const char *type = s->flash_type[i] ?: flash->default_type;
        DriveInfo *dinfo;

        if (!flash_type_allowed(flash, type)) {
            error_setg(&error_fatal, "%s does not permit flash type '%s'",
                       board->name, type);
            return;
        }
        s->flash[i] = new_child(machine, flash->name, type);
        x55_nor_trace_configure(OBJECT(s->flash[i]), i,
                                board_flash_context, &s->soc);
        if (object_property_find(OBJECT(s->flash[i]), "protected-start")) {
            qdev_prop_set_uint32(s->flash[i], "protected-start",
                                 flash->protected_start);
            qdev_prop_set_uint32(s->flash[i], "protected-size",
                                 flash->protected_size);
            qdev_prop_set_bit(s->flash[i], "secsi-factory-locked",
                              flash->secsi_factory_locked);
        }
        if (object_property_find(OBJECT(s->flash[i]), "instruction-timing")) {
            qdev_prop_set_bit(s->flash[i], "instruction-timing", true);
            qdev_connect_clock_in(s->flash[i], "sysclk", s->sysclk);
        }
        dinfo = drive_get(IF_PFLASH, 0, i);
        if (dinfo) {
            qdev_prop_set_drive(s->flash[i], "drive",
                                blk_by_legacy_dinfo(dinfo));
        }
        if (!sysbus_realize(SYS_BUS_DEVICE(s->flash[i]), &error_fatal)) {
            return;
        }
    }
    boot = sysbus_mmio_get_region(SYS_BUS_DEVICE(s->flash[0]),
                                  board->flash[0].boot_region);
    compose_folded_boot(s, board, &boot);

    s->ram = new_child(machine, "ram", board->ram.type);
    g_assert(siemens_x55_ram_get_size(s->ram) == board->ram.size);
    qdev_prop_set_string(s->ram, "region-name", board->ram.name);
    if (!sysbus_realize(SYS_BUS_DEVICE(s->ram), &error_fatal)) {
        return;
    }

    object_property_set_link(OBJECT(soc), "memory",
                             OBJECT(get_system_memory()), &error_abort);
    if (ringer_audio) {
        qdev_prop_set_uint32(soc, "capcom-observe-mask",
                             1u << board->audio_ringer_channel);
    }
    qdev_prop_set_bit(soc, "irq55-cc23-connected",
        board->irq55_sources & SIEMENS_X55_IRQ55_CC23);
    qdev_prop_set_bit(soc, "ssc0-unselected-rx-high",
                      board->ssc0_unselected_rx_high);
    object_property_set_link(OBJECT(soc), "boot-memory", OBJECT(boot),
                             &error_abort);
    for (unsigned cs = 0; cs < 5; cs++) {
        g_autofree char *name = g_strdup_printf("cs%u-memory", cs);
        MemoryRegion *region = machine->ram;

        if (cs == board->ram.cs) {
            region = siemens_x55_ram_get_memory(s->ram);
        }
        for (unsigned i = 0; i < board->flash_count; i++) {
            if (cs == board->flash[i].cs) {
                region = sysbus_mmio_get_region(SYS_BUS_DEVICE(s->flash[i]),
                                                 board->flash[i].cs_region);
            }
        }
        object_property_set_link(OBJECT(soc), name, OBJECT(region),
                                 &error_abort);
    }
    for (unsigned i = 1; i <= 6; i++) {
        g_autofree char *name = g_strdup_printf("xbus%u-memory", i);
        object_property_set_link(OBJECT(soc), name, OBJECT(machine->ram),
                                 &error_abort);
    }
    qdev_connect_clock_in(soc, "sysclk", s->sysclk);
    if (board->serial.available) {
        qdev_prop_set_chr(soc, "serial0", serial_hd(0));
    }
    for (unsigned i = 0; i < board->reset_seed_count; i++) {
        if (!pmb7850_add_reset_seed(&s->soc,
                                    board->reset_seeds[i].address,
                                    board->reset_seeds[i].value,
                                    &error_fatal)) {
            return;
        }
    }
    if (board->battery) {
        qdev_prop_set_uint8(soc, "adc-result-word",
                            board->battery->adc_result_word);
    }
    if (!pmb7850_set_instruction_timing(&s->soc, true, &error_fatal) ||
        !sysbus_realize(SYS_BUS_DEVICE(soc), &error_fatal)) {
        return;
    }

    s->xbus = new_child(machine, "product-xbus", TYPE_SIEMENS_XBUS_ENDPOINT);
    if (ringer_audio || xbus_audio) {
        s->audio = new_child(machine, "audio", TYPE_X55_AUDIO);
        if (s->audiodev && s->audiodev[0]) {
            qdev_prop_set_string(s->audio, "audiodev", s->audiodev);
        }
        x55_audio_configure(s->audio, board_audio_context, &s->soc, true);
        qdev_connect_clock_in(s->audio, "sysclk", s->sysclk);
        if (!qdev_realize(s->audio, NULL, &error_fatal)) {
            return;
        }
        if (xbus_audio) {
            object_property_set_link(OBJECT(s->xbus), "audio", OBJECT(s->audio),
                                     &error_abort);
        }
        if (ringer_audio) {
            s->ringer = qemu_allocate_irq(board_ringer, s, 0);
            qdev_connect_gpio_out_named(soc, "capcom-out",
                                        board->audio_ringer_channel, s->ringer);
        }
    }
    qdev_prop_set_uint16(s->xbus, "device-id", board->xbus_device_id);
    object_property_set_link(OBJECT(s->xbus), "memory",
                             OBJECT(get_system_memory()), &error_abort);
    qdev_connect_clock_in(s->xbus, "sysclk", s->sysclk);
    if (!siemens_xbus_set_instruction_timing(s->xbus, true, &error_fatal)) {
        return;
    }
    qdev_connect_gpio_out_named(s->xbus, "irq", 0,
        qdev_get_gpio_in_named(soc, "ext-irq", board->xbus_irq));
    if (!sysbus_realize(SYS_BUS_DEVICE(s->xbus), &error_fatal)) {
        return;
    }
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->xbus), 0, 0xe800, 40);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->xbus), 1, 0xec12, 40);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->xbus), 2, 0xef3a, 40);
    if (xbus_audio) {
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->xbus), 3, 0xe836, 40);
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->xbus), 4, 0xeb80, 40);
    }

    s->display = new_child(machine, "display", board->display.type);
    qdev_prop_set_uint32(s->display, "panel-width",
                         board->display.panel_width);
    qdev_prop_set_uint32(s->display, "panel-height",
                         board->display.panel_height);
    qdev_prop_set_uint32(s->display, "panel-origin-x",
                         board->display.panel_origin_x);
    qdev_prop_set_bit(s->display, "panel-mirror-x",
                      board->display.panel_mirror_x);
    qdev_prop_set_bit(s->display, "panel-mirror-y",
                      board->display.panel_mirror_y);
    qdev_prop_set_uint32(s->display, "scale", 1);
    if (!qdev_realize(s->display, BUS(s->soc.ssc0), &error_fatal)) {
        return;
    }
    qdev_connect_gpio_out_named(soc, "p3-out", 11,
        qdev_get_gpio_in_named(s->display, SSI_GPIO_CS, 0));
    qdev_connect_gpio_out_named(soc, port_output_name(board->display.dc_port),
        board->display.dc_bit, qdev_get_gpio_in_named(s->display, "dc", 0));
    qdev_connect_gpio_out_named(soc, "ssc0-frame-start", 0,
        qdev_get_gpio_in_named(s->display, "frame-start", 0));
    if (!strcmp(board->display.type, TYPE_PCF8813)) {
        qdev_connect_gpio_out_named(soc, "p7-out", 5,
            qdev_get_gpio_in_named(s->display, "serial-clock", 0));
        qdev_connect_gpio_out_named(soc, "p7-out", 7,
            qdev_get_gpio_in_named(s->display, "serial-data", 0));
        qdev_connect_gpio_out_named(soc, "p7-dir", 7,
            qdev_get_gpio_in_named(s->display,
                                   "serial-data-direction", 0));
        qdev_connect_gpio_out_named(s->display, "serial-data-out", 0,
            qdev_get_gpio_in_named(soc, "p7-in", 7));
    }
    if (board->display.reset_capcom_channel >= 0) {
        unsigned channel = board->display.reset_capcom_channel;
        qdev_connect_gpio_out_named(soc, "capcom-out", channel,
            qdev_get_gpio_in_named(s->display, "reset-level", 0));
        qdev_connect_gpio_out_named(soc, "capcom-driven", channel,
            qdev_get_gpio_in_named(s->display, "reset-driven", 0));
    }

    s->keypad = new_child(machine, "keypad", TYPE_SIEMENS_KEYPAD);
    if (!siemens_keypad_configure(s->keypad, board->keypad, &error_fatal)) {
        return;
    }
    if (board->irq55_sources & SIEMENS_X55_IRQ55_KEYPAD_ACTIVITY) {
        qdev_connect_gpio_out_named(s->keypad, "activity", 0,
            qdev_get_gpio_in_named(soc, "ext-irq", 0x37));
    }
    qdev_connect_gpio_out_named(soc, "keypad-matrix-select", 0,
        qdev_get_gpio_in_named(s->keypad, "matrix-select", 0));
    if (!sysbus_realize(SYS_BUS_DEVICE(s->keypad), &error_fatal)) {
        return;
    }
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->keypad), 0, 0xef1a, 40);

    if (board->battery) {
        s->battery = new_child(machine, "battery", TYPE_SIEMENS_X55_BATTERY);
        if (!siemens_battery_configure(s->battery, board->battery,
                                       &error_fatal) ||
            !siemens_battery_set_level(s->battery, s->battery_level,
                                       &error_fatal)) {
            return;
        }
        siemens_battery_set_charging(s->battery, s->battery_charging);
        qdev_connect_gpio_out_named(s->battery, "adc-raw",
            board->battery->battery_channel,
            qdev_get_gpio_in_named(soc, "adc-raw",
                                   board->battery->battery_channel));
        qdev_connect_gpio_out_named(s->battery, "adc-raw",
            board->battery->reference_channel,
            qdev_get_gpio_in_named(soc, "adc-raw",
                                   board->battery->reference_channel));
        qdev_connect_gpio_out_named(s->battery, "charging", 0,
            qdev_get_gpio_in_named(s->xbus, "charging", 0));
        if (!qdev_realize(s->battery, NULL, &error_fatal)) {
            return;
        }
    }
    if (board->twi.available) {
        static const uint8_t zero_registers[56];
        SiemensTWIConfig config = { "register-file", 0x31, zero_registers,
                                    board->twi.register_count };
        s->twi = new_child(machine, "twi", TYPE_SIEMENS_TWI);
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
    for (unsigned i = 0; i < board->port_input_count; i++) {
        const SiemensX55PortInputDescriptor *input = &board->port_inputs[i];
        for (unsigned bit = 0; bit < 16; bit++) {
            if (input->mask & (1u << bit)) {
                qemu_set_irq(qdev_get_gpio_in_named(soc,
                             port_input_name(input->port), bit),
                             !!(input->value & (1u << bit)));
            }
        }
    }
    if (board->serial.available) {
        qemu_set_irq(qdev_get_gpio_in_named(soc,
                     port_input_name(board->serial.port), board->serial.bit),
                     board->serial.idle_high);
    }
}

static void board_instance_init(Object *obj)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    const SiemensX55BoardDescriptor *board = siemens_x55_board(MACHINE(obj));

    s->battery_level = 100;
    s->ringer_audio = true;
    s->xbus_audio = true;
    /* Opt-outs only: the board descriptor remains the qualification gate. */
    object_property_add_bool(obj, "ringer-audio", get_ringer_audio,
                             set_ringer_audio);
    object_property_add_bool(obj, "xbus-audio", get_xbus_audio, set_xbus_audio);
    object_property_add_str(obj, "host-service-event", NULL,
                            set_host_service_event);
    object_property_add_str(obj, "audiodev", get_audiodev, set_audiodev);
    object_property_add(obj, "audio-available", "uint32",
                        get_audio_available, NULL, NULL, NULL);
    object_property_add_str(obj, "flash-type", get_flash_type, set_flash_type);
    object_property_add_str(obj, "secondary-flash-type",
                            get_secondary_flash_type,
                            set_secondary_flash_type);
    object_property_add(obj, "guest-instructions", "uint64",
                        get_guest_instructions, NULL, NULL, NULL);
    object_property_add(obj, "nor-trace-version", "uint32",
                        get_nor_trace_version, NULL, NULL, NULL);
    object_property_add(obj, "snapshot-version", "uint32",
                        get_snapshot_version, NULL, NULL, NULL);
    object_property_add(obj, "timing-ticks", "uint64",
                        get_timing_ticks, NULL, NULL, NULL);
    object_property_add(obj, "pc", "uint32", get_pc, NULL, NULL, NULL);
    if (board->battery) {
        object_property_add(obj, "battery-level", "uint32",
                            get_battery_level, set_battery_level,
                            NULL, NULL);
        object_property_add_bool(obj, "battery-charging",
                                 get_battery_charging,
                                 set_battery_charging);
    }
}

static void board_instance_finalize(Object *obj)
{
    SiemensX55MachineState *s = SIEMENS_X55_MACHINE(obj);
    g_free(s->flash_type[0]);
    g_free(s->flash_type[1]);
    g_free(s->audiodev);
    qemu_free_irq(s->ringer);
}

static void concrete_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    siemens_x55_machine_class_set_board(oc, data);
    mc->init = board_init;
}

static void siemens_x55_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->default_cpu_type = TYPE_C166S_V1_CPU;
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
    mc->default_ram_size = 0;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

static const TypeInfo siemens_x55_machine_type = {
    .name = TYPE_SIEMENS_X55_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(SiemensX55MachineState),
    .instance_align = __alignof__(SiemensX55MachineState),
    .instance_init = board_instance_init,
    .instance_finalize = board_instance_finalize,
    .class_size = sizeof(SiemensX55MachineClass),
    .class_init = siemens_x55_machine_class_init,
    .abstract = true,
};

static void register_types(void)
{
    type_register_static(&siemens_x55_machine_type);
    for (unsigned i = 0; i < siemens_x55_device_count; i++) {
        const SiemensX55BoardDescriptor *board = siemens_x55_devices[i];
        const TypeInfo type = {
            .name = g_strdup_printf(MACHINE_TYPE_NAME("siemens-%s"),
                                    board->name),
            .parent = TYPE_SIEMENS_X55_MACHINE,
            .class_init = concrete_machine_class_init,
            .class_data = (void *)board,
        };

        type_register_static(&type);
    }
}

type_init(register_types)
