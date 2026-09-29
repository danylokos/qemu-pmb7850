/*
 * AMD AM29LV uniform-sector NOR flash command engine.
 *
 * The low region is an array-only board view.  The native region exposes the
 * x16 command interface, autoselect data, SecSi, and erase status.  Erase time
 * is a deterministic calibration of the CEMU PMB7850 oracle's 80,000 ticks;
 * it is not a measured handset timing claim.  Production x55 machines convert
 * those ticks through the same instruction-time policy as the PMB and XBUS.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "exec/icount.h"
#include "hw/block/am29lv.h"
#include "hw/block/x55_nor_trace.h"
#include "hw/core/clock.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "system/block-backend.h"
#include "system/cpus.h"
#include "trace.h"
#include "x55_nor.h"
#include "hw/block/x55_nor_view.h"

#define TYPE_AM29LV_COMMON "am29lv-common"
#define AM29_UNLOCK1_WORD 0x555
#define AM29_UNLOCK2_WORD 0x2aa
#define AM29_WRITE_BUFFER_WORDS 16
#define AM29_WRITE_BUFFER_BYTES (AM29_WRITE_BUFFER_WORDS * 2)
#define AM29_FACTORY_SECSI_INDICATOR 0x0098
#define AM29_ERASE_TIMEOUT_TICKS 8
#define AM29_SECTOR_ERASE_TICKS 80000

typedef enum AM29Mode {
    AM29_MODE_ARRAY,
    AM29_MODE_AUTOSELECT,
    AM29_MODE_SECSI,
} AM29Mode;

typedef enum AM29Phase {
    AM29_PHASE_IDLE,
    AM29_PHASE_EXPECT_55,
    AM29_PHASE_EXPECT_COMMAND,
    AM29_PHASE_PROGRAM,
    AM29_PHASE_SECSI_EXIT_ZERO,
    AM29_PHASE_SECSI_PROTECT_40,
    AM29_PHASE_WRITE_BUFFER_COUNT,
    AM29_PHASE_WRITE_BUFFER_DATA,
    AM29_PHASE_WRITE_BUFFER_CONFIRM,
    AM29_PHASE_ERASE_UNLOCK_AA,
    AM29_PHASE_ERASE_UNLOCK_55,
    AM29_PHASE_ERASE_CONFIRM,
} AM29Phase;

typedef struct AM29Geometry {
    const char *description;
    const char *region_prefix;
    uint32_t size;
    uint16_t manufacturer_id;
    uint16_t device_id1;
    uint16_t device_id2;
    uint16_t device_id3;
    uint32_t protected_start;
    uint32_t protected_size;
} AM29Geometry;

typedef struct AM29WriteBufferEntry {
    uint32_t offset;
    uint16_t value;
} AM29WriteBufferEntry;

typedef struct AM29State AM29State;
typedef struct AM29Class AM29Class;

OBJECT_DECLARE_TYPE(AM29State, AM29Class, AM29LV_COMMON)

typedef struct AM29View {
    AM29State *flash;
    bool command_visible;
} AM29View;

struct AM29State {
    SysBusDevice parent_obj;
    X55NORTraceState nor_trace;

    BlockBackend *blk;
    char *factory_secsi;
    uint8_t secsi[AM29LV_SECSI_SIZE];
    uint8_t *storage;
    bool writable;
    Clock *sysclk;
    bool instruction_timing;

    MemoryRegion low;
    MemoryRegion native;
    X55NORStorage nor;
    AM29View low_view;
    AM29View native_view;

    AM29Mode mode;
    AM29Phase phase;
    uint32_t protected_start;
    uint32_t protected_size;
    bool secsi_locked;

    AM29WriteBufferEntry write_buffer[AM29_WRITE_BUFFER_WORDS];
    unsigned write_buffer_count;
    uint8_t write_buffer_words_left;
    uint32_t write_buffer_sector;
    uint32_t write_buffer_page;

    QEMUTimer *erase_timer;
    bool erase_active;
    bool erase_suspended;
    bool erase_timeout;
    uint32_t erase_sector;
    int64_t erase_deadline;
    int64_t erase_remaining;
    bool erase_toggle;
};

struct AM29Class {
    SysBusDeviceClass parent_class;
    const AM29Geometry *geometry;
};

static const AM29Geometry am29lv640mh_geometry = {
    .description = "AMD AM29LV640MH 64-Mbit NOR flash",
    .region_prefix = TYPE_AM29LV640MH,
    .size = AM29LV640MH_SIZE,
    .manufacturer_id = 0x0001,
    .device_id1 = 0x227e,
    .device_id2 = 0x220c,
    .device_id3 = 0x2201,
    .protected_start = 0,
    .protected_size = 256 * KiB,
};

static const AM29Geometry am29lv128mh_geometry = {
    .description = "AMD AM29LV128MH 128-Mbit NOR flash",
    .region_prefix = TYPE_AM29LV128MH,
    .size = AM29LV128MH_SIZE,
    .manufacturer_id = 0x0001,
    .device_id1 = 0x227e,
    .device_id2 = 0x2212,
    .device_id3 = 0x2200,
    .protected_start = 8 * MiB,
    .protected_size = 256 * KiB,
};

static bool am29_array_byte(Object *obj, MemoryRegion *view, uint32_t offset);

static const AM29Geometry *am29_geometry(AM29State *s)
{
    return AM29LV_COMMON_GET_CLASS(s)->geometry;
}

static uint64_t am29_trace_icount(void)
{
    return icount_enabled() ? icount_get_raw() : 0;
}

static uint32_t am29_trace_pc(void)
{
    CPUClass *cc;

    if (!current_cpu) {
        return 0;
    }
    cc = CPU_GET_CLASS(current_cpu);
    return cc->get_pc ? cc->get_pc(current_cpu) : 0;
}

static const char *am29_mode_name(AM29Mode mode)
{
    switch (mode) {
    case AM29_MODE_AUTOSELECT:
        return "autoselect";
    case AM29_MODE_SECSI:
        return "secsi";
    default:
        return "array";
    }
}

static const char *am29_phase_name(AM29Phase phase)
{
    switch (phase) {
    case AM29_PHASE_EXPECT_55: return "expect-55";
    case AM29_PHASE_EXPECT_COMMAND: return "expect-command";
    case AM29_PHASE_PROGRAM: return "program";
    case AM29_PHASE_SECSI_EXIT_ZERO: return "secsi-exit-zero";
    case AM29_PHASE_SECSI_PROTECT_40: return "secsi-protect-40";
    case AM29_PHASE_WRITE_BUFFER_COUNT: return "write-buffer-count";
    case AM29_PHASE_WRITE_BUFFER_DATA: return "write-buffer-data";
    case AM29_PHASE_WRITE_BUFFER_CONFIRM: return "write-buffer-confirm";
    case AM29_PHASE_ERASE_UNLOCK_AA: return "erase-unlock-aa";
    case AM29_PHASE_ERASE_UNLOCK_55: return "erase-unlock-55";
    case AM29_PHASE_ERASE_CONFIRM: return "erase-confirm";
    default: return "idle";
    }
}

static bool am29_parse_secsi(const char *text, uint8_t secsi[AM29LV_SECSI_SIZE])
{
    const char *p = text;

    memset(secsi, 0xff, AM29LV_SECSI_SIZE);
    if (!p) {
        return true;
    }
    if (g_str_has_prefix(p, "0x") || g_str_has_prefix(p, "0X")) {
        p += 2;
    }
    if (strlen(p) != 2 * AM29LV_SECSI_SIZE) {
        return false;
    }
    for (unsigned i = 0; i < AM29LV_SECSI_SIZE; i++) {
        int hi = g_ascii_xdigit_value(p[2 * i]);
        int lo = g_ascii_xdigit_value(p[2 * i + 1]);

        if (hi < 0 || lo < 0) {
            return false;
        }
        secsi[i] = (hi << 4) | lo;
    }
    return true;
}

static bool am29_address_is_protected(AM29State *s, uint32_t offset)
{
    uint64_t end = (uint64_t)s->protected_start + s->protected_size;

    return s->protected_size && offset >= s->protected_start && offset < end;
}

static int64_t am29_ticks_to_ns(AM29State *s, int64_t ticks)
{
    return s->instruction_timing ? icount_to_ns(ticks) :
           clock_ticks_to_ns(s->sysclk, ticks);
}

static uint32_t am29_ns_to_ticks(AM29State *s, int64_t ns)
{
    uint64_t ticks;

    if (ns <= 0) {
        return 0;
    }
    if (s->instruction_timing) {
        int64_t quantum = icount_to_ns(1);

        ticks = DIV_ROUND_UP((uint64_t)ns, (uint64_t)MAX(quantum, 1));
    } else {
        ticks = clock_ns_to_ticks(s->sysclk, ns);
    }
    return MIN(ticks, UINT32_MAX);
}

static void am29_update_views(AM29State *s)
{
    x55_nor_update(&s->nor);
}

static void am29_flush_storage(AM29State *s, uint32_t offset, uint32_t size)
{
    x55_nor_flush(&s->nor, offset, size);
}

static void am29_persist(AM29State *s, uint32_t offset, uint32_t size)
{
    int ret;

    if (!s->blk || !s->writable) {
        return;
    }
    ret = blk_pwrite(s->blk, offset, size, s->storage + offset, 0);
    trace_am29lv_persistence(am29_trace_icount(), am29_trace_pc(), offset,
                            size, ret);
    if (ret < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: backend write at 0x%x failed: %s\n",
                      object_get_typename(OBJECT(s)), offset, strerror(-ret));
    }
}

static bool am29_program_byte(AM29State *s, uint32_t offset, uint8_t value)
{
    uint8_t old;

    if (offset >= am29_geometry(s)->size ||
        am29_address_is_protected(s, offset)) {
        return false;
    }
    old = s->storage[offset];
    s->storage[offset] &= value;
    return (old & value) == value;
}

static bool am29_program(AM29State *s, uint32_t offset, uint64_t value,
                         unsigned size)
{
    bool valid = size == 1 || size == 2;

    if (offset + size > am29_geometry(s)->size) {
        return false;
    }
    for (unsigned i = 0; i < size; i++) {
        valid &= am29_program_byte(s, offset + i, value >> (8 * i));
    }
    am29_flush_storage(s, offset, size);
    am29_persist(s, offset, size);
    if (size == 2 && !am29_address_is_protected(s, offset) &&
        !am29_address_is_protected(s, offset + 1)) {
        x55_nor_trace_program(&s->nor_trace, offset, s->storage + offset, size);
    } else {
        for (unsigned i = 0; i < size; i++) {
            if (!am29_address_is_protected(s, offset + i)) {
                x55_nor_trace_program(&s->nor_trace, offset + i,
                                      s->storage + offset + i, 1);
            }
        }
    }
    trace_am29lv_program(am29_trace_icount(), am29_trace_pc(), offset, size,
                        value, valid);
    return valid;
}

static void am29_write_buffer_reset(AM29State *s)
{
    s->write_buffer_count = 0;
    s->write_buffer_words_left = 0;
    s->write_buffer_sector = 0;
    s->write_buffer_page = 0;
}

static void am29_erase_complete(void *opaque)
{
    AM29State *s = opaque;

    if (!s->erase_active || s->erase_suspended) {
        return;
    }
    memset(s->storage + s->erase_sector, 0xff, AM29LV_SECTOR_SIZE);
    am29_flush_storage(s, s->erase_sector, AM29LV_SECTOR_SIZE);
    am29_persist(s, s->erase_sector, AM29LV_SECTOR_SIZE);
    x55_nor_trace_erase(&s->nor_trace, s->erase_sector, AM29LV_SECTOR_SIZE);
    trace_am29lv_erase(am29_trace_icount(), am29_trace_pc(), "complete",
                      s->erase_sector, 0);
    s->erase_active = false;
    s->erase_suspended = false;
    s->erase_timeout = false;
    s->erase_deadline = 0;
    s->erase_remaining = 0;
    s->erase_toggle = false;
    am29_update_views(s);
}

static void am29_start_erase(AM29State *s, uint32_t sector)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t duration = am29_ticks_to_ns(s, AM29_SECTOR_ERASE_TICKS);

    s->mode = AM29_MODE_ARRAY;
    s->erase_active = true;
    s->erase_suspended = false;
    s->erase_timeout = false;
    s->erase_sector = sector;
    s->erase_deadline = now + duration;
    s->erase_remaining = duration;
    s->erase_toggle = false;
    timer_mod(s->erase_timer, s->erase_deadline);
    am29_update_views(s);
    trace_am29lv_erase(am29_trace_icount(), am29_trace_pc(), "start", sector,
                      AM29_SECTOR_ERASE_TICKS);
}

static uint16_t am29_autoselect_word(AM29State *s, uint32_t offset)
{
    const AM29Geometry *geometry = am29_geometry(s);
    uint32_t word = (offset >> 1) & 0xfff;
    uint32_t sector_offset = offset & (AM29LV_SECTOR_SIZE - 1);

    switch (word) {
    case 0x000: return geometry->manufacturer_id;
    case 0x001: return geometry->device_id1;
    case 0x00e: return geometry->device_id2;
    case 0x00f: return geometry->device_id3;
    case 0x003:
        return s->secsi_locked ? AM29_FACTORY_SECSI_INDICATOR : 0;
    default:
        break;
    }
    /* X02 is +4 in the datasheet; +8 is the measured PMB-visible alias. */
    if (sector_offset == 4 || sector_offset == 8) {
        return am29_address_is_protected(s, offset) ? 1 : 0;
    }
    return 0;
}

static uint8_t am29_read_byte(AM29View *view, uint32_t offset)
{
    AM29State *s = view->flash;

    if (!view->command_visible) {
        return s->storage[offset];
    }
    if (s->erase_active && !(offset & 1) &&
        (!s->erase_suspended ||
         (offset & ~(AM29LV_SECTOR_SIZE - 1)) == s->erase_sector)) {
        uint8_t value = 0;

        s->erase_toggle = !s->erase_toggle;
        if (s->erase_suspended) {
            value |= 0x80;
        }
        if (!s->erase_suspended && s->erase_toggle) {
            value |= 0x40;
        }
        if ((offset & ~(AM29LV_SECTOR_SIZE - 1)) == s->erase_sector &&
            s->erase_toggle) {
            value |= 0x04;
        }
        if (s->erase_timeout ||
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >=
                s->erase_deadline -
                am29_ticks_to_ns(s, AM29_SECTOR_ERASE_TICKS) +
                am29_ticks_to_ns(s, AM29_ERASE_TIMEOUT_TICKS)) {
            s->erase_timeout = true;
            value |= 0x08;
        }
        return value;
    }
    if (s->mode == AM29_MODE_ARRAY) {
        return s->storage[offset];
    }
    if (s->mode == AM29_MODE_SECSI) {
        uint32_t secsi_offset = offset & (AM29LV_SECTOR_SIZE - 1);

        return secsi_offset < AM29LV_SECSI_SIZE ?
               s->secsi[secsi_offset] : 0xff;
    }
    {
        uint16_t value = am29_autoselect_word(s, offset);

        return offset & 1 ? value >> 8 : value;
    }
}

static uint64_t am29_read(void *opaque, hwaddr offset, unsigned size)
{
    AM29View *view = opaque;
    AM29State *s = view->flash;
    bool status = view->command_visible && s->erase_active &&
                  (!s->erase_suspended ||
                   (offset & ~(AM29LV_SECTOR_SIZE - 1)) == s->erase_sector);
    uint64_t value = 0;

    for (unsigned i = 0; i < size; i++) {
        value |= (uint64_t)am29_read_byte(view, offset + i) << (8 * i);
    }
    trace_am29lv_read(am29_trace_icount(), am29_trace_pc(),
                      view->command_visible ? "native" : "low", offset,
                      size, value, am29_mode_name(s->mode), s->erase_active,
                      s->erase_suspended, status);
    return value;
}

static void am29_command_reset(AM29State *s)
{
    s->mode = AM29_MODE_ARRAY;
    s->phase = AM29_PHASE_IDLE;
    am29_write_buffer_reset(s);
    am29_update_views(s);
}

static void am29_write_buffer_commit(AM29State *s)
{
    for (unsigned i = 0; i < s->write_buffer_count; i++) {
        AM29WriteBufferEntry *entry = &s->write_buffer[i];

        am29_program(s, entry->offset, entry->value, 2);
    }
}

static void am29_write_native(AM29State *s, uint32_t offset,
                              uint64_t value, unsigned size)
{
    uint8_t command = value;
    uint32_t word = (offset >> 1) & 0x7ff;
    uint32_t sector = offset & ~(AM29LV_SECTOR_SIZE - 1);

    if (s->erase_active && !s->erase_suspended && command == 0xb0) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        s->erase_remaining = MAX(s->erase_deadline - now, 1);
        timer_del(s->erase_timer);
        s->erase_suspended = true;
        s->erase_timeout = true;
        s->erase_toggle = false;
        trace_am29lv_erase(am29_trace_icount(), am29_trace_pc(), "suspend",
                          s->erase_sector,
                          am29_ns_to_ticks(s, s->erase_remaining));
        am29_update_views(s);
        return;
    }
    if (s->erase_active && s->erase_suspended &&
        s->mode == AM29_MODE_ARRAY && s->phase == AM29_PHASE_IDLE &&
        command == 0x30 &&
        sector == s->erase_sector) {
        s->erase_suspended = false;
        s->erase_toggle = false;
        s->erase_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                            s->erase_remaining;
        timer_mod(s->erase_timer, s->erase_deadline);
        trace_am29lv_erase(am29_trace_icount(), am29_trace_pc(), "resume",
                          s->erase_sector,
                          am29_ns_to_ticks(s, s->erase_remaining));
        am29_update_views(s);
        return;
    }
    if (s->erase_active && s->erase_suspended &&
        s->mode == AM29_MODE_ARRAY && s->phase == AM29_PHASE_IDLE &&
        command == 0x30) {
        trace_am29lv_erase(am29_trace_icount(), am29_trace_pc(),
                          "resume-rejected", sector,
                          am29_ns_to_ticks(s, s->erase_remaining));
        return;
    }
    if (s->erase_active && !s->erase_suspended) {
        return;
    }

    if (s->phase == AM29_PHASE_WRITE_BUFFER_COUNT) {
        if (size <= 2 && value < AM29_WRITE_BUFFER_WORDS) {
            s->write_buffer_words_left = value + 1;
            s->phase = AM29_PHASE_WRITE_BUFFER_DATA;
        } else {
            s->phase = AM29_PHASE_IDLE;
            am29_write_buffer_reset(s);
        }
        return;
    }
    if (s->phase == AM29_PHASE_WRITE_BUFFER_DATA) {
        uint32_t page = offset & ~(AM29_WRITE_BUFFER_BYTES - 1);

        if (size != 2 || (offset & 1) || sector != s->write_buffer_sector ||
            page != s->write_buffer_page ||
            s->write_buffer_count >= AM29_WRITE_BUFFER_WORDS) {
            s->phase = AM29_PHASE_IDLE;
            am29_write_buffer_reset(s);
            return;
        }
        s->write_buffer[s->write_buffer_count++] = (AM29WriteBufferEntry) {
            .offset = offset,
            .value = value,
        };
        if (--s->write_buffer_words_left == 0) {
            s->phase = AM29_PHASE_WRITE_BUFFER_CONFIRM;
        }
        return;
    }
    if (s->phase == AM29_PHASE_WRITE_BUFFER_CONFIRM) {
        if (command == 0x29 && sector == s->write_buffer_sector) {
            am29_write_buffer_commit(s);
        }
        s->phase = AM29_PHASE_IDLE;
        am29_write_buffer_reset(s);
        return;
    }
    if (s->phase == AM29_PHASE_PROGRAM) {
        if (s->mode == AM29_MODE_SECSI) {
            uint32_t secsi_offset = offset & (AM29LV_SECTOR_SIZE - 1);

            if (!s->secsi_locked && secsi_offset < AM29LV_SECSI_SIZE) {
                s->secsi[secsi_offset] &= value;
                if (size == 2 && secsi_offset + 1 < AM29LV_SECSI_SIZE) {
                    s->secsi[secsi_offset + 1] &= value >> 8;
                }
            }
        } else if (!s->erase_active || sector != s->erase_sector) {
            am29_program(s, offset, value, size);
            s->mode = AM29_MODE_ARRAY;
        } else {
            trace_am29lv_program(am29_trace_icount(), am29_trace_pc(), offset,
                                size, value, false);
            s->mode = AM29_MODE_ARRAY;
        }
        s->phase = AM29_PHASE_IDLE;
        am29_update_views(s);
        return;
    }
    if (command == 0xf0) {
        am29_command_reset(s);
        return;
    }
    if (s->phase == AM29_PHASE_SECSI_EXIT_ZERO) {
        if (command == 0x00) {
            s->mode = AM29_MODE_ARRAY;
        }
        s->phase = AM29_PHASE_IDLE;
        am29_update_views(s);
        return;
    }
    if (s->phase == AM29_PHASE_SECSI_PROTECT_40) {
        if (command == 0x40) {
            s->secsi_locked = true;
        }
        s->phase = AM29_PHASE_IDLE;
        return;
    }
    if (s->phase == AM29_PHASE_ERASE_UNLOCK_AA) {
        s->phase = word == AM29_UNLOCK1_WORD && command == 0xaa ?
                   AM29_PHASE_ERASE_UNLOCK_55 : AM29_PHASE_IDLE;
        return;
    }
    if (s->phase == AM29_PHASE_ERASE_UNLOCK_55) {
        s->phase = word == AM29_UNLOCK2_WORD && command == 0x55 ?
                   AM29_PHASE_ERASE_CONFIRM : AM29_PHASE_IDLE;
        return;
    }
    if (s->phase == AM29_PHASE_ERASE_CONFIRM) {
        s->phase = AM29_PHASE_IDLE;
        if (command == 0x30 && sector < am29_geometry(s)->size &&
            !am29_address_is_protected(s, offset)) {
            am29_start_erase(s, sector);
        }
        return;
    }
    if (s->phase == AM29_PHASE_EXPECT_55) {
        s->phase = word == AM29_UNLOCK2_WORD && command == 0x55 ?
                   AM29_PHASE_EXPECT_COMMAND : AM29_PHASE_IDLE;
        return;
    }
    if (s->phase == AM29_PHASE_EXPECT_COMMAND) {
        s->phase = AM29_PHASE_IDLE;
        if (command == 0x25 && s->mode == AM29_MODE_ARRAY &&
            (!s->erase_active || sector != s->erase_sector)) {
            s->write_buffer_sector = sector;
            s->write_buffer_page = offset & ~(AM29_WRITE_BUFFER_BYTES - 1);
            s->phase = AM29_PHASE_WRITE_BUFFER_COUNT;
        } else if (word == AM29_UNLOCK1_WORD) {
            switch (command) {
            case 0x90:
                if (s->mode == AM29_MODE_SECSI) {
                    s->phase = AM29_PHASE_SECSI_EXIT_ZERO;
                } else {
                    s->mode = AM29_MODE_AUTOSELECT;
                }
                break;
            case 0x88:
                if (!s->erase_active) {
                    s->mode = AM29_MODE_SECSI;
                }
                break;
            case 0xa0:
                s->phase = AM29_PHASE_PROGRAM;
                break;
            case 0x80:
                if (s->mode == AM29_MODE_ARRAY && !s->erase_active) {
                    s->phase = AM29_PHASE_ERASE_UNLOCK_AA;
                }
                break;
            default:
                break;
            }
        }
        am29_update_views(s);
        return;
    }
    if (s->mode == AM29_MODE_SECSI && command == 0x60) {
        s->phase = AM29_PHASE_SECSI_PROTECT_40;
        return;
    }
    if (word == AM29_UNLOCK1_WORD && command == 0xaa) {
        s->phase = AM29_PHASE_EXPECT_55;
    }
}

static void am29_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    AM29View *view = opaque;
    AM29State *s = view->flash;

    if (view->command_visible) {
        am29_write_native(s, offset, value, size);
    }
    trace_am29lv_command(am29_trace_icount(), am29_trace_pc(),
                         view->command_visible ? "native" : "low", offset,
                         size, value, am29_mode_name(s->mode),
                         am29_phase_name(s->phase));
}

static const MemoryRegionOps am29_ops = {
    .read = am29_read,
    .write = am29_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .impl.unaligned = true,
};

static void am29_reset_hold(Object *obj, ResetType type)
{
    AM29State *s = AM29LV_COMMON(obj);

    timer_del(s->erase_timer);
    s->erase_active = false;
    s->erase_suspended = false;
    s->erase_timeout = false;
    s->erase_deadline = 0;
    s->erase_remaining = 0;
    s->erase_toggle = false;
    am29_command_reset(s);
    trace_am29lv_reset(am29_trace_icount(), am29_trace_pc());
}

static void am29_realize(DeviceState *dev, Error **errp)
{
    AM29State *s = AM29LV_COMMON(dev);
    const AM29Geometry *geometry = am29_geometry(s);
    g_autofree char *native_name = NULL;
    g_autofree char *low_name = NULL;
    int64_t length;
    uint64_t perm;
    int ret;

    if (!clock_has_source(s->sysclk)) {
        error_setg(errp, "sysclk must be connected");
        return;
    }
    if (!am29_parse_secsi(s->factory_secsi, s->secsi)) {
        error_setg(errp,
                   "factory-secsi must be exactly 512 hexadecimal digits");
        return;
    }
    if ((uint64_t)s->protected_start + s->protected_size > geometry->size) {
        error_setg(errp, "protected range exceeds flash geometry");
        return;
    }
    native_name = g_strdup_printf("%s.native", geometry->region_prefix);
    if (!x55_nor_init(&s->nor, &s->native, OBJECT(s), &am29_ops,
                                       &s->native_view, native_name,
                                       geometry->size, AM29LV_SECTOR_SIZE,
                                       am29_array_byte, errp)) {
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
    memory_region_init_io(&s->low, OBJECT(s), &am29_ops, &s->low_view,
                          low_name, geometry->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->low);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->native);
    device_cold_reset(dev);
}

static void am29_unrealize(DeviceState *dev)
{
    AM29State *s = AM29LV_COMMON(dev);

    timer_del(s->erase_timer);
    s->storage = NULL;
}

static void am29_init(Object *obj)
{
    AM29State *s = AM29LV_COMMON(obj);
    const AM29Geometry *geometry = am29_geometry(s);
    DeviceState *dev = DEVICE(obj);

    s->low_view.flash = s;
    s->low_view.command_visible = false;
    s->native_view.flash = s;
    s->native_view.command_visible = true;
    s->protected_start = geometry->protected_start;
    s->protected_size = geometry->protected_size;
    s->secsi_locked = true;
    s->sysclk = qdev_init_clock_in(dev, "sysclk", NULL, NULL, 0);
    s->erase_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, am29_erase_complete, s);
}

static void am29_finalize(Object *obj)
{
    AM29State *s = AM29LV_COMMON(obj);

    timer_free(s->erase_timer);
}

static const Property am29_properties[] = {
    DEFINE_PROP_DRIVE("drive", AM29State, blk),
    DEFINE_PROP_STRING("factory-secsi", AM29State, factory_secsi),
    DEFINE_PROP_UINT32("protected-start", AM29State, protected_start, 0),
    DEFINE_PROP_UINT32("protected-size", AM29State, protected_size, 0),
    DEFINE_PROP_BOOL("secsi-factory-locked", AM29State, secsi_locked, true),
    DEFINE_PROP_BOOL("instruction-timing", AM29State, instruction_timing,
                     false),
};

static X55NORTraceState *am29_nor_trace(Object *obj)
{
    return &AM29LV_COMMON(obj)->nor_trace;
}

static bool am29_array_byte(Object *obj, MemoryRegion *view, uint32_t offset)
{
    AM29State *s = AM29LV_COMMON(obj);

    if (offset >= am29_geometry(s)->size) {
        return false;
    }
    if (view == &s->low) {
        return true;
    }
    if (s->erase_active && !(offset & 1) &&
        (!s->erase_suspended ||
         (offset & ~(AM29LV_SECTOR_SIZE - 1)) == s->erase_sector)) {
        return false;
    }
    return s->mode == AM29_MODE_ARRAY;
}

static const VMStateDescription vmstate_am29_buffer = {
    .name = "am29/buffer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(offset, AM29WriteBufferEntry),
        VMSTATE_UINT16(value, AM29WriteBufferEntry),
        VMSTATE_END_OF_LIST()
    },
};

static int am29_post_load(void *opaque, int version_id)
{
    AM29State *s = opaque;

    if (s->mode > AM29_MODE_SECSI || s->phase > AM29_PHASE_ERASE_CONFIRM ||
        s->write_buffer_count > AM29_WRITE_BUFFER_WORDS ||
        s->write_buffer_words_left > AM29_WRITE_BUFFER_WORDS ||
        s->erase_sector >= am29_geometry(s)->size) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < s->write_buffer_count; i++) {
        if (s->write_buffer[i].offset >= am29_geometry(s)->size - 1) {
            return -EINVAL;
        }
    }
    am29_update_views(s);
    return 0;
}

static const VMStateDescription vmstate_am29_common = {
    .name = "am29",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = am29_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(write_buffer, AM29State, AM29_WRITE_BUFFER_WORDS,
                             0, vmstate_am29_buffer, AM29WriteBufferEntry),
        VMSTATE_BUFFER(secsi, AM29State),
        VMSTATE_UINT32(mode, AM29State),
        VMSTATE_UINT32(phase, AM29State),
        VMSTATE_UINT32(write_buffer_count, AM29State),
        VMSTATE_UINT32(write_buffer_sector, AM29State),
        VMSTATE_UINT32(write_buffer_page, AM29State),
        VMSTATE_UINT32(erase_sector, AM29State),
        VMSTATE_UINT32_EQUAL(protected_start, AM29State),
        VMSTATE_UINT32_EQUAL(protected_size, AM29State),
        VMSTATE_BOOL(secsi_locked, AM29State),
        VMSTATE_BOOL(erase_active, AM29State),
        VMSTATE_BOOL(erase_suspended, AM29State),
        VMSTATE_BOOL(erase_timeout, AM29State),
        VMSTATE_BOOL(erase_toggle, AM29State),
        VMSTATE_UINT8(write_buffer_words_left, AM29State),
        VMSTATE_TIMER_PTR(erase_timer, AM29State),
        VMSTATE_INT64(erase_deadline, AM29State),
        VMSTATE_INT64(erase_remaining, AM29State),
        VMSTATE_END_OF_LIST()
    },
};

static MemoryRegion *am29_native_view(Object *obj)
{
    return &AM29LV_COMMON(obj)->native;
}

static void am29_common_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    X55NORTraceClass *nc = X55_NOR_TRACE_CLASS(klass);

    nc->state = am29_nor_trace;
    nc->array_byte = am29_array_byte;
    X55_NOR_VIEW_CLASS(klass)->native_view = am29_native_view;
    dc->realize = am29_realize;
    dc->unrealize = am29_unrealize;
    device_class_set_props(dc, am29_properties);
    rc->phases.hold = am29_reset_hold;
}

static void am29_concrete_class_init(ObjectClass *klass, const void *data)
{
    AM29Class *ac = AM29LV_COMMON_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    ac->geometry = data;
    dc->vmsd = &vmstate_am29_common;
    dc->desc = ac->geometry->description;
}

static const TypeInfo am29_common_type_info = {
    .name = TYPE_AM29LV_COMMON,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AM29State),
    .instance_align = __alignof__(AM29State),
    .class_size = sizeof(AM29Class),
    .instance_init = am29_init,
    .instance_finalize = am29_finalize,
    .class_init = am29_common_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_X55_NOR_TRACE }, { TYPE_X55_NOR_VIEW }, { }
    },
    .abstract = true,
};

static const TypeInfo am29lv640mh_type_info = {
    .name = TYPE_AM29LV640MH,
    .parent = TYPE_AM29LV_COMMON,
    .class_init = am29_concrete_class_init,
    .class_data = &am29lv640mh_geometry,
};

static const TypeInfo am29lv128mh_type_info = {
    .name = TYPE_AM29LV128MH,
    .parent = TYPE_AM29LV_COMMON,
    .class_init = am29_concrete_class_init,
    .class_data = &am29lv128mh_geometry,
};

static void am29_register_types(void)
{
    type_register_static(&am29_common_type_info);
    type_register_static(&am29lv640mh_type_info);
    type_register_static(&am29lv128mh_type_info);
}

type_init(am29_register_types)
