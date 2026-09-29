/*
 * M58-family NOR flash command engine.
 *
 * The two exported regions model board wiring rather than two arrays.  The
 * low view is array-only; the native view exposes the shared command state.
 * Program and erase complete immediately because no target timing evidence is
 * available yet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/memalign.h"
#include "qemu/module.h"
#include "exec/icount.h"
#include "hw/block/m58lw064d.h"
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

#define M58_STATUS_READY         0x80
#define M58_STATUS_ERASE_ERROR   0x20
#define M58_STATUS_PROGRAM_ERROR 0x10
#define M58_STATUS_SEQUENCE_ERROR \
    (M58_STATUS_READY | M58_STATUS_ERASE_ERROR | M58_STATUS_PROGRAM_ERROR)
#define M58_WRITE_BUFFER_BYTES   32
#define M58_WRITE_BUFFER_WORDS   16

typedef enum M58ReadMode {
    M58_MODE_ARRAY,
    M58_MODE_ID,
    M58_MODE_STATUS,
    M58_MODE_CFI,
} M58ReadMode;

typedef enum M58CommandPhase {
    M58_PHASE_IDLE,
    M58_PHASE_PROGRAM_DATA,
    M58_PHASE_ERASE_CONFIRM,
} M58CommandPhase;

typedef enum M58WriteBufferState {
    M58_WB_IDLE,
    M58_WB_EXPECT_COUNT,
    M58_WB_LOADING,
    M58_WB_EXPECT_CONFIRM,
} M58WriteBufferState;

typedef struct M58WriteBufferEntry {
    uint32_t offset;
    uint16_t value;
} M58WriteBufferEntry;

typedef struct M58Geometry {
    const char *description;
    const char *region_prefix;
    uint32_t size;
    uint32_t block_size;
    uint8_t manufacturer_id;
    uint8_t device_id;
    uint8_t cfi_capacity;
    uint16_t cfi_erase_blocks_minus_one;
} M58Geometry;

typedef struct M58State M58State;
typedef struct M58Class M58Class;

#define TYPE_M58_COMMON "m58-common"
OBJECT_DECLARE_TYPE(M58State, M58Class, M58_COMMON)

typedef struct M58View {
    M58State *flash;
    bool command_visible;
} M58View;

struct M58State {
    SysBusDevice parent_obj;
    X55NORTraceState nor_trace;

    BlockBackend *blk;
    char *factory_uid;
    uint8_t uid[8];
    uint8_t *storage;
    bool writable;

    MemoryRegion low;
    MemoryRegion native;
    X55NORStorage nor;
    M58View low_view;
    M58View native_view;

    M58ReadMode mode;
    M58CommandPhase phase;
    M58WriteBufferState wb_state;
    uint8_t status;
    uint8_t wb_words_left;
    uint32_t wb_block;
    uint32_t wb_page;
    M58WriteBufferEntry wb[M58_WRITE_BUFFER_WORDS];
    unsigned wb_count;
};

struct M58Class {
    SysBusDeviceClass parent_class;
    const M58Geometry *geometry;
};

static const M58Geometry m58lw064d_geometry = {
    .description = "ST M58LW064D 64-Mbit NOR flash",
    .region_prefix = "m58lw064d",
    .size = M58LW064D_SIZE,
    .block_size = M58_UNIFORM_BLOCK_SIZE,
    .manufacturer_id = 0x20,
    .device_id = 0x17,
    .cfi_capacity = 0x17,
    .cfi_erase_blocks_minus_one = M58LW064D_BLOCKS - 1,
};

static const M58Geometry m58_compatible_32mbit_geometry = {
    .description = "Calibrated 32-Mbit M58-compatible NOR flash",
    .region_prefix = "m58-compatible-32mbit",
    .size = M58_COMPATIBLE_32MBIT_SIZE,
    .block_size = M58_UNIFORM_BLOCK_SIZE,
    .manufacturer_id = 0x20,
    .device_id = 0x17,
    .cfi_capacity = 0x16,
    .cfi_erase_blocks_minus_one = M58_COMPATIBLE_32MBIT_BLOCKS - 1,
};

static bool m58_array_byte(Object *obj, MemoryRegion *view, uint32_t offset);

static const M58Geometry *m58_geometry(M58State *s)
{
    return M58_COMMON_GET_CLASS(s)->geometry;
}

static uint64_t m58_trace_icount(void)
{
    return icount_enabled() ? icount_get_raw() : 0;
}

static uint32_t m58_trace_pc(void)
{
    CPUClass *cc;

    if (!current_cpu) {
        return 0;
    }
    cc = CPU_GET_CLASS(current_cpu);
    return cc->get_pc ? cc->get_pc(current_cpu) : 0;
}

static const char *m58_mode_name(M58ReadMode mode)
{
    switch (mode) {
    case M58_MODE_ID:
        return "id";
    case M58_MODE_STATUS:
        return "status";
    case M58_MODE_CFI:
        return "cfi";
    default:
        return "array";
    }
}

static const char *m58_phase_name(M58CommandPhase phase)
{
    switch (phase) {
    case M58_PHASE_PROGRAM_DATA:
        return "program-data";
    case M58_PHASE_ERASE_CONFIRM:
        return "erase-confirm";
    default:
        return "idle";
    }
}

static const char *m58_wb_name(M58WriteBufferState state)
{
    switch (state) {
    case M58_WB_EXPECT_COUNT:
        return "expect-count";
    case M58_WB_LOADING:
        return "loading";
    case M58_WB_EXPECT_CONFIRM:
        return "expect-confirm";
    default:
        return "idle";
    }
}

static void m58_wb_reset(M58State *s)
{
    s->wb_state = M58_WB_IDLE;
    s->wb_words_left = 0;
    s->wb_block = 0;
    s->wb_page = 0;
    s->wb_count = 0;
}

static void m58_command_reset(M58State *s)
{
    s->phase = M58_PHASE_IDLE;
    m58_wb_reset(s);
}

static void m58_sequence_error(M58State *s)
{
    s->status |= M58_STATUS_SEQUENCE_ERROR;
    s->mode = M58_MODE_STATUS;
    m58_command_reset(s);
}

static bool m58_parse_uid(const char *text, uint8_t uid[8])
{
    const char *p = text;

    if (!p) {
        memset(uid, 0, 8);
        return true;
    }
    if (g_str_has_prefix(p, "0x") || g_str_has_prefix(p, "0X")) {
        p += 2;
    }
    if (strlen(p) != 16) {
        return false;
    }
    for (unsigned i = 0; i < 8; i++) {
        int hi = g_ascii_toupper(p[2 * i]);
        int lo = g_ascii_toupper(p[2 * i + 1]);

        hi = hi >= '0' && hi <= '9' ? hi - '0' :
             hi >= 'A' && hi <= 'F' ? hi - 'A' + 10 : -1;
        lo = lo >= '0' && lo <= '9' ? lo - '0' :
             lo >= 'A' && lo <= 'F' ? lo - 'A' + 10 : -1;
        if (hi < 0 || lo < 0) {
            return false;
        }
        uid[i] = (hi << 4) | lo;
    }
    return true;
}

static void m58_persist(M58State *s, uint32_t offset, uint32_t bytes)
{
    int ret;

    if (!s->blk || !s->writable) {
        return;
    }
    ret = blk_pwrite(s->blk, offset, bytes, s->storage + offset, 0);
    trace_m58lw064d_persistence(m58_trace_icount(), m58_trace_pc(),
                               offset, bytes, ret);
    if (ret < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: backend write at 0x%x failed: %s\n",
                      object_get_typename(OBJECT(s)), offset, strerror(-ret));
    }
}

static uint8_t m58_cfi_byte(M58State *s, hwaddr offset)
{
    static const uint8_t cfi[] = {
        [0x10] = 'Q', [0x11] = 'R', [0x12] = 'Y',
        [0x13] = 0x01, [0x14] = 0x00,
        [0x15] = 0x31, [0x16] = 0x00,
        [0x17] = 0x00, [0x18] = 0x00,
        [0x1b] = 0x27, [0x1c] = 0x36, [0x1d] = 0x00, [0x1e] = 0x00,
        [0x1f] = 0x04, [0x20] = 0x08, [0x21] = 0x0a, [0x22] = 0x00,
        [0x23] = 0x04, [0x24] = 0x04, [0x25] = 0x04, [0x26] = 0x00,
        [0x27] = 0,
        [0x28] = 0x02, [0x29] = 0x00,
        [0x2a] = 0x05, [0x2b] = 0x00,
        [0x2c] = 0x01,
        [0x2d] = 0, [0x2e] = 0,
        [0x2f] = 0x00, [0x30] = 0x02,
        [0x31] = 'P', [0x32] = 'R', [0x33] = 'I',
        [0x34] = '1', [0x35] = '1',
        [0x36] = 0xce, [0x37] = 0x00, [0x38] = 0x00, [0x39] = 0x00,
        [0x3a] = 0x01, [0x3b] = 0x01, [0x3c] = 0x00, [0x3d] = 0x33,
        [0x3e] = 0x00, [0x3f] = 0x01, [0x40] = 0x80, [0x41] = 0x00,
        [0x42] = 0x03, [0x43] = 0x03,
    };
    hwaddr word = offset >> 1;

    if ((offset & 1) || word >= ARRAY_SIZE(cfi)) {
        return 0;
    }
    if (word == 0x27) {
        return m58_geometry(s)->cfi_capacity;
    }
    if (word == 0x2d) {
        return m58_geometry(s)->cfi_erase_blocks_minus_one;
    }
    if (word == 0x2e) {
        return m58_geometry(s)->cfi_erase_blocks_minus_one >> 8;
    }
    return cfi[word];
}

static uint8_t m58_id_byte(M58State *s, hwaddr offset)
{
    offset &= 0xffff;
    switch (offset) {
    case 0x0000:
        return m58_geometry(s)->manufacturer_id;
    case 0x0002:
        return m58_geometry(s)->device_id;
    case 0x0100:
        return 0xfe;
    default:
        if (offset >= 0x0102 && offset <= 0x0109) {
            return s->uid[offset - 0x0102];
        }
        return 0;
    }
}

static uint8_t m58_read_byte(M58View *view, hwaddr offset)
{
    M58State *s = view->flash;

    if (!view->command_visible || s->mode == M58_MODE_ARRAY) {
        return s->storage[offset];
    }
    switch (s->mode) {
    case M58_MODE_ID:
        return m58_id_byte(s, offset);
    case M58_MODE_STATUS:
        return (offset & 1) ? 0 : s->status;
    case M58_MODE_CFI:
        return m58_cfi_byte(s, offset);
    default:
        return s->storage[offset];
    }
}

static uint64_t m58_read(void *opaque, hwaddr offset, unsigned size)
{
    M58View *view = opaque;
    M58State *s = view->flash;
    uint64_t value = 0;

    for (unsigned i = 0; i < size; i++) {
        value |= (uint64_t)m58_read_byte(view, offset + i) << (8 * i);
    }
    trace_m58lw064d_read(m58_trace_icount(), m58_trace_pc(),
                        view->command_visible ? "native" : "low",
                        offset, size, value, m58_mode_name(s->mode));
    return value;
}

static bool m58_program_byte(M58State *s, uint32_t offset,
                             uint8_t value)
{
    uint8_t old = s->storage[offset];
    bool valid = (old & value) == value;

    s->storage[offset] = old & value;
    return valid;
}

static void m58_flush_storage(M58State *s, hwaddr offset, hwaddr size)
{
    x55_nor_flush(&s->nor, offset, size);
}

static bool m58_program(M58State *s, uint32_t offset,
                        uint64_t value, unsigned size)
{
    bool valid = true;

    if (offset + size > m58_geometry(s)->size ||
        (size != 1 && size != 2)) {
        return false;
    }
    for (unsigned i = 0; i < size; i++) {
        valid &= m58_program_byte(s, offset + i, value >> (8 * i));
    }
    m58_flush_storage(s, offset, size);
    m58_persist(s, offset, size);
    x55_nor_trace_program(&s->nor_trace, offset, s->storage + offset, size);
    trace_m58lw064d_program(m58_trace_icount(), m58_trace_pc(), offset,
                           size, value, valid, s->status);
    return valid;
}

static bool m58_erase(M58State *s, uint32_t offset)
{
    const M58Geometry *geometry = m58_geometry(s);
    uint32_t start;

    if (offset >= geometry->size) {
        return false;
    }
    start = offset & ~(geometry->block_size - 1);
    memset(s->storage + start, 0xff, geometry->block_size);
    m58_flush_storage(s, start, geometry->block_size);
    m58_persist(s, start, geometry->block_size);
    x55_nor_trace_erase(&s->nor_trace, start, geometry->block_size);
    trace_m58lw064d_erase(m58_trace_icount(), m58_trace_pc(), start,
                         geometry->block_size, s->status);
    return true;
}

static void m58_write_command(M58State *s, uint32_t offset,
                              uint8_t command)
{
    switch (command) {
    case 0xff:
        s->mode = M58_MODE_ARRAY;
        m58_command_reset(s);
        break;
    case 0x90:
        s->mode = M58_MODE_ID;
        m58_command_reset(s);
        break;
    case 0x70:
        s->mode = M58_MODE_STATUS;
        m58_command_reset(s);
        break;
    case 0x98:
        s->mode = M58_MODE_CFI;
        m58_command_reset(s);
        break;
    case 0x50:
        s->status = M58_STATUS_READY;
        m58_command_reset(s);
        break;
    case 0x40:
    case 0x10:
        s->phase = M58_PHASE_PROGRAM_DATA;
        m58_wb_reset(s);
        break;
    case 0x20:
        s->phase = M58_PHASE_ERASE_CONFIRM;
        m58_wb_reset(s);
        break;
    case 0xe8:
        s->mode = M58_MODE_STATUS;
        s->phase = M58_PHASE_IDLE;
        s->wb_state = M58_WB_EXPECT_COUNT;
        s->wb_words_left = 0;
        s->wb_block = offset & ~(m58_geometry(s)->block_size - 1);
        s->wb_page = UINT32_MAX;
        s->wb_count = 0;
        break;
    default:
        break;
    }
}

static void m58_write_native(M58State *s, hwaddr offset,
                             uint64_t value, unsigned size)
{
    uint8_t command = value;

    if (s->phase == M58_PHASE_PROGRAM_DATA) {
        bool valid = m58_program(s, offset, value, size);

        s->phase = M58_PHASE_IDLE;
        s->mode = M58_MODE_STATUS;
        s->status |= valid ? M58_STATUS_READY :
                             M58_STATUS_READY | M58_STATUS_PROGRAM_ERROR;
        return;
    }
    if (s->phase == M58_PHASE_ERASE_CONFIRM) {
        if (command == 0xd0 && m58_erase(s, offset)) {
            s->phase = M58_PHASE_IDLE;
            s->mode = M58_MODE_STATUS;
            s->status |= M58_STATUS_READY;
        } else {
            m58_sequence_error(s);
        }
        return;
    }
    if (s->wb_state == M58_WB_EXPECT_COUNT) {
        uint32_t block = offset & ~(m58_geometry(s)->block_size - 1);

        if (block != s->wb_block || command >= M58_WRITE_BUFFER_WORDS) {
            m58_sequence_error(s);
            return;
        }
        s->wb_words_left = command + 1;
        s->wb_state = M58_WB_LOADING;
        trace_m58lw064d_buffer(m58_trace_icount(), m58_trace_pc(), "count",
                              offset, s->wb_words_left,
                              s->wb_count);
        return;
    }
    if (s->wb_state == M58_WB_LOADING) {
        uint32_t block = offset & ~(m58_geometry(s)->block_size - 1);
        uint32_t page = offset & ~(M58_WRITE_BUFFER_BYTES - 1);

        if (size != 2 || (offset & 1) ||
            offset + 1 >= m58_geometry(s)->size ||
            block != s->wb_block ||
            (s->wb_page != UINT32_MAX && page != s->wb_page)) {
            m58_sequence_error(s);
            return;
        }
        s->wb_page = page;
        s->wb[s->wb_count++] = (M58WriteBufferEntry) {
            .offset = offset,
            .value = value,
        };
        if (--s->wb_words_left == 0) {
            s->wb_state = M58_WB_EXPECT_CONFIRM;
        }
        trace_m58lw064d_buffer(m58_trace_icount(), m58_trace_pc(), "data",
                              offset, s->wb_words_left,
                              s->wb_count);
        return;
    }
    if (s->wb_state == M58_WB_EXPECT_CONFIRM) {
        bool valid = true;

        if (command != 0xd0) {
            m58_sequence_error(s);
            return;
        }
        for (unsigned i = 0; i < s->wb_count; i++) {
            valid &= m58_program(s, s->wb[i].offset, s->wb[i].value, 2);
        }
        s->mode = M58_MODE_STATUS;
        s->status |= valid ? M58_STATUS_READY :
                             M58_STATUS_READY | M58_STATUS_PROGRAM_ERROR;
        trace_m58lw064d_buffer(m58_trace_icount(), m58_trace_pc(),
                              "confirm", offset, 0, s->wb_count);
        m58_wb_reset(s);
        return;
    }
    m58_write_command(s, offset, command);
}

static void m58_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    M58View *view = opaque;
    M58State *s = view->flash;

    if (view->command_visible) {
        m58_write_native(s, offset, value, size);
        x55_nor_update(&s->nor);
    }
    trace_m58lw064d_command(m58_trace_icount(), m58_trace_pc(),
                           view->command_visible ? "native" : "low",
                           offset, size, value, m58_mode_name(s->mode),
                           s->status, m58_phase_name(s->phase),
                           m58_wb_name(s->wb_state));
}

static const MemoryRegionOps m58_ops = {
    .read = m58_read,
    .write = m58_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .impl.unaligned = true,
};

static void m58_reset_hold(Object *obj, ResetType type)
{
    M58State *s = M58_COMMON(obj);

    s->mode = M58_MODE_ARRAY;
    s->status = M58_STATUS_READY;
    m58_command_reset(s);
    x55_nor_update(&s->nor);
    trace_m58lw064d_reset(m58_trace_icount(), m58_trace_pc());
}

static void m58_realize(DeviceState *dev, Error **errp)
{
    M58State *s = M58_COMMON(dev);
    const M58Geometry *geometry = m58_geometry(s);
    g_autofree char *native_name = NULL;
    g_autofree char *low_name = NULL;
    int64_t length;
    uint64_t perm;
    int ret;

    if (!m58_parse_uid(s->factory_uid, s->uid)) {
        error_setg(errp, "factory-uid must be exactly 16 hexadecimal digits");
        return;
    }
    native_name = g_strdup_printf("%s.native", geometry->region_prefix);
    if (!x55_nor_init(&s->nor, &s->native, OBJECT(s), &m58_ops,
                                       &s->native_view, native_name,
                                       geometry->size, geometry->size,
                                       m58_array_byte, errp)) {
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
    memory_region_init_io(&s->low, OBJECT(s), &m58_ops, &s->low_view,
                          low_name, geometry->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->low);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->native);
    device_cold_reset(dev);
}

static void m58_unrealize(DeviceState *dev)
{
    M58State *s = M58_COMMON(dev);

    s->storage = NULL;
}

static void m58_init(Object *obj)
{
    M58State *s = M58_COMMON(obj);

    s->low_view.flash = s;
    s->low_view.command_visible = false;
    s->native_view.flash = s;
    s->native_view.command_visible = true;
}

static const Property m58_properties[] = {
    DEFINE_PROP_DRIVE("drive", M58State, blk),
    DEFINE_PROP_STRING("factory-uid", M58State, factory_uid),
};

static X55NORTraceState *m58_nor_trace(Object *obj)
{
    return &M58_COMMON(obj)->nor_trace;
}

static bool m58_array_byte(Object *obj, MemoryRegion *view, uint32_t offset)
{
    M58State *s = M58_COMMON(obj);

    return offset < m58_geometry(s)->size &&
           (view == &s->low || s->mode == M58_MODE_ARRAY);
}

static const VMStateDescription vmstate_m58_buffer = {
    .name = "m58/buffer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(offset, M58WriteBufferEntry),
        VMSTATE_UINT16(value, M58WriteBufferEntry),
        VMSTATE_END_OF_LIST()
    },
};

static int m58_post_load(void *opaque, int version_id)
{
    M58State *s = opaque;

    if (s->mode > M58_MODE_CFI || s->phase > M58_PHASE_ERASE_CONFIRM ||
        s->wb_state > M58_WB_EXPECT_CONFIRM ||
        s->wb_count > M58_WRITE_BUFFER_WORDS ||
        s->wb_words_left > M58_WRITE_BUFFER_WORDS) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < s->wb_count; i++) {
        if (s->wb[i].offset >= m58_geometry(s)->size - 1) {
            return -EINVAL;
        }
    }
    x55_nor_update(&s->nor);
    return 0;
}

static const VMStateDescription vmstate_m58_common = {
    .name = "m58",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = m58_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(wb, M58State, M58_WRITE_BUFFER_WORDS, 0,
                             vmstate_m58_buffer, M58WriteBufferEntry),
        VMSTATE_BUFFER(uid, M58State),
        VMSTATE_UINT32(mode, M58State),
        VMSTATE_UINT32(phase, M58State),
        VMSTATE_UINT32(wb_state, M58State),
        VMSTATE_UINT32(wb_block, M58State),
        VMSTATE_UINT32(wb_page, M58State),
        VMSTATE_UINT32(wb_count, M58State),
        VMSTATE_UINT8(status, M58State),
        VMSTATE_UINT8(wb_words_left, M58State),
        VMSTATE_END_OF_LIST()
    },
};

static MemoryRegion *m58_native_view(Object *obj)
{
    return &M58_COMMON(obj)->native;
}

static void m58_common_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    X55NORTraceClass *nc = X55_NOR_TRACE_CLASS(klass);

    nc->state = m58_nor_trace;
    nc->array_byte = m58_array_byte;
    X55_NOR_VIEW_CLASS(klass)->native_view = m58_native_view;
    dc->realize = m58_realize;
    dc->unrealize = m58_unrealize;
    device_class_set_props(dc, m58_properties);
    rc->phases.hold = m58_reset_hold;
}

static void m58_concrete_class_init(ObjectClass *klass, const void *data)
{
    M58Class *mc = M58_COMMON_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    mc->geometry = data;
    dc->vmsd = &vmstate_m58_common;
    dc->desc = mc->geometry->description;
}

static const TypeInfo m58_common_type_info = {
    .name = TYPE_M58_COMMON,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(M58State),
    .instance_align = __alignof__(M58State),
    .class_size = sizeof(M58Class),
    .instance_init = m58_init,
    .class_init = m58_common_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_X55_NOR_TRACE }, { TYPE_X55_NOR_VIEW }, { }
    },
    .abstract = true,
};

static const TypeInfo m58lw064d_type_info = {
    .name = TYPE_M58LW064D,
    .parent = TYPE_M58_COMMON,
    .class_init = m58_concrete_class_init,
    .class_data = &m58lw064d_geometry,
};

static const TypeInfo m58_compatible_32mbit_type_info = {
    .name = TYPE_M58_COMPATIBLE_32MBIT,
    .parent = TYPE_M58_COMMON,
    .class_init = m58_concrete_class_init,
    .class_data = &m58_compatible_32mbit_geometry,
};

static void m58_register_types(void)
{
    type_register_static(&m58_common_type_info);
    type_register_static(&m58lw064d_type_info);
    type_register_static(&m58_compatible_32mbit_type_info);
}

type_init(m58_register_types)
