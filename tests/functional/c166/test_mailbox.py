#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Mailbox completion survives real CPU preemption, including nested IRQs."""

import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from run_vectors import Remote, connect
from test_soc import read_register, write_memory, write_register


def run_case(qemu, artifacts, mode, nested):
    with tempfile.TemporaryDirectory(prefix='c166-mailbox-',
                                     dir=artifacts) as tmp:
        root = Path(tmp)
        flash = bytearray(b'\xff' * (8 * 1024 * 1024))
        # M166 MOV encodings (PDF p.82), RETI (p.99). Main rings the
        # doorbell, requests priority-7 TDMA, then reads and acknowledges.
        main = bytes.fromhex(
            'e6f40100 f6f412ec e6f4dc00 f6f486f1 '
            'f2f512ec f2f616ec f2f740f1 e6f40000 f6f412ec cc00')
        flash[0x1000:0x1000 + len(main)] = main
        flash[0x100:0x104] = bytes.fromhex('fa800020')  # TDMA -> 80:2000
        flash[0x08c:0x090] = bytes.fromhex('fa800021')  # T3 -> 80:2100
        flash[0x140:0x144] = bytes.fromhex('fa800022')  # IRQ80 -> 80:2200
        handler = bytes.fromhex('cc00') * 40
        if nested:
            handler += bytes.fromhex('e6fbe000 f6fb62ff')  # T3, priority 8
        handler += bytes.fromhex('cc00') * 40 + bytes.fromhex('fb88')
        flash[0x2000:0x2000 + len(handler)] = handler
        nested_handler = bytes.fromhex('cc00') * 80 + bytes.fromhex('fb88')
        flash[0x2100:0x2100 + len(nested_handler)] = nested_handler
        # IRQ80 worker records entry, reads result, clears bit 2, returns.
        worker = bytes.fromhex('e6f90100 f2f816ec e6f40000 f6f412ec fb88')
        flash[0x2200:0x2200 + len(worker)] = worker
        image = root / 'flash.bin'
        image.write_bytes(flash)
        log = root / 'native.log'
        args = [str(qemu), '-M', 'siemens-c55', '-display', 'none',
                '-serial', 'null', '-S', '-no-reboot',
                '-icount', 'shift=0,align=off,sleep=off',
                '-drive', f'if=pflash,format=raw,file={image}',
                '-gdb', f'unix:{root / "gdb.sock"},server=on,wait=off',
                '-D', str(log), '-trace', 'enable=siemens_xbus_mailbox',
                '-trace', 'enable=pmb7850_irq']
        with (root / 'stderr.log').open('wb') as err:
            proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=err)
            try:
                with connect(root / 'gdb.sock', time.monotonic() + 5) as sock:
                    remote = Remote(sock)
                    remote.packet('?')
                    for addr, value in ((0xec10, mode), (0xf140, 0x51),
                                        (0xec14, 2), (0xec80, 0x162),
                                        (0xec82, 0xa), (0xec84, 8)):
                        write_memory(remote, addr,
                                     value.to_bytes(2, 'little').hex())
                    write_register(remote, 'psw', '0800')
                    write_register(remote, 'r9', '0000')
                    write_register(remote, 'pc', '00801000')
                    remote.continue_to(0x801024)
                    assert read_register(remote, 'r5') == (0 if mode & 5 else 2)
                    assert read_register(remote, 'r6') == 0x10
                    assert read_register(remote, 'r7') == 0x51
                    assert read_register(remote, 'r9') == bool(mode & 5)
                    assert remote.packet('mec12,2') == '0000'
                    assert remote.packet('mf140,2') == '5100'
                    assert read_register(remote, 'sp') == 0xfc00
                text = log.read_text()
                assert 'ack source=33 vector=0x40' in text
                assert ('vector=0x23' in text) == nested
                phases = re.findall(
                    r'siemens_xbus_mailbox icount=(\d+).*?phase=(\S+)', text)
                assert len(phases) == 2, phases
                assert phases[0][1] == 'sync-ready', phases
                if mode & 5:
                    assert phases[1][1] == 'irq-promoted', phases
                    assert int(phases[1][0]) - int(phases[0][0]) == 32
                    assert text.count('ack source=51 vector=0x50') == 1
                else:
                    assert phases[1][1] == 'sync-consumed', phases
                    assert int(phases[1][0]) - int(phases[0][0]) > 80
                    assert 'vector=0x50' not in text
            except Exception:
                for path in (log, root / 'stderr.log'):
                    if path.exists():
                        for line in path.read_text().splitlines()[-50:]:
                            print(f'# {path.name}: {line}', flush=True)
                raise
            finally:
                if proc.poll() is None:
                    proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


def main():
    qemu = Path(os.environ['QEMU_TEST_QEMU_BINARY']).resolve()
    artifacts = Path(os.environ['MESON_BUILD_ROOT'])
    cases = [(mode, nested) for mode in (0x1700, 0x1701, 0x1704)
             for nested in (False, True)]
    print(f'TAP version 13\n1..{len(cases)}', flush=True)
    failed = 0
    for index, (mode, nested) in enumerate(cases, 1):
        try:
            run_case(qemu, artifacts, mode, nested)
            print(f'ok {index} - mode={mode:04x} nested={nested}', flush=True)
        except Exception as exc:
            failed += 1
            print(f'not ok {index} - mode={mode:04x} nested={nested}',
                  flush=True)
            print(f'# {type(exc).__name__}: {exc}', flush=True)
    return bool(failed)


if __name__ == '__main__':
    sys.exit(main())
