/*
 * GDB remote register access for C166S.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/cpu-common.h"
#include "gdbstub/helpers.h"

uint16_t c166_cpu_read_gpr(CPUState *cs, unsigned reg)
{
    C166CPUState *env = cpu_env(cs);
    uint8_t bytes[2] = { 0, 0 };
    vaddr addr = (env->cp + 2 * (reg & 15)) & 0xffff;

    if (cpu_memory_rw_debug(cs, addr, bytes, sizeof(bytes), false) != 0) {
        return 0;
    }
    return lduw_le_p(bytes);
}

void c166_cpu_write_gpr(CPUState *cs, unsigned reg, uint16_t value)
{
    C166CPUState *env = cpu_env(cs);
    uint8_t bytes[2];
    vaddr addr = (env->cp + 2 * (reg & 15)) & 0xffff;

    stw_le_p(bytes, value);
    cpu_memory_rw_debug(cs, addr, bytes, sizeof(bytes), true);
}

int c166_cpu_gdb_read_register(CPUState *cs, GByteArray *buf, int reg)
{
    C166CPUState *env = cpu_env(cs);

    if (reg >= 0 && reg < 16) {
        return gdb_get_reg16(buf, c166_cpu_read_gpr(cs, reg));
    }
    switch (reg) {
    case 16 ... 19:
        return gdb_get_reg16(buf, env->dpp[reg - 16]);
    case 20:
        return gdb_get_reg16(buf, env->cp);
    case 21:
        return gdb_get_reg16(buf, env->sp);
    case 22:
        return gdb_get_reg16(buf, env->stkov);
    case 23:
        return gdb_get_reg16(buf, env->stkun);
    case 24:
        return gdb_get_reg16(buf, env->mdh);
    case 25:
        return gdb_get_reg16(buf, env->mdl);
    case 26:
        return gdb_get_reg16(buf, env->mdc);
    case 27:
        return gdb_get_reg16(buf, env->psw);
    case 28:
        return gdb_get_reg16(buf, env->csp);
    case 29:
        return gdb_get_reg16(buf, env->ip);
    case 30:
        return gdb_get_reg32(buf, c166_env_get_pc(env));
    case 31:
        return gdb_get_reg16(buf, env->ext_kind);
    case 32:
        return gdb_get_reg16(buf, env->ext_value);
    case 33:
        return gdb_get_reg16(buf, env->ext_count);
    case 34:
        return gdb_get_reg16(buf, env->extr);
    case 35:
        return gdb_get_reg64(buf, env->guest_icount);
    case 36:
        return gdb_get_reg16(buf, env->tfr);
    case 37:
        return gdb_get_reg16(buf, env->syscon);
    case 38:
        return gdb_get_reg16(buf, env->idle);
    default:
        return 0;
    }
}

int c166_cpu_gdb_write_register(CPUState *cs, uint8_t *buf, int reg)
{
    C166CPUState *env = cpu_env(cs);
    uint16_t value = lduw_le_p(buf);

    if (reg >= 0 && reg < 16) {
        c166_cpu_write_gpr(cs, reg, value);
        return 2;
    }
    switch (reg) {
    case 16 ... 19:
        env->dpp[reg - 16] = value & 0x3ff;
        return 2;
    case 20:
        env->cp = value & 0xfffe;
        return 2;
    case 21:
        env->sp = value & 0xfffe;
        return 2;
    case 22:
        env->stkov = value & 0xfffe;
        return 2;
    case 23:
        env->stkun = value & 0xfffe;
        return 2;
    case 24:
        env->mdh = value;
        return 2;
    case 25:
        env->mdl = value;
        return 2;
    case 26:
        env->mdc = value;
        return 2;
    case 27:
        env->psw = value;
        return 2;
    case 28:
        env->csp = value & 0xff;
        return 2;
    case 29:
        env->ip = value;
        return 2;
    case 30:
        c166_env_set_pc(env, ldl_le_p(buf));
        return 4;
    case 31:
        env->ext_kind = value;
        return 2;
    case 32:
        env->ext_value = value;
        return 2;
    case 33:
        env->ext_count = value;
        return 2;
    case 34:
        env->extr = value & 1;
        return 2;
    case 35:
        env->guest_icount = ldq_le_p(buf);
        return 8;
    case 36:
        env->tfr = value;
        return 2;
    case 37:
        /*
         * Debugger overrides remain valid after EINIT, but must notify the
         * board just like accepted CSFR writes so its mappings stay coherent.
         */
        env->syscon = value;
        if (env->syscon_write) {
            env->syscon_write(env->syscon_opaque);
        }
        return 2;
    case 38:
        env->idle = value & 1;
        return 2;
    default:
        return 0;
    }
}
