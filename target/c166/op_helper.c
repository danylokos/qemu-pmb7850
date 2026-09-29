/*
 * C166S operations that must leave translated execution.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "accel/tcg/probe.h"
#include "accel/tcg/cpu-ops.h"
#include "exec/target_page.h"
#include "exec/watchpoint.h"
#include "exec/breakpoint.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/helper-proto.h"
#include "accel/tcg/cpu-ldst.h"
#include "hw/core/irq.h"
#include "trace/trace-target_c166.h"

uint32_t helper_c166_stack_address(uint32_t sp, uint32_t syscon)
{
    return c166_stack_address(sp, syscon);
}

void helper_c166_idle(C166CPUState *env)
{
    CPUState *cs = env_cpu(env);

    /* A pending request wakes IDLE even when PSW makes it ineligible. */
    if (cs->interrupt_request & CPU_INTERRUPT_HARD) {
        env->idle = 0;
        cs->halted = 0;
        return;
    }

    env->idle = 1;
    cs->halted = 1;
    cs->exception_index = EXCP_HLT;
    cpu_loop_exit(cs);
}

void helper_c166_einit(C166CPUState *env)
{
    env->einit_done = 1;
    qemu_set_irq(env->einit, 1);
    qemu_set_irq(env->einit, 0);
}

void helper_c166_srst(C166CPUState *env)
{
    /* SRST resets internal trap delivery/service state as well as CSFRs. */
    memset(&env->stack_bound_request, 0,
           offsetof(C166CPUState, end_reset_fields) -
           offsetof(C166CPUState, stack_bound_request));
    qemu_set_irq(env->srst, 1);
    qemu_set_irq(env->srst, 0);
}

void helper_c166_data_read(C166CPUState *env, uint32_t addr, uint32_t value,
                           uint32_t size, uint32_t pc)
{
    env->data_read(env->data_read_opaque, addr, value, size, pc);
}

void helper_c166_write_syscon(C166CPUState *env, uint32_t value)
{
    if (!env->einit_done) {
        env->syscon = value;
        if (env->syscon_write) {
            env->syscon_write(env->syscon_opaque);
        }
    }
}

void helper_c166_trace_window(C166CPUState *env, uint32_t pc,
                              uint32_t opcode_action)
{
    trace_c166_window(env->guest_icount, pc, opcode_action & 0xffff,
                      opcode_action >> 16 ? "reload" : "retire",
                      env->psw, env->ext_kind, env->ext_value,
                      env->ext_count, env->extr);
}

/*
 * Fault and watchpoint checks before any irreversible part of a multi-access
 * instruction. Split at target pages as required by probe_access().
 */
void c166_probe_access(C166CPUState *env, uint32_t addr, uint32_t size,
                        bool write, uintptr_t ra)
{

    while (size) {
        unsigned part = MIN(size,
                            TARGET_PAGE_SIZE - (addr & ~TARGET_PAGE_MASK));
        MMUAccessType access = write ? MMU_DATA_STORE : MMU_DATA_LOAD;
        void *host = probe_access(env, addr, part, access, 0, ra);
        /*
         * probe_access folds all slow MMIO/subpage flags to TLB_MMIO,
         * including watchpoint flags. The identity MMU has no attributes;
         * check these ranges explicitly before any device transaction.
         */
        if (!host) {
            cpu_check_watchpoint(env_cpu(env), addr, part,
                                 MEMTXATTRS_UNSPECIFIED,
                                 write ? BP_MEM_WRITE : BP_MEM_READ, ra);
        }
        addr += part;
        size -= part;
    }
}

void helper_c166_trace_entry(C166CPUState *env, uint32_t pc)
{
    trace_c166_entry(pc, env->guest_icount, env->sp, env->cp, env->psw);
}

void helper_c166_trace_access(C166CPUState *env, uint32_t pc, uint32_t addr,
                              uint32_t value, uint32_t operation)
{
    trace_c166_access(pc, env->guest_icount, addr, operation & 0xff,
                      operation >> 8, value);
}

void helper_c166_probe(C166CPUState *env, uint32_t addr, uint32_t size,
                       uint32_t write)
{
    c166_probe_access(env, addr, size, write, GETPC());
}

/* C166S V1 PDF pp.64-67: TFR requests and trap priority outlive PSW changes. */
void c166_trap_enter(C166CPUState *env, unsigned flag)
{
    env->trap_depth++;
    if (flag & 0xe000) {
        env->class_a_depth = env->trap_depth;
    } else {
        env->class_b_depth = env->trap_depth;
    }
}

void helper_c166_trap_return(C166CPUState *env)
{
    if (env->class_a_depth == env->trap_depth) {
        env->class_a_depth = 0;
    }
    if (env->class_b_depth == env->trap_depth) {
        env->class_b_depth = 0;
    }
    if (env->trap_depth) {
        env->trap_depth--;
    }
}

void helper_c166_stack_arithmetic(C166CPUState *env, uint32_t overflow)
{
    unsigned flag = overflow ? 0x4000 : 0x2000;
    bool crossed = overflow ? env->sp < env->stkov : env->sp > env->stkun;

    if (crossed && !(env->tfr & flag)) {
        env->tfr |= flag;
        /*
         * V1 p.66 permits one or two following retirements. Choose one as
         * deterministic policy, not a measured hardware pipeline delay.
         */
        env->trap_delay[overflow ? 0 : 1] = 2;
    }
}

static unsigned c166_trap_select(C166CPUState *env)
{
    unsigned pending = env->tfr & 0xe08f;

    if (env->class_a_depth) {
        return 0;
    }
    if (env->trap_delay[0]) {
        pending &= ~0x4000;
    }
    if (env->trap_delay[1]) {
        pending &= ~0x2000;
    }
    /* Class B breaks an active window even if Class A is selected first. */
    if ((pending & 0x008f) && !env->class_b_depth) {
        env->ext_count = 0;
        env->ext_kind = C166_EXT_NONE;
        env->extr = 0;
    }
    if (!env->ext_count && (pending & 0xe000)) {
        return pending & 0x8000 ? 0x8000 : pending & 0x4000 ? 0x4000 : 0x2000;
    }
    return !env->class_b_depth ? pending & 0x008f : 0;
}

static void c166_trap_push(C166CPUState *env, uint32_t value, uintptr_t ra)
{
    env->sp = c166_stack_pointer(env->sp - 2);
    uint32_t addr = c166_stack_address(env->sp, env->syscon);

    cpu_stw_le_data_ra(env, addr, value, ra);
    trace_c166_access(env->trap_source_pc, env->guest_icount,
                      addr, 2, true, value);
    if (env->sp < env->stkov) {
        env->tfr |= 0x4000;
        env->trap_delay[0] = 0;
    }
}

static void c166_trap_frame(C166CPUState *env, unsigned flag,
                             uint32_t next, uintptr_t ra)
{
    unsigned vector = flag & 0x8000 ? 2 : flag & 0x4000 ? 4 :
                      flag & 0x2000 ? 6 : 10;

    c166_trap_push(env, env->psw, ra);
    if (!(env->syscon & 0x0800)) {
        c166_trap_push(env, next >> 16, ra);
    }
    c166_trap_push(env, next & 0xffff, ra);
    c166_trap_enter(env, flag);
    env->psw |= C166_PSW_ILVL_MASK;
    c166_env_set_pc(env, vector * 4);
    trace_c166_trap_delivery(env->guest_icount, env->trap_source_pc,
                             next, env->sp, env->tfr, flag,
                             env->trap_depth, env->ext_count);
    if (flag & 0x6000) {
        trace_c166_stack_trap(env->guest_icount, env->trap_source_pc,
                             next, env->sp, env->psw, env->tfr, flag);
    }
}

static bool c166_trap_deliver(C166CPUState *env, uintptr_t ra)
{
    unsigned flag = c166_trap_select(env);
    unsigned words = env->syscon & 0x0800 ? 2 : 3;
    bool overflow = false;
    uint32_t next = env->trap_fault_valid ? env->trap_fault_pc :
                                          env->trap_next_pc;

    if (!flag) {
        env->trap_fault_valid = 0;
        return false;
    }
    for (unsigned i = 1; i <= words; i++) {
        overflow |= c166_stack_pointer(env->sp - 2 * i) < env->stkov;
    }
    /* Class B entry may itself produce a higher-priority stack request. */
    bool nested = !(flag & 0xe000) && overflow;
    env->trap_retire_pending = 1;
    for (unsigned i = 1; i <= words * (nested ? 2 : 1); i++) {
        c166_probe_access(env, c166_stack_address(env->sp - 2 * i,
                                                 env->syscon), 2, true, ra);
    }
    c166_trap_frame(env, flag, next, ra);
    if (nested) {
        c166_trap_frame(env, 0x4000, c166_env_get_pc(env), ra);
    }
    env->trap_fault_valid = 0;
    env->trap_retire_pending = 0;
    return true;
}

uint32_t helper_c166_trap_retire(C166CPUState *env, uint32_t source,
                                uint32_t next)
{
    env->trap_source_pc = source;
    env->trap_next_pc = next & (env->syscon & 0x0800 ? 0xffff : C166_ADDR_MASK);
    for (unsigned i = 0; i < 2; i++) {
        if (env->trap_delay[i]) {
            env->trap_delay[i]--;
        }
    }
    trace_c166_trap_pending(env->guest_icount, source, env->tfr,
                            env->trap_delay[0], env->trap_delay[1],
                            env->ext_count, env->class_a_depth,
                            env->class_b_depth);
    return c166_trap_deliver(env, GETPC());
}

void helper_c166_trap_resume(C166CPUState *env)
{
    if (!c166_trap_deliver(env, GETPC())) {
        env->trap_retire_pending = 0;
        c166_env_set_pc(env, env->trap_next_pc);
    }
}
