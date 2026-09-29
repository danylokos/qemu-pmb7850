#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""PMB deadlines under real instruction timing, including IDLE and PEC."""
import os
import re
import subprocess
import time
from pathlib import Path

from run_vectors import Remote, connect
from test_soc import write_memory, write_register, read_register


def word(remote, addr, value):
    write_memory(remote, addr, value.to_bytes(2, 'little').hex())


def read(remote, addr):
    data = bytes.fromhex(remote.packet(f'm{addr:x},2'))
    return int.from_bytes(data, 'little')


def run(remote, count):
    write_register(remote, 'pc', '00002000')
    before = read_register(remote, 'guest_icount')
    remote.continue_to(0x2000 + 2 * count)
    assert read_register(remote, 'guest_icount') - before == count


def disabled(remote):
    write_memory(remote, 0x2000, 'cc00' * 4097)
    word(remote, 0xfe50, 0xfffe)
    word(remote, 0xfe54, 0xfff0)
    word(remote, 0xfe80, 0xffff)
    word(remote, 0xff52, 4)
    word(remote, 0xff50, 0x40)
    run(remote, 7)
    assert read(remote, 0xff78) == 0
    run(remote, 1)  # first compare, even with IE clear
    assert read(remote, 0xff78) == 0x80
    run(remote, 8)  # first wrap
    assert read(remote, 0xff9c) == 0x80
    run(remote, 1024)  # eight more wraps, both requests already latched
    assert read(remote, 0xfe50) == 0xfff0
    assert read(remote, 0xff78) == 0x80
    word(remote, 0xff78, 0)  # restore the next compare deadline
    run(remote, 120)
    assert read(remote, 0xff78) == 0x80
    word(remote, 0xff9c, 0)  # restore the next wrap
    run(remote, 8)
    assert read(remote, 0xff9c) == 0x80
    word(remote, 0xff78, 0x40)  # every enabled compare stays scheduled
    run(remote, 256)
    assert read(remote, 0xff78) == 0xc0
    word(remote, 0xff50, 0)
    word(remote, 0xff78, 0)
    word(remote, 0xff9c, 0)
    word(remote, 0xfe50, 0xffff)
    word(remote, 0xfe54, 0xffff)
    word(remote, 0xfe80, 0xfffe)  # unreachable after reload
    word(remote, 0xff50, 0x40)
    run(remote, 1024)
    assert read(remote, 0xff78) == 0
    assert read(remote, 0xfe50) == 0xffff
    word(remote, 0xfe54, 0xfffc)  # reload change makes compare reachable
    run(remote, 32)
    assert read(remote, 0xff78) == 0x80
    word(remote, 0xff50, 0)


def wake(remote, pec):
    write_memory(remote, 0x2000, '87788787cc00')
    write_memory(remote, 0x40, 'cc00')
    word(remote, 0xfe50, 0xfff0)
    word(remote, 0xfe54, 0xfff0)
    word(remote, 0xfe80, 0xffff)
    word(remote, 0xff52, 4)
    word(remote, 0xff78, 0x78 if pec else 0x7c)
    word(remote, 0xff9c, 0x80)
    if pec:
        word(remote, 0x200, 0x1234)
        word(remote, 0xfce0, 0x200)
        word(remote, 0xfce2, 0x300)
        word(remote, 0xfed0, 0)
        word(remote, 0xfec0, 0x4001)  # one transfer, EOP interrupt
    write_register(remote, 'psw', '0800')
    write_register(remote, 'pc', '00002000')
    word(remote, 0xff50, 0x40)
    remote.continue_to(0x2004 if pec else 0x40)
    assert read_register(remote, 'idle') == 0
    assert read(remote, 0xff78) & 0x80 == 0
    if pec:
        assert read(remote, 0x300) == 0x1234
        assert read(remote, 0xfec0) == 0x4000
    else:
        assert read_register(remote, 'sp') == 0xfbfa


def enable_pending(remote):
    write_memory(remote, 0x2000, 'cc00' * 2049)
    write_memory(remote, 0x40, 'cc00')
    word(remote, 0xfe50, 0xfff0)
    word(remote, 0xfe54, 0xfff0)
    word(remote, 0xfe80, 0xffff)
    word(remote, 0xff52, 4)
    word(remote, 0xff78, 0x80)
    word(remote, 0xff9c, 0x80)
    word(remote, 0xff50, 0x40)
    run(remote, 1024)
    write_register(remote, 'psw', '0800')
    word(remote, 0xff78, 0xfc)  # enabling an already pending request
    remote.continue_to(0x40)
    assert read(remote, 0xff78) == 0x7c
    assert read_register(remote, 'sp') == 0xfbfa


def observed(remote):
    write_memory(remote, 0x2000, 'cc00' * 2049)
    word(remote, 0xfe50, 0xfff0)
    word(remote, 0xfe54, 0xfff0)
    word(remote, 0xfe80, 0xffff)
    word(remote, 0xff52, 5)
    word(remote, 0xff78, 0x80)
    word(remote, 0xff9c, 0x80)
    word(remote, 0xff50, 0x40)
    run(remote, 1024)
    assert read(remote, 0xfe50) == 0xfff0


def arms(remote):
    # Writes without execution give exact same, earlier and later expirations.
    word(remote, 0xfe50, 0xff00)
    word(remote, 0xff50, 0x40)
    word(remote, 0xff50, 0x40)
    word(remote, 0xfe50, 0xff80)
    word(remote, 0xfe50, 0xfe00)
    word(remote, 0xfe42, 0xffff)  # competing GPT deadline wins
    word(remote, 0xff42, 0x40)
    word(remote, 0xff42, 0)
    word(remote, 0xff50, 0)
    write_memory(remote, 0, 'b748b7b7')  # SRST cancels active timers
    word(remote, 0xff50, 0x40)
    remote.step()
    assert read(remote, 0xff50) == 0


def main():
    binary = Path(os.environ['QEMU_TEST_QEMU_BINARY']).resolve()
    root = Path(os.environ['MESON_BUILD_ROOT']) / 'pmb7850-deadlines'
    root.mkdir(parents=True, exist_ok=True)
    cases = [('disabled', disabled), ('irq', lambda r: wake(r, False)),
             ('pec', lambda r: wake(r, True)), ('arms', arms),
             ('enable-pending', enable_pending), ('observed', observed),
             ('connected', observed), ('detailed', observed)]
    print(f'TAP version 13\n1..{len(cases)}', flush=True)
    for index, (name, body) in enumerate(cases, 1):
        sock, log = root / f'{name}.sock', root / f'{name}.log'
        sock.unlink(missing_ok=True)
        args = [str(binary), '-M', 'pmb7850-test', '-display', 'none', '-S',
                '-global', 'pmb7850-soc.instruction-timing=true',
                '-icount', 'shift=0,align=off,sleep=off',
                '-gdb', f'unix:{sock},server=on,wait=off',
                '-trace', 'enable=pmb7850_deadline*', '-D', str(log)]
        if name == 'detailed':
            args += ['-trace', 'enable=pmb7850_timer']
        if name == 'connected':
            args[args.index('pmb7850-test')] += (
                ',display-type=hm17cm4096')
            args += ['-trace', 'enable=hm17_reset']
        if name == 'observed':
            args += ['-global', 'pmb7850-soc.capcom-observe-mask=1']
        with (root / f'{name}.stderr').open('w') as err:
            p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=err)
            try:
                with connect(sock, time.monotonic() + 5) as client:
                    remote = Remote(client)
                    remote.packet('?')
                    body(remote)
            finally:
                p.terminate()
                p.wait(timeout=5)
                sock.unlink(missing_ok=True)
        rows = [dict(re.findall(r'(\w+)=(\S+)', line))
                for line in log.read_text().splitlines()
                if 'pmb7850_deadline ' in line]
        if name in ('irq', 'pec'):
            assert any(r['action'] == 'idle-wake' for r in rows)
        if name == 'disabled':
            assert not any(r['action'] == 'fire' and
                           16 < int(r['tick']) <= 1040 for r in rows)
        if name == 'detailed':
            fires = [r for r in rows if r['action'] == 'fire']
            ticks = [int(r['tick']) for r in fires]
            assert ticks == list(range(8, ticks[-1] + 1, 8))
            assert 1016 <= ticks[-1] <= 1024
        if name in ('observed', 'connected'):
            fires = [r for r in rows if r['action'] == 'fire']
            ticks = [int(r['tick']) for r in fires]
            assert ticks == list(range(120, 1025, 128))
            assert all(r['sources'] == '0x200' for r in fires)
        if name == 'connected':
            edges, tick = [], None
            for line in log.read_text().splitlines():
                fields = dict(re.findall(r'(\w+)=(\S+)', line))
                if 'pmb7850_deadline ' in line and fields['action'] == 'fire':
                    tick = int(fields['tick'])
                if 'hm17_reset ' in line and tick is not None:
                    edges.append((tick, int(fields['level']),
                                  int(fields['asserted'])))
            assert edges == [(120 + 128 * n, 1 - n % 2, n % 2)
                             for n in range(8)]
        if name == 'arms':
            assert any(r['action'] == 'unchanged' for r in rows)
            for direction in (int.__lt__, int.__gt__):
                assert any(r['action'] == 'arm' and int(r['previous']) > 0
                           and direction(int(r['requested']),
                                         int(r['previous']))
                           for r in rows)
            assert any(r['action'] == 'arm' and r['sources'] == '0x2'
                       for r in rows)
            assert rows[-1]['action'] == 'cancel'
        print(f'ok {index} - {name}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
