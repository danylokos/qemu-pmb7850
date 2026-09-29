/*
 * Intel W30-family top-parameter NOR flash command engine.
 *
 * Program and erase durations are deterministic CEMU calibrations expressed
 * at the PMB7850's 26 MHz clock, not measured handset timing claims.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "exec/icount.h"
#include "hw/block/w30.h"
#include "hw/block/x55_nor_trace.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "system/block-backend.h"
#include "system/cpus.h"
#include "trace.h"
#include "x55_nor.h"
#include "hw/block/x55_nor_view.h"

#define TYPE_W30_COMMON "w30-common"
#define W30_PARAMETER_BLOCKS 8
#define W30_MAX_PARTITIONS 32
#define W30_MAX_BLOCKS 263
#define W30_BITMAP_WORDS DIV_ROUND_UP(W30_MAX_BLOCKS, 64)
#define W30_PROGRAM_NS ((8 * NANOSECONDS_PER_SECOND) / 26000000)
#define W30_ERASE_NS ((80000 * NANOSECONDS_PER_SECOND) / 26000000)

#define W30_SR_READY           0x80
#define W30_SR_ERASE_SUSPEND   0x40
#define W30_SR_ERASE_ERROR     0x20
#define W30_SR_PROGRAM_ERROR   0x10
#define W30_SR_VPP_ERROR       0x08
#define W30_SR_PROGRAM_SUSPEND 0x04
#define W30_SR_LOCK_ERROR      0x02
#define W30_SR_OTHER_BUSY      0x01
#define W30_SR_CLEARABLE \
    (W30_SR_ERASE_ERROR | W30_SR_PROGRAM_ERROR | W30_SR_VPP_ERROR | \
     W30_SR_LOCK_ERROR)

typedef enum W30Mode {
    W30_MODE_ARRAY,
    W30_MODE_ID,
    W30_MODE_CFI,
    W30_MODE_STATUS,
} W30Mode;

typedef enum W30Phase {
    W30_PHASE_IDLE,
    W30_PHASE_PROGRAM_DATA,
    W30_PHASE_ERASE_CONFIRM,
    W30_PHASE_LOCK_CONFIRM,
    W30_PHASE_PROTECTION_DATA,
    W30_PHASE_EFP_CONFIRM,
} W30Phase;

typedef enum W30OperationKind {
    W30_OP_NONE,
    W30_OP_PROGRAM,
    W30_OP_ERASE,
    W30_OP_PROTECTION_PROGRAM,
} W30OperationKind;

typedef struct W30Geometry {
    const char *description;
    const char *region_prefix;
    uint32_t size;
    uint16_t manufacturer_id;
    uint16_t device_id;
    uint8_t cfi_device_size;
    uint8_t cfi_erase_regions;
    uint8_t partition_count;
    uint16_t main_block_count;
    uint32_t parameter_base;
    uint16_t block_count;
} W30Geometry;

typedef struct W30Operation {
    W30OperationKind kind;
    uint8_t partition;
    uint16_t block;
    uint32_t offset;
    uint16_t value;
    int64_t remaining_ns;
    int64_t deadline_ns;
    bool program_error;
} W30Operation;

typedef struct W30State W30State;
typedef struct W30Class W30Class;

OBJECT_DECLARE_TYPE(W30State, W30Class, W30_COMMON)

typedef struct W30View {
    W30State *flash;
    bool command_visible;
} W30View;

struct W30State {
    SysBusDevice parent_obj;
    X55NORTraceState nor_trace;

    BlockBackend *blk;
    char *factory_uid_text;
    uint8_t factory_uid[W30_FACTORY_UID_SIZE];
    uint8_t customer[8];
    bool customer_locked;
    uint8_t *storage;
    bool writable;

    MemoryRegion low;
    MemoryRegion native;
    X55NORStorage nor;
    W30View low_view;
    W30View native_view;

    W30Mode mode[W30_MAX_PARTITIONS];
    uint8_t status[W30_MAX_PARTITIONS];
    uint16_t configuration[W30_MAX_PARTITIONS];
    W30Phase phase;
    uint8_t setup_partition;
    uint32_t setup_offset;
    uint64_t erased_blocks[W30_BITMAP_WORDS];
    uint64_t locked_blocks[W30_BITMAP_WORDS];
    uint64_t lockdown_blocks[W30_BITMAP_WORDS];

    QEMUTimer *operation_timer;
    W30Operation active;
    W30Operation suspended_erase;
    W30Operation suspended_program;
};

struct W30Class {
    SysBusDeviceClass parent_class;
    const W30Geometry *geometry;
};

static const W30Geometry w30_64mbit_top_geometry = {
    .description = "Intel W30 64-Mbit top-parameter NOR flash",
    .region_prefix = TYPE_W30_64MBIT_TOP,
    .size = W30_64MBIT_TOP_SIZE,
    .manufacturer_id = 0x0089,
    .device_id = 0x8854,
    .cfi_device_size = 0x17,
    .cfi_erase_regions = 0x11,
    .partition_count = 16,
    .main_block_count = 127,
    .parameter_base = 0x7f0000,
    .block_count = 135,
};

static const W30Geometry w30_128mbit_top_geometry = {
    .description = "Intel W30 128-Mbit top-parameter NOR flash",
    .region_prefix = TYPE_W30_128MBIT_TOP,
    .size = W30_128MBIT_TOP_SIZE,
    .manufacturer_id = 0x0089,
    .device_id = 0x8856,
    .cfi_device_size = 0x18,
    .cfi_erase_regions = 0x21,
    .partition_count = 32,
    .main_block_count = 255,
    .parameter_base = 0xff0000,
    .block_count = 263,
};

static bool w30_array_byte(Object *obj, MemoryRegion *view, uint32_t offset);

static const W30Geometry *w30_geometry(W30State *s)
{
    return W30_COMMON_GET_CLASS(s)->geometry;
}

static uint64_t w30_trace_icount(void)
{
    return icount_enabled() ? icount_get_raw() : 0;
}

static uint32_t w30_trace_pc(void)
{
    CPUClass *cc;

    if (!current_cpu) {
        return 0;
    }
    cc = CPU_GET_CLASS(current_cpu);
    return cc->get_pc ? cc->get_pc(current_cpu) : 0;
}

static const char *w30_mode_name(W30Mode mode)
{
    switch (mode) {
    case W30_MODE_ID: return "id";
    case W30_MODE_CFI: return "cfi";
    case W30_MODE_STATUS: return "status";
    default: return "array";
    }
}

static const char *w30_phase_name(W30Phase phase)
{
    switch (phase) {
    case W30_PHASE_PROGRAM_DATA: return "program-data";
    case W30_PHASE_ERASE_CONFIRM: return "erase-confirm";
    case W30_PHASE_LOCK_CONFIRM: return "lock-confirm";
    case W30_PHASE_PROTECTION_DATA: return "protection-data";
    case W30_PHASE_EFP_CONFIRM: return "efp-confirm";
    default: return "idle";
    }
}

static const char *w30_operation_name(W30OperationKind kind)
{
    switch (kind) {
    case W30_OP_PROGRAM: return "program";
    case W30_OP_ERASE: return "erase";
    case W30_OP_PROTECTION_PROGRAM: return "protection-program";
    default: return "none";
    }
}

static bool w30_parse_uid(const char *text, uint8_t uid[W30_FACTORY_UID_SIZE])
{
    const char *p = text;

    memset(uid, 0, W30_FACTORY_UID_SIZE);
    if (!p) {
        return true;
    }
    if (g_str_has_prefix(p, "0x") || g_str_has_prefix(p, "0X")) {
        p += 2;
    }
    if (strlen(p) != 2 * W30_FACTORY_UID_SIZE) {
        return false;
    }
    for (unsigned i = 0; i < W30_FACTORY_UID_SIZE; i++) {
        int hi = g_ascii_xdigit_value(p[2 * i]);
        int lo = g_ascii_xdigit_value(p[2 * i + 1]);

        if (hi < 0 || lo < 0) {
            return false;
        }
        uid[i] = (hi << 4) | lo;
    }
    return true;
}

static unsigned w30_partition(W30State *s, uint32_t offset)
{
    return offset / W30_PARTITION_SIZE;
}

static bool w30_bit_test(uint64_t bits[W30_BITMAP_WORDS], unsigned index)
{
    return bits[index >> 6] & (UINT64_C(1) << (index & 63));
}

static void w30_bit_set(uint64_t bits[W30_BITMAP_WORDS], unsigned index,
                        bool value)
{
    uint64_t mask = UINT64_C(1) << (index & 63);

    if (value) {
        bits[index >> 6] |= mask;
    } else {
        bits[index >> 6] &= ~mask;
    }
}

static int w30_block(W30State *s, uint32_t offset, uint32_t *start,
                     uint32_t *size)
{
    const W30Geometry *geometry = w30_geometry(s);
    uint32_t block_start;
    uint32_t block_size;
    unsigned block;

    if (offset >= geometry->size) {
        return -1;
    }
    if (offset < geometry->parameter_base) {
        block = offset / W30_MAIN_BLOCK_SIZE;
        block_start = block * W30_MAIN_BLOCK_SIZE;
        block_size = W30_MAIN_BLOCK_SIZE;
    } else {
        block = geometry->main_block_count +
                (offset - geometry->parameter_base) / W30_PARAMETER_SIZE;
        block_start = geometry->parameter_base +
                      (block - geometry->main_block_count) *
                      W30_PARAMETER_SIZE;
        block_size = W30_PARAMETER_SIZE;
    }
    if (start) {
        *start = block_start;
    }
    if (size) {
        *size = block_size;
    }
    return block;
}

static void w30_update_views(W30State *s)
{
    x55_nor_update(&s->nor);
}

static void w30_flush(W30State *s, uint32_t offset, uint32_t size)
{
    x55_nor_flush(&s->nor, offset, size);
}

static void w30_persist(W30State *s, uint32_t offset, uint32_t size)
{
    int ret;

    if (!s->blk || !s->writable) {
        return;
    }
    ret = blk_pwrite(s->blk, offset, size, s->storage + offset, 0);
    trace_w30_persistence(w30_trace_icount(), w30_trace_pc(), offset, size,
                         ret);
    if (ret < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: backend write at 0x%x failed: %s\n",
                      object_get_typename(OBJECT(s)), offset, strerror(-ret));
    }
}

static uint8_t w30_cfi_byte(W30State *s, unsigned word)
{
    static const uint8_t cfi[0x78] = {
        [0x10] = 'Q', [0x11] = 'R', [0x12] = 'Y',
        [0x13] = 0x03, [0x15] = 0x39,
        [0x1b] = 0x17, [0x1c] = 0x19, [0x1d] = 0xb4, [0x1e] = 0xc6,
        [0x1f] = 0x04, [0x21] = 0x0a, [0x23] = 0x04,
        [0x25] = 0x03, [0x27] = 0x17, [0x28] = 0x01,
        [0x2c] = 0x11, [0x2d] = 0x07, [0x30] = 0x01,
        [0x31] = 0x06, [0x34] = 0x01, [0x35] = 0x07, [0x37] = 0x20,
        [0x39] = 'P', [0x3a] = 'R', [0x3b] = 'I',
        [0x3c] = '1', [0x3d] = '3', [0x3e] = 0xe6, [0x3f] = 0x03,
        [0x42] = 0x01, [0x43] = 0x03, [0x45] = 0x18, [0x46] = 0xc0,
        [0x47] = 0x01, [0x48] = 0x80, [0x4a] = 0x03, [0x4b] = 0x03,
        [0x4c] = 0x03, [0x4d] = 0x03, [0x4e] = 0x01,
        [0x4f] = 0x02, [0x50] = 0x07, [0x51] = 0x02, [0x52] = 0x0f,
        [0x54] = 0x01, [0x57] = 0x0f, [0x58] = 0x07,
        [0x5b] = 0x01, [0x5c] = 0x64, [0x5e] = 0x01, [0x5f] = 0x03,
        [0x60] = 0x01, [0x62] = 0x01, [0x65] = 0x02,
        [0x66] = 0x06, [0x69] = 0x01, [0x6a] = 0x64,
        [0x6c] = 0x01, [0x6d] = 0x03, [0x6e] = 0x07,
        [0x70] = 0x20, [0x72] = 0x64, [0x74] = 0x01, [0x75] = 0x02,
    };
    const W30Geometry *geometry = w30_geometry(s);

    if (word == 0x27) {
        return geometry->cfi_device_size;
    }
    if (word == 0x2c) {
        return geometry->cfi_erase_regions;
    }
    if (word == 0x52 || word == 0x57) {
        return geometry->partition_count - 1;
    }
    return word < ARRAY_SIZE(cfi) ? cfi[word] : 0;
}

static uint16_t w30_id_word(W30State *s, uint32_t offset)
{
    const W30Geometry *geometry = w30_geometry(s);
    uint32_t partition_base;
    uint32_t relative;
    uint32_t id_relative;
    uint32_t block_start;
    uint32_t block_relative;
    int block;

    offset &= ~1;
    partition_base = offset - offset % W30_PARTITION_SIZE;
    relative = offset - partition_base;
    id_relative = relative & 0x1ff;
    block = w30_block(s, offset, &block_start, NULL);
    block_relative = offset - block_start;
    if (block_relative == 0) {
        return geometry->manufacturer_id;
    }
    if (block_relative == 2) {
        return geometry->device_id;
    }
    if (block_relative == 4 && block >= 0) {
        return w30_bit_test(s->locked_blocks, block) |
               (w30_bit_test(s->lockdown_blocks, block) << 1);
    }
    if (id_relative == 0x0a) {
        return s->configuration[w30_partition(s, offset)];
    }
    if (id_relative == 0x100) {
        return s->customer_locked ? 0xfffc : 0xfffe;
    }
    if (id_relative >= 0x102 && id_relative <= 0x109) {
        unsigned i = (id_relative - 0x102) & ~1;

        return s->factory_uid[i] | (s->factory_uid[i + 1] << 8);
    }
    if (id_relative >= 0x10a && id_relative <= 0x111) {
        unsigned i = (id_relative - 0x10a) & ~1;

        return s->customer[i] | (s->customer[i + 1] << 8);
    }
    return 0;
}

static uint8_t w30_status(W30State *s, unsigned partition)
{
    uint8_t value = s->status[partition] & W30_SR_CLEARABLE;

    if (s->active.kind != W30_OP_NONE) {
        if (partition == s->active.partition) {
            value &= ~(W30_SR_READY | W30_SR_OTHER_BUSY);
        } else {
            value &= ~W30_SR_READY;
            value |= W30_SR_OTHER_BUSY;
        }
    } else {
        value |= W30_SR_READY;
    }
    if (s->suspended_erase.kind == W30_OP_ERASE) {
        value |= W30_SR_ERASE_SUSPEND;
    }
    if (s->suspended_program.kind == W30_OP_PROGRAM) {
        value |= W30_SR_PROGRAM_SUSPEND;
    }
    return value;
}

static uint8_t w30_read_byte(W30View *view, uint32_t offset)
{
    W30State *s = view->flash;
    unsigned partition;
    W30Mode mode;
    uint16_t value;

    if (!view->command_visible) {
        return s->storage[offset];
    }
    partition = w30_partition(s, offset);
    mode = s->mode[partition];
    if (s->active.kind != W30_OP_NONE &&
        partition == s->active.partition && mode != W30_MODE_CFI) {
        mode = W30_MODE_STATUS;
    }
    if (mode == W30_MODE_ARRAY) {
        return s->storage[offset];
    }
    if (mode == W30_MODE_ID) {
        value = w30_id_word(s, offset);
    } else if (mode == W30_MODE_CFI) {
        value = w30_cfi_byte(s, (offset % W30_PARTITION_SIZE) >> 1);
    } else {
        value = w30_status(s, partition);
    }
    return offset & 1 ? value >> 8 : value;
}

static uint64_t w30_read(void *opaque, hwaddr offset, unsigned size)
{
    W30View *view = opaque;
    W30State *s = view->flash;
    uint64_t value = 0;
    W30Mode mode = view->command_visible ?
                   s->mode[w30_partition(s, offset)] : W30_MODE_ARRAY;

    for (unsigned i = 0; i < size; i++) {
        value |= (uint64_t)w30_read_byte(view, offset + i) << (8 * i);
    }
    trace_w30_read(w30_trace_icount(), w30_trace_pc(),
                   view->command_visible ? "native" : "low", offset, size,
                   value, w30_mode_name(mode),
                   w30_operation_name(s->active.kind));
    return value;
}

static void w30_sequence_error(W30State *s, unsigned partition)
{
    s->status[partition] |= W30_SR_PROGRAM_ERROR | W30_SR_ERASE_ERROR;
    s->mode[partition] = W30_MODE_STATUS;
    s->phase = W30_PHASE_IDLE;
}

static void w30_operation_complete(void *opaque)
{
    W30State *s = opaque;
    W30Operation op = s->active;

    if (op.kind == W30_OP_NONE) {
        return;
    }
    memset(&s->active, 0, sizeof(s->active));
    if (op.program_error) {
        s->status[op.partition] |= W30_SR_PROGRAM_ERROR;
    }
    if (op.kind == W30_OP_PROGRAM) {
        stw_le_p(s->storage + op.offset, op.value);
        w30_flush(s, op.offset, 2);
        w30_persist(s, op.offset, 2);
        x55_nor_trace_program(&s->nor_trace, op.offset,
                              s->storage + op.offset, 2);
    } else if (op.kind == W30_OP_PROTECTION_PROGRAM) {
        uint32_t relative = op.offset % W30_PARTITION_SIZE;

        if (relative == 0x100) {
            if (!(op.value & 2)) {
                s->customer_locked = true;
            }
        } else {
            unsigned i = relative - 0x10a;

            s->customer[i] = op.value;
            s->customer[i + 1] = op.value >> 8;
        }
    } else {
        uint32_t start;
        uint32_t block_size;

        if (w30_block(s, op.offset, &start, &block_size) >= 0) {
            memset(s->storage + start, 0xff, block_size);
            w30_bit_set(s->erased_blocks, op.block, true);
            w30_flush(s, start, block_size);
            w30_persist(s, start, block_size);
            x55_nor_trace_erase(&s->nor_trace, start, block_size);
        }
    }
    trace_w30_operation(w30_trace_icount(), w30_trace_pc(), "complete",
                        w30_operation_name(op.kind), op.offset,
                        op.remaining_ns);
    w30_update_views(s);
}

static void w30_start_operation(W30State *s, W30Operation operation)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    operation.deadline_ns = now + operation.remaining_ns;
    s->active = operation;
    timer_mod(s->operation_timer, operation.deadline_ns);
    trace_w30_operation(w30_trace_icount(), w30_trace_pc(), "start",
                        w30_operation_name(operation.kind), operation.offset,
                        operation.remaining_ns);
    w30_update_views(s);
}

static void w30_start_program(W30State *s, uint32_t offset, uint64_t value,
                              unsigned size)
{
    unsigned partition = w30_partition(s, offset);
    int block = w30_block(s, offset, NULL, NULL);
    uint16_t old;

    if (size != 2 || (offset & 1) || offset + 1 >= w30_geometry(s)->size ||
        block < 0) {
        w30_sequence_error(s, partition);
        return;
    }
    if (w30_bit_test(s->locked_blocks, block)) {
        s->status[partition] |= W30_SR_LOCK_ERROR | W30_SR_PROGRAM_ERROR;
        s->mode[partition] = W30_MODE_STATUS;
        return;
    }
    if (s->suspended_erase.kind == W30_OP_ERASE &&
        s->suspended_erase.block == block) {
        w30_sequence_error(s, partition);
        return;
    }
    old = lduw_le_p(s->storage + offset);
    w30_start_operation(s, (W30Operation) {
        .kind = W30_OP_PROGRAM,
        .partition = partition,
        .block = block,
        .offset = offset,
        .value = old & value,
        .remaining_ns = MAX(W30_PROGRAM_NS, 1),
        .program_error = (old & value) != value,
    });
    s->mode[partition] = W30_MODE_STATUS;
}

static void w30_start_erase(W30State *s, uint32_t offset)
{
    unsigned partition = w30_partition(s, offset);
    int block = w30_block(s, offset, NULL, NULL);

    if (block < 0 || s->suspended_erase.kind != W30_OP_NONE) {
        w30_sequence_error(s, partition);
        return;
    }
    if (w30_bit_test(s->locked_blocks, block)) {
        s->status[partition] |= W30_SR_LOCK_ERROR | W30_SR_ERASE_ERROR;
        s->mode[partition] = W30_MODE_STATUS;
        return;
    }
    w30_start_operation(s, (W30Operation) {
        .kind = W30_OP_ERASE,
        .partition = partition,
        .block = block,
        .offset = offset,
        .remaining_ns = W30_ERASE_NS,
    });
    s->mode[partition] = W30_MODE_STATUS;
}

static void w30_protection_program(W30State *s, uint32_t offset,
                                   uint64_t value, unsigned size)
{
    unsigned partition = w30_partition(s, offset);
    uint32_t relative = offset % W30_PARTITION_SIZE;
    bool program_error = false;

    if (partition != 0 || size != 2 || (offset & 1) || relative < 0x100 ||
        relative > 0x111) {
        s->status[partition] |= W30_SR_PROGRAM_ERROR;
        s->mode[partition] = W30_MODE_STATUS;
        return;
    }
    if (relative == 0x100) {
        if (s->customer_locked) {
            s->status[partition] |= W30_SR_LOCK_ERROR |
                                    W30_SR_PROGRAM_ERROR;
            s->mode[partition] = W30_MODE_STATUS;
            return;
        }
    } else if (relative >= 0x10a && !s->customer_locked) {
        unsigned i = relative - 0x10a;
        uint16_t old = s->customer[i] | (s->customer[i + 1] << 8);

        program_error = (old & value) != value;
        value &= old;
    } else {
        s->status[partition] |= W30_SR_LOCK_ERROR | W30_SR_PROGRAM_ERROR;
        s->mode[partition] = W30_MODE_STATUS;
        return;
    }
    w30_start_operation(s, (W30Operation) {
        .kind = W30_OP_PROTECTION_PROGRAM,
        .partition = partition,
        .offset = offset,
        .value = value,
        .remaining_ns = MAX(W30_PROGRAM_NS, 1),
        .program_error = program_error,
    });
    s->mode[partition] = W30_MODE_STATUS;
}

static bool w30_read_command_while_active(W30State *s, unsigned partition,
                                          uint8_t command)
{
    return s->active.kind != W30_OP_NONE &&
           ((partition != s->active.partition &&
             (command == 0xff || command == 0x90 || command == 0x98 ||
              command == 0x70 || command == 0x50)) ||
            (partition == s->active.partition &&
             (command == 0x98 || command == 0x70)));
}

static void w30_suspend_active(W30State *s, uint32_t offset,
                               unsigned partition)
{
    int block = w30_block(s, offset, NULL, NULL);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    W30Operation *suspended;

    if (s->active.kind == W30_OP_ERASE) {
        suspended = &s->suspended_erase;
    } else if (s->active.kind == W30_OP_PROGRAM) {
        suspended = &s->suspended_program;
    } else {
        suspended = NULL;
    }
    if (!suspended || suspended->kind != W30_OP_NONE || block < 0 ||
        block != s->active.block) {
        w30_sequence_error(s, partition);
        return;
    }
    s->active.remaining_ns = MAX(s->active.deadline_ns - now, 1);
    timer_del(s->operation_timer);
    *suspended = s->active;
    trace_w30_operation(w30_trace_icount(), w30_trace_pc(), "suspend",
                        w30_operation_name(s->active.kind), offset,
                        s->active.remaining_ns);
    memset(&s->active, 0, sizeof(s->active));
    w30_update_views(s);
}

static void w30_resume(W30State *s)
{
    W30Operation op;

    if (s->suspended_program.kind == W30_OP_PROGRAM) {
        op = s->suspended_program;
        memset(&s->suspended_program, 0, sizeof(s->suspended_program));
    } else {
        op = s->suspended_erase;
        memset(&s->suspended_erase, 0, sizeof(s->suspended_erase));
    }
    w30_start_operation(s, op);
}

static void w30_write_native(W30State *s, uint32_t offset, uint64_t value,
                             unsigned size)
{
    uint8_t command = value;
    unsigned partition = w30_partition(s, offset);

    if (s->active.kind != W30_OP_NONE &&
        !w30_read_command_while_active(s, partition, command)) {
        if (command == 0xb0) {
            w30_suspend_active(s, offset, partition);
        }
        return;
    }
    if (s->suspended_program.kind == W30_OP_PROGRAM && command != 0xd0 &&
        command != 0xff && command != 0x90 && command != 0x98 &&
        command != 0x70 && command != 0x50) {
        w30_sequence_error(s, partition);
        return;
    }

    switch (s->phase) {
    case W30_PHASE_PROGRAM_DATA:
        s->phase = W30_PHASE_IDLE;
        if (offset == s->setup_offset) {
            w30_start_program(s, offset, value, size);
        } else {
            w30_sequence_error(s, partition);
        }
        return;
    case W30_PHASE_ERASE_CONFIRM:
        s->phase = W30_PHASE_IDLE;
        if (command == 0xd0 &&
            w30_block(s, offset, NULL, NULL) ==
            w30_block(s, s->setup_offset, NULL, NULL)) {
            w30_start_erase(s, offset);
        } else {
            w30_sequence_error(s, partition);
        }
        return;
    case W30_PHASE_PROTECTION_DATA:
        s->phase = W30_PHASE_IDLE;
        if (offset == s->setup_offset) {
            w30_protection_program(s, offset, value, size);
        } else {
            w30_sequence_error(s, partition);
        }
        return;
    case W30_PHASE_EFP_CONFIRM:
        s->phase = W30_PHASE_IDLE;
        w30_sequence_error(s, partition);
        return;
    case W30_PHASE_LOCK_CONFIRM: {
        int block = w30_block(s, offset, NULL, NULL);
        int setup_block = w30_block(s, s->setup_offset, NULL, NULL);

        s->phase = W30_PHASE_IDLE;
        if (block < 0 || (command == 0x03 ? offset != s->setup_offset :
                                           block != setup_block)) {
            w30_sequence_error(s, partition);
        } else if (command == 0x01) {
            w30_bit_set(s->locked_blocks, block, true);
        } else if (command == 0xd0) {
            w30_bit_set(s->locked_blocks, block, false);
        } else if (command == 0x2f) {
            w30_bit_set(s->locked_blocks, block, true);
            w30_bit_set(s->lockdown_blocks, block, true);
        } else if (command == 0x03) {
            s->configuration[partition] = offset >> 1;
            s->mode[partition] = W30_MODE_ARRAY;
        } else {
            w30_sequence_error(s, partition);
        }
        w30_update_views(s);
        return;
    }
    default:
        break;
    }

    s->setup_partition = partition;
    s->setup_offset = offset;
    switch (command) {
    case 0xff:
        s->mode[partition] = W30_MODE_ARRAY;
        break;
    case 0x90:
        s->mode[partition] = W30_MODE_ID;
        break;
    case 0x98:
        s->mode[partition] = W30_MODE_CFI;
        break;
    case 0x70:
        s->mode[partition] = W30_MODE_STATUS;
        break;
    case 0x50:
        s->status[partition] &= ~W30_SR_CLEARABLE;
        s->mode[partition] = W30_MODE_ARRAY;
        break;
    case 0x40:
    case 0x10:
        s->phase = W30_PHASE_PROGRAM_DATA;
        break;
    case 0x20:
        s->phase = W30_PHASE_ERASE_CONFIRM;
        break;
    case 0x60:
        s->phase = W30_PHASE_LOCK_CONFIRM;
        break;
    case 0xc0:
        s->phase = W30_PHASE_PROTECTION_DATA;
        break;
    case 0x30:
        s->phase = W30_PHASE_EFP_CONFIRM;
        break;
    case 0xd0:
        if (s->suspended_program.kind == W30_OP_PROGRAM ||
            s->suspended_erase.kind == W30_OP_ERASE) {
            w30_resume(s);
        }
        break;
    default:
        break;
    }
    w30_update_views(s);
}

static void w30_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    W30View *view = opaque;
    W30State *s = view->flash;

    if (view->command_visible) {
        w30_write_native(s, offset, value, size);
    }
    trace_w30_command(w30_trace_icount(), w30_trace_pc(),
                      view->command_visible ? "native" : "low", offset,
                      size, value,
                      w30_mode_name(s->mode[w30_partition(s, offset)]),
                      w30_phase_name(s->phase),
                      w30_operation_name(s->active.kind));
}

static const MemoryRegionOps w30_ops = {
    .read = w30_read,
    .write = w30_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .impl.unaligned = true,
};

static void w30_reset_hold(Object *obj, ResetType type)
{
    W30State *s = W30_COMMON(obj);
    const W30Geometry *geometry = w30_geometry(s);

    timer_del(s->operation_timer);
    memset(s->mode, 0, sizeof(s->mode));
    memset(s->status, 0, sizeof(s->status));
    for (unsigned i = 0; i < geometry->partition_count; i++) {
        s->status[i] = W30_SR_READY;
    }
    s->phase = W30_PHASE_IDLE;
    memset(&s->active, 0, sizeof(s->active));
    memset(&s->suspended_erase, 0, sizeof(s->suspended_erase));
    memset(&s->suspended_program, 0, sizeof(s->suspended_program));
    memset(s->locked_blocks, 0xff, sizeof(s->locked_blocks));
    memset(s->lockdown_blocks, 0, sizeof(s->lockdown_blocks));
    w30_update_views(s);
    trace_w30_reset(w30_trace_icount(), w30_trace_pc());
}

static void w30_realize(DeviceState *dev, Error **errp)
{
    W30State *s = W30_COMMON(dev);
    const W30Geometry *geometry = w30_geometry(s);
    g_autofree char *native_name = NULL;
    g_autofree char *low_name = NULL;
    int64_t length;
    uint64_t perm;
    int ret;

    if (!w30_parse_uid(s->factory_uid_text, s->factory_uid)) {
        error_setg(errp, "factory-uid must be exactly 16 hexadecimal digits");
        return;
    }
    native_name = g_strdup_printf("%s.native", geometry->region_prefix);
    if (!x55_nor_init(&s->nor, &s->native, OBJECT(s), &w30_ops,
                                       &s->native_view, native_name,
                                       geometry->size, W30_PARTITION_SIZE,
                                       w30_array_byte, errp)) {
        return;
    }
    s->storage = memory_region_get_ram_ptr(&s->nor.backing);
    if (!s->blk) {
        memset(s->storage, 0xff, geometry->size);
    } else {
        length = blk_getlength(s->blk);
        if (length != geometry->size) {
            error_setg(errp, "backend must be exactly %u MiB (got %" PRId64
                       " bytes)", (unsigned)(geometry->size / MiB), length);
            return;
        }
        s->writable = blk_supports_write_perm(s->blk);
        perm = BLK_PERM_CONSISTENT_READ |
               (s->writable ? BLK_PERM_WRITE : 0);
        ret = blk_set_perm(s->blk, perm, BLK_PERM_ALL, errp);
        if (ret < 0) {
            return;
        }
        ret = blk_pread(s->blk, 0, geometry->size, s->storage, 0);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "failed to read flash backend");
            return;
        }
    }
    low_name = g_strdup_printf("%s.low-array", geometry->region_prefix);
    memory_region_init_io(&s->low, OBJECT(s), &w30_ops, &s->low_view,
                          low_name, geometry->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->low);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->native);
    device_cold_reset(dev);
}

static void w30_unrealize(DeviceState *dev)
{
    W30State *s = W30_COMMON(dev);

    timer_del(s->operation_timer);
    s->storage = NULL;
}

static void w30_init(Object *obj)
{
    W30State *s = W30_COMMON(obj);

    s->low_view.flash = s;
    s->low_view.command_visible = false;
    s->native_view.flash = s;
    s->native_view.command_visible = true;
    memset(s->customer, 0xff, sizeof(s->customer));
    s->operation_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      w30_operation_complete, s);
}

static void w30_finalize(Object *obj)
{
    W30State *s = W30_COMMON(obj);

    timer_free(s->operation_timer);
}

static const Property w30_properties[] = {
    DEFINE_PROP_DRIVE("drive", W30State, blk),
    DEFINE_PROP_STRING("factory-uid", W30State, factory_uid_text),
};

static X55NORTraceState *w30_nor_trace(Object *obj)
{
    return &W30_COMMON(obj)->nor_trace;
}

static bool w30_array_byte(Object *obj, MemoryRegion *view, uint32_t offset)
{
    W30State *s = W30_COMMON(obj);
    unsigned partition;

    if (offset >= w30_geometry(s)->size) {
        return false;
    }
    if (view == &s->low) {
        return true;
    }
    partition = w30_partition(s, offset);
    return s->mode[partition] == W30_MODE_ARRAY &&
           (s->active.kind == W30_OP_NONE || s->active.partition != partition);
}

static const VMStateDescription vmstate_w30_operation = {
    .name = "w30/operation",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(kind, W30Operation),
        VMSTATE_UINT32(offset, W30Operation),
        VMSTATE_UINT8(partition, W30Operation),
        VMSTATE_UINT16(block, W30Operation),
        VMSTATE_UINT16(value, W30Operation),
        VMSTATE_INT64(remaining_ns, W30Operation),
        VMSTATE_INT64(deadline_ns, W30Operation),
        VMSTATE_BOOL(program_error, W30Operation),
        VMSTATE_END_OF_LIST()
    },
};

static int w30_post_load(void *opaque, int version_id)
{
    W30State *s = opaque;
    const W30Geometry *g = w30_geometry(s);
    W30Operation *ops[] = { &s->active, &s->suspended_erase,
                           &s->suspended_program };

    if (s->phase > W30_PHASE_EFP_CONFIRM ||
        s->setup_partition >= g->partition_count ||
        s->setup_offset >= g->size) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < W30_MAX_PARTITIONS; i++) {
        if (s->mode[i] > W30_MODE_STATUS) {
            return -EINVAL;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(ops); i++) {
        W30Operation *op = ops[i];
        if (op->kind > W30_OP_PROTECTION_PROGRAM ||
            (op->kind && (op->partition >= g->partition_count ||
                          op->block >= g->block_count ||
                          op->offset >= g->size))) {
            return -EINVAL;
        }
    }
    w30_update_views(s);
    return 0;
}

static const VMStateDescription vmstate_w30_common = {
    .name = "w30",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = w30_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(active, W30State, 0, vmstate_w30_operation,
                       W30Operation),
        VMSTATE_STRUCT(suspended_erase, W30State, 0, vmstate_w30_operation,
                       W30Operation),
        VMSTATE_STRUCT(suspended_program, W30State, 0,
                       vmstate_w30_operation, W30Operation),
        VMSTATE_BUFFER(factory_uid, W30State),
        VMSTATE_BUFFER(customer, W30State),
        VMSTATE_BUFFER(status, W30State),
        VMSTATE_BOOL(customer_locked, W30State),
        VMSTATE_UINT32_ARRAY(mode, W30State, W30_MAX_PARTITIONS),
        VMSTATE_UINT16_ARRAY(configuration, W30State, W30_MAX_PARTITIONS),
        VMSTATE_UINT32(phase, W30State),
        VMSTATE_UINT32(setup_offset, W30State),
        VMSTATE_UINT8(setup_partition, W30State),
        VMSTATE_UINT64_ARRAY(erased_blocks, W30State, W30_BITMAP_WORDS),
        VMSTATE_UINT64_ARRAY(locked_blocks, W30State, W30_BITMAP_WORDS),
        VMSTATE_UINT64_ARRAY(lockdown_blocks, W30State, W30_BITMAP_WORDS),
        VMSTATE_TIMER_PTR(operation_timer, W30State),
        VMSTATE_END_OF_LIST()
    },
};

static MemoryRegion *w30_native_view(Object *obj)
{
    return &W30_COMMON(obj)->native;
}

static void w30_common_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    X55NORTraceClass *nc = X55_NOR_TRACE_CLASS(klass);

    nc->state = w30_nor_trace;
    nc->array_byte = w30_array_byte;
    X55_NOR_VIEW_CLASS(klass)->native_view = w30_native_view;
    dc->realize = w30_realize;
    dc->unrealize = w30_unrealize;
    device_class_set_props(dc, w30_properties);
    rc->phases.hold = w30_reset_hold;
}

static void w30_concrete_class_init(ObjectClass *klass, const void *data)
{
    W30Class *wc = W30_COMMON_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    wc->geometry = data;
    dc->vmsd = &vmstate_w30_common;
    dc->desc = wc->geometry->description;
}

static const TypeInfo w30_common_type_info = {
    .name = TYPE_W30_COMMON,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(W30State),
    .instance_align = __alignof__(W30State),
    .class_size = sizeof(W30Class),
    .instance_init = w30_init,
    .instance_finalize = w30_finalize,
    .class_init = w30_common_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_X55_NOR_TRACE }, { TYPE_X55_NOR_VIEW }, { }
    },
    .abstract = true,
};

static const TypeInfo w30_64mbit_top_type_info = {
    .name = TYPE_W30_64MBIT_TOP,
    .parent = TYPE_W30_COMMON,
    .class_init = w30_concrete_class_init,
    .class_data = &w30_64mbit_top_geometry,
};

static const TypeInfo w30_128mbit_top_type_info = {
    .name = TYPE_W30_128MBIT_TOP,
    .parent = TYPE_W30_COMMON,
    .class_init = w30_concrete_class_init,
    .class_data = &w30_128mbit_top_geometry,
};

static void w30_register_types(void)
{
    type_register_static(&w30_common_type_info);
    type_register_static(&w30_64mbit_top_type_info);
    type_register_static(&w30_128mbit_top_type_info);
}

type_init(w30_register_types)
