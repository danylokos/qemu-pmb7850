/*
 * Evidence-bounded Siemens startup handshake and runtime keypad matrix.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "exec/icount.h"
#include "hw/c166/siemens_x55/keypad.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "system/cpus.h"
#include "ui/input.h"
#include "trace/trace-hw_c166.h"

#define SIEMENS_KEYPAD_MAX_BUTTONS 32

struct SiemensKeypadState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq activity;
    QemuInputHandlerState *input_handler;

    uint16_t scan_command;
    uint16_t power_result;
    uint16_t idle_result;
    uint16_t matrix_idle_result;
    uint16_t command;
    uint16_t command_register;
    uint16_t matrix_result;
    uint16_t matrix_select_mask;
    uint8_t matrix_commands[SIEMENS_KEYPAD_MAX_MATRIX_COMMANDS];
    unsigned matrix_command_count;
    uint8_t matrix_scan_phase;
    bool matrix_result_valid;
    bool command_valid;
    bool release_activity_on_last_key_up;
    bool release_activity_pending;
    bool startup_pending;
    bool startup_result_tagged;
    uint8_t startup_read_mask;

    SiemensKeypadButton buttons[SIEMENS_KEYPAD_MAX_BUTTONS];
    unsigned button_count;
    uint32_t pressed;
    uint32_t ui_pressed;
    uint32_t sampled_pressed;
    uint8_t button_refs[SIEMENS_KEYPAD_MAX_BUTTONS];
    int8_t host_button[Q_KEY_CODE__MAX];
    bool shift;
};

static uint64_t keypad_icount(void)
{
    return icount_enabled() ? icount_get_raw() : 0;
}

static uint32_t keypad_pc(void)
{
    CPUClass *cc;

    if (!current_cpu) {
        return 0;
    }
    cc = CPU_GET_CLASS(current_cpu);
    return cc->get_pc ? cc->get_pc(current_cpu) : 0;
}

static bool keypad_acknowledged(SiemensKeypadState *s)
{
    return !s->startup_pending;
}

static void keypad_startup_result(SiemensKeypadState *s)
{
    s->matrix_result = s->power_result;
    s->matrix_result_valid = true;
    s->startup_result_tagged = true;
    s->startup_read_mask = 0;
}

static uint16_t keypad_matrix_result(SiemensKeypadState *s,
                                     uint16_t command, bool commit)
{
    uint16_t result = s->matrix_idle_result;

    for (unsigned i = 0; i < s->button_count; i++) {
        uint16_t raw;
        uint16_t column;
        uint16_t column_command;
        uint16_t selected_column;

        if (!(s->pressed & (1u << i))) {
            continue;
        }
        raw = s->buttons[i].raw_code;
        if (raw == 0x000d) {
            if (command == s->scan_command) {
                if (commit) {
                    s->sampled_pressed |= 1u << i;
                }
                result = (result & 0xfff0) |
                         ((result & 0x000f) & (raw & 0x000f));
            }
            continue;
        }
        column = raw >> 4;
        column_command = ~column & 0x00fd;
        selected_column = ~command & 0x00ff;
        if (selected_column == column ||
            (command & 0xff) == column_command) {
            if (commit) {
                s->sampled_pressed |= 1u << i;
            }
            result = (result & 0xfff0) |
                     ((result & 0x000f) & (raw & 0x000f));
        }
    }
    return result;
}

static uint16_t keypad_matrix_column_result(SiemensKeypadState *s,
                                             uint16_t column, bool commit)
{
    uint16_t result = s->matrix_idle_result;

    for (unsigned i = 0; i < s->button_count; i++) {
        uint16_t raw = s->buttons[i].raw_code;

        if ((s->pressed & (1u << i)) && (raw >> 4) == column) {
            if (commit) {
                s->sampled_pressed |= 1u << i;
            }
            result = (result & 0xfff0) |
                     ((result & 0x000f) & (raw & 0x000f));
        }
    }
    return result;
}

static uint16_t keypad_matrix_all_result(SiemensKeypadState *s, bool commit)
{
    uint16_t direct_result = s->matrix_idle_result;
    bool direct_pressed = false;
    bool matrix_pressed = false;

    for (unsigned i = 0; i < s->button_count; i++) {
        uint16_t raw = s->buttons[i].raw_code;

        if (!(s->pressed & (1u << i))) {
            continue;
        }
        if (raw >> 4) {
            matrix_pressed = true;
        } else {
            if (commit) {
                s->sampled_pressed |= 1u << i;
            }
            direct_pressed = true;
            direct_result = (direct_result & 0xfff0) |
                            ((direct_result & 0x000f) &
                             (raw & 0x000f));
        }
    }
    if (direct_pressed) {
        return direct_result;
    }
    if (matrix_pressed) {
        return s->matrix_idle_result;
    }
    if (s->release_activity_pending) {
        if (commit) {
            s->release_activity_pending = false;
        }
        return s->matrix_idle_result;
    }
    return s->idle_result;
}

static bool keypad_is_matrix_command(SiemensKeypadState *s,
                                     uint16_t command)
{
    for (unsigned i = 0; i < s->matrix_command_count; i++) {
        if ((command & 0xff) == s->matrix_commands[i]) {
            return true;
        }
    }
    return false;
}

static uint16_t keypad_result(SiemensKeypadState *s, bool commit)
{
    if (s->matrix_result_valid) {
        return s->matrix_result;
    }
    if (!s->command_valid) {
        return 0;
    }
    if (s->command == s->scan_command) {
        if (s->matrix_select_mask) {
            return keypad_matrix_all_result(s, commit);
        }
        return keypad_matrix_result(s, s->command, commit);
    }
    if (keypad_is_matrix_command(s, s->command)) {
        return keypad_matrix_result(s, s->command, commit);
    }
    return 0;
}

static MemTxResult keypad_read(void *opaque, hwaddr offset, uint64_t *data,
                                unsigned size, MemTxAttrs attrs)
{
    SiemensKeypadState *s = opaque;
    bool consume = offset < 2 && !attrs.debug;
    uint16_t result = keypad_result(s, consume);
    uint16_t word = offset < 2 ? result : s->command_register;
    uint64_t value = size == 1 ?
        extract32(word, 8 * (offset & 1), 8) : word;

    if (consume && s->startup_result_tagged) {
        s->startup_read_mask |= size == 2 ? 3 : 1u << offset;
        if (s->startup_read_mask == 3) {
            s->startup_pending = false;
            s->startup_result_tagged = false;
            trace_siemens_keypad_startup_release(keypad_icount(), keypad_pc(),
                                                result);
        }
    }
    if (consume && s->matrix_scan_phase == 1 &&
        (result & 0x000f) == 0x000f && result != s->idle_result) {
        s->matrix_scan_phase = 2;
    }
    if (!attrs.debug) {
        trace_siemens_keypad_access(keypad_icount(), keypad_pc(), false,
                                offset, size, value, s->command, result,
                                keypad_acknowledged(s));
    }
    *data = value;
    return MEMTX_OK;
}

static void keypad_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    SiemensKeypadState *s = opaque;

    if (offset >= 2) {
        if (size == 1) {
            unsigned shift = 8 * (offset & 1);

            s->command_register = deposit32(s->command_register, shift,
                                             8, value);
        } else {
            s->command_register = value;
        }
        /* Only a low-byte/word command store starts a new scanner request. */
        if (offset == 2) {
            s->matrix_result_valid = false;
            s->startup_result_tagged = false;
            s->startup_read_mask = 0;
            s->command_valid = true;
            s->command = s->command_register;
            if (s->startup_pending && s->command == s->scan_command) {
                keypad_startup_result(s);
            }
        }
    }
    trace_siemens_keypad_access(keypad_icount(), keypad_pc(), true,
                                offset, size, value, s->command,
                                keypad_result(s, false),
                                keypad_acknowledged(s));
}

static void keypad_matrix_select(void *opaque, int n, int level)
{
    SiemensKeypadState *s = opaque;
    uint16_t column;

    if (!s->matrix_select_mask) {
        return;
    }
    column = level & s->matrix_select_mask;
    if (!keypad_acknowledged(s)) {
        if (!column) {
            s->command = level;
            s->command_valid = true;
            keypad_startup_result(s);
            trace_siemens_keypad_matrix_select(keypad_icount(), keypad_pc(),
                                               column, s->command,
                                               s->matrix_result, true);
        }
        return;
    }

    s->command = ~column & 0x00fd;
    s->startup_result_tagged = false;
    s->startup_read_mask = 0;
    s->command_valid = true;
    s->matrix_result_valid = true;
    if (column == s->matrix_select_mask) {
        s->matrix_scan_phase = 0;
        s->matrix_result = s->idle_result;
    } else if (s->matrix_scan_phase == 0 && !column) {
        s->matrix_scan_phase = 1;
        s->matrix_result = keypad_matrix_all_result(s, true);
    } else if (s->matrix_scan_phase == 2 && !column) {
        s->matrix_result = s->matrix_idle_result;
    } else if (!column || (column & (column - 1))) {
        s->matrix_result = s->matrix_idle_result;
    } else {
        s->matrix_result = keypad_matrix_column_result(s, column, true);
    }
    trace_siemens_keypad_matrix_select(keypad_icount(), keypad_pc(), column,
                                       s->command, s->matrix_result, false);
}

static const MemoryRegionOps keypad_ops = {
    .read_with_attrs = keypad_read,
    .write = keypad_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
};

static int keypad_button_for_event(SiemensKeypadState *s, QKeyCode qcode)
{
    uint8_t logical = 0;

    if (s->shift && qcode == Q_KEY_CODE_8) {
        logical = 0x2a;
    } else if (s->shift && qcode == Q_KEY_CODE_3) {
        logical = 0x23;
    }
    for (unsigned i = 0; i < s->button_count; i++) {
        if ((logical && s->buttons[i].logical_code == logical) ||
            (s->buttons[i].qcode != Q_KEY_CODE_UNMAPPED &&
             s->buttons[i].qcode == qcode) ||
            (s->buttons[i].alias_qcode != Q_KEY_CODE_UNMAPPED &&
             s->buttons[i].alias_qcode == qcode)) {
            return i;
        }
    }
    return -1;
}

static void keypad_set_button(SiemensKeypadState *s, unsigned button,
                              QKeyCode host, bool down)
{
    uint32_t mask = 1u << button;
    bool changed = false;

    if (down) {
        if (s->host_button[host] >= 0) {
            return;
        }
        s->host_button[host] = button;
        if (s->button_refs[button]++ == 0) {
            changed = !(s->pressed & mask);
            s->pressed |= mask;
            s->sampled_pressed &= ~mask;
            s->release_activity_pending = false;
        }
    } else {
        if (s->host_button[host] < 0) {
            return;
        }
        button = s->host_button[host];
        s->host_button[host] = -1;
        if (s->button_refs[button] && --s->button_refs[button] == 0) {
            mask = 1u << button;
            if (!(s->ui_pressed & mask)) {
                uint32_t pressed_before = s->pressed;

                s->pressed &= ~mask;
                s->sampled_pressed &= ~mask;
                if (s->release_activity_on_last_key_up && pressed_before &&
                    !s->pressed) {
                    s->release_activity_pending = true;
                }
                changed = true;
            }
        }
    }
    if (changed) {
        qemu_set_irq(s->activity, 1);
        qemu_set_irq(s->activity, 0);
        trace_siemens_keypad_input(keypad_icount(), keypad_pc(),
                                   s->buttons[button].name,
                                   s->buttons[button].raw_code,
                                   s->buttons[button].logical_code, down);
    }
}

static bool keypad_set_ui_button(SiemensKeypadState *s, unsigned button,
                                 bool down)
{
    uint32_t mask = 1u << button;
    bool was_ui_down = s->ui_pressed & mask;
    bool was_pressed = s->pressed & mask;
    bool pressed;

    if (was_ui_down == down) {
        return false;
    }
    if (down) {
        s->ui_pressed |= mask;
        s->sampled_pressed &= ~mask;
        s->release_activity_pending = false;
    } else {
        s->ui_pressed &= ~mask;
    }
    pressed = (s->ui_pressed & mask) || s->button_refs[button];
    if (pressed) {
        s->pressed |= mask;
    } else {
        s->pressed &= ~mask;
        s->sampled_pressed &= ~mask;
    }
    if (was_pressed != pressed) {
        if (!pressed && s->release_activity_on_last_key_up &&
            was_pressed && !s->pressed) {
            s->release_activity_pending = true;
        }
        qemu_set_irq(s->activity, 1);
        qemu_set_irq(s->activity, 0);
        trace_siemens_keypad_input(keypad_icount(), keypad_pc(),
                                   s->buttons[button].name,
                                   s->buttons[button].raw_code,
                                   s->buttons[button].logical_code, pressed);
    }
    return true;
}

static void keypad_input_event(DeviceState *dev, QemuConsole *src,
                               QemuInputEvent *evt)
{
    SiemensKeypadState *s = SIEMENS_KEYPAD(dev);
    QKeyCode qcode = qemu_input_linux_to_qcode(evt->key.key);
    int button;

    if (qcode >= Q_KEY_CODE__MAX) {
        return;
    }
    if (qcode == Q_KEY_CODE_SHIFT || qcode == Q_KEY_CODE_SHIFT_R) {
        s->shift = evt->key.down;
        return;
    }
    if (!evt->key.down && s->host_button[qcode] >= 0) {
        keypad_set_button(s, s->host_button[qcode], qcode, false);
        return;
    }
    button = keypad_button_for_event(s, qcode);
    if (button >= 0) {
        keypad_set_button(s, button, qcode, evt->key.down);
    }
}

static const QemuInputHandler keypad_input_handler = {
    .name = "Siemens handset keypad",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = keypad_input_event,
};

static void keypad_reset_hold(Object *obj, ResetType type)
{
    SiemensKeypadState *s = SIEMENS_KEYPAD(obj);

    s->command = 0;
    s->command_register = 0;
    s->command_valid = false;
    s->pressed = 0;
    s->ui_pressed = 0;
    s->sampled_pressed = 0;
    s->matrix_result = 0;
    s->matrix_scan_phase = 0;
    s->matrix_result_valid = false;
    s->release_activity_pending = false;
    s->startup_pending = true;
    s->startup_result_tagged = false;
    s->startup_read_mask = 0;
    s->shift = false;
    memset(s->button_refs, 0, sizeof(s->button_refs));
    memset(s->host_button, -1, sizeof(s->host_button));
}

static void keypad_realize(DeviceState *dev, Error **errp)
{
    SiemensKeypadState *s = SIEMENS_KEYPAD(dev);

    if (!s->power_result || !s->button_count ||
        (!s->scan_command && !s->matrix_select_mask)) {
        error_setg(errp, "keypad scan and Power button required");
        return;
    }
    s->input_handler = qemu_input_handler_register(dev,
                                                    &keypad_input_handler);
    device_cold_reset(dev);
}

static void keypad_unrealize(DeviceState *dev)
{
    SiemensKeypadState *s = SIEMENS_KEYPAD(dev);

    g_clear_pointer(&s->input_handler, qemu_input_handler_unregister);
}

bool siemens_keypad_configure(DeviceState *dev,
                              const SiemensKeypadConfig *config,
                              Error **errp)
{
    SiemensKeypadState *s = SIEMENS_KEYPAD(dev);

    if (dev->realized) {
        error_setg(errp, "keypad buttons must be configured before realize");
        return false;
    }
    if (!config || !config->buttons || !config->button_count ||
        config->button_count > ARRAY_SIZE(s->buttons)) {
        error_setg(errp, "keypad button count must be 1..%zu",
                   ARRAY_SIZE(s->buttons));
        return false;
    }
    if (!config->scan_command && !config->matrix_select_mask) {
        error_setg(errp, "keypad scan interface required");
        return false;
    }
    if (config->matrix_command_count > ARRAY_SIZE(s->matrix_commands)) {
        error_setg(errp, "keypad matrix command count must be 0..%zu",
                   ARRAY_SIZE(s->matrix_commands));
        return false;
    }
    memcpy(s->buttons, config->buttons,
           config->button_count * sizeof(*config->buttons));
    s->button_count = config->button_count;
    if (config->matrix_command_count) {
        memcpy(s->matrix_commands, config->matrix_commands,
               config->matrix_command_count);
    }
    s->matrix_command_count = config->matrix_command_count;
    s->scan_command = config->scan_command;
    s->power_result = 0;
    for (unsigned i = 0; i < s->button_count; i++) {
        if (!strcmp(s->buttons[i].name, "power")) {
            s->power_result = s->buttons[i].raw_code;
        }
    }
    if (!s->power_result) {
        error_setg(errp, "keypad requires a nonzero Power-key table entry");
        return false;
    }
    s->idle_result = config->idle_result;
    s->matrix_idle_result = config->matrix_idle_result;
    s->matrix_select_mask = config->matrix_select_mask;
    s->release_activity_on_last_key_up =
        config->release_activity_on_last_key_up;
    return true;
}

const SiemensKeypadButton *siemens_keypad_buttons(DeviceState *dev,
                                                  size_t *count)
{
    SiemensKeypadState *s;

    if (!dev || !object_dynamic_cast(OBJECT(dev), TYPE_SIEMENS_KEYPAD)) {
        return NULL;
    }
    s = SIEMENS_KEYPAD(dev);
    if (count) {
        *count = s->button_count;
    }
    return s->buttons;
}

bool siemens_keypad_set_ui_button(DeviceState *dev, unsigned button,
                                  bool down)
{
    SiemensKeypadState *s;

    if (!dev || !object_dynamic_cast(OBJECT(dev), TYPE_SIEMENS_KEYPAD)) {
        return false;
    }
    s = SIEMENS_KEYPAD(dev);
    return button < s->button_count && keypad_set_ui_button(s, button, down);
}

void siemens_keypad_release_ui_buttons(DeviceState *dev)
{
    SiemensKeypadState *s;
    uint32_t pressed;

    if (!dev || !object_dynamic_cast(OBJECT(dev), TYPE_SIEMENS_KEYPAD)) {
        return;
    }
    s = SIEMENS_KEYPAD(dev);
    pressed = s->ui_pressed;
    for (unsigned i = 0; i < s->button_count; i++) {
        if (pressed & (1u << i)) {
            keypad_set_ui_button(s, i, false);
        }
    }
}

uint32_t siemens_keypad_sampled_buttons(DeviceState *dev)
{
    return dev && object_dynamic_cast(OBJECT(dev), TYPE_SIEMENS_KEYPAD) ?
           SIEMENS_KEYPAD(dev)->sampled_pressed : 0;
}

uint32_t siemens_keypad_ui_buttons(DeviceState *dev)
{
    return dev && object_dynamic_cast(OBJECT(dev), TYPE_SIEMENS_KEYPAD) ?
           SIEMENS_KEYPAD(dev)->ui_pressed : 0;
}

static void keypad_get_ui_buttons(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    SiemensKeypadState *s = SIEMENS_KEYPAD(obj);
    uint32_t value = s->ui_pressed;

    visit_type_uint32(v, name, &value, errp);
}

static void keypad_set_ui_buttons(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    SiemensKeypadState *s = SIEMENS_KEYPAD(obj);
    uint32_t value;
    uint32_t valid = s->button_count == 32 ? UINT32_MAX :
                     (1u << s->button_count) - 1u;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value & ~valid) {
        error_setg(errp, "ui-buttons contains unsupported bits 0x%08x",
                   value & ~valid);
        return;
    }
    for (unsigned i = 0; i < s->button_count; i++) {
        keypad_set_ui_button(s, i, value & (1u << i));
    }
}

static void keypad_get_sampled_buttons(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    SiemensKeypadState *s = SIEMENS_KEYPAD(obj);
    uint32_t value = s->sampled_pressed;

    visit_type_uint32(v, name, &value, errp);
}

static void keypad_set_ui_release_event(Object *obj, const char *value,
                                        Error **errp)
{
    SiemensKeypadState *s = SIEMENS_KEYPAD(obj);
    unsigned index;
    char phase[16], extra;

    if (sscanf(value, "%u %15s %c", &index, phase, &extra) != 2 ||
        index >= s->button_count ||
        (strcmp(phase, "queued") && strcmp(phase, "sampled") &&
         strcmp(phase, "cancelled") && strcmp(phase, "forced"))) {
        error_setg(errp, "ui-release-event requires a valid button and phase");
        return;
    }
    /*
     * Host lifecycle observation only: never changes electrical key state.
     * Use the native producer so it remains ordered with guest scan events.
     * PC zero denotes host origin, as with ordinary UI key input.
     */
    trace_siemens_keypad_deferred_release(keypad_icount(), 0,
                                          s->buttons[index].name,
                                          s->buttons[index].raw_code,
                                          index, phase);
}

static void keypad_init(Object *obj)
{
    SiemensKeypadState *s = SIEMENS_KEYPAD(obj);

    memory_region_init_io(&s->mmio, obj, &keypad_ops, s,
                          "siemens-keypad", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    qdev_init_gpio_out_named(DEVICE(obj), &s->activity, "activity", 1);
    qdev_init_gpio_in_named(DEVICE(obj), keypad_matrix_select,
                            "matrix-select", 1);
    memset(s->host_button, -1, sizeof(s->host_button));
    object_property_add(obj, "ui-buttons", "uint32",
                        keypad_get_ui_buttons, keypad_set_ui_buttons,
                        NULL, NULL);
    object_property_add(obj, "sampled-buttons", "uint32",
                        keypad_get_sampled_buttons, NULL, NULL, NULL);
    object_property_add_str(obj, "ui-release-event", NULL,
                            keypad_set_ui_release_event);
}

static const VMStateDescription vmstate_keypad = {
    .name = "x55/keypad",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(command, SiemensKeypadState),
        VMSTATE_UINT16(command_register, SiemensKeypadState),
        VMSTATE_UINT16(matrix_result, SiemensKeypadState),
        VMSTATE_UINT8(matrix_scan_phase, SiemensKeypadState),
        VMSTATE_UINT8(startup_read_mask, SiemensKeypadState),
        VMSTATE_BOOL(matrix_result_valid, SiemensKeypadState),
        VMSTATE_BOOL(command_valid, SiemensKeypadState),
        VMSTATE_BOOL(release_activity_pending, SiemensKeypadState),
        VMSTATE_BOOL(startup_pending, SiemensKeypadState),
        VMSTATE_BOOL(startup_result_tagged, SiemensKeypadState),
        VMSTATE_BOOL(shift, SiemensKeypadState),
        VMSTATE_UINT32(pressed, SiemensKeypadState),
        VMSTATE_UINT32(ui_pressed, SiemensKeypadState),
        VMSTATE_UINT32(sampled_pressed, SiemensKeypadState),
        VMSTATE_BUFFER(button_refs, SiemensKeypadState),
        VMSTATE_ARRAY(host_button, SiemensKeypadState, Q_KEY_CODE__MAX, 0,
                      vmstate_info_int8, int8_t),
        VMSTATE_END_OF_LIST()
    },
};

static void keypad_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_keypad;
    dc->desc = "Siemens handset keypad scanner";
    dc->realize = keypad_realize;
    dc->unrealize = keypad_unrealize;
    rc->phases.hold = keypad_reset_hold;
}

static const TypeInfo keypad_type = {
    .name = TYPE_SIEMENS_KEYPAD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SiemensKeypadState),
    .instance_align = __alignof__(SiemensKeypadState),
    .instance_init = keypad_init,
    .class_init = keypad_class_init,
};

static void keypad_register_types(void)
{
    type_register_static(&keypad_type);
}

type_init(keypad_register_types)
