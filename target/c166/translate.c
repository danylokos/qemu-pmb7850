/*
 * Infineon C166S decodetree/TCG frontend.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/qemu-print.h"
#include "cpu.h"
#include "exec/log.h"
#include "exec/translator.h"
#include "exec/translation-block.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H
#include "tcg/tcg-op.h"
#include "trace/trace-target_c166.h"

typedef struct DisasContext {
    DisasContextBase base;
    C166CPUState *env;
    unsigned window_count;
    uint32_t insn_pc;
    uint16_t opcode;
    bool watched;
    bool stack_check;
    TCGLabel *trap_resume;
    bool boundary_finished;
    bool window_reload;
    bool window_finished;
    bool window_exit;
    bool irq_exit;
} DisasContext;

#define DISAS_EXIT DISAS_TARGET_0
#define C166_DPRAM_BASE 0xf200u
#define C166_DPRAM_SIZE 0x0c00u

static TCGv_i32 cpu_ip;
static TCGv_i32 cpu_csp;
static TCGv_i32 cpu_dpp[4];
static TCGv_i32 cpu_cp;
static TCGv_i32 cpu_sp;
static TCGv_i32 cpu_stkov;
static TCGv_i32 cpu_stkun;
static TCGv_i32 cpu_psw;
static TCGv_i32 cpu_mdh;
static TCGv_i32 cpu_mdl;
static TCGv_i32 cpu_mdc;
static TCGv_i32 cpu_tfr;
static TCGv_i32 cpu_syscon;
static TCGv_i32 cpu_einit_done;
static TCGv_i32 cpu_ext_kind;
static TCGv_i32 cpu_ext_value;
static TCGv_i32 cpu_ext_count;
static TCGv_i32 cpu_extr;
static TCGv_i32 cpu_idle;
static TCGv_i64 cpu_guest_icount;
static TCGv_i64 cpu_insn_icount;
static TCGv_ptr cpu_dpram;

static void c166_gen_access_trace(DisasContext *ctx, TCGv_i32 addr,
                                   TCGv_i32 value, MemOp op, bool write)
{
    if (trace_event_get_state_backends(TRACE_C166_ACCESS)) {
        gen_helper_c166_trace_access(tcg_env, tcg_constant_i32(ctx->insn_pc),
                                     addr, value,
                                     tcg_constant_i32((1u << (op & MO_SIZE)) |
                                                      (write ? 0x100 : 0)));
    }
}

static void c166_gen_data_store(DisasContext *ctx, TCGv_i32 value,
                                 TCGv_i32 addr, MemOp op)
{
    tcg_gen_qemu_st_i32(value, addr, 0, op);
    c166_gen_access_trace(ctx, addr, value, op, true);
}

static void c166_gen_data_load(DisasContext *ctx, TCGv_i32 value,
                               TCGv_i32 addr, MemOp op)
{
    tcg_gen_qemu_ld_i32(value, addr, 0, op);
    c166_gen_access_trace(ctx, addr, value, op, false);
    if (ctx->env->data_read) {
        gen_helper_c166_data_read(tcg_env, addr, value,
                                  tcg_constant_i32(1u << (op & MO_SIZE)),
                                  tcg_constant_i32(ctx->insn_pc));
    }
}

static uint16_t c166_fetch_u16(DisasContext *ctx)
{
    uint16_t value = translator_lduw_end(ctx->env, &ctx->base,
                                         ctx->base.pc_next, MO_LE);
    ctx->base.pc_next += 2;
    return value;
}

static void c166_gen_gpr_addr(TCGv_i32 addr, unsigned reg)
{
    tcg_gen_addi_i32(addr, cpu_cp, 2 * (reg & 15));
    tcg_gen_andi_i32(addr, addr, 0xffff);
}

/*
 * A probe does not read or write the device. Multi-access instructions are
 * also TB terminators, so deterministic I/O cannot request replay after an
 * earlier operand has committed.
 */
static void c166_gen_probe(TCGv_i32 addr, unsigned size, bool write)
{
    gen_helper_c166_probe(tcg_env, addr, tcg_constant_i32(size),
                          tcg_constant_i32(write));
}

static void c166_gen_probe_reg(DisasContext *ctx, unsigned reg,
                               unsigned width, bool write)
{
    TCGv_i32 addr = tcg_temp_new_i32();

    if ((reg & 0xf0) == 0xf0) {
        tcg_gen_addi_i32(addr, cpu_cp,
                        width == 8 ? reg & 15 : 2 * (reg & 15));
        tcg_gen_andi_i32(addr, addr, 0xffff);
    } else {
        /*
         * Explicit CSFR operands have the same optimized register path as
         * execution. EXTR redirects those operands to memory.
         */
        TCGLabel *done = gen_new_label();
        switch (reg) {
        case 0x00 ... 0x04:
        case 0x06 ... 0x0b:
        case 0x87 ... 0x89:
        case 0x8e ... 0x8f:
        case 0xd6:
            tcg_gen_brcondi_i32(TCG_COND_EQ, cpu_extr, 0, done);
            break;
        }
        tcg_gen_movi_i32(addr, 0xfe00 + 2 * reg);
        tcg_gen_movcond_i32(TCG_COND_NE, addr, cpu_extr,
                            tcg_constant_i32(0),
                            tcg_constant_i32(0xf000 + 2 * reg), addr);
        c166_gen_probe(addr, 2, write);
        gen_set_label(done);
        return;
    }
    c166_gen_probe(addr, width / 8, write);
}

static void c166_gen_stack_pointer(TCGv_i32 dest, TCGv_i32 sp, int delta)
{
    tcg_gen_addi_i32(dest, sp, delta);
    tcg_gen_andi_i32(dest, dest, 0x0ffe);
    tcg_gen_ori_i32(dest, dest, 0xf000);
}

static void c166_gen_probe_stack(int first, unsigned words, bool write)
{
    for (unsigned i = 0; i < words; i++) {
        TCGv_i32 addr = tcg_temp_new_i32();
        tcg_gen_addi_i32(addr, cpu_sp, first + (write ? -2 : 2) * i);
        gen_helper_c166_stack_address(addr, addr, cpu_syscon);
        c166_gen_probe(addr, 2, write);
    }
}

static TCGv_i32 c166_gen_read_gpr(DisasContext *ctx, unsigned reg)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    TCGv_i32 offset = tcg_temp_new_i32();
    TCGv_i32 value = tcg_temp_new_i32();
    TCGv_ptr ptr = tcg_temp_new_ptr();
    TCGLabel *slow = gen_new_label();
    TCGLabel *done = gen_new_label();

    c166_gen_gpr_addr(addr, reg);
    if (ctx->watched || trace_event_get_state_backends(TRACE_C166_ACCESS)) {
        tcg_gen_br(slow);
    }
    tcg_gen_brcondi_ptr(TCG_COND_EQ, cpu_dpram, 0, slow);
    tcg_gen_subi_i32(offset, addr, C166_DPRAM_BASE);
    tcg_gen_brcondi_i32(TCG_COND_GTU, offset, C166_DPRAM_SIZE - 2, slow);
    tcg_gen_ext_i32_ptr(ptr, offset);
    tcg_gen_add_ptr(ptr, cpu_dpram, ptr);
    tcg_gen_ld16u_i32(value, ptr, 0);
#if HOST_BIG_ENDIAN
    tcg_gen_bswap16_i32(value, value, TCG_BSWAP_OS);
#endif
    tcg_gen_br(done);
    gen_set_label(slow);
    c166_gen_data_load(ctx, value, addr, MO_LEUW);
    gen_set_label(done);
    return value;
}

static void c166_gen_write_gpr(DisasContext *ctx, unsigned reg, TCGv_i32 value)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    c166_gen_gpr_addr(addr, reg);
    c166_gen_data_store(ctx, value, addr, MO_LEUW);
}

static TCGv_i32 c166_gen_read_gpr_byte(DisasContext *ctx, unsigned reg)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    TCGv_i32 value = tcg_temp_new_i32();

    tcg_gen_addi_i32(addr, cpu_cp, reg & 15);
    tcg_gen_andi_i32(addr, addr, 0xffff);
    c166_gen_data_load(ctx, value, addr, MO_UB);
    return value;
}

static void c166_gen_write_gpr_byte(DisasContext *ctx, unsigned reg,
                                    TCGv_i32 value)
{
    TCGv_i32 addr = tcg_temp_new_i32();

    tcg_gen_addi_i32(addr, cpu_cp, reg & 15);
    tcg_gen_andi_i32(addr, addr, 0xffff);
    c166_gen_data_store(ctx, value, addr, MO_UB);
}

static TCGv_i32 c166_gen_read_reg(DisasContext *ctx, unsigned reg)
{
    TCGv_i32 value;
    TCGLabel *extended;
    TCGLabel *done;

    if ((reg & 0xf0) == 0xf0) {
        return c166_gen_read_gpr(ctx, reg & 15);
    }

    value = tcg_temp_new_i32();
    extended = gen_new_label();
    done = gen_new_label();
    tcg_gen_brcondi_i32(TCG_COND_NE, cpu_extr, 0, extended);
    switch (reg) {
    case 0x00:
        tcg_gen_mov_i32(value, cpu_dpp[0]);
        break;
    case 0x01:
        tcg_gen_mov_i32(value, cpu_dpp[1]);
        break;
    case 0x02:
        tcg_gen_mov_i32(value, cpu_dpp[2]);
        break;
    case 0x03:
        tcg_gen_mov_i32(value, cpu_dpp[3]);
        break;
    case 0x04:
        tcg_gen_mov_i32(value, cpu_csp);
        break;
    case 0x06:
        tcg_gen_mov_i32(value, cpu_mdh);
        break;
    case 0x07:
        tcg_gen_mov_i32(value, cpu_mdl);
        break;
    case 0x08:
        tcg_gen_mov_i32(value, cpu_cp);
        break;
    case 0x09:
        tcg_gen_mov_i32(value, cpu_sp);
        break;
    case 0x0a:
        tcg_gen_mov_i32(value, cpu_stkov);
        break;
    case 0x0b:
        tcg_gen_mov_i32(value, cpu_stkun);
        break;
    case 0xd6:
        tcg_gen_mov_i32(value, cpu_tfr);
        break;
    case 0x87:
        tcg_gen_mov_i32(value, cpu_mdc);
        break;
    case 0x88:
        tcg_gen_mov_i32(value, cpu_psw);
        break;
    case 0x89:
        tcg_gen_mov_i32(value, cpu_syscon);
        break;
    case 0x8e:
        tcg_gen_movi_i32(value, 0);
        break;
    case 0x8f:
        tcg_gen_movi_i32(value, 0xffff);
        break;
    default:
        {
            TCGv_i32 addr = tcg_temp_new_i32();

            tcg_gen_movi_i32(addr, 0xfe00 + 2 * reg);
            c166_gen_data_load(ctx, value, addr, MO_LEUW);
        }
        break;
    }
    tcg_gen_br(done);
    gen_set_label(extended);
    c166_gen_data_load(ctx, value, tcg_constant_i32(0xf000 + 2 * reg), MO_LEUW);
    gen_set_label(done);
    return value;
}

static void c166_gen_write_reg(DisasContext *ctx, unsigned reg,
                               TCGv_i32 value)
{
    TCGLabel *extended;
    TCGLabel *done;

    if ((reg & 0xf0) == 0xf0) {
        c166_gen_write_gpr(ctx, reg & 15, value);
        return;
    }

    extended = gen_new_label();
    done = gen_new_label();
    tcg_gen_brcondi_i32(TCG_COND_NE, cpu_extr, 0, extended);
    switch (reg) {
    case 0x00:
        tcg_gen_andi_i32(cpu_dpp[0], value, 0x3ff);
        break;
    case 0x01:
        tcg_gen_andi_i32(cpu_dpp[1], value, 0x3ff);
        break;
    case 0x02:
        tcg_gen_andi_i32(cpu_dpp[2], value, 0x3ff);
        break;
    case 0x03:
        tcg_gen_andi_i32(cpu_dpp[3], value, 0x3ff);
        break;
    case 0x06:
        tcg_gen_ext16u_i32(cpu_mdh, value);
        break;
    case 0x07:
        tcg_gen_ext16u_i32(cpu_mdl, value);
        break;
    case 0x08:
        tcg_gen_andi_i32(cpu_cp, value, 0xfffe);
        break;
    case 0x09:
        c166_gen_stack_pointer(cpu_sp, value, 0);
        TCGv_i32 serial = tcg_temp_new_i32();
        tcg_gen_ld_i32(serial, tcg_env,
                       offsetof(C166CPUState, sp_write_serial));
        tcg_gen_addi_i32(serial, serial, 1);
        tcg_gen_st_i32(serial, tcg_env,
                       offsetof(C166CPUState, sp_write_serial));
        break;
    case 0x0a:
        tcg_gen_andi_i32(cpu_stkov, value, 0x0ffe);
        tcg_gen_ori_i32(cpu_stkov, cpu_stkov, 0xf000);
        break;
    case 0x0b:
        tcg_gen_andi_i32(cpu_stkun, value, 0x0ffe);
        tcg_gen_ori_i32(cpu_stkun, cpu_stkun, 0xf000);
        break;
    case 0xd6:
        tcg_gen_andi_i32(cpu_tfr, value, 0xe08f);
        break;
    case 0x87:
        tcg_gen_ext16u_i32(cpu_mdc, value);
        break;
    case 0x88:
        tcg_gen_ext16u_i32(cpu_psw, value);
        break;
    case 0x89:
        gen_helper_c166_write_syscon(tcg_env, value);
        break;
    default:
        c166_gen_data_store(ctx, value,
                            tcg_constant_i32(0xfe00 + 2 * reg), MO_LEUW);
        break;
    }
    tcg_gen_br(done);
    gen_set_label(extended);
    c166_gen_data_store(ctx, value,
                        tcg_constant_i32(0xf000 + 2 * reg), MO_LEUW);
    gen_set_label(done);
    /* C166S V1 PDF pp.55,59-60: re-arbitrate after changing PSW eligibility. */
    if (reg == 0x88) {
        ctx->irq_exit = true;
    }
    if (reg == 0x88 || reg == 0x89) {
        tcg_gen_movi_i32(cpu_ip, ctx->base.pc_next & 0xffff);
        ctx->base.is_jmp = DISAS_EXIT;
    }
}

static TCGv_i32 c166_gen_read_reg_byte(DisasContext *ctx, unsigned reg)
{
    TCGv_i32 value;
    TCGv_i32 narrowed;

    if ((reg & 0xf0) == 0xf0) {
        return c166_gen_read_gpr_byte(ctx, reg & 15);
    }
    value = c166_gen_read_reg(ctx, reg);
    narrowed = tcg_temp_new_i32();
    tcg_gen_andi_i32(narrowed, value, 0xff);
    return narrowed;
}

static void c166_gen_write_reg_byte(DisasContext *ctx, unsigned reg,
                                    TCGv_i32 value)
{
    TCGv_i32 narrowed;

    if ((reg & 0xf0) == 0xf0) {
        c166_gen_write_gpr_byte(ctx, reg & 15, value);
        return;
    }
    narrowed = tcg_temp_new_i32();
    tcg_gen_andi_i32(narrowed, value, 0xff);
    c166_gen_write_reg(ctx, reg, narrowed);
}

static void c166_gen_mov_flags(TCGv_i32 value)
{
    TCGv_i32 flags = tcg_temp_new_i32();
    TCGv_i32 tmp = tcg_temp_new_i32();

    tcg_gen_andi_i32(flags, cpu_psw,
                     ~(C166_PSW_E | C166_PSW_Z | C166_PSW_N));
    tcg_gen_setcondi_i32(TCG_COND_EQ, tmp, value, 0);
    tcg_gen_shli_i32(tmp, tmp, 3);
    tcg_gen_or_i32(flags, flags, tmp);
    tcg_gen_andi_i32(tmp, value, 0x8000);
    tcg_gen_shri_i32(tmp, tmp, 15);
    tcg_gen_or_i32(flags, flags, tmp);
    tcg_gen_setcondi_i32(TCG_COND_EQ, tmp, value, 0x8000);
    tcg_gen_shli_i32(tmp, tmp, 4);
    tcg_gen_or_i32(cpu_psw, flags, tmp);
}

static void c166_gen_movb_flags(TCGv_i32 value)
{
    TCGv_i32 flags = tcg_temp_new_i32();
    TCGv_i32 tmp = tcg_temp_new_i32();

    tcg_gen_andi_i32(flags, cpu_psw,
                     ~(C166_PSW_E | C166_PSW_Z | C166_PSW_N));
    tcg_gen_setcondi_i32(TCG_COND_EQ, tmp, value, 0);
    tcg_gen_shli_i32(tmp, tmp, 3);
    tcg_gen_or_i32(flags, flags, tmp);
    tcg_gen_andi_i32(tmp, value, 0x80);
    tcg_gen_shri_i32(tmp, tmp, 7);
    tcg_gen_or_i32(flags, flags, tmp);
    tcg_gen_setcondi_i32(TCG_COND_EQ, tmp, value, 0x80);
    tcg_gen_shli_i32(tmp, tmp, 4);
    tcg_gen_or_i32(cpu_psw, flags, tmp);
}

static void c166_gen_movzx_flags(TCGv_i32 value, bool sign_extend)
{
    TCGv_i32 flags = tcg_temp_new_i32();
    TCGv_i32 tmp = tcg_temp_new_i32();

    tcg_gen_andi_i32(flags, cpu_psw,
                     ~(C166_PSW_E | C166_PSW_Z | C166_PSW_N));
    tcg_gen_setcondi_i32(TCG_COND_EQ, tmp, value, 0);
    tcg_gen_shli_i32(tmp, tmp, 3);
    tcg_gen_or_i32(flags, flags, tmp);
    if (sign_extend) {
        tcg_gen_andi_i32(tmp, value, 0x80);
        tcg_gen_shri_i32(tmp, tmp, 7);
        tcg_gen_or_i32(flags, flags, tmp);
    }
    tcg_gen_mov_i32(cpu_psw, flags);
}

static TCGv_i32 c166_gen_resolve_data(uint16_t offset)
{
    unsigned select = offset >> 14;
    TCGv_i32 addr = tcg_temp_new_i32();
    TCGv_i32 segment = tcg_temp_new_i32();
    TCGv_i32 page = tcg_temp_new_i32();

    tcg_gen_shli_i32(addr, cpu_dpp[select], 14);
    tcg_gen_ori_i32(addr, addr, offset & 0x3fff);
    tcg_gen_shli_i32(segment, cpu_ext_value, 16);
    tcg_gen_ori_i32(segment, segment, offset);
    tcg_gen_shli_i32(page, cpu_ext_value, 14);
    tcg_gen_ori_i32(page, page, offset & 0x3fff);
    tcg_gen_movcond_i32(TCG_COND_EQ, addr, cpu_ext_kind,
                        tcg_constant_i32(C166_EXT_SEGMENT), segment, addr);
    tcg_gen_movcond_i32(TCG_COND_EQ, addr, cpu_ext_kind,
                        tcg_constant_i32(C166_EXT_PAGE), page, addr);
    tcg_gen_andi_i32(addr, addr, C166_ADDR_MASK);
    return addr;
}

static TCGv_i32 c166_gen_resolve_data_dynamic(TCGv_i32 offset)
{
    TCGv_i32 select = tcg_temp_new_i32();
    TCGv_i32 page = tcg_temp_new_i32();
    TCGv_i32 addr = tcg_temp_new_i32();
    TCGv_i32 low = tcg_temp_new_i32();
    TCGv_i32 extended = tcg_temp_new_i32();

    tcg_gen_shri_i32(select, offset, 14);
    tcg_gen_andi_i32(select, select, 3);
    tcg_gen_mov_i32(page, cpu_dpp[0]);
    for (unsigned i = 1; i < 4; i++) {
        tcg_gen_movcond_i32(TCG_COND_EQ, page, select,
                            tcg_constant_i32(i), cpu_dpp[i], page);
    }
    tcg_gen_shli_i32(addr, page, 14);
    tcg_gen_andi_i32(low, offset, 0x3fff);
    tcg_gen_or_i32(addr, addr, low);
    tcg_gen_shli_i32(extended, cpu_ext_value, 16);
    tcg_gen_or_i32(extended, extended, offset);
    tcg_gen_movcond_i32(TCG_COND_EQ, addr, cpu_ext_kind,
                        tcg_constant_i32(C166_EXT_SEGMENT), extended, addr);
    tcg_gen_shli_i32(extended, cpu_ext_value, 14);
    tcg_gen_or_i32(extended, extended, low);
    tcg_gen_movcond_i32(TCG_COND_EQ, addr, cpu_ext_kind,
                        tcg_constant_i32(C166_EXT_PAGE), extended, addr);
    tcg_gen_andi_i32(addr, addr, C166_ADDR_MASK);
    return addr;
}

static void c166_gen_set_next_ip(DisasContext *ctx)
{
    tcg_gen_movi_i32(cpu_ip, ctx->base.pc_next & 0xffff);
}

static void c166_gen_trace_window(DisasContext *ctx, bool reload)
{
    if (!trace_event_get_state_backends(TRACE_C166_WINDOW)) {
        return;
    }
    gen_helper_c166_trace_window(
        tcg_env, tcg_constant_i32(ctx->insn_pc),
        tcg_constant_i32(ctx->opcode | (reload ? 1u << 16 : 0)));
}

static void c166_gen_window_finish(DisasContext *ctx)
{
    if (!ctx->window_count || ctx->window_reload || ctx->window_finished) {
        return;
    }
    if (ctx->window_count == 1) {
        tcg_gen_movi_i32(cpu_ext_count, 0);
        tcg_gen_movi_i32(cpu_ext_kind, C166_EXT_NONE);
        tcg_gen_movi_i32(cpu_extr, 0);
    } else {
        tcg_gen_subi_i32(cpu_ext_count, cpu_ext_count, 1);
    }
    c166_gen_trace_window(ctx, false);
    ctx->window_finished = true;
    ctx->window_exit = ctx->window_count == 1;
}

static TCGv_i32 c166_gen_segmented_pc(TCGv_i32 ip);

static void c166_gen_trap_boundary(DisasContext *ctx, TCGv_i32 next)
{
    TCGLabel *done = gen_new_label();
    TCGv_i32 pending = tcg_temp_new_i32();
    TCGv_i32 taken = tcg_temp_new_i32();

    c166_gen_window_finish(ctx);
    tcg_gen_andi_i32(pending, cpu_tfr, 0xe08f);
    tcg_gen_brcondi_i32(TCG_COND_EQ, pending, 0, done);
    gen_helper_c166_trap_retire(taken, tcg_env,
                                tcg_constant_i32(ctx->insn_pc), next);
    tcg_gen_brcondi_i32(TCG_COND_EQ, taken, 0, done);
    tcg_gen_lookup_and_goto_ptr();
    gen_set_label(done);
}

static void c166_gen_class_b_trap(DisasContext *ctx, unsigned flag,
                                  TCGv_i32 saved_ip);

static void c166_gen_goto_tb(DisasContext *ctx, unsigned slot, uint32_t dest)
{
    TCGv_i32 segmentation_disabled;
    unsigned segment = (dest >> 16) & 0xff;

    if (dest & 1) {
        c166_gen_class_b_trap(ctx, 0x0002,
                              tcg_constant_i32(dest & 0xffff));
        return;
    }
    if (!ctx->boundary_finished) {
        c166_gen_trap_boundary(ctx, tcg_constant_i32(dest));
    }
    segmentation_disabled = tcg_temp_new_i32();
    tcg_gen_andi_i32(segmentation_disabled, cpu_syscon, 0x0800);
    if (ctx->window_exit || ctx->irq_exit) {
        tcg_gen_movcond_i32(TCG_COND_NE, cpu_csp, segmentation_disabled,
                            tcg_constant_i32(0), tcg_constant_i32(0),
                            tcg_constant_i32(segment));
        tcg_gen_movi_i32(cpu_ip, dest & 0xffff);
        tcg_gen_exit_tb(NULL, 0);
        ctx->base.is_jmp = DISAS_NORETURN;
        return;
    }
    if (translator_use_goto_tb(&ctx->base, dest)) {
        TCGLabel *lookup = segment ? gen_new_label() : NULL;

        if (lookup) {
            tcg_gen_brcondi_i32(TCG_COND_NE, segmentation_disabled, 0,
                                lookup);
        }
        tcg_gen_goto_tb(slot);
        tcg_gen_movi_i32(cpu_csp, segment);
        tcg_gen_movi_i32(cpu_ip, dest & 0xffff);
        tcg_gen_exit_tb(ctx->base.tb, slot);
        if (lookup) {
            gen_set_label(lookup);
            tcg_gen_movi_i32(cpu_csp, 0);
            tcg_gen_movi_i32(cpu_ip, dest & 0xffff);
            tcg_gen_lookup_and_goto_ptr();
        }
    } else {
        tcg_gen_movcond_i32(TCG_COND_NE, cpu_csp, segmentation_disabled,
                            tcg_constant_i32(0), tcg_constant_i32(0),
                            tcg_constant_i32(segment));
        tcg_gen_movi_i32(cpu_ip, dest & 0xffff);
        tcg_gen_lookup_and_goto_ptr();
    }
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void c166_gen_lookup_ip(DisasContext *ctx, TCGv_i32 ip)
{
    TCGLabel *aligned = gen_new_label();
    TCGv_i32 odd = tcg_temp_new_i32();
    TCGv_i32 segmentation_disabled = tcg_temp_new_i32();

    tcg_gen_andi_i32(odd, ip, 1);
    tcg_gen_brcondi_i32(TCG_COND_EQ, odd, 0, aligned);
    bool finished = ctx->window_finished;
    c166_gen_class_b_trap(ctx, 0x0002, ip);
    ctx->window_finished = finished;
    gen_set_label(aligned);
    c166_gen_trap_boundary(ctx, c166_gen_segmented_pc(ip));
    tcg_gen_andi_i32(segmentation_disabled, cpu_syscon, 0x0800);
    tcg_gen_movcond_i32(TCG_COND_NE, cpu_csp, segmentation_disabled,
                        tcg_constant_i32(0), tcg_constant_i32(0), cpu_csp);
    tcg_gen_andi_i32(cpu_ip, ip, 0xffff);
    if (ctx->window_exit || ctx->irq_exit) {
        tcg_gen_exit_tb(NULL, 0);
    } else {
        tcg_gen_lookup_and_goto_ptr();
    }
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void c166_gen_stack_request(DisasContext *ctx, bool overflow)
{
    TCGv_i32 flag;
    TCGv_i32 request;

    if (!ctx->stack_check) {
        return;
    }
    flag = tcg_temp_new_i32();
    request = tcg_temp_new_i32();
    tcg_gen_setcond_i32(overflow ? TCG_COND_LTU : TCG_COND_GTU, flag,
                       cpu_sp, overflow ? cpu_stkov : cpu_stkun);
    tcg_gen_shli_i32(flag, flag, overflow ? 14 : 13);
    tcg_gen_ld_i32(request, tcg_env,
                   offsetof(C166CPUState, stack_bound_request));
    tcg_gen_or_i32(request, request, flag);
    tcg_gen_st_i32(request, tcg_env,
                   offsetof(C166CPUState, stack_bound_request));
}

static void c166_gen_push(DisasContext *ctx, TCGv_i32 value)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    TCGv_i32 sp = tcg_temp_new_i32();

    c166_gen_stack_pointer(sp, cpu_sp, -2);
    gen_helper_c166_stack_address(addr, sp, cpu_syscon);
    c166_gen_data_store(ctx, value, addr, MO_LEUW);
    tcg_gen_mov_i32(cpu_sp, sp);
    c166_gen_stack_request(ctx, true);
}

static TCGv_i32 c166_gen_pop(DisasContext *ctx)
{
    TCGv_i32 value = tcg_temp_new_i32();
    TCGv_i32 addr = tcg_temp_new_i32();

    gen_helper_c166_stack_address(addr, cpu_sp, cpu_syscon);
    c166_gen_data_load(ctx, value, addr, MO_LEUW);
    c166_gen_stack_pointer(cpu_sp, cpu_sp, 2);
    c166_gen_stack_request(ctx, false);
    return value;
}

static void c166_gen_pop_csp_if_segmented(DisasContext *ctx)
{
    TCGLabel *segmentation_disabled = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv_i32 sgtdis = tcg_temp_new_i32();
    TCGv_i32 csp;

    tcg_gen_andi_i32(sgtdis, cpu_syscon, 0x0800);
    tcg_gen_brcondi_i32(TCG_COND_NE, sgtdis, 0,
                        segmentation_disabled);
    csp = c166_gen_pop(ctx);
    tcg_gen_andi_i32(cpu_csp, csp, 0xff);
    tcg_gen_br(done);
    gen_set_label(segmentation_disabled);
    tcg_gen_movi_i32(cpu_csp, 0);
    gen_set_label(done);
}

static TCGv_i32 c166_gen_condition(unsigned cc)
{
    TCGv_i32 z = tcg_temp_new_i32();
    TCGv_i32 n = tcg_temp_new_i32();
    TCGv_i32 c = tcg_temp_new_i32();
    TCGv_i32 v = tcg_temp_new_i32();
    TCGv_i32 e = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();
    TCGv_i32 tmp = tcg_temp_new_i32();

    tcg_gen_shri_i32(n, cpu_psw, 0);
    tcg_gen_shri_i32(c, cpu_psw, 1);
    tcg_gen_shri_i32(v, cpu_psw, 2);
    tcg_gen_shri_i32(z, cpu_psw, 3);
    tcg_gen_shri_i32(e, cpu_psw, 4);
    tcg_gen_andi_i32(n, n, 1);
    tcg_gen_andi_i32(c, c, 1);
    tcg_gen_andi_i32(v, v, 1);
    tcg_gen_andi_i32(z, z, 1);
    tcg_gen_andi_i32(e, e, 1);

    switch (cc) {
    case 0x0:
        tcg_gen_movi_i32(result, 1);
        break;
    case 0x1:
        tcg_gen_or_i32(tmp, z, e);
        tcg_gen_xori_i32(result, tmp, 1);
        break;
    case 0x2:
        tcg_gen_mov_i32(result, z);
        break;
    case 0x3:
        tcg_gen_xori_i32(result, z, 1);
        break;
    case 0x4:
        tcg_gen_mov_i32(result, v);
        break;
    case 0x5:
        tcg_gen_xori_i32(result, v, 1);
        break;
    case 0x6:
        tcg_gen_mov_i32(result, n);
        break;
    case 0x7:
        tcg_gen_xori_i32(result, n, 1);
        break;
    case 0x8:
        tcg_gen_mov_i32(result, c);
        break;
    case 0x9:
        tcg_gen_xori_i32(result, c, 1);
        break;
    case 0xa:
        tcg_gen_xor_i32(tmp, n, v);
        tcg_gen_or_i32(tmp, tmp, z);
        tcg_gen_xori_i32(result, tmp, 1);
        break;
    case 0xb:
        tcg_gen_xor_i32(tmp, n, v);
        tcg_gen_or_i32(result, tmp, z);
        break;
    case 0xc:
        tcg_gen_xor_i32(result, n, v);
        break;
    case 0xd:
        tcg_gen_xor_i32(tmp, n, v);
        tcg_gen_xori_i32(result, tmp, 1);
        break;
    case 0xe:
        tcg_gen_or_i32(tmp, z, c);
        tcg_gen_xori_i32(result, tmp, 1);
        break;
    case 0xf:
        tcg_gen_or_i32(result, z, c);
        break;
    default:
        g_assert_not_reached();
    }
    return result;
}

static void c166_gen_conditional_goto(DisasContext *ctx, TCGv_i32 condition,
                                      uint32_t taken, uint32_t fallthrough)
{
    TCGLabel *take = gen_new_label();

    c166_gen_window_finish(ctx);
    tcg_gen_brcondi_i32(TCG_COND_NE, condition, 0, take);
    c166_gen_goto_tb(ctx, 1, fallthrough);
    gen_set_label(take);
    c166_gen_goto_tb(ctx, 0, taken);
}

enum {
    C166_ALU_ADD,
    C166_ALU_ADDC,
    C166_ALU_SUB,
    C166_ALU_SUBC,
    C166_ALU_CMP,
    C166_ALU_XOR,
    C166_ALU_AND,
    C166_ALU_OR,
};

static void c166_gen_pack_flags(TCGv_i32 result, TCGv_i32 carry,
                                TCGv_i32 overflow, TCGv_i32 equal,
                                unsigned width, bool sticky_zero)
{
    unsigned sign = 1u << (width - 1);
    TCGv_i32 flags = tcg_temp_new_i32();
    TCGv_i32 bit = tcg_temp_new_i32();

    tcg_gen_andi_i32(flags, cpu_psw,
                     ~(C166_PSW_N | C166_PSW_C | C166_PSW_V |
                       C166_PSW_Z | C166_PSW_E));
    tcg_gen_andi_i32(bit, result, sign);
    tcg_gen_shri_i32(bit, bit, width - 1);
    tcg_gen_or_i32(flags, flags, bit);
    tcg_gen_shli_i32(bit, carry, 1);
    tcg_gen_or_i32(flags, flags, bit);
    tcg_gen_shli_i32(bit, overflow, 2);
    tcg_gen_or_i32(flags, flags, bit);
    tcg_gen_setcondi_i32(TCG_COND_EQ, bit, result, 0);
    if (sticky_zero) {
        TCGv_i32 previous = tcg_temp_new_i32();

        tcg_gen_shri_i32(previous, cpu_psw, 3);
        tcg_gen_andi_i32(previous, previous, 1);
        tcg_gen_and_i32(bit, bit, previous);
    }
    tcg_gen_shli_i32(bit, bit, 3);
    tcg_gen_or_i32(flags, flags, bit);
    tcg_gen_shli_i32(bit, equal, 4);
    tcg_gen_or_i32(cpu_psw, flags, bit);
}

static void c166_gen_logic_flags(TCGv_i32 result, TCGv_i32 operand,
                                 unsigned width)
{
    unsigned sign = 1u << (width - 1);
    TCGv_i32 zero = tcg_constant_i32(0);
    TCGv_i32 equal = tcg_temp_new_i32();

    tcg_gen_setcondi_i32(TCG_COND_EQ, equal, operand, sign);
    c166_gen_pack_flags(result, zero, zero, equal, width, false);
}

static TCGv_i32 c166_gen_alu(unsigned kind, TCGv_i32 left,
                             TCGv_i32 right, unsigned width)
{
    unsigned mask = width == 8 ? 0xff : 0xffff;
    unsigned sign = 1u << (width - 1);
    TCGv_i32 a = tcg_temp_new_i32();
    TCGv_i32 b = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();
    TCGv_i32 full = tcg_temp_new_i32();
    TCGv_i32 carry = tcg_temp_new_i32();
    TCGv_i32 overflow = tcg_temp_new_i32();
    TCGv_i32 equal = tcg_temp_new_i32();
    TCGv_i32 tmp = tcg_temp_new_i32();
    bool sticky = kind == C166_ALU_ADDC || kind == C166_ALU_SUBC;

    tcg_gen_andi_i32(a, left, mask);
    tcg_gen_andi_i32(b, right, mask);
    if (kind >= C166_ALU_XOR) {
        if (kind == C166_ALU_XOR) {
            tcg_gen_xor_i32(result, a, b);
        } else if (kind == C166_ALU_AND) {
            tcg_gen_and_i32(result, a, b);
        } else {
            tcg_gen_or_i32(result, a, b);
        }
        c166_gen_logic_flags(result, b, width);
        return result;
    }

    if (kind == C166_ALU_ADD || kind == C166_ALU_ADDC) {
        tcg_gen_add_i32(full, a, b);
        if (kind == C166_ALU_ADDC) {
            tcg_gen_shri_i32(tmp, cpu_psw, 1);
            tcg_gen_andi_i32(tmp, tmp, 1);
            tcg_gen_add_i32(full, full, tmp);
        }
        tcg_gen_andi_i32(result, full, mask);
        tcg_gen_shri_i32(carry, full, width);
        tcg_gen_andi_i32(carry, carry, 1);
        tcg_gen_xor_i32(overflow, a, b);
        tcg_gen_not_i32(overflow, overflow);
        tcg_gen_xor_i32(tmp, a, result);
        tcg_gen_and_i32(overflow, overflow, tmp);
    } else {
        TCGv_i32 subtrahend = tcg_temp_new_i32();

        tcg_gen_mov_i32(subtrahend, b);
        if (kind == C166_ALU_SUBC) {
            tcg_gen_shri_i32(tmp, cpu_psw, 1);
            tcg_gen_andi_i32(tmp, tmp, 1);
            tcg_gen_add_i32(subtrahend, subtrahend, tmp);
        }
        tcg_gen_sub_i32(full, a, subtrahend);
        tcg_gen_andi_i32(result, full, mask);
        tcg_gen_setcond_i32(TCG_COND_LTU, carry, a, subtrahend);
        tcg_gen_xor_i32(overflow, a, b);
        tcg_gen_xor_i32(tmp, a, result);
        tcg_gen_and_i32(overflow, overflow, tmp);
    }
    tcg_gen_andi_i32(overflow, overflow, sign);
    tcg_gen_shri_i32(overflow, overflow, width - 1);
    tcg_gen_setcondi_i32(TCG_COND_EQ, equal, b, sign);
    c166_gen_pack_flags(result, carry, overflow, equal, width, sticky);
    return result;
}

static TCGv_i32 c166_gen_arith_begin(unsigned kind, unsigned width)
{
    if (width != 16 || (kind != C166_ALU_ADD && kind != C166_ALU_SUB)) {
        return NULL;
    }
    TCGv_i32 serial = tcg_temp_new_i32();
    tcg_gen_ld_i32(serial, tcg_env,
                       offsetof(C166CPUState, sp_write_serial));
    return serial;
}

static void c166_gen_arith_end(unsigned kind, TCGv_i32 before)
{
    if (!before) {
        return;
    }
    TCGLabel *done = gen_new_label();
    TCGv_i32 after = tcg_temp_new_i32();
    tcg_gen_ld_i32(after, tcg_env, offsetof(C166CPUState, sp_write_serial));
    tcg_gen_brcond_i32(TCG_COND_EQ, before, after, done);
    gen_helper_c166_stack_arithmetic(tcg_env,
                                     tcg_constant_i32(kind == C166_ALU_SUB));
    gen_set_label(done);
}

static void c166_gen_alu_rr(DisasContext *ctx, unsigned kind, unsigned dst,
                            unsigned src, unsigned width)
{
    TCGv_i32 sp_serial = c166_gen_arith_begin(kind, width);

    translator_io_start(&ctx->base);
    if (kind != C166_ALU_CMP) {
        c166_gen_probe_reg(ctx, 0xf0 | dst, width, true);
    }

    TCGv_i32 left = width == 8 ? c166_gen_read_gpr_byte(ctx, dst) :
                                 c166_gen_read_gpr(ctx, dst);
    TCGv_i32 right = width == 8 ? c166_gen_read_gpr_byte(ctx, src) :
                                  c166_gen_read_gpr(ctx, src);
    TCGv_i32 result = c166_gen_alu(kind, left, right, width);

    if (kind == C166_ALU_CMP) {
        return;
    }
    if (width == 8) {
        c166_gen_write_gpr_byte(ctx, dst, result);
    } else {
        c166_gen_write_gpr(ctx, dst, result);
    }
    c166_gen_arith_end(kind, sp_serial);
}

static void c166_gen_alu_ri16(DisasContext *ctx, unsigned kind,
                              unsigned reg, unsigned width)
{
    TCGv_i32 sp_serial = c166_gen_arith_begin(kind, width);

    translator_io_start(&ctx->base);
    if (kind != C166_ALU_CMP) {
        c166_gen_probe_reg(ctx, reg, width, true);
    }

    uint16_t immediate = c166_fetch_u16(ctx);
    TCGv_i32 left = width == 8 ? c166_gen_read_reg_byte(ctx, reg) :
                                 c166_gen_read_reg(ctx, reg);
    TCGv_i32 right = tcg_constant_i32(width == 8 ? immediate & 0xff :
                                                   immediate);
    TCGv_i32 result = c166_gen_alu(kind, left, right, width);

    if (kind == C166_ALU_CMP) {
        return;
    }
    if (width == 8) {
        c166_gen_write_reg_byte(ctx, reg, result);
    } else {
        c166_gen_write_reg(ctx, reg, result);
    }
    c166_gen_arith_end(kind, sp_serial);
}

enum {
    C166_SHIFT_ROL,
    C166_SHIFT_ROR,
    C166_SHIFT_SHL,
    C166_SHIFT_SHR,
    C166_SHIFT_ASHR,
};

static void c166_gen_shift(DisasContext *ctx, unsigned kind, unsigned dst,
                           TCGv_i32 raw_count)
{
    TCGv_i32 count = tcg_temp_new_i32();
    TCGv_i32 result = c166_gen_read_gpr(ctx, dst);
    TCGv_i32 carry = tcg_temp_new_i32();
    TCGv_i32 overflow = tcg_temp_new_i32();

    tcg_gen_andi_i32(count, raw_count, 0xf);
    tcg_gen_movi_i32(carry, 0);
    tcg_gen_movi_i32(overflow, 0);
    for (unsigned i = 0; i < 15; i++) {
        TCGv_i32 active = tcg_temp_new_i32();
        TCGv_i32 next = tcg_temp_new_i32();
        TCGv_i32 next_carry = tcg_temp_new_i32();

        tcg_gen_setcondi_i32(TCG_COND_GTU, active, count, i);
        if (kind == C166_SHIFT_ROL || kind == C166_SHIFT_SHL) {
            tcg_gen_shri_i32(next_carry, result, 15);
            tcg_gen_shli_i32(next, result, 1);
            if (kind == C166_SHIFT_ROL) {
                tcg_gen_or_i32(next, next, next_carry);
            }
        } else {
            tcg_gen_andi_i32(next_carry, result, 1);
            tcg_gen_shri_i32(next, result, 1);
            if (kind == C166_SHIFT_ROR) {
                TCGv_i32 high = tcg_temp_new_i32();

                tcg_gen_shli_i32(high, next_carry, 15);
                tcg_gen_or_i32(next, next, high);
            } else if (kind == C166_SHIFT_ASHR) {
                TCGv_i32 high = tcg_temp_new_i32();

                tcg_gen_andi_i32(high, result, 0x8000);
                tcg_gen_or_i32(next, next, high);
            }
            {
                TCGv_i32 next_overflow = tcg_temp_new_i32();

                tcg_gen_or_i32(next_overflow, overflow, carry);
                tcg_gen_movcond_i32(TCG_COND_NE, overflow, active,
                                    tcg_constant_i32(0), next_overflow,
                                    overflow);
            }
        }
        tcg_gen_andi_i32(next, next, 0xffff);
        tcg_gen_movcond_i32(TCG_COND_NE, carry, active,
                            tcg_constant_i32(0), next_carry, carry);
        tcg_gen_movcond_i32(TCG_COND_NE, result, active,
                            tcg_constant_i32(0), next, result);
    }
    c166_gen_write_gpr(ctx, dst, result);
    c166_gen_pack_flags(result, carry, overflow, tcg_constant_i32(0),
                        16, false);
}

static TCGv_i32 c166_gen_bitoff_addr(unsigned bitoff)
{
    TCGv_i32 addr = tcg_temp_new_i32();

    if (bitoff <= 0x7f) {
        tcg_gen_movi_i32(addr, 0xfd00 + 2 * bitoff);
    } else if (bitoff <= 0xef) {
        TCGv_i32 esfr = tcg_constant_i32(0xf100 +
                                         2 * (bitoff & 0x7f));

        tcg_gen_movi_i32(addr, 0xff00 + 2 * (bitoff & 0x7f));
        tcg_gen_movcond_i32(TCG_COND_NE, addr, cpu_extr,
                            tcg_constant_i32(0), esfr, addr);
    } else {
        tcg_gen_addi_i32(addr, cpu_cp, 2 * (bitoff & 15));
        tcg_gen_andi_i32(addr, addr, 0xffff);
    }
    return addr;
}

static TCGv_i32 c166_gen_read_bitoff(DisasContext *ctx, unsigned bitoff)
{
    TCGv_i32 value = tcg_temp_new_i32();

    if (bitoff >= 0x80) {
        return c166_gen_read_reg(ctx, bitoff);
    }
    c166_gen_data_load(ctx, value, c166_gen_bitoff_addr(bitoff), MO_LEUW);
    return value;
}

static TCGv_i32 c166_gen_read_bit_branch(DisasContext *ctx, unsigned bitoff,
                                         bool conditional_write)
{
    TCGv_i32 word;
    TCGv_i32 valid;
    TCGLabel *read;
    TCGLabel *done;

    if (!ctx->watched || !conditional_write) {
        return c166_gen_read_bitoff(ctx, bitoff);
    }

    /*
     * A conditional write cannot be probed before reading its condition:
     * doing so would invent a write watch hit on the not-taken path. Latch
     * the committed read instead, so write-watch replay does not repeat an
     * MMIO side effect. Read-watch replay exits before this latch is set.
     * The branch clears the latch after its last access, before either exit.
     */
    word = tcg_temp_new_i32();
    valid = tcg_temp_new_i32();
    read = gen_new_label();
    done = gen_new_label();
    tcg_gen_ld_i32(valid, tcg_env, offsetof(C166CPUState, bitop_valid));
    tcg_gen_brcondi_i32(TCG_COND_EQ, valid, 0, read);
    tcg_gen_ld_i32(word, tcg_env, offsetof(C166CPUState, bitop_value));
    tcg_gen_br(done);
    gen_set_label(read);
    tcg_gen_mov_i32(word, c166_gen_read_bitoff(ctx, bitoff));
    tcg_gen_st_i32(word, tcg_env, offsetof(C166CPUState, bitop_value));
    tcg_gen_st_i32(tcg_constant_i32(1), tcg_env,
                   offsetof(C166CPUState, bitop_valid));
    gen_set_label(done);
    return word;
}

static void c166_gen_write_bitoff(DisasContext *ctx, unsigned bitoff,
                                  TCGv_i32 value)
{
    if (bitoff >= 0x80) {
        c166_gen_write_reg(ctx, bitoff, value);
        return;
    }
    c166_gen_data_store(ctx, value, c166_gen_bitoff_addr(bitoff), MO_LEUW);
}

static void c166_gen_probe_bitoff(DisasContext *ctx, unsigned bitoff,
                                  bool write)
{
    if (bitoff >= 0x80) {
        c166_gen_probe_reg(ctx, bitoff, 16, write);
    } else {
        c166_gen_probe(c166_gen_bitoff_addr(bitoff), 2, write);
    }
}

static void c166_gen_tested_bit_flags(TCGv_i32 bit)
{
    TCGv_i32 flags = tcg_temp_new_i32();
    TCGv_i32 zero = tcg_temp_new_i32();

    tcg_gen_andi_i32(flags, cpu_psw,
                     ~(C166_PSW_N | C166_PSW_C | C166_PSW_V |
                       C166_PSW_Z | C166_PSW_E));
    tcg_gen_or_i32(flags, flags, bit);
    tcg_gen_xori_i32(zero, bit, 1);
    tcg_gen_shli_i32(zero, zero, 3);
    tcg_gen_or_i32(cpu_psw, flags, zero);
}

static void c166_gen_bit_set_clear(DisasContext *ctx, unsigned bitoff,
                                   unsigned bit, bool set)
{
    translator_io_start(&ctx->base);
    c166_gen_probe_bitoff(ctx, bitoff, true);

    TCGv_i32 value = c166_gen_read_bitoff(ctx, bitoff);
    TCGv_i32 previous = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();

    tcg_gen_shri_i32(previous, value, bit);
    tcg_gen_andi_i32(previous, previous, 1);
    if (set) {
        tcg_gen_ori_i32(result, value, 1u << bit);
    } else {
        tcg_gen_andi_i32(result, value, ~(1u << bit));
    }
    c166_gen_write_bitoff(ctx, bitoff, result);
    c166_gen_tested_bit_flags(previous);
}

enum {
    C166_BIT_BAND,
    C166_BIT_BOR,
    C166_BIT_BXOR,
    C166_BIT_BMOV,
    C166_BIT_BMOVN,
    C166_BIT_BCMP,
};

static void c166_gen_bit_logic(DisasContext *ctx, unsigned kind,
                               unsigned src_bitoff)
{
    uint16_t operands = c166_fetch_u16(ctx);
    unsigned dst_bitoff = operands & 0xff;
    unsigned dst_bit = (operands >> 8) & 15;
    unsigned src_bit = operands >> 12;
    translator_io_start(&ctx->base);
    if (kind != C166_BIT_BCMP) {
        c166_gen_probe_bitoff(ctx, dst_bitoff, true);
    }
    c166_gen_probe_bitoff(ctx, dst_bitoff, false);
    TCGv_i32 src_word = c166_gen_read_bitoff(ctx, src_bitoff);
    TCGv_i32 dst_word = c166_gen_read_bitoff(ctx, dst_bitoff);
    TCGv_i32 src = tcg_temp_new_i32();
    TCGv_i32 dst = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();
    TCGv_i32 flags = tcg_temp_new_i32();
    TCGv_i32 tmp = tcg_temp_new_i32();

    tcg_gen_shri_i32(src, src_word, src_bit);
    tcg_gen_andi_i32(src, src, 1);
    tcg_gen_shri_i32(dst, dst_word, dst_bit);
    tcg_gen_andi_i32(dst, dst, 1);
    switch (kind) {
    case C166_BIT_BAND:
        tcg_gen_and_i32(result, dst, src);
        break;
    case C166_BIT_BOR:
        tcg_gen_or_i32(result, dst, src);
        break;
    case C166_BIT_BXOR:
        tcg_gen_xor_i32(result, dst, src);
        break;
    case C166_BIT_BMOV:
        tcg_gen_mov_i32(result, src);
        break;
    case C166_BIT_BMOVN:
        tcg_gen_xori_i32(result, src, 1);
        break;
    case C166_BIT_BCMP:
        tcg_gen_movi_i32(result, 0);
        break;
    default:
        g_assert_not_reached();
    }

    tcg_gen_andi_i32(flags, cpu_psw,
                     ~(C166_PSW_N | C166_PSW_C | C166_PSW_V |
                       C166_PSW_Z | C166_PSW_E));
    if (kind == C166_BIT_BMOV || kind == C166_BIT_BMOVN) {
        tcg_gen_or_i32(flags, flags, src);
        tcg_gen_xori_i32(tmp, src, 1);
        tcg_gen_shli_i32(tmp, tmp, 3);
        tcg_gen_or_i32(cpu_psw, flags, tmp);
    } else {
        tcg_gen_xor_i32(tmp, dst, src);
        tcg_gen_or_i32(flags, flags, tmp);
        tcg_gen_and_i32(tmp, dst, src);
        tcg_gen_shli_i32(tmp, tmp, 1);
        tcg_gen_or_i32(flags, flags, tmp);
        tcg_gen_or_i32(tmp, dst, src);
        tcg_gen_shli_i32(tmp, tmp, 2);
        tcg_gen_or_i32(flags, flags, tmp);
        tcg_gen_or_i32(tmp, dst, src);
        tcg_gen_xori_i32(tmp, tmp, 1);
        tcg_gen_shli_i32(tmp, tmp, 3);
        tcg_gen_or_i32(cpu_psw, flags, tmp);
    }
    if (kind != C166_BIT_BCMP) {
        TCGv_i32 cleared = tcg_temp_new_i32();
        TCGv_i32 shifted = tcg_temp_new_i32();

        tcg_gen_andi_i32(cleared, dst_word, ~(1u << dst_bit));
        tcg_gen_shli_i32(shifted, result, dst_bit);
        tcg_gen_or_i32(cleared, cleared, shifted);
        c166_gen_write_bitoff(ctx, dst_bitoff, cleared);
    }
}

static void c166_gen_bfld(DisasContext *ctx, unsigned bitoff, bool high)
{
    translator_io_start(&ctx->base);
    c166_gen_probe_bitoff(ctx, bitoff, true);

    uint16_t operands = c166_fetch_u16(ctx);
    unsigned mask = high ? operands >> 8 : operands & 0xff;
    unsigned data = high ? operands & 0xff : operands >> 8;
    unsigned shift = high ? 8 : 0;
    TCGv_i32 value = c166_gen_read_bitoff(ctx, bitoff);
    TCGv_i32 result = tcg_temp_new_i32();

    tcg_gen_andi_i32(result, value, ~((uint32_t)mask << shift));
    tcg_gen_ori_i32(result, result, (uint32_t)data << shift);
    c166_gen_write_bitoff(ctx, bitoff, result);
    c166_gen_logic_flags(result, tcg_constant_i32(0), 16);
}

static void c166_gen_mul(DisasContext *ctx, unsigned dst, unsigned src,
                         bool signed_multiply)
{
    TCGv_i32 left = c166_gen_read_gpr(ctx, dst);
    TCGv_i32 right = c166_gen_read_gpr(ctx, src);
    TCGv_i32 result = tcg_temp_new_i32();
    TCGv_i32 high = tcg_temp_new_i32();
    TCGv_i32 expected_high = tcg_temp_new_i32();
    TCGv_i32 overflow = tcg_temp_new_i32();

    if (signed_multiply) {
        tcg_gen_ext16s_i32(left, left);
        tcg_gen_ext16s_i32(right, right);
    }
    tcg_gen_mul_i32(result, left, right);
    tcg_gen_andi_i32(cpu_mdl, result, 0xffff);
    tcg_gen_shri_i32(high, result, 16);
    tcg_gen_andi_i32(cpu_mdh, high, 0xffff);
    if (signed_multiply) {
        tcg_gen_shri_i32(expected_high, cpu_mdl, 15);
        tcg_gen_neg_i32(expected_high, expected_high);
        tcg_gen_andi_i32(expected_high, expected_high, 0xffff);
    } else {
        tcg_gen_movi_i32(expected_high, 0);
    }
    tcg_gen_setcond_i32(TCG_COND_NE, overflow, cpu_mdh, expected_high);
    c166_gen_pack_flags(result, tcg_constant_i32(0), overflow,
                        tcg_constant_i32(0), 32, false);
}

static void c166_gen_div(DisasContext *ctx, unsigned reg, bool signed_divide,
                         bool dividend_32)
{
    TCGv_i32 divisor = c166_gen_read_gpr(ctx, reg);
    TCGv_i32 dividend = tcg_temp_new_i32();
    TCGv_i32 quotient = tcg_temp_new_i32();
    TCGv_i32 remainder = tcg_temp_new_i32();
    TCGv_i32 overflow = tcg_temp_new_i32();
    TCGLabel *divide_zero = gen_new_label();
    TCGLabel *done = gen_new_label();

    tcg_gen_brcondi_i32(TCG_COND_EQ, divisor, 0, divide_zero);
    if (dividend_32) {
        tcg_gen_shli_i32(dividend, cpu_mdh, 16);
        tcg_gen_or_i32(dividend, dividend, cpu_mdl);
    } else {
        tcg_gen_mov_i32(dividend, cpu_mdl);
    }
    if (signed_divide) {
        tcg_gen_ext16s_i32(divisor, divisor);
        if (!dividend_32) {
            tcg_gen_ext16s_i32(dividend, dividend);
        }
        tcg_gen_div_i32(quotient, dividend, divisor);
        tcg_gen_rem_i32(remainder, dividend, divisor);
        {
            TCGv_i32 sign_extended = tcg_temp_new_i32();

            tcg_gen_ext16s_i32(sign_extended, quotient);
            tcg_gen_setcond_i32(TCG_COND_NE, overflow, quotient,
                                sign_extended);
        }
    } else {
        tcg_gen_divu_i32(quotient, dividend, divisor);
        tcg_gen_remu_i32(remainder, dividend, divisor);
        tcg_gen_setcondi_i32(TCG_COND_GTU, overflow, quotient, 0xffff);
    }
    tcg_gen_andi_i32(cpu_mdl, quotient, 0xffff);
    tcg_gen_andi_i32(cpu_mdh, remainder, 0xffff);
    c166_gen_pack_flags(cpu_mdl, tcg_constant_i32(0), overflow,
                        tcg_constant_i32(0), 16, false);
    tcg_gen_br(done);

    gen_set_label(divide_zero);
    tcg_gen_andi_i32(cpu_psw, cpu_psw,
                     ~(C166_PSW_N | C166_PSW_C | C166_PSW_V |
                       C166_PSW_Z | C166_PSW_E));
    tcg_gen_ori_i32(cpu_psw, cpu_psw, C166_PSW_V);
    gen_set_label(done);
}

enum {
    C166_BIT_JB,
    C166_BIT_JNB,
    C166_BIT_JBC,
    C166_BIT_JNBS,
};

static void c166_gen_bit_branch(DisasContext *ctx, unsigned kind,
                                unsigned bitoff)
{
    uint16_t operands = c166_fetch_u16(ctx);
    unsigned bit_number = operands >> 12;
    int8_t relative = operands & 0xff;
    uint32_t fallthrough = ctx->base.pc_next & C166_ADDR_MASK;
    uint32_t dest = (fallthrough & 0xff0000) |
                    ((fallthrough + relative * 2) & 0xffff);
    bool conditional_write = kind == C166_BIT_JBC || kind == C166_BIT_JNBS;
    TCGv_i32 word;
    TCGv_i32 bit = tcg_temp_new_i32();
    TCGv_i32 condition = tcg_temp_new_i32();
    TCGLabel *take = gen_new_label();

    translator_io_start(&ctx->base);
    word = c166_gen_read_bit_branch(ctx, bitoff, conditional_write);
    tcg_gen_shri_i32(bit, word, bit_number);
    tcg_gen_andi_i32(bit, bit, 1);
    if (kind == C166_BIT_JNB || kind == C166_BIT_JNBS) {
        tcg_gen_xori_i32(condition, bit, 1);
    } else {
        tcg_gen_mov_i32(condition, bit);
    }
    if (conditional_write) {
        TCGLabel *no_write = gen_new_label();
        tcg_gen_brcondi_i32(TCG_COND_EQ, condition, 0, no_write);
        c166_gen_probe_bitoff(ctx, bitoff, true);
        if (kind == C166_BIT_JBC) {
            tcg_gen_andi_i32(word, word, ~(1u << bit_number));
        } else {
            tcg_gen_ori_i32(word, word, 1u << bit_number);
        }
        c166_gen_write_bitoff(ctx, bitoff, word);
        gen_set_label(no_write);
        c166_gen_tested_bit_flags(bit);
        if (ctx->watched) {
            tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                           offsetof(C166CPUState, bitop_valid));
        }
    }
    /* Both accesses belong to this window, including its last instruction. */
    c166_gen_window_finish(ctx);
    tcg_gen_brcondi_i32(TCG_COND_NE, condition, 0, take);
    c166_gen_goto_tb(ctx, 1, fallthrough);

    gen_set_label(take);
    c166_gen_goto_tb(ctx, 0, dest);
}

static void c166_gen_trap_frame(DisasContext *ctx, TCGv_i32 saved_psw,
                                TCGv_i32 saved_csp,
                                TCGv_i32 saved_ip, unsigned vector,
                                bool hardware)
{
    c166_gen_probe_stack(-2, 2, true);
    TCGLabel *probed = gen_new_label();
    TCGv_i32 segmented = tcg_temp_new_i32();
    tcg_gen_andi_i32(segmented, cpu_syscon, 0x0800);
    tcg_gen_brcondi_i32(TCG_COND_NE, segmented, 0, probed);
    c166_gen_probe_stack(-6, 1, true);
    gen_set_label(probed);

    TCGLabel *segmentation_disabled = gen_new_label();
    TCGv_i32 sgtdis = tcg_temp_new_i32();

    c166_gen_push(ctx, saved_psw);
    tcg_gen_andi_i32(sgtdis, cpu_syscon, 0x0800);
    tcg_gen_brcondi_i32(TCG_COND_NE, sgtdis, 0,
                        segmentation_disabled);
    c166_gen_push(ctx, saved_csp);
    gen_set_label(segmentation_disabled);
    c166_gen_push(ctx, saved_ip);
    if (hardware) {
        tcg_gen_andi_i32(cpu_psw, cpu_psw, ~C166_PSW_ILVL_MASK);
        tcg_gen_ori_i32(cpu_psw, cpu_psw, C166_PSW_ILVL_MASK);
    }
    tcg_gen_movi_i32(cpu_csp, 0);
    tcg_gen_movi_i32(cpu_ip, (vector * 4) & 0xffff);
}

static void c166_gen_window_cancel(DisasContext *ctx)
{
    tcg_gen_movi_i32(cpu_ext_count, 0);
    tcg_gen_movi_i32(cpu_ext_kind, C166_EXT_NONE);
    tcg_gen_movi_i32(cpu_extr, 0);
    ctx->window_finished = true;
}

static void c166_gen_class_b_trap(DisasContext *ctx, unsigned flag,
                                  TCGv_i32 saved_ip)
{
    tcg_gen_ori_i32(cpu_tfr, cpu_tfr, flag);
    tcg_gen_st_i32(c166_gen_segmented_pc(saved_ip), tcg_env,
                   offsetof(C166CPUState, trap_fault_pc));
    tcg_gen_st_i32(tcg_constant_i32(1), tcg_env,
                   offsetof(C166CPUState, trap_fault_valid));
    c166_gen_window_cancel(ctx);
    c166_gen_trap_boundary(ctx,
        tcg_constant_i32(ctx->base.pc_next & C166_ADDR_MASK));
    /* A Class-B fault during Class-A service is retained, its fault IP lost. */
    c166_gen_set_next_ip(ctx);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void c166_gen_stack_bound_check(DisasContext *ctx, bool overflow)
{
    TCGLabel *in_bounds = gen_new_label();

    tcg_gen_brcond_i32(overflow ? TCG_COND_GEU : TCG_COND_LEU,
                       cpu_sp, overflow ? cpu_stkov : cpu_stkun, in_bounds);
    tcg_gen_ori_i32(cpu_tfr, cpu_tfr, overflow ? 0x4000 : 0x2000);
    tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                   offsetof(C166CPUState, trap_delay) + (overflow ? 0 : 4));
    gen_set_label(in_bounds);
    c166_gen_goto_tb(ctx, 0, ctx->base.pc_next & C166_ADDR_MASK);
}

static void c166_gen_stack_transfer_begin(DisasContext *ctx)
{
    translator_io_start(&ctx->base);

    tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                   offsetof(C166CPUState, stack_bound_request));
    ctx->stack_check = true;
}

static TCGv_i32 c166_gen_segmented_pc(TCGv_i32 ip)
{
    TCGv_i32 pc = tcg_temp_new_i32();

    TCGv_i32 low = tcg_temp_new_i32();

    tcg_gen_andi_i32(low, ip, 0xffff);
    tcg_gen_shli_i32(pc, cpu_csp, 16);
    tcg_gen_or_i32(pc, pc, low);
    return pc;
}

static void c166_gen_stack_transfer_end(DisasContext *ctx,
                                        TCGv_i32 destination, bool overflow)
{
    TCGv_i32 next = tcg_temp_new_i32();
    TCGv_i32 request = tcg_temp_new_i32();
    TCGv_i32 segment = tcg_temp_new_i32();
    TCGv_i32 ip = tcg_temp_new_i32();
    TCGv_i32 sgtdis = tcg_temp_new_i32();
    TCGLabel *no_request = gen_new_label();

    ctx->stack_check = false;
    tcg_gen_andi_i32(next, destination, C166_ADDR_MASK);
    tcg_gen_andi_i32(ip, next, 0xffff);
    tcg_gen_andi_i32(sgtdis, cpu_syscon, 0x0800);
    tcg_gen_movcond_i32(TCG_COND_NE, next, sgtdis, tcg_constant_i32(0),
                        ip, next);
    tcg_gen_ld_i32(request, tcg_env,
                   offsetof(C166CPUState, stack_bound_request));
    tcg_gen_or_i32(cpu_tfr, cpu_tfr, request);
    tcg_gen_brcondi_i32(TCG_COND_EQ, request, 0, no_request);
    tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                   offsetof(C166CPUState, trap_delay) + (overflow ? 0 : 4));
    gen_set_label(no_request);
    tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                   offsetof(C166CPUState, stack_bound_request));
    /* Preserve the pre-transfer CSP on a failed direct/indirect call target. */
    tcg_gen_shri_i32(segment, next, 16);
    tcg_gen_andi_i32(ip, next, 1);
    tcg_gen_movcond_i32(TCG_COND_EQ, cpu_csp, ip, tcg_constant_i32(0),
                        segment, cpu_csp);
    c166_gen_lookup_ip(ctx, next);
}

static void c166_gen_undefined_trap(DisasContext *ctx)
{
    uint32_t fault = (ctx->base.pc_next - 2) & 0xffff;

    c166_gen_class_b_trap(ctx, 0x0080, tcg_constant_i32(fault));
}

static bool c166_check_protected_tail(DisasContext *ctx, uint16_t expected)
{
    uint16_t actual = c166_fetch_u16(ctx);

    if (actual != expected) {
        uint32_t fault = (ctx->base.pc_next - 4) & 0xffff;

        c166_gen_class_b_trap(ctx, 0x0008,
                              tcg_constant_i32(fault));
        return false;
    }
    return true;
}

#include "decode-insns.c.inc"

static bool trans_NOP(DisasContext *ctx, arg_NOP *a)
{
    return true;
}

static bool trans_PRIOR(DisasContext *ctx, arg_PRIOR *a)
{
    TCGv_i32 source = c166_gen_read_gpr(ctx, a->src);
    TCGv_i32 count = tcg_temp_new_i32();
    TCGv_i32 flags = tcg_temp_new_i32();
    TCGv_i32 zero = tcg_temp_new_i32();

    tcg_gen_clzi_i32(count, source, 16);
    tcg_gen_subi_i32(count, count, 16);
    tcg_gen_setcondi_i32(TCG_COND_EQ, zero, source, 0);
    tcg_gen_movcond_i32(TCG_COND_EQ, count, source,
                        tcg_constant_i32(0), tcg_constant_i32(0), count);
    c166_gen_write_gpr(ctx, a->dst, count);
    tcg_gen_andi_i32(flags, cpu_psw, ~0x1f);
    tcg_gen_shli_i32(zero, zero, 3);
    tcg_gen_or_i32(cpu_psw, flags, zero);
    return true;
}

static void c166_gen_scxt(DisasContext *ctx, unsigned reg, TCGv_i32 value)
{
    translator_io_start(&ctx->base);

    c166_gen_probe_reg(ctx, reg, 16, true);
    c166_gen_probe_stack(-2, 1, true);

    c166_gen_push(ctx, c166_gen_read_reg(ctx, reg));
    c166_gen_write_reg(ctx, reg, value);
    c166_gen_stack_bound_check(ctx, true);
}

static bool trans_SCXT_ri16(DisasContext *ctx, arg_SCXT_ri16 *a)
{
    c166_gen_scxt(ctx, a->reg, tcg_constant_i32(c166_fetch_u16(ctx)));
    return true;
}

static bool trans_SCXT_rm16(DisasContext *ctx, arg_SCXT_rm16 *a)
{
    translator_io_start(&ctx->base);

    c166_gen_probe_reg(ctx, a->reg, 16, true);
    c166_gen_probe_stack(-2, 1, true);

    uint16_t direct = c166_fetch_u16(ctx);
    TCGv_i32 value = tcg_temp_new_i32();

    /*
     * The memory operand observes the updated SP.  This matters for the
     * compiler interrupt-frame idiom "SCXT CP, SP; ADD CP, #2": SCXT first
     * saves the old context, then reads SP to select the reserved GPR bank.
     */
    c166_gen_probe(c166_gen_resolve_data(direct), 2, false);
    c166_gen_push(ctx, c166_gen_read_reg(ctx, a->reg));
    c166_gen_data_load(ctx, value, c166_gen_resolve_data(direct), MO_LEUW);
    c166_gen_write_reg(ctx, a->reg, value);
    c166_gen_stack_bound_check(ctx, true);
    return true;
}

static bool trans_ALU_rr(DisasContext *ctx, arg_ALU_rr *a)
{
    c166_gen_alu_rr(ctx, a->kind, a->dst, a->src, 16);
    return true;
}

static bool trans_ALUB_rr(DisasContext *ctx, arg_ALUB_rr *a)
{
    c166_gen_alu_rr(ctx, a->kind, a->dst, a->src, 8);
    return true;
}

static void c166_gen_alu_direct(DisasContext *ctx, unsigned kind,
                                unsigned reg, unsigned width,
                                bool memory_destination)
{
    TCGv_i32 sp_serial = c166_gen_arith_begin(kind, width);

    uint16_t direct;
    TCGv_i32 addr;
    TCGv_i32 reg_value;
    TCGv_i32 mem_value;
    TCGv_i32 result;

    if (memory_destination && kind == C166_ALU_CMP) {
        c166_gen_undefined_trap(ctx);
        return;
    }
    direct = c166_fetch_u16(ctx);
    addr = c166_gen_resolve_data(direct);
    translator_io_start(&ctx->base);
    if (kind != C166_ALU_CMP) {
        if (memory_destination) {
            c166_gen_probe(addr, width / 8, true);
        } else {
            c166_gen_probe_reg(ctx, reg, width, true);
        }
    }
    c166_gen_probe(addr, width / 8, false);
    reg_value = width == 8 ? c166_gen_read_reg_byte(ctx, reg) :
                             c166_gen_read_reg(ctx, reg);
    mem_value = tcg_temp_new_i32();
    c166_gen_data_load(ctx, mem_value, addr,
                        width == 8 ? MO_UB : MO_LEUW);
    result = c166_gen_alu(kind,
                          memory_destination ? mem_value : reg_value,
                          memory_destination ? reg_value : mem_value,
                          width);
    if (kind == C166_ALU_CMP) {
        return;
    }
    if (memory_destination) {
        c166_gen_data_store(ctx, result, addr, width == 8 ? MO_UB : MO_LEUW);
    } else if (width == 8) {
        c166_gen_write_reg_byte(ctx, reg, result);
    } else {
        c166_gen_write_reg(ctx, reg, result);
    }
    c166_gen_arith_end(kind, sp_serial);
}

static bool trans_ALU_rm16(DisasContext *ctx, arg_ALU_rm16 *a)
{
    c166_gen_alu_direct(ctx, a->kind, a->reg, 16, false);
    return true;
}

static bool trans_ALUB_rm16(DisasContext *ctx, arg_ALUB_rm16 *a)
{
    c166_gen_alu_direct(ctx, a->kind, a->reg, 8, false);
    return true;
}

static bool trans_ALU_mr16(DisasContext *ctx, arg_ALU_mr16 *a)
{
    c166_gen_alu_direct(ctx, a->kind, a->reg, 16, true);
    return true;
}

static bool trans_ALUB_mr16(DisasContext *ctx, arg_ALUB_mr16 *a)
{
    c166_gen_alu_direct(ctx, a->kind, a->reg, 8, true);
    return true;
}

static bool trans_ALU_ri16(DisasContext *ctx, arg_ALU_ri16 *a)
{
    c166_gen_alu_ri16(ctx, a->kind, a->reg, 16);
    return true;
}

static bool trans_ALUB_ri16(DisasContext *ctx, arg_ALUB_ri16 *a)
{
    c166_gen_alu_ri16(ctx, a->kind, a->reg, 8);
    return true;
}

static void c166_gen_alu_compact(DisasContext *ctx, unsigned kind, unsigned dst,
                                 unsigned operand, unsigned width)
{
    TCGv_i32 sp_serial = c166_gen_arith_begin(kind, width);

    if (operand >= 12) {
        c166_gen_probe_reg(ctx, 0xf0 | (operand & 3), 16, true);
    }
    c166_gen_probe_reg(ctx, 0xf0 | dst, width, false);

    translator_io_start(&ctx->base);
    if (kind != C166_ALU_CMP) {
        c166_gen_probe_reg(ctx, 0xf0 | dst, width, true);
    }

    TCGv_i32 right;
    TCGv_i32 left;
    TCGv_i32 result;

    if (operand < 8) {
        right = tcg_constant_i32(operand);
    } else {
        unsigned pointer = operand & 3;
        TCGv_i32 offset = c166_gen_read_gpr(ctx, pointer);
        TCGv_i32 addr = c166_gen_resolve_data_dynamic(offset);

        right = tcg_temp_new_i32();
        c166_gen_data_load(ctx, right, addr,
                            width == 8 ? MO_UB : MO_LEUW);
        if (operand >= 12) {
            tcg_gen_addi_i32(offset, offset, width == 8 ? 1 : 2);
            tcg_gen_andi_i32(offset, offset, 0xffff);
            c166_gen_write_gpr(ctx, pointer, offset);
        }
    }

    left = width == 8 ? c166_gen_read_gpr_byte(ctx, dst) :
                        c166_gen_read_gpr(ctx, dst);
    result = c166_gen_alu(kind, left, right, width);

    if (kind == C166_ALU_CMP) {
        return;
    }
    if (width == 8) {
        c166_gen_write_gpr_byte(ctx, dst, result);
    } else {
        c166_gen_write_gpr(ctx, dst, result);
    }
    c166_gen_arith_end(kind, sp_serial);
}

static bool trans_ALU_compact(DisasContext *ctx, arg_ALU_compact *a)
{
    c166_gen_alu_compact(ctx, a->kind, a->dst, a->operand, 16);
    return true;
}

static bool trans_ALUB_compact(DisasContext *ctx, arg_ALUB_compact *a)
{
    c166_gen_alu_compact(ctx, a->kind, a->dst, a->operand, 8);
    return true;
}

static bool trans_NEG(DisasContext *ctx, arg_NEG *a)
{
    translator_io_start(&ctx->base);
    c166_gen_probe_reg(ctx, 0xf0 | a->reg, 16, true);

    TCGv_i32 result = c166_gen_alu(C166_ALU_SUB, tcg_constant_i32(0),
                                    c166_gen_read_gpr(ctx, a->reg), 16);

    c166_gen_write_gpr(ctx, a->reg, result);
    return true;
}

static bool trans_CPL(DisasContext *ctx, arg_CPL *a)
{
    TCGv_i32 original = c166_gen_read_gpr(ctx, a->reg);
    TCGv_i32 result = tcg_temp_new_i32();

    tcg_gen_xori_i32(result, original, 0xffff);
    c166_gen_write_gpr(ctx, a->reg, result);
    c166_gen_logic_flags(result, original, 16);
    return true;
}

static bool trans_NEGB(DisasContext *ctx, arg_NEGB *a)
{
    translator_io_start(&ctx->base);
    c166_gen_probe_reg(ctx, 0xf0 | a->reg, 8, true);

    TCGv_i32 result = c166_gen_alu(C166_ALU_SUB, tcg_constant_i32(0),
                                    c166_gen_read_gpr_byte(ctx, a->reg), 8);

    c166_gen_write_gpr_byte(ctx, a->reg, result);
    return true;
}

static bool trans_CPLB(DisasContext *ctx, arg_CPLB *a)
{
    TCGv_i32 original = c166_gen_read_gpr_byte(ctx, a->reg);
    TCGv_i32 result = tcg_temp_new_i32();

    tcg_gen_xori_i32(result, original, 0xff);
    c166_gen_write_gpr_byte(ctx, a->reg, result);
    c166_gen_logic_flags(result, original, 8);
    return true;
}

static bool trans_ROL_r(DisasContext *ctx, arg_ROL_r *a)
{
    c166_gen_shift(ctx, C166_SHIFT_ROL, a->dst, c166_gen_read_gpr(ctx, a->src));
    return true;
}

static bool trans_ROL_i(DisasContext *ctx, arg_ROL_i *a)
{
    c166_gen_shift(ctx, C166_SHIFT_ROL, a->dst,
                   tcg_constant_i32(a->count));
    return true;
}

static bool trans_ROR_r(DisasContext *ctx, arg_ROR_r *a)
{
    c166_gen_shift(ctx, C166_SHIFT_ROR, a->dst, c166_gen_read_gpr(ctx, a->src));
    return true;
}

static bool trans_ROR_i(DisasContext *ctx, arg_ROR_i *a)
{
    c166_gen_shift(ctx, C166_SHIFT_ROR, a->dst,
                   tcg_constant_i32(a->count));
    return true;
}

static bool trans_SHL_r(DisasContext *ctx, arg_SHL_r *a)
{
    c166_gen_shift(ctx, C166_SHIFT_SHL, a->dst, c166_gen_read_gpr(ctx, a->src));
    return true;
}

static bool trans_SHL_i(DisasContext *ctx, arg_SHL_i *a)
{
    c166_gen_shift(ctx, C166_SHIFT_SHL, a->dst,
                   tcg_constant_i32(a->count));
    return true;
}

static bool trans_SHR_r(DisasContext *ctx, arg_SHR_r *a)
{
    c166_gen_shift(ctx, C166_SHIFT_SHR, a->dst, c166_gen_read_gpr(ctx, a->src));
    return true;
}

static bool trans_SHR_i(DisasContext *ctx, arg_SHR_i *a)
{
    c166_gen_shift(ctx, C166_SHIFT_SHR, a->dst,
                   tcg_constant_i32(a->count));
    return true;
}

static bool trans_ASHR_r(DisasContext *ctx, arg_ASHR_r *a)
{
    c166_gen_shift(ctx, C166_SHIFT_ASHR, a->dst,
                    c166_gen_read_gpr(ctx, a->src));
    return true;
}

static bool trans_ASHR_i(DisasContext *ctx, arg_ASHR_i *a)
{
    c166_gen_shift(ctx, C166_SHIFT_ASHR, a->dst,
                   tcg_constant_i32(a->count));
    return true;
}

static void c166_gen_cmpi_value(DisasContext *ctx, unsigned dst,
                                TCGv_i32 operand, int delta)
{
    translator_io_start(&ctx->base);
    c166_gen_probe_reg(ctx, 0xf0 | dst, 16, true);

    TCGv_i32 value = c166_gen_read_gpr(ctx, dst);
    TCGv_i32 updated = tcg_temp_new_i32();

    c166_gen_alu(C166_ALU_CMP, value, operand, 16);
    tcg_gen_addi_i32(updated, value, delta);
    tcg_gen_andi_i32(updated, updated, 0xffff);
    c166_gen_write_gpr(ctx, dst, updated);
}

static void c166_gen_cmpi(DisasContext *ctx, unsigned dst, unsigned immediate,
                          int delta)
{
    c166_gen_cmpi_value(ctx, dst, tcg_constant_i32(immediate), delta);
}

static void c166_gen_cmpi_ri16(DisasContext *ctx, unsigned dst, int delta)
{
    c166_gen_cmpi(ctx, dst, c166_fetch_u16(ctx), delta);
}

static void c166_gen_cmpi_rm16(DisasContext *ctx, unsigned dst, int delta)
{
    translator_io_start(&ctx->base);
    c166_gen_probe_reg(ctx, 0xf0 | dst, 16, true);

    uint16_t direct = c166_fetch_u16(ctx);
    TCGv_i32 value = tcg_temp_new_i32();

    c166_gen_data_load(ctx, value, c166_gen_resolve_data(direct), MO_LEUW);
    c166_gen_cmpi_value(ctx, dst, value, delta);
}

static bool trans_CMPI1_ri4(DisasContext *ctx, arg_CMPI1_ri4 *a)
{
    c166_gen_cmpi(ctx, a->dst, a->imm, 1);
    return true;
}

static bool trans_CMPI2_ri4(DisasContext *ctx, arg_CMPI2_ri4 *a)
{
    c166_gen_cmpi(ctx, a->dst, a->imm, 2);
    return true;
}

static bool trans_CMPD1_ri4(DisasContext *ctx, arg_CMPD1_ri4 *a)
{
    c166_gen_cmpi(ctx, a->dst, a->imm, -1);
    return true;
}

static bool trans_CMPD2_ri4(DisasContext *ctx, arg_CMPD2_ri4 *a)
{
    c166_gen_cmpi(ctx, a->dst, a->imm, -2);
    return true;
}

static bool trans_CMPI1_ri16(DisasContext *ctx, arg_CMPI1_ri16 *a)
{
    c166_gen_cmpi_ri16(ctx, a->reg, 1);
    return true;
}

static bool trans_CMPI2_ri16(DisasContext *ctx, arg_CMPI2_ri16 *a)
{
    c166_gen_cmpi_ri16(ctx, a->reg, 2);
    return true;
}

static bool trans_CMPD1_ri16(DisasContext *ctx, arg_CMPD1_ri16 *a)
{
    c166_gen_cmpi_ri16(ctx, a->reg, -1);
    return true;
}

static bool trans_CMPD2_ri16(DisasContext *ctx, arg_CMPD2_ri16 *a)
{
    c166_gen_cmpi_ri16(ctx, a->reg, -2);
    return true;
}

static bool trans_CMPI1_rm16(DisasContext *ctx, arg_CMPI1_rm16 *a)
{
    c166_gen_cmpi_rm16(ctx, a->reg, 1);
    return true;
}

static bool trans_CMPI2_rm16(DisasContext *ctx, arg_CMPI2_rm16 *a)
{
    c166_gen_cmpi_rm16(ctx, a->reg, 2);
    return true;
}

static bool trans_CMPD1_rm16(DisasContext *ctx, arg_CMPD1_rm16 *a)
{
    c166_gen_cmpi_rm16(ctx, a->reg, -1);
    return true;
}

static bool trans_CMPD2_rm16(DisasContext *ctx, arg_CMPD2_rm16 *a)
{
    c166_gen_cmpi_rm16(ctx, a->reg, -2);
    return true;
}

static bool trans_BCLR(DisasContext *ctx, arg_BCLR *a)
{
    c166_gen_bit_set_clear(ctx, a->bitoff, a->bit, false);
    return true;
}

static bool trans_BSET(DisasContext *ctx, arg_BSET *a)
{
    c166_gen_bit_set_clear(ctx, a->bitoff, a->bit, true);
    return true;
}

static bool trans_BAND(DisasContext *ctx, arg_BAND *a)
{
    c166_gen_bit_logic(ctx, C166_BIT_BAND, a->reg);
    return true;
}

static bool trans_BOR(DisasContext *ctx, arg_BOR *a)
{
    c166_gen_bit_logic(ctx, C166_BIT_BOR, a->reg);
    return true;
}

static bool trans_BXOR(DisasContext *ctx, arg_BXOR *a)
{
    c166_gen_bit_logic(ctx, C166_BIT_BXOR, a->reg);
    return true;
}

static bool trans_BMOV(DisasContext *ctx, arg_BMOV *a)
{
    c166_gen_bit_logic(ctx, C166_BIT_BMOV, a->reg);
    return true;
}

static bool trans_BMOVN(DisasContext *ctx, arg_BMOVN *a)
{
    c166_gen_bit_logic(ctx, C166_BIT_BMOVN, a->reg);
    return true;
}

static bool trans_BCMP(DisasContext *ctx, arg_BCMP *a)
{
    c166_gen_bit_logic(ctx, C166_BIT_BCMP, a->reg);
    return true;
}

static bool trans_BFLDL(DisasContext *ctx, arg_BFLDL *a)
{
    c166_gen_bfld(ctx, a->reg, false);
    return true;
}

static bool trans_BFLDH(DisasContext *ctx, arg_BFLDH *a)
{
    c166_gen_bfld(ctx, a->reg, true);
    return true;
}

static bool trans_MUL(DisasContext *ctx, arg_MUL *a)
{
    c166_gen_mul(ctx, a->dst, a->src, true);
    return true;
}

static bool trans_MULU(DisasContext *ctx, arg_MULU *a)
{
    c166_gen_mul(ctx, a->dst, a->src, false);
    return true;
}

static bool trans_DIV(DisasContext *ctx, arg_DIV *a)
{
    if (a->reg != a->mirror) {
        c166_gen_undefined_trap(ctx);
        return true;
    }
    c166_gen_div(ctx, a->reg, true, false);
    return true;
}

static bool trans_DIVU(DisasContext *ctx, arg_DIVU *a)
{
    if (a->reg != a->mirror) {
        c166_gen_undefined_trap(ctx);
        return true;
    }
    c166_gen_div(ctx, a->reg, false, false);
    return true;
}

static bool trans_DIVL(DisasContext *ctx, arg_DIVL *a)
{
    if (a->reg != a->mirror) {
        c166_gen_undefined_trap(ctx);
        return true;
    }
    c166_gen_div(ctx, a->reg, true, true);
    return true;
}

static bool trans_DIVLU(DisasContext *ctx, arg_DIVLU *a)
{
    if (a->reg != a->mirror) {
        c166_gen_undefined_trap(ctx);
        return true;
    }
    c166_gen_div(ctx, a->reg, false, true);
    return true;
}

static void c166_gen_set_window(DisasContext *ctx, unsigned mode,
                                unsigned count, TCGv_i32 value)
{
    bool segment = mode == 0 || mode == 2;

    tcg_gen_movi_i32(cpu_ext_kind, segment ? C166_EXT_SEGMENT :
                                                C166_EXT_PAGE);
    tcg_gen_andi_i32(cpu_ext_value, value, segment ? 0xff : 0x3ff);
    tcg_gen_movi_i32(cpu_extr, mode >= 2);
    tcg_gen_movi_i32(cpu_ext_count, count + 1);
    c166_gen_trace_window(ctx, true);
    c166_gen_set_next_ip(ctx);
    ctx->window_reload = true;
    ctx->base.is_jmp = DISAS_EXIT;
}

static bool trans_EXTI(DisasContext *ctx, arg_EXTI *a)
{
    TCGv_i32 value = tcg_constant_i32(c166_fetch_u16(ctx));

    c166_gen_set_window(ctx, a->mode, a->count, value);
    return true;
}

static bool trans_EXTRI(DisasContext *ctx, arg_EXTRI *a)
{
    c166_gen_set_window(ctx, a->mode, a->count,
                        c166_gen_read_gpr(ctx, a->reg));
    return true;
}

static bool trans_ATOMIC_EXTR(DisasContext *ctx, arg_ATOMIC_EXTR *a)
{
    if (a->mode == 0 || a->mode == 2) {
        tcg_gen_movi_i32(cpu_ext_kind, C166_EXT_NONE);
        tcg_gen_movi_i32(cpu_ext_value, 0);
        tcg_gen_movi_i32(cpu_extr, a->mode == 2);
        tcg_gen_movi_i32(cpu_ext_count, a->count + 1);
        c166_gen_trace_window(ctx, true);
        c166_gen_set_next_ip(ctx);
        ctx->window_reload = true;
        ctx->base.is_jmp = DISAS_EXIT;
    }
    return true;
}

static bool trans_JB(DisasContext *ctx, arg_JB *a)
{
    c166_gen_bit_branch(ctx, C166_BIT_JB, a->reg);
    return true;
}

static bool trans_JNB(DisasContext *ctx, arg_JNB *a)
{
    c166_gen_bit_branch(ctx, C166_BIT_JNB, a->reg);
    return true;
}

static bool trans_JBC(DisasContext *ctx, arg_JBC *a)
{
    c166_gen_bit_branch(ctx, C166_BIT_JBC, a->reg);
    return true;
}

static bool trans_JNBS(DisasContext *ctx, arg_JNBS *a)
{
    c166_gen_bit_branch(ctx, C166_BIT_JNBS, a->reg);
    return true;
}

static bool trans_TRAP(DisasContext *ctx, arg_TRAP *a)
{
    c166_gen_stack_transfer_begin(ctx);

    c166_gen_trap_frame(ctx, cpu_psw, cpu_csp,
                        tcg_constant_i32(ctx->base.pc_next & 0xffff),
                        a->trap, false);
    TCGv_i32 depth = tcg_temp_new_i32();
    tcg_gen_ld_i32(depth, tcg_env, offsetof(C166CPUState, trap_depth));
    tcg_gen_addi_i32(depth, depth, 1);
    tcg_gen_st_i32(depth, tcg_env, offsetof(C166CPUState, trap_depth));
    c166_gen_stack_transfer_end(ctx,
                                tcg_constant_i32(a->trap * 4), true);
    return true;
}

static bool trans_DISWDT(DisasContext *ctx, arg_DISWDT *a)
{
    if (!c166_check_protected_tail(ctx, 0xa5a5)) {
        return true;
    }
    c166_gen_set_next_ip(ctx);
    ctx->base.is_jmp = DISAS_EXIT;
    return true;
}

static bool trans_EINIT(DisasContext *ctx, arg_EINIT *a)
{
    if (!c166_check_protected_tail(ctx, 0xb5b5)) {
        return true;
    }
    c166_gen_set_next_ip(ctx);
    c166_gen_window_finish(ctx);
    gen_helper_c166_einit(tcg_env);
    ctx->base.is_jmp = DISAS_EXIT;
    return true;
}

static bool trans_IDLE(DisasContext *ctx, arg_IDLE *a)
{
    if (!c166_check_protected_tail(ctx, 0x8787)) {
        return true;
    }
    c166_gen_set_next_ip(ctx);
    c166_gen_trap_boundary(ctx,
        tcg_constant_i32(ctx->base.pc_next & C166_ADDR_MASK));
    gen_helper_c166_idle(tcg_env);
    ctx->boundary_finished = true;
    ctx->base.is_jmp = DISAS_EXIT;
    return true;
}

static bool trans_PWRDN(DisasContext *ctx, arg_PWRDN *a)
{
    if (!c166_check_protected_tail(ctx, 0x9797)) {
        return true;
    }
    c166_gen_set_next_ip(ctx);
    c166_gen_trap_boundary(ctx,
        tcg_constant_i32(ctx->base.pc_next & C166_ADDR_MASK));
    gen_helper_c166_idle(tcg_env);
    ctx->boundary_finished = true;
    ctx->base.is_jmp = DISAS_EXIT;
    return true;
}

static bool trans_SRST(DisasContext *ctx, arg_SRST *a)
{
    if (!c166_check_protected_tail(ctx, 0xb7b7)) {
        return true;
    }
    tcg_gen_movi_i32(cpu_ip, 0);
    tcg_gen_movi_i32(cpu_csp, 0);
    for (unsigned i = 0; i < 4; i++) {
        tcg_gen_movi_i32(cpu_dpp[i], i);
    }
    tcg_gen_movi_i32(cpu_cp, 0xfc00);
    tcg_gen_movi_i32(cpu_sp, 0xfc00);
    tcg_gen_movi_i32(cpu_stkov, 0xfa00);
    tcg_gen_movi_i32(cpu_stkun, 0xfc00);
    tcg_gen_movi_i32(cpu_psw, 0);
    tcg_gen_movi_i32(cpu_mdh, 0);
    tcg_gen_movi_i32(cpu_mdl, 0);
    tcg_gen_movi_i32(cpu_mdc, 0);
    tcg_gen_movi_i32(cpu_tfr, 0);
    tcg_gen_movi_i32(cpu_syscon, 0);
    tcg_gen_movi_i32(cpu_einit_done, 0);
    tcg_gen_movi_i32(cpu_ext_kind, C166_EXT_NONE);
    tcg_gen_movi_i32(cpu_ext_value, 0);
    tcg_gen_movi_i32(cpu_ext_count, 0);
    tcg_gen_movi_i32(cpu_extr, 0);
    tcg_gen_movi_i32(cpu_idle, 0);
    gen_helper_c166_srst(tcg_env);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_SRVWDT(DisasContext *ctx, arg_SRVWDT *a)
{
    if (!c166_check_protected_tail(ctx, 0xa7a7)) {
        return true;
    }
    c166_gen_set_next_ip(ctx);
    ctx->base.is_jmp = DISAS_EXIT;
    return true;
}

static bool trans_MOV_ri4(DisasContext *ctx, arg_MOV_ri4 *a)
{
    TCGv_i32 value = tcg_constant_i32(a->imm);

    c166_gen_write_gpr(ctx, a->dst, value);
    c166_gen_mov_flags(value);
    return true;
}

static bool trans_MOVB_ri4(DisasContext *ctx, arg_MOVB_ri4 *a)
{
    TCGv_i32 value = tcg_constant_i32(a->imm);

    c166_gen_write_gpr_byte(ctx, a->dst, value);
    c166_gen_movb_flags(value);
    return true;
}

static bool trans_MOV_rr(DisasContext *ctx, arg_MOV_rr *a)
{
    TCGv_i32 value = c166_gen_read_gpr(ctx, a->src);

    c166_gen_write_gpr(ctx, a->dst, value);
    c166_gen_mov_flags(value);
    return true;
}

static bool trans_MOVB_rr(DisasContext *ctx, arg_MOVB_rr *a)
{
    TCGv_i32 value = c166_gen_read_gpr_byte(ctx, a->src);

    c166_gen_write_gpr_byte(ctx, a->dst, value);
    c166_gen_movb_flags(value);
    return true;
}

static bool trans_MOV_ri16(DisasContext *ctx, arg_MOV_ri16 *a)
{
    TCGv_i32 value = tcg_constant_i32(c166_fetch_u16(ctx));

    c166_gen_write_reg(ctx, a->reg, value);
    c166_gen_mov_flags(value);
    return true;
}

static bool trans_MOVB_ri16(DisasContext *ctx, arg_MOVB_ri16 *a)
{
    TCGv_i32 value = tcg_constant_i32(c166_fetch_u16(ctx) & 0xff);

    c166_gen_write_reg_byte(ctx, a->reg, value);
    c166_gen_movb_flags(value);
    return true;
}

static bool trans_MOV_rm16(DisasContext *ctx, arg_MOV_rm16 *a)
{
    translator_io_start(&ctx->base);
    c166_gen_probe_reg(ctx, a->reg, 16, true);

    uint16_t offset = c166_fetch_u16(ctx);
    TCGv_i32 addr = c166_gen_resolve_data(offset);
    TCGv_i32 value = tcg_temp_new_i32();

    c166_gen_data_load(ctx, value, addr, MO_LEUW);
    c166_gen_write_reg(ctx, a->reg, value);
    c166_gen_mov_flags(value);
    return true;
}

static bool trans_MOVB_rm16(DisasContext *ctx, arg_MOVB_rm16 *a)
{
    translator_io_start(&ctx->base);
    c166_gen_probe_reg(ctx, a->reg, 8, true);

    uint16_t offset = c166_fetch_u16(ctx);
    TCGv_i32 addr = c166_gen_resolve_data(offset);
    TCGv_i32 value = tcg_temp_new_i32();

    c166_gen_data_load(ctx, value, addr, MO_UB);
    c166_gen_write_reg_byte(ctx, a->reg, value);
    c166_gen_movb_flags(value);
    return true;
}

static bool trans_MOV_mr16(DisasContext *ctx, arg_MOV_mr16 *a)
{
    translator_io_start(&ctx->base);

    uint16_t offset = c166_fetch_u16(ctx);
    TCGv_i32 addr = c166_gen_resolve_data(offset);
    c166_gen_probe(addr, 2, true);
    TCGv_i32 value = c166_gen_read_reg(ctx, a->reg);

    c166_gen_data_store(ctx, value, addr, MO_LEUW);
    c166_gen_mov_flags(value);
    return true;
}

static bool trans_MOVB_mr16(DisasContext *ctx, arg_MOVB_mr16 *a)
{
    translator_io_start(&ctx->base);

    uint16_t offset = c166_fetch_u16(ctx);
    TCGv_i32 addr = c166_gen_resolve_data(offset);
    c166_gen_probe(addr, 1, true);
    TCGv_i32 value = c166_gen_read_reg_byte(ctx, a->reg);

    c166_gen_data_store(ctx, value, addr, MO_UB);
    c166_gen_movb_flags(value);
    return true;
}

static bool trans_MOV_idx_store(DisasContext *ctx, arg_MOV_idx_store *a)
{
    uint16_t offset = c166_fetch_u16(ctx);
    TCGv_i32 base = c166_gen_read_gpr(ctx, a->src);
    TCGv_i32 effective = tcg_temp_new_i32();
    TCGv_i32 addr;
    TCGv_i32 value = c166_gen_read_gpr(ctx, a->dst);

    tcg_gen_addi_i32(effective, base, offset);
    tcg_gen_andi_i32(effective, effective, 0xffff);
    addr = c166_gen_resolve_data_dynamic(effective);
    c166_gen_data_store(ctx, value, addr, MO_LEUW);
    c166_gen_mov_flags(value);
    return true;
}

static bool trans_MOV_idx_load(DisasContext *ctx, arg_MOV_idx_load *a)
{
    translator_io_start(&ctx->base);
    c166_gen_probe_reg(ctx, 0xf0 | a->dst, 16, true);

    uint16_t offset = c166_fetch_u16(ctx);
    TCGv_i32 base = c166_gen_read_gpr(ctx, a->src);
    TCGv_i32 effective = tcg_temp_new_i32();
    TCGv_i32 value = tcg_temp_new_i32();
    TCGv_i32 addr;

    tcg_gen_addi_i32(effective, base, offset);
    tcg_gen_andi_i32(effective, effective, 0xffff);
    addr = c166_gen_resolve_data_dynamic(effective);
    c166_gen_data_load(ctx, value, addr, MO_LEUW);
    c166_gen_write_gpr(ctx, a->dst, value);
    c166_gen_mov_flags(value);
    return true;
}

static bool trans_MOVB_idx_store(DisasContext *ctx, arg_MOVB_idx_store *a)
{
    uint16_t offset = c166_fetch_u16(ctx);
    TCGv_i32 base = c166_gen_read_gpr(ctx, a->src);
    TCGv_i32 effective = tcg_temp_new_i32();
    TCGv_i32 addr;
    TCGv_i32 value = c166_gen_read_gpr_byte(ctx, a->dst);

    tcg_gen_addi_i32(effective, base, offset);
    tcg_gen_andi_i32(effective, effective, 0xffff);
    addr = c166_gen_resolve_data_dynamic(effective);
    c166_gen_data_store(ctx, value, addr, MO_UB);
    c166_gen_movb_flags(value);
    return true;
}

static bool trans_MOVB_idx_load(DisasContext *ctx, arg_MOVB_idx_load *a)
{
    translator_io_start(&ctx->base);
    c166_gen_probe_reg(ctx, 0xf0 | a->dst, 8, true);

    uint16_t offset = c166_fetch_u16(ctx);
    TCGv_i32 base = c166_gen_read_gpr(ctx, a->src);
    TCGv_i32 effective = tcg_temp_new_i32();
    TCGv_i32 value = tcg_temp_new_i32();
    TCGv_i32 addr;

    tcg_gen_addi_i32(effective, base, offset);
    tcg_gen_andi_i32(effective, effective, 0xffff);
    addr = c166_gen_resolve_data_dynamic(effective);
    c166_gen_data_load(ctx, value, addr, MO_UB);
    c166_gen_write_gpr_byte(ctx, a->dst, value);
    c166_gen_movb_flags(value);
    return true;
}

static void c166_gen_indirect_direct(DisasContext *ctx, unsigned reg,
                                     bool byte, bool memory_destination)
{
    translator_io_start(&ctx->base);

    uint16_t direct = c166_fetch_u16(ctx);
    TCGv_i32 indirect = c166_gen_resolve_data_dynamic(
        c166_gen_read_gpr(ctx, reg));
    TCGv_i32 direct_addr = c166_gen_resolve_data(direct);
    TCGv_i32 value = tcg_temp_new_i32();
    MemOp memop = byte ? MO_UB : MO_LEUW;

    c166_gen_probe(memory_destination ? direct_addr : indirect,
                    byte ? 1 : 2, true);
    c166_gen_data_load(ctx, value,
                        memory_destination ? indirect : direct_addr, memop);
    c166_gen_data_store(ctx, value,
                        memory_destination ? direct_addr : indirect, memop);
    if (byte) {
        c166_gen_movb_flags(value);
    } else {
        c166_gen_mov_flags(value);
    }
}

static bool trans_MOV_ind_mem(DisasContext *ctx, arg_MOV_ind_mem *a)
{
    c166_gen_indirect_direct(ctx, a->reg, false, false);
    return true;
}

static bool trans_MOV_mem_ind(DisasContext *ctx, arg_MOV_mem_ind *a)
{
    c166_gen_indirect_direct(ctx, a->reg, false, true);
    return true;
}

static bool trans_MOVB_ind_mem(DisasContext *ctx, arg_MOVB_ind_mem *a)
{
    c166_gen_indirect_direct(ctx, a->reg, true, false);
    return true;
}

static bool trans_MOVB_mem_ind(DisasContext *ctx, arg_MOVB_mem_ind *a)
{
    c166_gen_indirect_direct(ctx, a->reg, true, true);
    return true;
}

static void c166_gen_indirect_load(DisasContext *ctx, unsigned dst,
                                   unsigned src, bool byte, unsigned increment)
{
    translator_io_start(&ctx->base);
    c166_gen_probe_reg(ctx, 0xf0 | dst, byte ? 8 : 16, true);
    if (increment) {
        c166_gen_probe_reg(ctx, 0xf0 | src, 16, true);
    }

    TCGv_i32 offset = c166_gen_read_gpr(ctx, src);
    TCGv_i32 addr = c166_gen_resolve_data_dynamic(offset);
    TCGv_i32 value = tcg_temp_new_i32();

    c166_gen_data_load(ctx, value, addr, byte ? MO_UB : MO_LEUW);
    if (byte) {
        c166_gen_write_gpr_byte(ctx, dst, value);
        c166_gen_movb_flags(value);
    } else {
        c166_gen_write_gpr(ctx, dst, value);
        c166_gen_mov_flags(value);
    }
    if (increment) {
        tcg_gen_addi_i32(offset, offset, increment);
        tcg_gen_andi_i32(offset, offset, 0xffff);
        c166_gen_write_gpr(ctx, src, offset);
    }
}

static void c166_gen_indirect_store(DisasContext *ctx, unsigned src,
                                    unsigned dst, bool byte, unsigned decrement)
{
    translator_io_start(&ctx->base);
    if (decrement) {
        c166_gen_probe_reg(ctx, 0xf0 | dst, 16, true);
    }

    TCGv_i32 offset = c166_gen_read_gpr(ctx, dst);
    TCGv_i32 value;
    TCGv_i32 addr;

    if (decrement) {
        tcg_gen_subi_i32(offset, offset, decrement);
        tcg_gen_andi_i32(offset, offset, 0xffff);
    }
    value = byte ? c166_gen_read_gpr_byte(ctx, src) :
                   c166_gen_read_gpr(ctx, src);
    addr = c166_gen_resolve_data_dynamic(offset);
    c166_gen_data_store(ctx, value, addr, byte ? MO_UB : MO_LEUW);
    if (decrement) {
        c166_gen_write_gpr(ctx, dst, offset);
    }
    if (byte) {
        c166_gen_movb_flags(value);
    } else {
        c166_gen_mov_flags(value);
    }
}

static bool trans_MOV_ind_load(DisasContext *ctx, arg_MOV_ind_load *a)
{
    c166_gen_indirect_load(ctx, a->dst, a->src, false, 0);
    return true;
}

static bool trans_MOV_post_load(DisasContext *ctx, arg_MOV_post_load *a)
{
    c166_gen_indirect_load(ctx, a->dst, a->src, false, 2);
    return true;
}

static bool trans_MOV_ind_store(DisasContext *ctx, arg_MOV_ind_store *a)
{
    c166_gen_indirect_store(ctx, a->dst, a->src, false, 0);
    return true;
}

static bool trans_MOV_pre_store(DisasContext *ctx, arg_MOV_pre_store *a)
{
    c166_gen_indirect_store(ctx, a->dst, a->src, false, 2);
    return true;
}

static void c166_gen_memory_move(DisasContext *ctx, unsigned dst, unsigned src,
                                 bool byte, bool increment_dst,
                                 bool increment_src)
{
    translator_io_start(&ctx->base);
    if (increment_dst || increment_src) {
        c166_gen_probe_reg(ctx, 0xf0 | (increment_dst ? dst : src), 16, true);
    }

    TCGv_i32 dst_offset = c166_gen_read_gpr(ctx, dst);
    TCGv_i32 src_offset = c166_gen_read_gpr(ctx, src);
    TCGv_i32 dst_addr = c166_gen_resolve_data_dynamic(dst_offset);
    TCGv_i32 src_addr = c166_gen_resolve_data_dynamic(src_offset);
    TCGv_i32 value = tcg_temp_new_i32();

    c166_gen_probe(dst_addr, byte ? 1 : 2, true);
    c166_gen_data_load(ctx, value, src_addr, byte ? MO_UB : MO_LEUW);
    c166_gen_data_store(ctx, value, dst_addr, byte ? MO_UB : MO_LEUW);
    if (byte) {
        c166_gen_movb_flags(value);
    } else {
        c166_gen_mov_flags(value);
    }
    if (increment_dst || increment_src) {
        unsigned reg = increment_dst ? dst : src;
        TCGv_i32 offset = increment_dst ? dst_offset : src_offset;

        tcg_gen_addi_i32(offset, offset, byte ? 1 : 2);
        tcg_gen_andi_i32(offset, offset, 0xffff);
        c166_gen_write_gpr(ctx, reg, offset);
    }
}

static bool trans_MOV_mem_mem(DisasContext *ctx, arg_MOV_mem_mem *a)
{
    c166_gen_memory_move(ctx, a->dst, a->src, false, false, false);
    return true;
}

static bool trans_MOV_mem_post(DisasContext *ctx, arg_MOV_mem_post *a)
{
    c166_gen_memory_move(ctx, a->dst, a->src, false, true, false);
    return true;
}

static bool trans_MOV_post_mem(DisasContext *ctx, arg_MOV_post_mem *a)
{
    c166_gen_memory_move(ctx, a->dst, a->src, false, false, true);
    return true;
}

static bool trans_MOVB_ind_load(DisasContext *ctx, arg_MOVB_ind_load *a)
{
    c166_gen_indirect_load(ctx, a->dst, a->src, true, 0);
    return true;
}

static bool trans_MOVB_post_load(DisasContext *ctx, arg_MOVB_post_load *a)
{
    c166_gen_indirect_load(ctx, a->dst, a->src, true, 1);
    return true;
}

static bool trans_MOVB_ind_store(DisasContext *ctx, arg_MOVB_ind_store *a)
{
    c166_gen_indirect_store(ctx, a->dst, a->src, true, 0);
    return true;
}

static bool trans_MOVB_pre_store(DisasContext *ctx, arg_MOVB_pre_store *a)
{
    c166_gen_indirect_store(ctx, a->dst, a->src, true, 1);
    return true;
}

static bool trans_MOVB_mem_mem(DisasContext *ctx, arg_MOVB_mem_mem *a)
{
    c166_gen_memory_move(ctx, a->dst, a->src, true, false, false);
    return true;
}

static bool trans_MOVB_mem_post(DisasContext *ctx, arg_MOVB_mem_post *a)
{
    c166_gen_memory_move(ctx, a->dst, a->src, true, true, false);
    return true;
}

static bool trans_MOVB_post_mem(DisasContext *ctx, arg_MOVB_post_mem *a)
{
    c166_gen_memory_move(ctx, a->dst, a->src, true, false, true);
    return true;
}

static void c166_gen_movx_rr(DisasContext *ctx, unsigned dst, unsigned src,
                             bool sign_extend)
{
    TCGv_i32 value = c166_gen_read_gpr_byte(ctx, src);
    TCGv_i32 result = tcg_temp_new_i32();

    if (sign_extend) {
        tcg_gen_ext8s_i32(result, value);
        tcg_gen_andi_i32(result, result, 0xffff);
    } else {
        tcg_gen_mov_i32(result, value);
    }
    c166_gen_write_gpr(ctx, dst, result);
    c166_gen_movzx_flags(value, sign_extend);
}

static bool trans_MOVBZ_rr(DisasContext *ctx, arg_MOVBZ_rr *a)
{
    c166_gen_movx_rr(ctx, a->src, a->dst, false);
    return true;
}

static bool trans_MOVBS_rr(DisasContext *ctx, arg_MOVBS_rr *a)
{
    c166_gen_movx_rr(ctx, a->src, a->dst, true);
    return true;
}

static void c166_gen_movx_rm16(DisasContext *ctx, unsigned reg,
                               bool sign_extend)
{
    translator_io_start(&ctx->base);
    c166_gen_probe_reg(ctx, reg, 16, true);

    uint16_t offset = c166_fetch_u16(ctx);
    TCGv_i32 addr = c166_gen_resolve_data(offset);
    TCGv_i32 value = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();

    c166_gen_data_load(ctx, value, addr, MO_UB);
    if (sign_extend) {
        tcg_gen_ext8s_i32(result, value);
        tcg_gen_andi_i32(result, result, 0xffff);
    } else {
        tcg_gen_mov_i32(result, value);
    }
    c166_gen_write_reg(ctx, reg, result);
    c166_gen_movzx_flags(value, sign_extend);
}

static bool trans_MOVBZ_rm16(DisasContext *ctx, arg_MOVBZ_rm16 *a)
{
    c166_gen_movx_rm16(ctx, a->reg, false);
    return true;
}

static bool trans_MOVBS_rm16(DisasContext *ctx, arg_MOVBS_rm16 *a)
{
    c166_gen_movx_rm16(ctx, a->reg, true);
    return true;
}

static void c166_gen_movx_mr16(DisasContext *ctx, unsigned reg,
                               bool sign_extend)
{
    translator_io_start(&ctx->base);

    uint16_t offset = c166_fetch_u16(ctx);
    TCGv_i32 addr = c166_gen_resolve_data(offset);
    c166_gen_probe(addr, 2, true);
    TCGv_i32 value = c166_gen_read_reg_byte(ctx, reg);
    TCGv_i32 result = tcg_temp_new_i32();

    if (sign_extend) {
        tcg_gen_ext8s_i32(result, value);
        tcg_gen_andi_i32(result, result, 0xffff);
    } else {
        tcg_gen_mov_i32(result, value);
    }
    c166_gen_data_store(ctx, result, addr, MO_LEUW);
    c166_gen_movzx_flags(value, sign_extend);
}

static bool trans_MOVBZ_mr16(DisasContext *ctx, arg_MOVBZ_mr16 *a)
{
    c166_gen_movx_mr16(ctx, a->reg, false);
    return true;
}

static bool trans_MOVBS_mr16(DisasContext *ctx, arg_MOVBS_mr16 *a)
{
    c166_gen_movx_mr16(ctx, a->reg, true);
    return true;
}

static bool trans_JMPR(DisasContext *ctx, arg_JMPR *a)
{
    uint32_t fallthrough = ctx->base.pc_next & C166_ADDR_MASK;
    uint32_t dest = (fallthrough & 0xff0000) |
                    ((fallthrough + a->rel * 2) & 0xffff);
    TCGv_i32 condition = c166_gen_condition(a->cc);

    c166_gen_conditional_goto(ctx, condition, dest, fallthrough);
    return true;
}

static bool trans_JMPA(DisasContext *ctx, arg_JMPA *a)
{
    uint16_t ip = c166_fetch_u16(ctx);
    uint32_t fallthrough = ctx->base.pc_next & C166_ADDR_MASK;
    uint32_t dest = (fallthrough & 0xff0000) | ip;
    TCGv_i32 condition = c166_gen_condition(a->cc);

    c166_gen_conditional_goto(ctx, condition, dest, fallthrough);
    return true;
}

static bool trans_CALLA(DisasContext *ctx, arg_CALLA *a)
{
    c166_gen_stack_transfer_begin(ctx);
    uint16_t ip = c166_fetch_u16(ctx);
    uint32_t fallthrough = ctx->base.pc_next & C166_ADDR_MASK;
    uint32_t dest = (fallthrough & 0xff0000) | ip;
    TCGv_i32 condition = c166_gen_condition(a->cc);
    TCGLabel *not_taken = gen_new_label();

    TCGLabel *probed = gen_new_label();
    tcg_gen_brcondi_i32(TCG_COND_EQ, condition, 0, probed);
    c166_gen_probe_stack(-2, 1, true);
    gen_set_label(probed);
    c166_gen_window_finish(ctx);
    tcg_gen_brcondi_i32(TCG_COND_EQ, condition, 0, not_taken);
    c166_gen_push(ctx, tcg_constant_i32(fallthrough & 0xffff));
    c166_gen_stack_transfer_end(ctx, tcg_constant_i32(dest), true);
    gen_set_label(not_taken);
    c166_gen_goto_tb(ctx, 1, fallthrough);
    return true;
}

static bool trans_CALLR(DisasContext *ctx, arg_CALLR *a)
{
    c166_gen_stack_transfer_begin(ctx);
    uint32_t fallthrough = ctx->base.pc_next & C166_ADDR_MASK;
    uint32_t dest = (fallthrough & 0xff0000) |
                    ((fallthrough + a->rel * 2) & 0xffff);

    c166_gen_probe_stack(-2, 1, true);
    c166_gen_push(ctx, tcg_constant_i32(fallthrough & 0xffff));
    c166_gen_stack_transfer_end(ctx, tcg_constant_i32(dest), true);
    return true;
}

static bool trans_PCALL(DisasContext *ctx, arg_PCALL *a)
{
    c166_gen_stack_transfer_begin(ctx);

    c166_gen_probe_stack(-2, 2, true);
    c166_gen_probe_reg(ctx, a->reg, 16, false);

    uint16_t ip = c166_fetch_u16(ctx);
    uint32_t fallthrough = ctx->base.pc_next & C166_ADDR_MASK;
    uint32_t dest = (fallthrough & 0xff0000) | ip;
    TCGv_i32 value = c166_gen_read_reg(ctx, a->reg);

    c166_gen_push(ctx, value);
    c166_gen_mov_flags(value);
    c166_gen_push(ctx, tcg_constant_i32(fallthrough & 0xffff));
    c166_gen_stack_transfer_end(ctx, tcg_constant_i32(dest), true);
    return true;
}

static bool trans_JMPS(DisasContext *ctx, arg_JMPS *a)
{
    uint16_t addr = c166_fetch_u16(ctx);

    c166_gen_goto_tb(ctx, 0, ((uint32_t)a->seg << 16) | addr);
    return true;
}

static bool trans_CALLS(DisasContext *ctx, arg_CALLS *a)
{
    c166_gen_stack_transfer_begin(ctx);

    c166_gen_probe_stack(-2, 2, true);

    uint16_t addr = c166_fetch_u16(ctx);
    uint32_t fallthrough = ctx->base.pc_next & C166_ADDR_MASK;

    c166_gen_push(ctx, cpu_csp);
    c166_gen_push(ctx, tcg_constant_i32(fallthrough & 0xffff));
    c166_gen_stack_transfer_end(ctx,
                                tcg_constant_i32((a->seg << 16) | addr), true);
    return true;
}

static void c166_gen_indirect_jump(DisasContext *ctx, unsigned cc,
                                   unsigned reg, bool call)
{
    if (call) {
        c166_gen_stack_transfer_begin(ctx);
    }
    uint32_t fallthrough = ctx->base.pc_next & C166_ADDR_MASK;
    TCGv_i32 condition = c166_gen_condition(cc);
    TCGLabel *not_taken = gen_new_label();
    TCGv_i32 dest;

    TCGLabel *probed = gen_new_label();
    tcg_gen_brcondi_i32(TCG_COND_EQ, condition, 0, probed);
    c166_gen_probe_reg(ctx, 0xf0 | reg, 16, false);
    if (call) {
        c166_gen_probe_stack(-2, 1, true);
    }
    gen_set_label(probed);
    c166_gen_window_finish(ctx);
    tcg_gen_brcondi_i32(TCG_COND_EQ, condition, 0, not_taken);
    if (call) {
        c166_gen_push(ctx, tcg_constant_i32(fallthrough & 0xffff));
    }
    dest = c166_gen_read_gpr(ctx, reg);
    if (call) {
        c166_gen_stack_transfer_end(ctx,
                                    c166_gen_segmented_pc(dest), true);
    } else {
        c166_gen_lookup_ip(ctx, dest);
    }
    gen_set_label(not_taken);
    c166_gen_goto_tb(ctx, 1, fallthrough);
}

static bool trans_JMPI(DisasContext *ctx, arg_JMPI *a)
{
    c166_gen_indirect_jump(ctx, a->cc, a->reg, false);
    return true;
}

static bool trans_CALLI(DisasContext *ctx, arg_CALLI *a)
{
    c166_gen_indirect_jump(ctx, a->cc, a->reg, true);
    return true;
}

static bool trans_RET(DisasContext *ctx, arg_RET *a)
{
    c166_gen_stack_transfer_begin(ctx);

    c166_gen_probe_stack(0, 1, false);

    TCGv_i32 ip = c166_gen_pop(ctx);

    c166_gen_stack_transfer_end(ctx, c166_gen_segmented_pc(ip), false);
    return true;
}

static bool trans_RETP(DisasContext *ctx, arg_RETP *a)
{
    c166_gen_stack_transfer_begin(ctx);

    c166_gen_probe_reg(ctx, a->reg, 16, true);

    c166_gen_probe_stack(0, 2, false);

    TCGv_i32 ip = c166_gen_pop(ctx);
    TCGv_i32 value = c166_gen_pop(ctx);

    c166_gen_write_reg(ctx, a->reg, value);
    c166_gen_mov_flags(value);
    c166_gen_stack_transfer_end(ctx, c166_gen_segmented_pc(ip), false);
    return true;
}

static bool trans_RETS(DisasContext *ctx, arg_RETS *a)
{
    c166_gen_stack_transfer_begin(ctx);

    c166_gen_probe_stack(0, 2, false);

    TCGv_i32 ip = c166_gen_pop(ctx);
    TCGv_i32 csp = c166_gen_pop(ctx);

    tcg_gen_andi_i32(cpu_csp, csp, 0xff);
    c166_gen_stack_transfer_end(ctx, c166_gen_segmented_pc(ip), false);
    return true;
}

static bool trans_RETI(DisasContext *ctx, arg_RETI *a)
{
    c166_gen_stack_transfer_begin(ctx);

    c166_gen_probe_stack(0, 2, false);
    TCGLabel *probed = gen_new_label();
    TCGv_i32 segmented = tcg_temp_new_i32();
    tcg_gen_andi_i32(segmented, cpu_syscon, 0x0800);
    tcg_gen_brcondi_i32(TCG_COND_NE, segmented, 0, probed);
    c166_gen_probe_stack(4, 1, false);
    gen_set_label(probed);

    TCGv_i32 ip = c166_gen_pop(ctx);
    TCGv_i32 psw;

    c166_gen_pop_csp_if_segmented(ctx);
    psw = c166_gen_pop(ctx);
    tcg_gen_ext16u_i32(cpu_psw, psw);
    gen_helper_c166_trap_return(tcg_env);
    /* Restored PSW can admit an IRQ already pending before RETI. */
    ctx->irq_exit = true;
    c166_gen_stack_transfer_end(ctx, c166_gen_segmented_pc(ip), false);
    return true;
}

static bool trans_PUSH(DisasContext *ctx, arg_PUSH *a)
{
    translator_io_start(&ctx->base);

    c166_gen_probe_stack(-2, 1, true);

    TCGv_i32 value = c166_gen_read_reg(ctx, a->reg);

    c166_gen_push(ctx, value);
    c166_gen_mov_flags(value);
    c166_gen_stack_bound_check(ctx, true);
    return true;
}

static bool trans_POP(DisasContext *ctx, arg_POP *a)
{
    translator_io_start(&ctx->base);

    c166_gen_probe_reg(ctx, a->reg, 16, true);
    c166_gen_probe_stack(0, 1, false);

    TCGv_i32 value = c166_gen_pop(ctx);

    c166_gen_write_reg(ctx, a->reg, value);
    c166_gen_mov_flags(value);
    c166_gen_stack_bound_check(ctx, false);
    return true;
}

void c166_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    C166CPUState *env = cpu_env(cs);

    qemu_fprintf(f, "PC=%06x CSP=%02x IP=%04x PSW=%04x CP=%04x SP=%04x ",
                 c166_env_get_pc(env), env->csp, env->ip, env->psw,
                 env->cp, env->sp);
    qemu_fprintf(f, "DPP=%03x/%03x/%03x/%03x ICOUNT=%" PRIu64 "\n",
                 env->dpp[0], env->dpp[1], env->dpp[2], env->dpp[3],
                 env->guest_icount);
    qemu_fprintf(f, "MDH=%04x MDL=%04x MDC=%04x TFR=%04x SYSCON=%04x ",
                 env->mdh, env->mdl, env->mdc, env->tfr, env->syscon);
    qemu_fprintf(f, "EXT=%u:%03x/%u EXTR=%u IDLE=%u\n",
                 env->ext_kind, env->ext_value, env->ext_count,
                 env->extr, env->idle);
    for (unsigned i = 0; i < 16; i += 4) {
        qemu_fprintf(f, "R%-2u=%04x R%-2u=%04x R%-2u=%04x R%-2u=%04x\n",
                     i, c166_cpu_read_gpr(cs, i),
                     i + 1, c166_cpu_read_gpr(cs, i + 1),
                     i + 2, c166_cpu_read_gpr(cs, i + 2),
                     i + 3, c166_cpu_read_gpr(cs, i + 3));
    }
}

static void c166_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    ctx->env = cpu_env(cs);
    ctx->watched = ctx->base.tb->flags & C166_TB_WATCH;
    ctx->window_count = (ctx->base.tb->flags >> 3) & 7;
    ctx->window_reload = false;
    ctx->window_finished = false;
    ctx->window_exit = false;
    ctx->irq_exit = false;
    if (ctx->window_count) {
        ctx->base.max_insns = 1;
    }
}

static void c166_tr_tb_start(DisasContextBase *dcbase, CPUState *cs)
{
}

static void c166_tr_insn_start(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    tcg_gen_insn_start(ctx->base.pc_next, 0, 0);
}

static void c166_tr_translate_insn(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    uint16_t insn = c166_fetch_u16(ctx);

    ctx->insn_pc = ctx->base.pc_next - 2;
    ctx->opcode = insn;
    ctx->stack_check = false;
    ctx->boundary_finished = false;
    ctx->trap_resume = ctx->watched ? gen_new_label() : NULL;

    tcg_gen_mov_i64(cpu_insn_icount, cpu_guest_icount);
    tcg_gen_addi_i64(cpu_guest_icount, cpu_guest_icount, 1);
    if (trace_event_get_state_backends(TRACE_C166_ENTRY)) {
        gen_helper_c166_trace_entry(tcg_env, tcg_constant_i32(ctx->insn_pc));
    }
    if (ctx->watched) {
        TCGv_i32 pending = tcg_temp_new_i32();
        tcg_gen_ld_i32(pending, tcg_env,
                       offsetof(C166CPUState, trap_retire_pending));
        tcg_gen_brcondi_i32(TCG_COND_NE, pending, 0, ctx->trap_resume);
    }
    if (!decode(ctx, insn)) {
        qemu_log_mask(LOG_UNIMP, "C166: unsupported opcode %04x at %06x\n",
                      insn, (unsigned)(ctx->base.pc_next - 2));
        c166_gen_undefined_trap(ctx);
    }
    if (ctx->base.is_jmp != DISAS_NORETURN && !ctx->boundary_finished) {
        c166_gen_trap_boundary(ctx,
            tcg_constant_i32(ctx->base.pc_next & C166_ADDR_MASK));
        ctx->boundary_finished = true;
    }
    if (ctx->watched) {
        TCGLabel *done = gen_new_label();
        tcg_gen_br(done);
        gen_set_label(ctx->trap_resume);
        gen_helper_c166_trap_resume(tcg_env);
        tcg_gen_lookup_and_goto_ptr();
        gen_set_label(done);
    }
    /*
     * The generic breakpoint dispatcher checks the TB's starting page.
     * Do not translate another instruction on the following page: a timer
     * exit can otherwise shift a long NOP TB across a pending breakpoint.
     * A four-byte instruction may still fetch its operand across the boundary.
     */
    if (ctx->base.is_jmp == DISAS_NEXT &&
        !translator_is_same_page(&ctx->base, ctx->base.pc_next)) {
        ctx->base.is_jmp = DISAS_TOO_MANY;
    }
}

static void c166_tr_tb_stop(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    switch (ctx->base.is_jmp) {
    case DISAS_NEXT:
    case DISAS_TOO_MANY:
        c166_gen_goto_tb(ctx, 0, ctx->base.pc_next & C166_ADDR_MASK);
        break;
    case DISAS_EXIT:
        tcg_gen_exit_tb(NULL, 0);
        break;
    case DISAS_NORETURN:
        break;
    default:
        g_assert_not_reached();
    }
}

static const TranslatorOps c166_tr_ops = {
    .init_disas_context = c166_tr_init_disas_context,
    .tb_start = c166_tr_tb_start,
    .insn_start = c166_tr_insn_start,
    .translate_insn = c166_tr_translate_insn,
    .tb_stop = c166_tr_tb_stop,
};

void c166_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext dc;

    translator_loop(cs, tb, max_insns, pc, host_pc, &c166_tr_ops, &dc.base,
                    TCG_TYPE_VA);
}

#define C166_TCG_GLOBAL(field, name) \
    tcg_global_mem_new_i32(tcg_env, offsetof(C166CPUState, field), name)

void c166_translate_init(void)
{
    cpu_ip = C166_TCG_GLOBAL(ip, "IP");
    cpu_csp = C166_TCG_GLOBAL(csp, "CSP");
    for (unsigned i = 0; i < 4; i++) {
        cpu_dpp[i] = tcg_global_mem_new_i32(tcg_env,
                            offsetof(C166CPUState, dpp[i]), "DPP");
    }
    cpu_cp = C166_TCG_GLOBAL(cp, "CP");
    cpu_sp = C166_TCG_GLOBAL(sp, "SP");
    cpu_stkov = C166_TCG_GLOBAL(stkov, "STKOV");
    cpu_stkun = C166_TCG_GLOBAL(stkun, "STKUN");
    cpu_psw = C166_TCG_GLOBAL(psw, "PSW");
    cpu_mdh = C166_TCG_GLOBAL(mdh, "MDH");
    cpu_mdl = C166_TCG_GLOBAL(mdl, "MDL");
    cpu_mdc = C166_TCG_GLOBAL(mdc, "MDC");
    cpu_tfr = C166_TCG_GLOBAL(tfr, "TFR");
    cpu_syscon = C166_TCG_GLOBAL(syscon, "SYSCON");
    cpu_einit_done = C166_TCG_GLOBAL(einit_done, "EINIT done");
    cpu_ext_kind = C166_TCG_GLOBAL(ext_kind, "EXT kind");
    cpu_ext_value = C166_TCG_GLOBAL(ext_value, "EXT value");
    cpu_ext_count = C166_TCG_GLOBAL(ext_count, "EXT count");
    cpu_extr = C166_TCG_GLOBAL(extr, "EXTR");
    cpu_idle = C166_TCG_GLOBAL(idle, "IDLE");
    cpu_insn_icount = tcg_global_mem_new_i64(tcg_env,
                  offsetof(C166CPUState, insn_icount), "C166 entry icount");
    cpu_guest_icount = tcg_global_mem_new_i64(tcg_env,
                  offsetof(C166CPUState, guest_icount), "C166 guest icount");
    cpu_dpram = tcg_global_mem_new_ptr(tcg_env,
                  offsetof(C166CPUState, dpram), "C166 DPRAM");
}
