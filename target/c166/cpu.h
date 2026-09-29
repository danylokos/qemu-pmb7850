/*
 * Infineon C166S architectural state.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_C166_CPU_H
#define QEMU_C166_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"

#ifdef CONFIG_USER_ONLY
#error "C166 does not support user-mode emulation"
#endif

#define CPU_RESOLVING_TYPE TYPE_C166_CPU
#define C166_TB_WATCH (1u << 6)
#define C166_ADDR_MASK 0x00ffffffu
#define C166_IRQ_ASSERT 0x8000u
#define C166_IRQ_PEC    0x4000u

enum {
    C166_PSW_N = 1u << 0,
    C166_PSW_C = 1u << 1,
    C166_PSW_V = 1u << 2,
    C166_PSW_Z = 1u << 3,
    C166_PSW_E = 1u << 4,
    C166_PSW_MULIP = 1u << 5,
    C166_PSW_USR0 = 1u << 6,
    C166_PSW_HLDEN = 1u << 10,
    C166_PSW_IEN = 1u << 11,
    C166_PSW_ILVL_MASK = 0xf000,
};

typedef enum C166ExtKind {
    C166_EXT_NONE,
    C166_EXT_SEGMENT,
    C166_EXT_PAGE,
} C166ExtKind;

typedef struct CPUArchState {
    uint32_t ip;
    uint32_t csp;

    uint32_t dpp[4];
    uint32_t cp;
    uint32_t sp;
    uint32_t stkov;
    uint32_t stkun;
    uint32_t psw;
    uint32_t mdh;
    uint32_t mdl;
    uint32_t mdc;
    uint32_t tfr;
    uint32_t syscon;
    uint32_t einit_done;

    uint32_t ext_kind;
    uint32_t ext_value;
    uint32_t ext_count;
    uint32_t extr;
    uint32_t idle;
    uint64_t guest_icount;
    uint64_t insn_icount;
    /* Committed conditional-bit operand retained across write-watch replay. */
    uint32_t bitop_value;
    uint32_t bitop_valid;
    /* Each implicit transfer update latches before destination writes. */
    uint32_t stack_bound_request;
    /* Arithmetic requests mature at the following instruction retirement. */
    uint32_t trap_delay[2];
    uint32_t sp_write_serial;
    /* Hardware service priority is separate from the interrupt PSW. */
    uint32_t trap_depth;
    uint32_t class_a_depth;
    uint32_t class_b_depth;
    /* A completed instruction awaiting only its preflighted hardware frame. */
    uint32_t trap_retire_pending;
    uint32_t trap_next_pc;
    uint32_t trap_source_pc;
    uint32_t trap_fault_pc;
    uint32_t trap_fault_valid;

    /* Fields above this marker are initialized by architectural reset. */
    struct {} end_reset_fields;

    void *dpram;
    void (*syscon_write)(void *opaque);
    void *syscon_opaque;
    /* Optional, immutable after translation starts; excludes debug/fetch. */
    void (*data_read)(void *opaque, uint32_t addr, uint32_t value,
                      unsigned size, uint32_t pc);
    void *data_read_opaque;
    uint32_t irq_vector;
    uint32_t irq_ilvl;
    uint32_t irq_pec;
    bool irq_trace_checked;
    bool irq_watch_pending;
    uint32_t irq_entry_vector;
    uint32_t irq_entry_ilvl;
    qemu_irq irq_ack;
    qemu_irq einit;
    qemu_irq srst;
} C166CPUState;

struct ArchCPU {
    CPUState parent_obj;
    C166CPUState env;
};

struct C166CPUClass {
    CPUClass parent_class;
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

static inline uint32_t c166_env_get_pc(const C166CPUState *env)
{
    return (((env->csp & 0xff) << 16) | (env->ip & 0xffff)) & C166_ADDR_MASK;
}

static inline void c166_env_set_pc(C166CPUState *env, uint32_t pc)
{
    env->csp = (pc >> 16) & 0xff;
    env->ip = pc & 0xffff;
}

/* C166S V1 PDF pp.97,101-102: SP is virtual, even, with bits 15:12 set. */
static inline uint32_t c166_stack_pointer(uint32_t value)
{
    return (value & 0x0ffe) | 0xf000;
}

static inline uint32_t c166_stack_address(uint32_t sp, uint32_t syscon)
{
    static const uint16_t masks[8] = {
        0x01ff, 0x00ff, 0x007f, 0x003f, 0x03ff, 0x0fff, 0x0fff, 0x0fff,
    };
    unsigned mode = (syscon >> 13) & 7;
    uint32_t mask = masks[mode];

    /* Reserved modes 5/6 use linear compatibility, not hardware semantics. */
    return (c166_stack_pointer(sp) & mask) | (0xfbfe & ~mask);
}

uint16_t c166_cpu_read_gpr(CPUState *cs, unsigned reg);
void c166_cpu_write_gpr(CPUState *cs, unsigned reg, uint16_t value);
void c166_cpu_set_dpram(CPUState *cs, void *dpram);
void c166_cpu_set_syscon_write_hook(CPUState *cs,
                                    void (*write)(void *opaque),
                                    void *opaque);
bool c166_cpu_csfr_read(CPUState *cs, hwaddr addr, uint16_t *value);
void c166_cpu_set_data_read_hook(CPUState *cs,
    void (*read)(void *opaque, uint32_t addr, uint32_t value,
                 unsigned size, uint32_t pc), void *opaque);
bool c166_cpu_csfr_write(CPUState *cs, hwaddr addr, uint16_t value);

hwaddr c166_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr);
void c166_cpu_dump_state(CPUState *cs, FILE *f, int flags);
int c166_print_insn(bfd_vma addr, disassemble_info *info);
int c166_cpu_gdb_read_register(CPUState *cs, GByteArray *buf, int reg);
int c166_cpu_gdb_write_register(CPUState *cs, uint8_t *buf, int reg);

void c166_probe_access(C166CPUState *env, uint32_t addr, uint32_t size,
                        bool write, uintptr_t ra);

void c166_trap_enter(C166CPUState *env, unsigned flag);

void c166_translate_init(void);
void c166_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc);

#endif
