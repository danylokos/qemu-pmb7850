/*
 * Firmware-calibrated Siemens product/XBUS endpoint.
 *
 * The register behavior is measured firmware compatibility, not a claim that
 * the physical device has been identified.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "exec/icount.h"
#include "hw/c166/siemens_x55/xbus.h"
#include "hw/audio/x55_audio.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "system/cpus.h"
#include "system/memory.h"
#include "trace/trace-hw_c166.h"

#define XBUS_ID_BASE       0xe800
#define XBUS_STATUS        0xe806
#define XBUS_CONTROL       0xec10
#define XBUS_MAILBOX_BASE  0xec12
#define XBUS_MAILBOX       0xec12
#define XBUS_COMMAND       0xec14
#define XBUS_RESULT        0xec16
#define XBUS_LAUNCH_BASE   0xef3a
#define XBUS_REQUEST       0xec80
#define XBUS_INDEX         0xec82
#define XBUS_TRAILER       0xec84

#define XBUS_PENDING       0x01
#define XBUS_SYNC_COMPLETE 0x02
#define XBUS_COMPLETE      0x04
#define XBUS_READY         0x10
#define XBUS_DONE          0x8000
#define XBUS_IRQ_DELAY_CYCLES 32
/*
 * Observed EC10 modes: 1700 synchronous, 1701 queued, 1704 legacy IRQ.
 * These flags are firmware compatibility calibration, not silicon bit names.
 */
#define XBUS_INFERRED_IRQ_MODE_MASK 0x0005

typedef struct SiemensXBusRegion {
    struct SiemensXBusEndpointState *endpoint;
    hwaddr base;
    uint8_t *data;
    unsigned size;
} SiemensXBusRegion;

struct SiemensXBusEndpointState {
    SysBusDevice parent_obj;

    MemoryRegion *memory;
    DeviceState *audio;
    AddressSpace memory_as;
    bool memory_as_valid;
    Clock *sysclk;
    qemu_irq irq;
    QEMUTimer timer;

    uint16_t device_id;
    uint8_t irq_vector;
    bool charging;
    bool instruction_timing;
    bool pending;
    uint16_t control; /* Sampled at acceptance; independent of timer state. */
    uint64_t transaction;

    uint8_t id_status[8];
    uint8_t mailbox[6];
    uint8_t launch[2];
    uint8_t audio_command[6];
    uint8_t audio_packet[128];
    MemoryRegion regions[5];
    SiemensXBusRegion views[5];
};

static uint64_t xbus_trace_icount(void)
{
    return icount_enabled() ? icount_get_raw() : 0;
}

static uint32_t xbus_trace_pc(void)
{
    CPUClass *cc;

    if (!current_cpu) {
        return 0;
    }
    cc = CPU_GET_CLASS(current_cpu);
    return cc->get_pc ? cc->get_pc(current_cpu) : 0;
}

static uint16_t xbus_load_word(const uint8_t *p)
{
    return lduw_le_p(p);
}

static void xbus_store_word(uint8_t *p, uint16_t value)
{
    stw_le_p(p, value);
}

static uint16_t xbus_external_word(SiemensXBusEndpointState *s,
                                   hwaddr addr)
{
    uint8_t buf[2] = { 0, 0 };

    if (s->memory_as_valid) {
        address_space_read(&s->memory_as, addr, MEMTXATTRS_UNSPECIFIED,
                           buf, sizeof(buf));
    }
    return lduw_le_p(buf);
}

static uint8_t xbus_result(SiemensXBusEndpointState *s)
{
    uint16_t command = xbus_load_word(s->mailbox + XBUS_COMMAND -
                                                   XBUS_MAILBOX_BASE);
    uint16_t request = xbus_external_word(s, XBUS_REQUEST);
    uint16_t index = xbus_external_word(s, XBUS_INDEX);
    uint16_t trailer = xbus_external_word(s, XBUS_TRAILER);

    if (command == 2 && request == 0x0162 && trailer == 0x0163) {
        if (index == 0) {
            return 1;
        }
        if (index == 2) {
            return s->charging ? XBUS_READY : 0;
        }
    }
    return XBUS_READY;
}

static void xbus_trace_phase(SiemensXBusEndpointState *s, const char *phase,
                            bool irq)
{
    trace_siemens_xbus_mailbox(xbus_trace_icount(), xbus_trace_pc(), phase,
                              s->transaction, xbus_load_word(s->mailbox),
                              xbus_load_word(s->mailbox + 4), irq, s->control,
                              !!(s->control & XBUS_INFERRED_IRQ_MODE_MASK));
}

static void xbus_cancel(SiemensXBusEndpointState *s, const char *phase)
{
    s->pending = false;
    timer_del(&s->timer);
    xbus_trace_phase(s, phase, false);
}

static void xbus_promote(void *opaque)
{
    SiemensXBusEndpointState *s = opaque;
    uint16_t value = xbus_load_word(s->mailbox);

    if (!s->pending || !(s->control & XBUS_INFERRED_IRQ_MODE_MASK)) {
        return;
    }
    value = (value & ~(XBUS_PENDING | XBUS_SYNC_COMPLETE)) | XBUS_COMPLETE;
    xbus_store_word(s->mailbox, value);
    s->pending = false;
    qemu_set_irq(s->irq, 1);
    qemu_set_irq(s->irq, 0);
    xbus_trace_phase(s, "irq-promoted", true);
}

static void xbus_accept(SiemensXBusEndpointState *s, uint16_t before)
{
    uint16_t ready = (before & ~(XBUS_PENDING | XBUS_COMPLETE)) |
                     XBUS_SYNC_COMPLETE;

    s->control = xbus_external_word(s, XBUS_CONTROL);
    xbus_store_word(s->mailbox, ready);
    xbus_store_word(s->mailbox + 4, xbus_result(s));
    s->transaction++;
    s->pending = true;
    /*
     * A preempted synchronous caller still owns its completion. Elapsed
     * time cannot turn that request into queued work. Retain the calibrated
     * delivery delay only for a request submitted in an interrupt mode.
     */
    if (s->control & XBUS_INFERRED_IRQ_MODE_MASK) {
        uint64_t delay = s->instruction_timing ?
                         icount_to_ns(XBUS_IRQ_DELAY_CYCLES) :
                         clock_ticks_to_ns(s->sysclk, XBUS_IRQ_DELAY_CYCLES);

        timer_mod_ns(&s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
    }
    xbus_trace_phase(s, "sync-ready", false);
}

static uint8_t xbus_read_byte(SiemensXBusRegion *view, hwaddr offset)
{
    SiemensXBusEndpointState *s = view->endpoint;
    hwaddr addr = view->base + offset;

    if (s->audio && (view->base == 0xe836 || view->base == 0xeb80)) {
        return x55_audio_read(s->audio, addr);
    }

    if (addr == XBUS_ID_BASE) {
        return s->device_id;
    }
    if (addr == XBUS_ID_BASE + 1) {
        return s->device_id >> 8;
    }
    if (addr == XBUS_STATUS + 1) {
        uint16_t launch = xbus_load_word(s->launch);

        return view->data[offset] | (launch & 1 ? XBUS_DONE >> 8 : 0);
    }
    return view->data[offset];
}

static uint64_t xbus_read(void *opaque, hwaddr offset, unsigned size)
{
    SiemensXBusRegion *view = opaque;
    uint64_t value = 0;

    for (unsigned i = 0; i < size; i++) {
        value |= (uint64_t)xbus_read_byte(view, offset + i) << (8 * i);
    }
    trace_siemens_xbus_access(xbus_trace_icount(), xbus_trace_pc(), false,
                              view->base + offset, size, value);
    return value;
}

static void xbus_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    SiemensXBusRegion *view = opaque;
    SiemensXBusEndpointState *s = view->endpoint;
    hwaddr addr = view->base + offset;

    if (s->audio && (view->base == 0xe836 || view->base == 0xeb80)) {
        x55_audio_write(s->audio, addr, value, size);
        trace_siemens_xbus_access(xbus_trace_icount(), xbus_trace_pc(), true,
                                  addr, size, value);
        return;
    }

    for (unsigned i = 0; i < size; i++) {
        view->data[offset + i] = value >> (8 * i);
    }
    if (s->audio && addr == XBUS_LAUNCH_BASE) {
        x55_audio_write(s->audio, addr, xbus_load_word(s->launch), 2);
    }
    if (addr <= XBUS_STATUS + 1 && addr + size > XBUS_STATUS) {
        s->launch[0] &= ~1;
    }
    if (view->base == XBUS_MAILBOX_BASE &&
        addr <= XBUS_MAILBOX + 1 && addr + size > XBUS_MAILBOX) {
        uint16_t mailbox = xbus_load_word(s->mailbox);

        if (s->pending && !(mailbox & XBUS_SYNC_COMPLETE)) {
            xbus_cancel(s, "sync-consumed");
        }
        if (mailbox & XBUS_PENDING) {
            if (s->pending) {
                xbus_store_word(s->mailbox, mailbox & ~XBUS_PENDING);
            } else {
                xbus_accept(s, mailbox);
            }
        }
    }
    trace_siemens_xbus_access(xbus_trace_icount(), xbus_trace_pc(), true,
                              addr, size, value);
}

static const MemoryRegionOps xbus_ops = {
    .read = xbus_read,
    .write = xbus_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
};

static void xbus_charging_input(void *opaque, int n, int level)
{
    SiemensXBusEndpointState *s = opaque;

    s->charging = !!level;
}

static void xbus_reset_hold(Object *obj, ResetType type)
{
    SiemensXBusEndpointState *s = SIEMENS_XBUS_ENDPOINT(obj);

    memset(s->id_status, 0, sizeof(s->id_status));
    memset(s->mailbox, 0, sizeof(s->mailbox));
    memset(s->launch, 0, sizeof(s->launch));
    s->pending = false;
    s->control = 0;
    s->transaction = 0;
    timer_del(&s->timer);
    qemu_set_irq(s->irq, 0);
}

static void xbus_realize(DeviceState *dev, Error **errp)
{
    SiemensXBusEndpointState *s = SIEMENS_XBUS_ENDPOINT(dev);

    if (!s->memory) {
        error_setg(errp, "memory link must be set");
        return;
    }
    if (!clock_has_source(s->sysclk)) {
        error_setg(errp, "sysclk must be connected");
        return;
    }
    address_space_init(&s->memory_as, s->memory,
                       "siemens-xbus-endpoint-memory");
    s->memory_as_valid = true;
    device_cold_reset(dev);
}

static void xbus_unrealize(DeviceState *dev)
{
    SiemensXBusEndpointState *s = SIEMENS_XBUS_ENDPOINT(dev);

    if (s->memory_as_valid) {
        address_space_destroy(&s->memory_as);
        s->memory_as_valid = false;
    }
}

bool siemens_xbus_set_instruction_timing(DeviceState *dev, bool enabled,
                                         Error **errp)
{
    SiemensXBusEndpointState *s = SIEMENS_XBUS_ENDPOINT(dev);

    if (dev->realized) {
        error_setg(errp, "instruction timing must be set before realization");
        return false;
    }
    s->instruction_timing = enabled;
    return true;
}

static void xbus_init(Object *obj)
{
    SiemensXBusEndpointState *s = SIEMENS_XBUS_ENDPOINT(obj);
    DeviceState *dev = DEVICE(obj);
    static const hwaddr bases[] = {
        XBUS_ID_BASE, XBUS_MAILBOX_BASE, XBUS_LAUNCH_BASE,
        0xe836, 0xeb80,
    };
    static const unsigned sizes[] = { 8, 6, 2, 6, 128 };
    uint8_t *data[] = { s->id_status, s->mailbox, s->launch,
                       s->audio_command, s->audio_packet };

    s->sysclk = qdev_init_clock_in(dev, "sysclk", NULL, NULL, 0);
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, xbus_promote, s);
    qdev_init_gpio_in_named(dev, xbus_charging_input, "charging", 1);
    qdev_init_gpio_out_named(dev, &s->irq, "irq", 1);
    for (unsigned i = 0; i < ARRAY_SIZE(s->regions); i++) {
        g_autofree char *name = g_strdup_printf("siemens-xbus-%06x",
                                                (unsigned)bases[i]);

        s->views[i] = (SiemensXBusRegion) {
            .endpoint = s,
            .base = bases[i],
            .data = data[i],
            .size = sizes[i],
        };
        memory_region_init_io(&s->regions[i], obj, &xbus_ops, &s->views[i],
                              name, sizes[i]);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->regions[i]);
    }
}

static const Property xbus_properties[] = {
    DEFINE_PROP_LINK("audio", SiemensXBusEndpointState, audio,
                     TYPE_X55_AUDIO, DeviceState *),
    DEFINE_PROP_LINK("memory", SiemensXBusEndpointState, memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT16("device-id", SiemensXBusEndpointState, device_id,
                       0x1202),
    DEFINE_PROP_UINT8("irq-vector", SiemensXBusEndpointState, irq_vector,
                      0x50),
};

static const VMStateDescription vmstate_xbus = {
    .name = "x55/xbus",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER(timer, SiemensXBusEndpointState),
        VMSTATE_BOOL(charging, SiemensXBusEndpointState),
        VMSTATE_BOOL(pending, SiemensXBusEndpointState),
        VMSTATE_UINT16(control, SiemensXBusEndpointState),
        VMSTATE_UINT64(transaction, SiemensXBusEndpointState),
        VMSTATE_BUFFER(id_status, SiemensXBusEndpointState),
        VMSTATE_BUFFER(mailbox, SiemensXBusEndpointState),
        VMSTATE_BUFFER(launch, SiemensXBusEndpointState),
        VMSTATE_PARTIAL_BUFFER(audio_command, SiemensXBusEndpointState, 2),
        VMSTATE_PARTIAL_BUFFER(audio_packet, SiemensXBusEndpointState, 48),
        VMSTATE_END_OF_LIST()
    },
};

static void xbus_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_xbus;
    dc->desc = "Siemens firmware-calibrated product/XBUS endpoint";
    dc->realize = xbus_realize;
    dc->unrealize = xbus_unrealize;
    device_class_set_props(dc, xbus_properties);
    rc->phases.hold = xbus_reset_hold;
}

static const TypeInfo xbus_type_info = {
    .name = TYPE_SIEMENS_XBUS_ENDPOINT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SiemensXBusEndpointState),
    .instance_align = __alignof__(SiemensXBusEndpointState),
    .instance_init = xbus_init,
    .class_init = xbus_class_init,
};

static void xbus_register_types(void)
{
    type_register_static(&xbus_type_info);
}

type_init(xbus_register_types)
