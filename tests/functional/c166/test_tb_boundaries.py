#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Do not hide a next-page GDB breakpoint inside a C166 translation block."""

import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from run_vectors import Remote, connect
from test_soc import read_register, write_register


def run_case(qemu, artifacts, wide):
    with tempfile.TemporaryDirectory(prefix='tb-page-', dir=artifacts) as tmp:
        root = Path(tmp)
        flash = bytearray(b'\xff' * (4 * 1024 * 1024))
        flash[0xf00:0x2000] = bytes.fromhex('cc00') * (0x1100 // 2)
        flash[0x2000:0x2002] = bytes.fromhex('0dff')
        if wide:
            flash[0xffe:0x1002] = bytes.fromhex('e6f03412')
        (root / 'flash.bin').write_bytes(flash)
        args = [str(qemu), '-M',
                'pmb7850-test,flash-type=m58-compatible-32mbit',
                '-S', '-display', 'none',
                '-icount', 'shift=0,align=off,sleep=off',
                '-drive', f'if=pflash,format=raw,file={root / "flash.bin"}',
                '-gdb', f'unix:{root / "gdb.sock"},server=on,wait=off']
        with (root / 'stderr.log').open('wb') as err:
            proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=err)
            try:
                with connect(root / 'gdb.sock', time.monotonic() + 5) as sock:
                    remote = Remote(sock)
                    remote.packet('?')
                    write_register(remote, 'pc', '00800ff0')
                    target = 0x801002 if wide else 0x801010
                    assert remote.packet(f'Z0,{target:x},2') == 'OK'
                    remote.send_command('c')
                    # A bounded failed probe does not keep sending commands
                    # to the running CPU after a missed breakpoint.
                    try:
                        remote.receive()
                    except TimeoutError as exc:
                        raise AssertionError(
                            'next-page breakpoint skipped') from exc
                    assert read_register(remote, 'pc') == target
                    assert read_register(remote, 'guest_icount') == (
                        8 if wide else 16)
                    if wide:
                        assert read_register(remote, 'r0') == 0x1234
            except Exception:
                for line in (root / 'stderr.log').read_text().splitlines():
                    print(f'# QEMU: {line}', flush=True)
                raise
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


def main():
    print('TAP version 13\n1..2', flush=True)
    failed = 0
    for i, wide in enumerate((False, True), 1):
        try:
            run_case(Path(os.environ['QEMU_TEST_QEMU_BINARY']),
                     Path(os.environ['MESON_BUILD_ROOT']), wide)
            print(f'ok {i} - page boundary wide={wide}', flush=True)
        except Exception as exc:
            failed += 1
            print(f'not ok {i} - page boundary wide={wide}', flush=True)
            print(f'# {type(exc).__name__}: {exc}', flush=True)
    return bool(failed)


if __name__ == '__main__':
    sys.exit(main())
