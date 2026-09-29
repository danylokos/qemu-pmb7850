/*
 * Infineon C166S CPU object and identity MMU.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/qemu-print.h"
#include "qemu/main-loop.h"
#include "accel/tcg/probe.h"
#include "exec/breakpoint.h"
#include "cpu.h"
#include "exec/cputlb.h"
#include "accel/tcg/cpu-ldst.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "hw/core/irq.h"
#include "accel/tcg/cpu-ops.h"
#include "tcg/debug-assert.h"
#include "trace/trace-target_c166.h"

static void c166_cpu_set_pc(CPUState *cs, vaddr value)
{
    c166_env_set_pc(cpu_env(cs), value);
}

static vaddr c166_cpu_get_pc(CPUState *cs)
{
    return c166_env_get_pc(cpu_env(cs));
}

static TCGTBCPUState c166_get_tb_cpu_state(CPUState *cs)
{
    C166CPUState *env = cpu_env(cs);
    uint32_t flags = (env->ext_kind & 3) |
                     ((env->extr & 1) << 2) |
                     ((env->ext_count & 7) << 3) |
                     (QTAILQ_EMPTY(&cs->watchpoints) ? 0 : C166_TB_WATCH);

    return (TCGTBCPUState) { .pc = c166_env_get_pc(env), .flags = flags };
}

static void c166_cpu_synchronize_from_tb(CPUState *cs,
                                         const TranslationBlock *tb)
{
    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    c166_env_set_pc(cpu_env(cs), tb->pc);
}

static void c166_restore_state_to_opc(CPUState *cs,
                                      const TranslationBlock *tb,
                                      const uint64_t *data)
{
    C166CPUState *env = cpu_env(cs);

    trace_c166_restore(data[0], env->guest_icount, env->insn_icount);
    c166_env_set_pc(env, data[0]);
    env->guest_icount = env->insn_icount;
}

static bool c166_cpu_has_work(CPUState *cs)
{
    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD);
}

void c166_cpu_set_dpram(CPUState *cs, void *dpram)
{
    cpu_env(cs)->dpram = dpram;
}

void c166_cpu_set_syscon_write_hook(CPUState *cs,
                                    void (*write)(void *opaque),
                                    void *opaque)
{
    C166CPUState *env = cpu_env(cs);

    env->syscon_write = write;
    env->syscon_opaque = opaque;
}

static int c166_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return 0;
}

void c166_cpu_set_data_read_hook(CPUState *cs,
    void (*read)(void *opaque, uint32_t addr, uint32_t value,
                 unsigned size, uint32_t pc), void *opaque)
{
    C166CPUState *env = cpu_env(cs);

    env->data_read = read;
    env->data_read_opaque = opaque;
}

static bool c166_cpu_tlb_fill(CPUState *cs, vaddr addr, int size,
                              MMUAccessType access_type, int mmu_idx,
                              bool probe, uintptr_t retaddr)
{
    hwaddr page = addr & TARGET_PAGE_MASK & C166_ADDR_MASK;

    tlb_set_page(cs, page, page, PAGE_READ | PAGE_WRITE | PAGE_EXEC,
                 mmu_idx, TARGET_PAGE_SIZE);
    return true;
}

hwaddr c166_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr)
{
    return addr & C166_ADDR_MASK;
}

static bool c166_csfr_index(hwaddr addr, unsigned *reg)
{
    if ((addr & 1) || addr < 0xfe00 || addr > 0xfffe) {
        return false;
    }
    *reg = (addr - 0xfe00) >> 1;
    return true;
}

bool c166_cpu_csfr_read(CPUState *cs, hwaddr addr, uint16_t *value)
{
    C166CPUState *env = cpu_env(cs);
    unsigned reg;

    if (!c166_csfr_index(addr, &reg)) {
        return false;
    }
    switch (reg) {
    case 0x00 ... 0x03:
        *value = env->dpp[reg];
        break;
    case 0x04:
        *value = env->csp;
        break;
    case 0x06:
        *value = env->mdh;
        break;
    case 0x07:
        *value = env->mdl;
        break;
    case 0x08:
        *value = env->cp;
        break;
    case 0x09:
        *value = env->sp;
        break;
    case 0x0a:
        *value = env->stkov;
        break;
    case 0x0b:
        *value = env->stkun;
        break;
    case 0xd6:
        *value = env->tfr;
        break;
    case 0x87:
        *value = env->mdc;
        break;
    case 0x88:
        *value = env->psw;
        break;
    case 0x89:
        *value = env->syscon;
        break;
    case 0x8e:
        *value = 0;
        break;
    case 0x8f:
        *value = 0xffff;
        break;
    default:
        return false;
    }
    return true;
}

bool c166_cpu_csfr_write(CPUState *cs, hwaddr addr, uint16_t value)
{
    C166CPUState *env = cpu_env(cs);
    unsigned reg;

    if (!c166_csfr_index(addr, &reg)) {
        return false;
    }
    switch (reg) {
    case 0x00 ... 0x03:
        env->dpp[reg] = value & 0x3ff;
        break;
    case 0x04: /* CSP is read-only through the CSFR window. */
        break;
    case 0x06:
        env->mdh = value;
        break;
    case 0x07:
        env->mdl = value;
        break;
    case 0x08:
        env->cp = value & 0xfffe;
        break;
    case 0x09:
        env->sp = c166_stack_pointer(value);
        env->sp_write_serial++;
        break;
    case 0x0a:
        env->stkov = (value & 0x0ffe) | 0xf000;
        break;
    case 0x0b:
        env->stkun = (value & 0x0ffe) | 0xf000;
        break;
    case 0xd6:
        env->tfr = value & 0xe08f;
        break;
    case 0x87:
        env->mdc = value;
        break;
    case 0x88:
        env->psw = value;
        /* Memory-addressed PSW writes must also break cached TB chaining. */
        if (cs->interrupt_request & CPU_INTERRUPT_HARD) {
            cpu_interrupt(cs, CPU_INTERRUPT_HARD);
        }
        break;
    case 0x89:
        if (!env->einit_done) {
            env->syscon = value;
            if (env->syscon_write) {
                env->syscon_write(env->syscon_opaque);
            }
        }
        break;
    case 0x8e:
    case 0x8f:
        break;
    default:
        return false;
    }
    return true;
}

static void c166_cpu_reset_hold(Object *obj, ResetType type)
{
    C166CPU *cpu = C166_CPU(obj);
    C166CPUClass *ccc = C166_CPU_GET_CLASS(obj);
    C166CPUState *env = &cpu->env;

    if (ccc->parent_phases.hold) {
        ccc->parent_phases.hold(obj, type);
    }

    memset(env, 0, offsetof(C166CPUState, end_reset_fields));
    env->dpp[0] = 0;
    env->dpp[1] = 1;
    env->dpp[2] = 2;
    env->dpp[3] = 3;
    env->cp = 0xfc00;
    env->sp = 0xfc00;
    env->stkov = 0xfa00;
    env->stkun = 0xfc00;
    env->irq_trace_checked = false;
    env->irq_watch_pending = false;
}

static ObjectClass *c166_cpu_class_by_name(const char *cpu_model)
{
    ObjectClass *oc = object_class_by_name(cpu_model);
    g_autofree char *typename = NULL;

    if (oc && object_class_dynamic_cast(oc, TYPE_C166_CPU)) {
        return oc;
    }
    typename = g_strdup_printf("%s%s", cpu_model, C166_CPU_TYPE_SUFFIX);
    return object_class_by_name(typename);
}

static void c166_cpu_realize(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    C166CPUClass *ccc = C166_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu_exec_realizefn(cs, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    qemu_init_vcpu(cs);
    cpu_reset(cs);
    ccc->parent_realize(dev, errp);
}

static void c166_cpu_set_irq(void *opaque, int n, int level)
{
    C166CPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    if (level) {
        unsigned vector = level & 0x7f;
        unsigned ilvl = (level >> 8) & 0xf;
        bool pec = !!(level & C166_IRQ_PEC);

        if (!cpu_test_interrupt(cs, CPU_INTERRUPT_HARD) ||
            cpu->env.irq_vector != vector || cpu->env.irq_ilvl != ilvl ||
            cpu->env.irq_pec != pec) {
            cpu->env.irq_trace_checked = false;
        }
        cpu->env.irq_vector = vector;
        cpu->env.irq_ilvl = ilvl;
        cpu->env.irq_pec = pec;
        trace_c166_irq_request(cpu->env.irq_vector, cpu->env.irq_ilvl, true);
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu->env.irq_trace_checked = false;
        trace_c166_irq_request(cpu->env.irq_vector, cpu->env.irq_ilvl, false);
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

/*
 * Interrupt entry is a CPU transition outside translated instructions. Probe
 * its entire frame first. A GDB watch hit enters debug_excp_handler before any
 * store; that handler completes the transition before exposing the stop.
 * This avoids replaying an unrelated guest instruction under CF_NOIRQ.
 */
static void c166_irq_watch_flags(CPUState *cs, bool prepare)
{
    CPUWatchpoint *wp;

    QTAILQ_FOREACH(wp, &cs->watchpoints, entry) {
        if (wp->flags & BP_GDB) {
            if (prepare) {
                wp->flags |= BP_STOP_BEFORE_ACCESS;
            } else {
                wp->flags &= ~BP_STOP_BEFORE_ACCESS;
            }
        }
    }
}

static void c166_irq_push(C166CPUState *env, uint16_t value)
{
    env->sp = c166_stack_pointer(env->sp - 2);
    cpu_stw_le_data_ra(env, c166_stack_address(env->sp, env->syscon), value, 0);
}

static bool c166_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    C166CPUState *env = cpu_env(cs);
    uint16_t saved_psw = env->psw;
    uint32_t source_pc = c166_env_get_pc(env);
    bool overflow = false;

    if (!env->irq_watch_pending && !(interrupt_request & CPU_INTERRUPT_HARD)) {
        return false;
    }
    if (!env->irq_trace_checked) {
        trace_c166_irq_check(env->guest_icount, c166_env_get_pc(env),
                             env->irq_vector, env->irq_ilvl, env->irq_pec,
                             env->psw, env->ext_kind, env->ext_value,
                             env->ext_count, env->extr);
        env->irq_trace_checked = true;
    }
    env->idle = 0;
    if (!env->irq_watch_pending &&
        (env->ext_kind != C166_EXT_NONE || env->extr || env->ext_count)) {
        return false;
    }
    if (!env->irq_watch_pending &&
        (!(saved_psw & C166_PSW_IEN) ||
         env->irq_ilvl <= ((saved_psw & C166_PSW_ILVL_MASK) >> 12))) {
        return false;
    }

    /* A PEC request consumes one transfer without entering an ISR. */
    if (!env->irq_watch_pending && env->irq_pec) {
        uint32_t payload = C166_IRQ_ASSERT | C166_IRQ_PEC |
                           (env->irq_ilvl << 8) | env->irq_vector;

        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
        qemu_set_irq(env->irq_ack, payload);
        qemu_set_irq(env->irq_ack, 0);
        return true;
    }

    /*
     * Checked accesses can enter the watchpoint/I/O slow path, whose helpers
     * acquire the BQL themselves. Restore the caller's lock after the frame.
     */
    if (!env->irq_watch_pending) {
        /*
         * Device arbitration can change while checked accesses drop the BQL.
         * Complete and acknowledge the source selected at entry, including
         * when a watchpoint resumes this transition in the debug handler.
         */
        env->irq_entry_vector = env->irq_vector;
        env->irq_entry_ilvl = env->irq_ilvl;
    }
    env->irq_watch_pending = true;
    bql_unlock();
    c166_irq_watch_flags(cs, true);
    unsigned words = (env->syscon & 0x0800) ? 2 : 3;
    for (unsigned i = 1; i <= words; i++) {
        overflow |= c166_stack_pointer(env->sp - 2 * i) < env->stkov;
    }
    /* Class-A service cannot be preempted by another Class-A request. */
    bool deliver_overflow = overflow && !env->class_a_depth;
    /* Preflight both complete frames before any store or acknowledgement. */
    for (unsigned i = 1; i <= words * (deliver_overflow ? 2 : 1); i++) {
        c166_probe_access(env, c166_stack_address(env->sp - 2 * i,
                                                 env->syscon), 2, true, 0);
    }
    c166_irq_watch_flags(cs, false);
    env->irq_watch_pending = false;

    c166_irq_push(env, saved_psw);
    if (!(env->syscon & 0x0800)) {
        c166_irq_push(env, env->csp);
    }
    c166_irq_push(env, env->ip);
    env->trap_depth++;
    uint16_t entry_psw = (saved_psw & ~C166_PSW_ILVL_MASK) |
                        (env->irq_entry_ilvl << 12);
    uint16_t entry_ip = env->irq_entry_vector * 4;
    if (deliver_overflow) {
        /* V1 pp.66,99: reserve space for both the ISR and stack-trap state. */
        c166_irq_push(env, entry_psw);
        if (!(env->syscon & 0x0800)) {
            c166_irq_push(env, 0);
        }
        c166_irq_push(env, entry_ip);
        c166_trap_enter(env, 0x4000);
    }
    bql_lock();

    env->psw = entry_psw | (deliver_overflow ? C166_PSW_ILVL_MASK : 0);
    env->csp = 0;
    env->ip = deliver_overflow ? 0x10 : entry_ip;
    trace_c166_irq_accept(env->guest_icount, env->irq_entry_vector,
                          env->irq_entry_ilvl, entry_ip);
    if (overflow) {
        env->tfr |= 0x4000;
        env->trap_delay[0] = 0;
    }
    if (deliver_overflow) {
        trace_c166_stack_trap(env->guest_icount, source_pc, entry_ip,
                             env->sp, env->psw, env->tfr, 0x4000);
    }
    /* Clear first: acknowledgement may synchronously select another source. */
    cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    qemu_set_irq(env->irq_ack,
                 C166_IRQ_ASSERT | (env->irq_entry_ilvl << 8) |
                 env->irq_entry_vector);
    qemu_set_irq(env->irq_ack, 0);
    return true;
}

static void c166_debug_excp_handler(CPUState *cs)
{
    C166CPUState *env = cpu_env(cs);

    if (env->irq_watch_pending) {
        bool locked = bql_locked();
        if (!locked) {
            bql_lock();
        }
        c166_irq_watch_flags(cs, false);
        c166_cpu_exec_interrupt(cs, CPU_INTERRUPT_HARD);
        /* The stop being returned already reports this completed IRQ frame. */
        cpu_reset_interrupt(cs, CPU_INTERRUPT_DEBUG);
        if (!locked) {
            bql_unlock();
        }
    }
    if (cs->watchpoint_hit) {
        trace_c166_watch_stop(c166_env_get_pc(env), env->guest_icount,
                              cs->watchpoint_hit->hitaddr,
                              cs->watchpoint_hit->flags);
    }
}

static void c166_cpu_do_interrupt(CPUState *cs)
{
    g_assert_not_reached();
}

static void c166_cpu_disas_set_info(const CPUState *cs, disassemble_info *info)
{
    info->endian = BFD_ENDIAN_LITTLE;
    info->mach = bfd_arch_unknown;
    info->print_insn = c166_print_insn;
}

static void c166_cpu_init(Object *obj)
{
    C166CPU *cpu = C166_CPU(obj);

    qdev_init_gpio_in(DEVICE(cpu), c166_cpu_set_irq, 1);
    qdev_init_gpio_out_named(DEVICE(cpu), &cpu->env.irq_ack, "irq-ack", 1);
    qdev_init_gpio_out_named(DEVICE(cpu), &cpu->env.einit, "einit", 1);
    qdev_init_gpio_out_named(DEVICE(cpu), &cpu->env.srst, "srst", 1);
}

#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps c166_sysemu_ops = {
    .has_work = c166_cpu_has_work,
    .get_phys_addr_debug = c166_cpu_get_phys_addr_debug,
};

static const TCGCPUOps c166_tcg_ops = {
    .guest_default_memory_order = TCG_MO_ALL,
    .mttcg_supported = false,
    .initialize = c166_translate_init,
    .translate_code = c166_translate_code,
    .get_tb_cpu_state = c166_get_tb_cpu_state,
    .synchronize_from_tb = c166_cpu_synchronize_from_tb,
    .restore_state_to_opc = c166_restore_state_to_opc,
    .debug_excp_handler = c166_debug_excp_handler,
    .mmu_index = c166_cpu_mmu_index,
    .tlb_fill = c166_cpu_tlb_fill,
    .pointer_wrap = cpu_pointer_wrap_uint32,
    .cpu_exec_interrupt = c166_cpu_exec_interrupt,
    .cpu_exec_halt = c166_cpu_has_work,
    .cpu_exec_reset = cpu_reset,
    .do_interrupt = c166_cpu_do_interrupt,
};

static const char *c166_gdb_arch_name(CPUState *cs)
{
    return "c166";
}

/* Host hooks and DPRAM pointers are recreated by realization. */
static const VMStateDescription vmstate_c166 = {
    .name = "c166/cpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parent_obj, C166CPU, 0, vmstate_cpu_common, CPUState),
        VMSTATE_UINT32(env.ip, C166CPU),
        VMSTATE_UINT32(env.csp, C166CPU),
        VMSTATE_UINT32(env.cp, C166CPU),
        VMSTATE_UINT32(env.sp, C166CPU),
        VMSTATE_UINT32(env.stkov, C166CPU),
        VMSTATE_UINT32(env.stkun, C166CPU),
        VMSTATE_UINT32(env.psw, C166CPU),
        VMSTATE_UINT32(env.mdh, C166CPU),
        VMSTATE_UINT32(env.mdl, C166CPU),
        VMSTATE_UINT32(env.mdc, C166CPU),
        VMSTATE_UINT32(env.tfr, C166CPU),
        VMSTATE_UINT32(env.syscon, C166CPU),
        VMSTATE_UINT32(env.einit_done, C166CPU),
        VMSTATE_UINT32(env.ext_kind, C166CPU),
        VMSTATE_UINT32(env.ext_value, C166CPU),
        VMSTATE_UINT32(env.ext_count, C166CPU),
        VMSTATE_UINT32(env.extr, C166CPU),
        VMSTATE_UINT32(env.idle, C166CPU),
        VMSTATE_UINT32(env.bitop_value, C166CPU),
        VMSTATE_UINT32(env.bitop_valid, C166CPU),
        VMSTATE_UINT32(env.stack_bound_request, C166CPU),
        VMSTATE_UINT32(env.sp_write_serial, C166CPU),
        VMSTATE_UINT32(env.trap_depth, C166CPU),
        VMSTATE_UINT32(env.class_a_depth, C166CPU),
        VMSTATE_UINT32(env.class_b_depth, C166CPU),
        VMSTATE_UINT32(env.trap_retire_pending, C166CPU),
        VMSTATE_UINT32(env.trap_next_pc, C166CPU),
        VMSTATE_UINT32(env.trap_source_pc, C166CPU),
        VMSTATE_UINT32(env.trap_fault_pc, C166CPU),
        VMSTATE_UINT32(env.trap_fault_valid, C166CPU),
        VMSTATE_UINT32(env.irq_vector, C166CPU),
        VMSTATE_UINT32(env.irq_ilvl, C166CPU),
        VMSTATE_UINT32(env.irq_pec, C166CPU),
        VMSTATE_UINT32(env.irq_entry_vector, C166CPU),
        VMSTATE_UINT32(env.irq_entry_ilvl, C166CPU),
        VMSTATE_UINT32_ARRAY(env.dpp, C166CPU, 4),
        VMSTATE_UINT32_ARRAY(env.trap_delay, C166CPU, 2),
        VMSTATE_UINT64(env.guest_icount, C166CPU),
        VMSTATE_UINT64(env.insn_icount, C166CPU),
        VMSTATE_BOOL(env.irq_trace_checked, C166CPU),
        VMSTATE_BOOL(env.irq_watch_pending, C166CPU),
        VMSTATE_END_OF_LIST()
    },
};

static void c166_cpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    CPUClass *cc = CPU_CLASS(klass);
    C166CPUClass *ccc = C166_CPU_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_parent_realize(dc, c166_cpu_realize,
                                    &ccc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, c166_cpu_reset_hold, NULL,
                                       &ccc->parent_phases);
    dc->vmsd = &vmstate_c166;
    cc->class_by_name = c166_cpu_class_by_name;
    cc->dump_state = c166_cpu_dump_state;
    cc->set_pc = c166_cpu_set_pc;
    cc->get_pc = c166_cpu_get_pc;
    cc->sysemu_ops = &c166_sysemu_ops;
    cc->gdb_read_register = c166_cpu_gdb_read_register;
    cc->gdb_write_register = c166_cpu_gdb_write_register;
    cc->gdb_core_xml_file = "c166-core.xml";
    cc->gdb_arch_name = c166_gdb_arch_name;
    cc->disas_set_info = c166_cpu_disas_set_info;
    cc->tcg_ops = &c166_tcg_ops;
}

static const TypeInfo c166_cpu_types[] = {
    {
        .name = TYPE_C166_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(C166CPU),
        .instance_align = __alignof(C166CPU),
        .instance_init = c166_cpu_init,
        .abstract = true,
        .class_size = sizeof(C166CPUClass),
        .class_init = c166_cpu_class_init,
    }, {
        .name = TYPE_C166S_V1_CPU,
        .parent = TYPE_C166_CPU,
    },
};

DEFINE_TYPES(c166_cpu_types)
