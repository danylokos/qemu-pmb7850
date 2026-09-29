/*
 * Minimal C166 disassembler, extended alongside decodetree coverage.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "disas/dis-asm.h"
#include "cpu.h"

/* C166S V1 manual, PDF pp.82-85: byte indices alias halves of R0..R7. */
static const char *const byte_regs[16] = {
    "rl0", "rh0", "rl1", "rh1", "rl2", "rh2", "rl3", "rh3",
    "rl4", "rh4", "rl5", "rh5", "rl6", "rh6", "rl7", "rh7",
};

int c166_print_insn(bfd_vma addr, disassemble_info *info)
{
    static const char * const cc_name[16] = {
        "uc", "net", "eq", "ne", "v", "nv", "n", "nn",
        "c", "nc", "sgt", "sle", "slt", "sge", "ugt", "ule",
    };
    bfd_byte bytes[4];
    uint8_t op;

    if (info->read_memory_func(addr, bytes, 2, info) != 0) {
        return -1;
    }
    op = bytes[0];
    switch (op) {
    case 0xcc:
        info->fprintf_func(info->stream, "nop");
        return 2;
    case 0xe0:
        info->fprintf_func(info->stream, "mov r%u, #0x%x",
                           bytes[1] & 15, bytes[1] >> 4);
        return 2;
    case 0xe1:
        info->fprintf_func(info->stream, "movb %s, #0x%x",
                           byte_regs[bytes[1] & 15], bytes[1] >> 4);
        return 2;
    case 0xf0:
        info->fprintf_func(info->stream, "mov r%u, r%u",
                           bytes[1] >> 4, bytes[1] & 15);
        return 2;
    case 0xf1:
        info->fprintf_func(info->stream, "movb %s, %s",
                           byte_regs[bytes[1] >> 4], byte_regs[bytes[1] & 15]);
        return 2;
    case 0x00: case 0x10: case 0x20: case 0x30:
    case 0x40: case 0x50: case 0x60: case 0x70: {
        static const char * const alu_name[8] = {
            "add", "addc", "sub", "subc",
            "cmp", "xor", "and", "or",
        };

        info->fprintf_func(info->stream, "%s r%u, r%u",
                           alu_name[op >> 4], bytes[1] >> 4,
                           bytes[1] & 15);
        return 2;
    }
    case 0x0c: case 0x1c: case 0x2c: case 0x3c:
    case 0x4c: case 0x5c: case 0x6c: case 0x7c:
    case 0xac: case 0xbc: {
        static const char * const shift_name[5] = {
            "rol", "ror", "shl", "shr", "ashr",
        };
        unsigned kind = op < 0x80 ? op >> 5 : 4;

        if (op & 0x10) {
            info->fprintf_func(info->stream, "%s r%u, #%u",
                               shift_name[kind], bytes[1] & 15,
                               bytes[1] >> 4);
        } else {
            info->fprintf_func(info->stream, "%s r%u, r%u",
                               shift_name[kind], bytes[1] >> 4,
                               bytes[1] & 15);
        }
        return 2;
    }
    case 0xd1:
        info->fprintf_func(info->stream, "%s #%u",
                           bytes[1] & 0x10 ? "extr" : "atomic",
                           ((bytes[1] >> 4) & 3) + 1);
        return 2;
    case 0xe6:
    case 0xe7:
    case 0xf2:
    case 0xf3:
    case 0xf6:
    case 0xf7:
        if (info->read_memory_func(addr + 2, bytes + 2, 2, info) != 0) {
            return -1;
        }
        if (op == 0xe6 && bytes[1] >= 0xf0) {
            info->fprintf_func(info->stream, "mov r%u, #0x%04x",
                               bytes[1] & 15, lduw_le_p(bytes + 2));
        } else if (op == 0xf2 && bytes[1] >= 0xf0) {
            info->fprintf_func(info->stream, "mov r%u, 0x%04x",
                               bytes[1] & 15, lduw_le_p(bytes + 2));
        } else if (op == 0xf6 && bytes[1] >= 0xf0) {
            info->fprintf_func(info->stream, "mov 0x%04x, r%u",
                               lduw_le_p(bytes + 2), bytes[1] & 15);
        } else if (op == 0xe7 && bytes[1] >= 0xf0) {
            info->fprintf_func(info->stream, "movb %s, #0x%02x",
                               byte_regs[bytes[1] & 15], bytes[2]);
        } else if (op == 0xf3 && bytes[1] >= 0xf0) {
            info->fprintf_func(info->stream, "movb %s, 0x%04x",
                               byte_regs[bytes[1] & 15], lduw_le_p(bytes + 2));
        } else if (op == 0xf7 && bytes[1] >= 0xf0) {
            info->fprintf_func(info->stream, "movb 0x%04x, %s",
                               lduw_le_p(bytes + 2), byte_regs[bytes[1] & 15]);
        } else {
            info->fprintf_func(info->stream, ".byte 0x%02x, 0x%02x, "
                               "0x%02x, 0x%02x",
                               bytes[0], bytes[1], bytes[2], bytes[3]);
        }
        return 4;
    case 0xc0:
        info->fprintf_func(info->stream, "movbz r%u, %s",
                           bytes[1] & 15, byte_regs[bytes[1] >> 4]);
        return 2;
    case 0xd0:
        info->fprintf_func(info->stream, "movbs r%u, %s",
                           bytes[1] & 15, byte_regs[bytes[1] >> 4]);
        return 2;
    case 0x0d: case 0x1d: case 0x2d: case 0x3d:
    case 0x4d: case 0x5d: case 0x6d: case 0x7d:
    case 0x8d: case 0x9d: case 0xad: case 0xbd:
    case 0xcd: case 0xdd: case 0xed: case 0xfd:
        info->fprintf_func(info->stream, "jmpr cc_%s, %d",
                           cc_name[op >> 4], (int8_t)bytes[1]);
        return 2;
    case 0xea:
    case 0xca:
        if (info->read_memory_func(addr + 2, bytes + 2, 2, info) != 0) {
            return -1;
        }
        info->fprintf_func(info->stream, "%s cc_%s, 0x%04x",
                           op == 0xea ? "jmpa" : "calla",
                           cc_name[bytes[1] >> 4], lduw_le_p(bytes + 2));
        return 4;
    case 0xbb:
        info->fprintf_func(info->stream, "callr %d", (int8_t)bytes[1]);
        return 2;
    case 0xfa:
    case 0xda:
        if (info->read_memory_func(addr + 2, bytes + 2, 2, info) != 0) {
            return -1;
        }
        info->fprintf_func(info->stream, "%s 0x%02x, 0x%04x",
                           op == 0xfa ? "jmps" : "calls", bytes[1],
                           lduw_le_p(bytes + 2));
        return 4;
    case 0x9c:
    case 0xab:
        info->fprintf_func(info->stream, "%s cc_%s, [r%u]",
                           op == 0x9c ? "jmpi" : "calli",
                           cc_name[bytes[1] >> 4], bytes[1] & 15);
        return 2;
    case 0xcb:
        info->fprintf_func(info->stream, "ret");
        return 2;
    case 0xdb:
        info->fprintf_func(info->stream, "rets");
        return 2;
    case 0xfb:
        info->fprintf_func(info->stream, "reti");
        return 2;
    case 0xec:
    case 0xfc:
        info->fprintf_func(info->stream, "%s r%u",
                           op == 0xec ? "push" : "pop", bytes[1] & 15);
        return 2;
    default:
        info->fprintf_func(info->stream, ".byte 0x%02x, 0x%02x",
                           bytes[0], bytes[1]);
        return 2;
    }
}
