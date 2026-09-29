#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""CPU-executed NOR alias mutations must invalidate cached flash instructions.

One uninterrupted guest run calls flash, programs it through another alias,
then erases it and calls it again. No debugger writes or breakpoint changes
occur between those calls. M166 PDF pp.48,53,79,82,90,100 document the fixture
instructions; FF FF is BSET R15.15, allowing execution of erased bytes to be
checked without depending on an undefined opcode.
"""
import os
import tempfile
from pathlib import Path

from test_snapshot import machine, word, physical_effects, nor_write
from test_soc import read_register, write_memory, write_register

MODELS = ('m58lw064d', 'm58-compatible-32mbit', 'am29lv640mh',
          'am29lv128mh', 'w30-64mbit-top', 'w30-128mbit-top')


def store(address, value):
    return ('e6f0' + value.to_bytes(2, 'little').hex() + 'd700' +
            (address >> 16).to_bytes(2, 'little').hex() +
            ('f7f0' if address & 1 else 'f6f0') +
            (address & 0xffff).to_bytes(2, 'little').hex())


def call(address):
    return ('da' + bytes([address >> 16]).hex() +
            (address & 0xffff).to_bytes(2, 'little').hex())


def wait():
    # MOV R2,#60000; ADD R2,#-1; JMPR cc_NZ,-3 (120000 instructions).
    return 'e6f260ea06f2ffff3dfd'


def routing_precedence(r, alias, small):
    selector = 0x800b if small else 0x400a
    address = alias + 0x80000
    assert r.packet(f'm{address:x},2') == 'db00'
    # Each higher-priority programmable CS must hide the NOR alias completely.
    for cs in (2, 4, 1):
        word(r, 0xfe18 + 2 * (cs - 1), selector)
        word(r, 0xff14 + 2 * (cs - 1), 0x400)
        assert r.packet(f'm{address:x},2') == '0000', cs
        word(r, 0xff14 + 2 * (cs - 1), 0)
        assert r.packet(f'm{address:x},2') == 'db00', cs
    # XBUS5 has the same aperture geometry and wins over all chip selects.
    syscon = read_register(r, 'syscon')
    word(r, 0xf01c, selector)
    word(r, 0xf11c, 0x400)
    word(r, 0xf024, 0x10)
    word(r, 0xff12, syscon | 4)
    assert r.packet(f'm{address:x},2') == '0000'
    word(r, 0xf024, 0)
    assert r.packet(f'm{address:x},2') == 'db00'
    # Seed internal memory before introducing the overlapping flash window.
    # Readback then distinguishes internal memory from the erased NOR bytes.
    word(r, 0xff12, syscon | 0x1400)
    word(r, 0x20000, 0x5678)
    word(r, 0xf600, 0x1234)
    word(r, 0xfe1c, 0x000a)
    assert r.packet('m20000,2') == '7856'
    assert r.packet('mf600,2') == '3412'
    word(r, 0xfe1c, selector)
    word(r, 0xff12, syscon)


def observed_reads(r, alias, model):
    address = alias + 0x80000
    amd = model.startswith('am29')
    identity = 1 if amd else 0x89 if model.startswith('w30') else 0x20
    for mode, expected in ((False, 0xdb), (True, identity)):
        if mode:
            if amd:
                nor_write(r, alias + 0xaaa, 0xaa)
                nor_write(r, alias + 0x555, 0x55)
                nor_write(r, alias + 0xaaa, 0x90)
            else:
                nor_write(r, address, 0x90)
        code = ('d700' + (address >> 16).to_bytes(2, 'little').hex() +
                'f2f30000' + 'cc00' * 28)
        write_memory(r, 0xf400, code)
        write_register(r, 'pc', '0000f400')
        r.continue_to(0xf440)
        assert read_register(r, 'r3') == expected
        word(r, 0xfce0, 0)
        word(r, 0xfce2, 0xf600)
        word(r, 0xfed0, address >> 16)
        word(r, 0xfec0, 0x4001)
        word(r, 0xff9a, 0xf8)
        write_memory(r, 0xf400, 'cc00')
        write_register(r, 'pc', '0000f400')
        write_register(r, 'psw', '0800')
        r.continue_to(0xf402)
        assert r.packet('mf600,2') == expected.to_bytes(2, 'little').hex()
        write_register(r, 'psw', '0000')
    nor_write(r, address, 0xf0 if amd else 0xff)


def run_case(binary, root, model, routed):
    small = model == 'm58-compatible-32mbit'
    size = (4 if small else 16 if '128' in model else 8) << 20
    flash = bytearray(b'\xff' * size)
    # MOV R1,#FFFF; RETS straddles a W30 partition / AM29 sector boundary.
    offset = 0x7fffc
    flash[offset:offset + 6] = bytes.fromhex('e6f1ffffdb00')
    path = root / (model + '.bin')
    path.write_bytes(flash)
    amd = model.startswith('am29')
    w30 = model.startswith('w30')
    # CS3 repeats a 4-MiB chip twice across 8 MiB. For other geometries the
    # 4-MiB CS3 aperture is smaller than the chip; native CS0 remains distinct.
    alias = 0x800000 if small else 0x400000
    execute = 0xc00000 if small else 0 if size == 16 << 20 else 0x800000
    address = alias + offset
    code = call(execute + offset) + 'f061'  # save initial result in R6
    code += call(execute + offset)         # reuse the compiled flash code
    if amd:
        for off, value in ((0xaaa, 0xaa), (0x555, 0x55), (0xaaa, 0xa0)):
            code += store(alias + off, value)
    else:
        if w30:
            code += store(address, 0x60) + store(address, 0xd0)
        code += store(address + 2, 0x40)
    code += store(address + 2, 0x1234) + wait()
    code += store(address, 0xf0 if amd else 0xff)
    code += call(execute + offset) + 'f071'  # R7 = changed immediate
    if amd:
        for off, value in ((0xaaa, 0xaa), (0x555, 0x55), (0xaaa, 0x80),
                           (0xaaa, 0xaa), (0x555, 0x55), (offset, 0x30)):
            code += store(alias + off, value)
    else:
        code += store(address, 0x20) + store(address, 0xd0)
    code += wait() + store(address, 0xf0 if amd else 0xff)
    code += 'e6f17856e6ff0000'  # R1=5678, R15=0 before erased BSETs
    code += call(execute + offset) + 'f081'  # R8 must retain 5678
    extra = ['-drive', f'if=pflash,format=raw,file={path}',
             '-global', 'pmb7850-soc.observe-flash-reads=on']
    if amd:
        extra += ['-global', model + '.instruction-timing=on']
    if routed:
        extra += ['-trace', 'enable=pmb7850_bus']
    name = model + ('-routed' if routed else '-direct')
    machine_type = 'pmb7850-test,flash-type=' + model
    with machine(binary, root, name, extra=extra,
                 model=machine_type, shift=5) as (r, q, d):
        word(r, 0xfe1c, 0x800b if small else 0x400a)  # ADDRSEL3
        word(r, 0xff18, 0x0400)  # BUSCON3.BUSACT
        write_memory(r, 0xf400, code + 'cc00')
        write_register(r, 'pc', '0000f400')
        r.sock.settimeout(20)
        r.continue_to(0xf400 + len(code) // 2)
        actual = [read_register(r, n) for n in ('r6', 'r7', 'r8', 'r15')]
        assert actual == [0xffff, 0x1234, 0x5678, 0x8000], (model, actual)
        # The erased code is durable; RETS in the next block is untouched.
        assert path.read_bytes()[offset:offset + 6] == bytes.fromhex(
            'ffffffffdb00')
        routing_precedence(r, alias, small)
        observed_reads(r, alias, model)
        trace = (d / 'trace.log').read_bytes()
        mutations = [line for line in trace.splitlines()
                     if line.startswith(b'x55_nor_mutation ')]
        assert len(mutations) == 2, mutations
        program = b'offset=0x7fffe size=2 operation=program value=0x1234'
        assert program in mutations[0]
        block = 0x20000 if model.startswith('m58') else 0x10000
        expected = (f'offset=0x{offset & ~(block - 1):x} '
                    f'size={block} operation=erase')
        assert expected.encode() in mutations[1], mutations[1]
        reads = []
        for line in trace.splitlines():
            if line.startswith(b'x55_nor_read '):
                fields = dict(item.split(b'=', 1) for item in line.split()[1:])
                reads.append(tuple(fields[key] for key in
                                   (b'offset', b'value', b'array', b'source')))
        identity = b'0x1' if amd else b'0x89' if w30 else b'0x20'
        assert reads == [(b'0x80000', b'0xdb', b'1', b'cpu'),
                         (b'0x80000', b'0xdb', b'1', b'pec'),
                         (b'0x80000', identity, b'0', b'cpu'),
                         (b'0x80000', identity, b'0', b'pec')], reads
        flushes = [line for line in trace.splitlines()
                   if line.startswith(b'x55_nor_flush ')]
        assert len(flushes) == 2, flushes
        return actual, physical_effects(b'\n'.join(mutations)), reads


def main():
    binary = Path(os.environ['QEMU_TEST_QEMU_BINARY']).resolve()
    base = Path(os.environ['MESON_BUILD_ROOT'])
    print('TAP version 13\n1..6', flush=True)
    for i, model in enumerate(MODELS, 1):
        with tempfile.TemporaryDirectory(dir=base) as tmp:
            root = Path(tmp)
            direct = run_case(binary, root, model, False)
            routed = run_case(binary, root, model, True)
            assert direct == routed, (direct, routed)
        print(f'ok {i} - {model} cached program/erase and routed parity',
              flush=True)


if __name__ == '__main__':
    main()
