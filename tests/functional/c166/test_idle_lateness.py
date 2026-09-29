#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bound PMB idle overshoot with deterministic, synthetic late delivery.

The test board's EFF4 fixture presents an elapsed idle interval to the real PMB
callback. Even when delivery would cross a frame wrap, the CPU must see XP1
at its compare before XP0. No firmware image or wall-clock sleeps are involved.
"""
import os
import subprocess
import time
from pathlib import Path

from run_vectors import Remote, connect
from test_soc import read_register, write_memory, write_register
from test_deadlines import read, word


def check(remote, elapsed):
    # A small counterpart of C55 SW24's 977A28..977A38 predicate.
    write_memory(remote, 0x104, 'f2f534e0f2f426e040548d01cc00cc00')
    word(remote, 0xe022, 9999)
    word(remote, 0xe024, 0)
    word(remote, 0xe026, 9000)
    word(remote, 0xe034, 8999)
    word(remote, 0xf18e, 0x59)  # XP1 enabled, ILVL 6, no PEC
    word(remote, 0xe060, 2)
    write_register(remote, 'psw', '0800')
    write_register(remote, 'pc', '00002000')
    # Synthetic stopped-IDLE setup, not a natural firmware execution claim.
    write_register(remote, 'idle', '0001')
    before = read_register(remote, 'guest_icount')
    word(remote, 0xeff4, elapsed)
    assert read_register(remote, 'guest_icount') == before
    assert read(remote, 0xe034) == 9000
    assert read(remote, 0xf18e) == 0xd9
    assert read(remote, 0xf186) & 0x80 == 0
    # A second host synchronization before the vCPU runs must not consume
    # another deadline while the first enabled request is waiting for service.
    word(remote, 0xeff4, 12432)
    assert read(remote, 0xe034) == 9000
    assert read(remote, 0xf186) & 0x80 == 0
    # Both branches stop before a following instruction can mask the result.
    assert remote.packet('Z0,110,2') == 'OK'
    assert remote.packet('Z0,112,2') == 'OK'
    assert remote.packet('c').startswith('T05')
    assert read_register(remote, 'pc') == 0x110
    assert read_register(remote, 'r4') == 9000
    assert read_register(remote, 'r5') == 9000
    assert read_register(remote, 'idle') == 0
    assert read(remote, 0xf18e) == 0x59  # real CPU acceptance acknowledged XP1


def main():
    binary = Path(os.environ['QEMU_TEST_QEMU_BINARY']).resolve()
    root = Path(os.environ['MESON_BUILD_ROOT']) / 'pmb7850-idle-lateness'
    root.mkdir(parents=True, exist_ok=True)
    print('TAP version 13\n1..6', flush=True)
    index = 0
    for shift in (0, 5):
        for elapsed in (12, 12012, 12432):
            index += 1
            name = f'shift-{shift}-elapsed-{elapsed}'
            sock = root / f'{name}.sock'
            sock.unlink(missing_ok=True)
            args = [str(binary), '-M', 'pmb7850-test', '-display', 'none', '-S',
                    '-global', 'pmb7850-soc.instruction-timing=true',
                    '-icount', f'shift={shift},align=off,sleep=off',
                    '-gdb', f'unix:{sock},server=on,wait=off',
                    '-trace', 'enable=pmb7850_tdma_irq',
                    '-trace', 'enable=c166_irq_accept']
            with (root / f'{name}.log').open('w') as log:
                proc = subprocess.Popen(args, stdout=log, stderr=log)
                try:
                    with connect(sock, time.monotonic() + 5) as client:
                        remote = Remote(client)
                        remote.packet('?')
                        check(remote, elapsed)
                finally:
                    proc.terminate()
                    proc.wait(timeout=5)
                    sock.unlink(missing_ok=True)
            print(f'ok {index} - {name}', flush=True)


if __name__ == '__main__':
    main()
