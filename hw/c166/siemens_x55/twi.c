/*
 * GPIO-observed open-drain TWI register-file slave.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/c166/siemens_x55/twi.h"
#include "hw/core/irq.h"
#include "trace/trace-hw_c166.h"

typedef enum TWIPhase {
    TWI_IDLE, TWI_RX_BITS, TWI_ACK_ASSERT, TWI_ACK_HIGH, TWI_ACK_RELEASE,
    TWI_TX_BITS, TWI_TX_RELEASE, TWI_MASTER_ACK, TWI_MASTER_ACK_RELEASE,
} TWIPhase;
typedef enum TWIRxKind {
    TWI_RX_ADDRESS, TWI_RX_REGISTER, TWI_RX_DATA,
} TWIRxKind;
typedef enum TWIAfterAck {
    TWI_AFTER_IDLE, TWI_AFTER_RX, TWI_AFTER_TX,
} TWIAfterAck;

typedef struct SiemensTWIState {
    DeviceState parent_obj;
    SiemensTWIConfig config;
    char *model;
    uint8_t initial[SIEMENS_TWI_MAX_REGISTERS];
    uint8_t registers[SIEMENS_TWI_MAX_REGISTERS];
    TWIPhase phase;
    TWIRxKind rx_kind;
    TWIAfterAck after_ack;
    uint8_t rx_byte, tx_byte, bit_count, register_pointer;
    bool selected, read, ack, master_ack, slave_sda_low;
    bool prev_scl, prev_sda, lines_initialized;
    uint64_t starts, stops, register_reads, register_writes;
    qemu_irq scl_in, sda_in;
    bool configured;
} SiemensTWIState;

OBJECT_DECLARE_SIMPLE_TYPE(SiemensTWIState, SIEMENS_TWI)

bool siemens_twi_configure(DeviceState *dev, const SiemensTWIConfig *config,
                           Error **errp)
{
    SiemensTWIState *s = SIEMENS_TWI(dev);

    if (dev->realized) {
        error_setg(errp, "TWI responder must be configured before realize");
        return false;
    }
    if (!config || !config->model || !config->model[0] ||
        config->address > 0x7f || !config->register_count ||
        config->register_count > SIEMENS_TWI_MAX_REGISTERS ||
        !config->registers) {
        error_setg(errp, "invalid TWI register-file configuration");
        return false;
    }
    s->config = *config;
    s->model = g_strdup(config->model);
    s->config.model = s->model;
    memcpy(s->initial, config->registers, config->register_count);
    s->configured = true;
    return true;
}

static void twi_receive(SiemensTWIState *s, TWIRxKind kind)
{
    s->phase = TWI_RX_BITS;
    s->rx_kind = kind;
    s->rx_byte = s->bit_count = 0;
}

static void twi_transmit(SiemensTWIState *s)
{
    s->tx_byte = s->register_pointer < s->config.register_count ?
                 s->registers[s->register_pointer] : 0xff;
    s->bit_count = 0;
    s->phase = TWI_TX_BITS;
    s->slave_sda_low = !(s->tx_byte & 0x80);
    s->register_reads++;
    trace_siemens_twi(s->model, "read", s->config.address, true,
                      s->register_pointer, s->tx_byte, true);
}

static void twi_start(SiemensTWIState *s)
{
    s->slave_sda_low = s->selected = s->read = false;
    s->starts++;
    twi_receive(s, TWI_RX_ADDRESS);
    trace_siemens_twi(s->model, "start", 0, false, 0, 0, false);
}

static void twi_stop(SiemensTWIState *s)
{
    s->slave_sda_low = s->selected = s->read = false;
    s->phase = TWI_IDLE;
    s->stops++;
    trace_siemens_twi(s->model, "stop", 0, false, 0, 0, false);
}

static void twi_received_byte(SiemensTWIState *s)
{
    uint8_t value = s->rx_byte;

    s->ack = false;
    s->after_ack = TWI_AFTER_IDLE;
    if (s->rx_kind == TWI_RX_ADDRESS) {
        uint8_t address = value >> 1;

        s->read = value & 1;
        s->selected = address == s->config.address;
        s->ack = s->selected;
        if (s->selected) {
            s->after_ack = s->read ? TWI_AFTER_TX : TWI_AFTER_RX;
        }
        trace_siemens_twi(s->model, "address", address, s->read,
                          0, 0, s->ack);
    } else if (s->rx_kind == TWI_RX_REGISTER) {
        s->register_pointer = value;
        s->ack = value < s->config.register_count;
        s->after_ack = s->ack ? TWI_AFTER_RX : TWI_AFTER_IDLE;
        trace_siemens_twi(s->model, "pointer", 0, false, value, 0, s->ack);
    } else {
        uint8_t reg = s->register_pointer;

        s->ack = reg < s->config.register_count;
        if (s->ack) {
            s->registers[reg] = value;
            s->register_pointer++;
            s->register_writes++;
        }
        s->after_ack = s->ack ? TWI_AFTER_RX : TWI_AFTER_IDLE;
        trace_siemens_twi(s->model, "write", 0, false, reg, value, s->ack);
    }
    s->phase = TWI_ACK_ASSERT;
}

static void twi_rising(SiemensTWIState *s, bool sda)
{
    switch (s->phase) {
    case TWI_RX_BITS:
        s->rx_byte = (s->rx_byte << 1) | sda;
        if (++s->bit_count == 8) {
            twi_received_byte(s);
        }
        break;
    case TWI_ACK_HIGH:
        s->phase = TWI_ACK_RELEASE;
        break;
    case TWI_TX_BITS:
        if (++s->bit_count == 8) {
            s->phase = TWI_TX_RELEASE;
        }
        break;
    case TWI_MASTER_ACK:
        s->master_ack = !sda;
        s->phase = TWI_MASTER_ACK_RELEASE;
        trace_siemens_twi(s->model, "master-ack", 0, false,
                          s->register_pointer, 0, s->master_ack);
        break;
    default:
        break;
    }
}

static void twi_falling(SiemensTWIState *s)
{
    switch (s->phase) {
    case TWI_ACK_ASSERT:
        s->slave_sda_low = s->ack;
        s->phase = TWI_ACK_HIGH;
        break;
    case TWI_ACK_RELEASE:
        s->slave_sda_low = false;
        if (!s->ack || s->after_ack == TWI_AFTER_IDLE) {
            s->phase = TWI_IDLE;
        } else if (s->after_ack == TWI_AFTER_TX) {
            twi_transmit(s);
        } else {
            twi_receive(s, s->rx_kind == TWI_RX_ADDRESS ?
                        TWI_RX_REGISTER : TWI_RX_DATA);
        }
        break;
    case TWI_TX_BITS:
        s->slave_sda_low = !(s->tx_byte & (0x80 >> s->bit_count));
        break;
    case TWI_TX_RELEASE:
        s->slave_sda_low = false;
        s->phase = TWI_MASTER_ACK;
        break;
    case TWI_MASTER_ACK_RELEASE:
        if (s->master_ack) {
            s->register_pointer++;
            twi_transmit(s);
        } else {
            s->selected = false;
            s->phase = TWI_IDLE;
        }
        break;
    default:
        break;
    }
}

static void twi_port_state(void *opaque, int n, int level)
{
    SiemensTWIState *s = opaque;
    uint32_t state = level;
    uint16_t data = state;
    uint16_t dir = state >> 16;
    bool scl = !((dir & (1 << 12)) && !(data & (1 << 12)));
    bool master_sda_low = (dir & (1 << 13)) && !(data & (1 << 13));
    bool sda = !(master_sda_low || s->slave_sda_low);

    if (s->lines_initialized && scl && s->prev_sda != sda) {
        if (s->prev_sda && !sda) {
            twi_start(s);
        } else {
            twi_stop(s);
        }
    }
    if (s->lines_initialized && !s->prev_scl && scl) {
        twi_rising(s, sda);
    } else if (s->lines_initialized && s->prev_scl && !scl) {
        twi_falling(s);
    }
    sda = !(master_sda_low || s->slave_sda_low);
    s->prev_scl = scl;
    s->prev_sda = sda;
    s->lines_initialized = true;
    qemu_set_irq(s->scl_in, scl);
    qemu_set_irq(s->sda_in, sda);
}

static void twi_reset(DeviceState *dev)
{
    SiemensTWIState *s = SIEMENS_TWI(dev);

    memcpy(s->registers, s->initial, s->config.register_count);
    s->phase = TWI_IDLE;
    s->rx_kind = TWI_RX_ADDRESS;
    s->after_ack = TWI_AFTER_IDLE;
    s->rx_byte = s->tx_byte = s->bit_count = s->register_pointer = 0;
    s->selected = s->read = s->ack = s->master_ack = false;
    s->slave_sda_low = false;
    s->prev_scl = s->prev_sda = true;
    s->lines_initialized = false;
    s->starts = s->stops = s->register_reads = s->register_writes = 0;
    qemu_set_irq(s->scl_in, 1);
    qemu_set_irq(s->sda_in, 1);
}

static void twi_realize(DeviceState *dev, Error **errp)
{
    if (!SIEMENS_TWI(dev)->configured) {
        error_setg(errp, "TWI register-file configuration is required");
        return;
    }
    twi_reset(dev);
}

static void twi_init(Object *obj)
{
    SiemensTWIState *s = SIEMENS_TWI(obj);
    DeviceState *dev = DEVICE(obj);

    qdev_init_gpio_in_named(dev, twi_port_state, "port-state", 1);
    qdev_init_gpio_out_named(dev, &s->scl_in, "scl-in", 1);
    qdev_init_gpio_out_named(dev, &s->sda_in, "sda-in", 1);
    object_property_add_uint64_ptr(obj, "starts", &s->starts,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "stops", &s->stops,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "register-reads", &s->register_reads,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "register-writes",
                                   &s->register_writes, OBJ_PROP_FLAG_READ);
}

static void twi_finalize(Object *obj)
{
    g_free(SIEMENS_TWI(obj)->model);
}

static int twi_post_load(void *opaque, int version_id)
{
    SiemensTWIState *s = opaque;

    if (s->phase > TWI_MASTER_ACK_RELEASE || s->rx_kind > TWI_RX_DATA ||
        s->after_ack > TWI_AFTER_TX || s->bit_count > 8) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_twi = {
    .name = "x55/twi",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = twi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER(registers, SiemensTWIState),
        VMSTATE_UINT32(phase, SiemensTWIState),
        VMSTATE_UINT32(rx_kind, SiemensTWIState),
        VMSTATE_UINT32(after_ack, SiemensTWIState),
        VMSTATE_UINT8(rx_byte, SiemensTWIState),
        VMSTATE_UINT8(tx_byte, SiemensTWIState),
        VMSTATE_UINT8(bit_count, SiemensTWIState),
        VMSTATE_UINT8(register_pointer, SiemensTWIState),
        VMSTATE_BOOL(selected, SiemensTWIState),
        VMSTATE_BOOL(read, SiemensTWIState),
        VMSTATE_BOOL(ack, SiemensTWIState),
        VMSTATE_BOOL(master_ack, SiemensTWIState),
        VMSTATE_BOOL(slave_sda_low, SiemensTWIState),
        VMSTATE_BOOL(prev_scl, SiemensTWIState),
        VMSTATE_BOOL(prev_sda, SiemensTWIState),
        VMSTATE_BOOL(lines_initialized, SiemensTWIState),
        VMSTATE_UINT64(starts, SiemensTWIState),
        VMSTATE_UINT64(stops, SiemensTWIState),
        VMSTATE_UINT64(register_reads, SiemensTWIState),
        VMSTATE_UINT64(register_writes, SiemensTWIState),
        VMSTATE_END_OF_LIST()
    },
};

static void twi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->vmsd = &vmstate_twi;
    dc->desc = "Siemens x55 open-drain TWI register-file responder";
    dc->realize = twi_realize;
    device_class_set_legacy_reset(dc, twi_reset);
}

static const TypeInfo twi_type = {
    .name = TYPE_SIEMENS_TWI,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SiemensTWIState),
    .instance_align = __alignof__(SiemensTWIState),
    .instance_init = twi_init,
    .instance_finalize = twi_finalize,
    .class_init = twi_class_init,
};

static void twi_register_types(void)
{
    type_register_static(&twi_type);
}
type_init(twi_register_types)
