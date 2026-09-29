#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualified board audio: real CPU time, XBUS ownership and final PCM."""

import os
import hashlib
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from run_vectors import Remote, connect
from test_soc import write_memory, write_register

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts/pmb7850'))
from run_c55_boot import QMP, terminate

NOTE = bytes.fromhex('01911018147314140a4d7252f8ffa0a0')
WAVE = bytes.fromhex(
    '524946464000000057415645666d74201400000011000100401f0000'
    'c71b0000080004000200090066616374040000000a000000'
    '646174610c000000000000001111111100000000')
SAMPLED = bytes.fromhex('bcad0000000000001111')


def run_ticks(remote, count):
    while count:
        n = min(count, 30000)
        write_register(remote, 'pc', '00801000')
        remote.continue_to(0x801000 + 2 * n)
        count -= n


def qom(qmp, name):
    return qmp.execute('qom-get', {'path': '/machine/audio', 'property': name})


def command(remote, word, rate=1):
    write_memory(remote, 0xe836, word.to_bytes(2, 'little').hex())
    if word == 0x14:
        write_memory(remote, 0xe838, rate.to_bytes(2, 'little').hex())
    write_memory(remote, 0xef3a, '0400')


def packet(remote, data):
    write_memory(remote, 0xeb80, data.hex())


def run_case(qemu, artifacts, device, traced, disabled=False):
    with tempfile.TemporaryDirectory(prefix='x55-audio-', dir=artifacts) as tmp:
        root = Path(tmp)
        large_flash = ('m55', 'a65', 'c60', 'cf62', 'mc60')
        mib = 4 if device == 'a52' else 16 if device in large_flash else 8
        flash = bytearray(b'\xff' * (mib * 1024 * 1024))
        flash[0x1000:0x1000 + 60002] = bytes.fromhex('cc00') * 30001
        (root / 'flash.bin').write_bytes(flash)
        machine = 'siemens-' + device
        if disabled:
            machine += ',ringer-audio=off,xbus-audio=off'
        args = [str(qemu), '-M', machine, '-S',
                '-display', 'none', '-icount', 'shift=0,align=off,sleep=off',
                '-drive', f'if=pflash,format=raw,file={root / "flash.bin"}',
                '-gdb', f'unix:{root / "gdb.sock"},server=on,wait=off',
                '-qmp', f'unix:{root / "qmp.sock"},server=on,wait=off',
                '-D', str(root / 'native.log')]
        if traced:
            args += ['-trace', 'enable=x55_audio_*']
        with (root / 'stderr.log').open('wb') as err:
            proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=err)
            try:
                with connect(root / 'gdb.sock', time.monotonic() + 5) as sock:
                    with connect(root / 'qmp.sock', time.monotonic() + 5) as qs:
                        remote, qmp = Remote(sock), QMP(qs)
                        remote.packet('?')
                        command(remote, 0)
                        packet(remote, bytes(48))
                        run_ticks(remote, 100)
                        assert remote.packet('me836,2') == '0000'
                        assert remote.packet('meb80,2') == '0000'
                        if disabled:
                            command(remote, 0x16)
                            packet(remote, NOTE)
                            run_ticks(remote, 100)
                            assert remote.packet('me836,2') == '1600'
                            assert remote.packet('meb80,2') == '0191'
                            children = qmp.execute('qom-list',
                                                   {'path': '/machine'})
                            assert not any(c['name'] == 'audio'
                                           for c in children)
                        else:
                            # Packet ownership survives a long unclassified gap.
                            packet(remote, NOTE)
                            run_ticks(remote, 1200100)
                            assert remote.packet('meb80,2') == '0191'
                            assert qom(qmp, 'stream-accepted') == 0
                            command(remote, 0x16)
                            assert remote.packet('me836,2') == '1680'
                            assert remote.packet('meb80,2') == '5555'
                            for unused in range(4):
                                packet(remote, bytes.fromhex('ccc1'))
                                run_ticks(remote, 40)
                            run_ticks(remote, 600000)
                            assert qom(qmp, 'stream-accepted') == 5
                            assert qom(qmp, 'pcm-frames') == 768
                            assert qom(qmp, 'pcm-sha256') == (
                                'f1cce669eeea097c23b979cbf99c8f69f844fa50db8391e60491dd560cbc672c')
                            command(remote, 0x17)
                            run_ticks(remote, 10000)
                            assert qom(qmp, 'pcm-frames') == 768
                            # Decode delivered data without a RIFF anywhere.
                            packet(remote, SAMPLED)
                            run_ticks(remote, 40)
                            command(remote, 0x14)
                            packet(remote, bytes.fromhex('bcadffff1111'))
                            run_ticks(remote, 40)
                            packet(remote, bytes.fromhex('bcad000000000000'))
                            run_ticks(remote, 40)
                            command(remote, 0x15)
                            run_ticks(remote, 40000)
                            assert qom(qmp, 'pcm-frames') == 828
                            # Independently exercise the 16-kHz selector.
                            qmp.execute('system_reset')
                            packet(remote, bytes.fromhex('bcad00000600000017bd0824'))
                            run_ticks(remote, 40)
                            command(remote, 0x14, rate=2)
                            command(remote, 0x15)
                            run_ticks(remote, 40000)
                            assert qom(qmp, 'pcm-frames') == 27
                            assert qom(qmp, 'pcm-sha256') == (
                                '102759b620d3fcbaa46535fdbe34289ede'
                                '777f702181386e5f7fa9e81fd4ae91')
                            # Every enabled board gets the same complete 128-byte grammar.
                            qmp.execute('system_reset')
                            command(remote, 0x16)
                            extended = bytearray(NOTE + bytes(10))
                            extended[3] |= 0x80
                            full = bytes(extended) * 4 + bytes.fromhex('01a20000ffff') + bytes.fromhex('ccc1') * 9
                            assert len(full) == 128
                            packet(remote, full)
                            run_ticks(remote, 1200000)
                            assert remote.packet('meb80,2') == '5555'
                            assert qom(qmp, 'stream-accepted') == 1
                            assert qom(qmp, 'pcm-frames') == 1792
                            # Reset discards all held samples and output state;
                            # reset GPIO levels must not create a ringer tail.
                            qmp.execute('system_reset')
                            run_ticks(remote, 100)
                            assert qom(qmp, 'pcm-frames') == 0
                            assert qom(qmp, 'stream-accepted') == 0
                            assert qom(qmp, 'output-resets') == 0
                            if device in ('c55', 'a60', 'c60'):
                                # CC2 compare mode 1, /8 T0: four edges at
                                # 8, 2056, 4104, 6152 ticks after timer start.
                                for addr, value in ((0xfe50, 0xfffe),
                                                    (0xfe54, 0xff00),
                                                    (0xfe84, 0xffff),
                                                    (0xff52, 0x0500),
                                                    (0xff50, 0x0040)):
                                    write_memory(remote, addr,
                                                 struct.pack('<H', value).hex())
                                run_ticks(remote, 8192)
                                write_memory(remote, 0xff50, '0000')
                                run_ticks(remote, 1300000)
                                edges = [0, 2048, 4096, 6144]
                                end = edges[-1] + 1300000
                                pcm = bytearray()
                                for n in range(1, end * 48000 // 26000000 + 1):
                                    tick = (n * 26000000 + 47999) // 48000
                                    # A transition consumes the preceding
                                    # interval before applying its new level.
                                    high = sum(e < tick for e in edges) % 2
                                    pcm += struct.pack('<h', 6553 if high
                                                       else -6553)
                                assert qom(qmp, 'pcm-frames') == len(pcm) // 2
                                assert qom(qmp, 'pcm-sha256') == (
                                    hashlib.sha256(pcm).hexdigest())
                        qmp.close()
            except Exception:
                for name in ('stderr.log', 'native.log'):
                    for line in (root / name).read_text()[-4000:].splitlines():
                        print(f'# {name}: {line}', flush=True)
                raise
            finally:
                terminate(proc)


def run_routes(qemu, artifacts, ringer, xbus):
    """Exercise independent opt-outs on an already qualified board."""
    with tempfile.TemporaryDirectory(prefix='x55-routes-',
                                     dir=artifacts) as tmp:
        root = Path(tmp)
        flash = bytearray(b'\xff' * (8 * 1024 * 1024))
        flash[0x1000:0x1000 + 60002] = bytes.fromhex('cc00') * 30001
        (root / 'flash.bin').write_bytes(flash)
        args = [str(qemu), '-M',
                f'siemens-c55,ringer-audio={"on" if ringer else "off"},'
                f'xbus-audio={"on" if xbus else "off"}',
                '-S', '-display', 'none',
                '-icount', 'shift=0,align=off,sleep=off',
                '-drive', f'if=pflash,format=raw,file={root / "flash.bin"}',
                '-gdb', f'unix:{root / "gdb.sock"},server=on,wait=off',
                '-qmp', f'unix:{root / "qmp.sock"},server=on,wait=off',
                '-D', str(root / 'native.log'),
                '-trace', 'enable=pmb7850_internal_io',
                '-trace', 'enable=pmb7850_capcom_output']
        with (root / 'stderr.log').open('wb') as err:
            proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=err)
            try:
                with connect(root / 'gdb.sock', time.monotonic() + 5) as gs:
                    with connect(root / 'qmp.sock', time.monotonic() + 5) as qs:
                        remote, qmp = Remote(gs), QMP(qs)
                        remote.packet('?')
                        children = qmp.execute('qom-list', {'path': '/machine'})
                        assert any(c['name'] == 'audio' for c in children) == (
                            ringer or xbus)
                        command(remote, 0x16)
                        packet(remote, NOTE)
                        run_ticks(remote, 100)
                        assert remote.packet('me836,2') == (
                            '1680' if xbus else '1600')
                        assert remote.packet('meb80,2') == (
                            '5555' if xbus else '0191')
                        if ringer or xbus:
                            assert qom(qmp, 'stream-accepted') == int(xbus)
                        qmp.execute('system_reset')
                        for addr, value in ((0xfe50, 0xfffe), (0xfe54, 0xff00),
                                            (0xfe84, 0xffff), (0xff52, 0x0500),
                                            (0xff50, 0x0040)):
                            write_memory(remote, addr,
                                         struct.pack('<H', value).hex())
                        run_ticks(remote, 8192)
                        write_memory(remote, 0xff50, '0000')
                        run_ticks(remote, 1300000)
                        if ringer or xbus:
                            assert (qom(qmp, 'pcm-frames') > 0) == ringer
                        trace = (root / 'native.log').read_text()
                        assert 'channel=2 level=1' in trace
                        if not xbus:
                            assert ('write=1 addr=0x00e836 size=2 '
                                    'value=0x16') in trace
                            assert ('write=0 addr=0x00eb80 size=2 '
                                    'value=0x9101') in trace
                        qmp.close()
            finally:
                terminate(proc)


def main():
    qemu = Path(os.environ['QEMU_TEST_QEMU_BINARY'])
    artifacts = Path(os.environ['MESON_BUILD_ROOT'])
    cases = [(device, True, False) for device in (
        'a52', 'a55', 'c55', 'm55', 'a60', 'a62', 'a65', 'c60', 'cf62', 'mc60')]
    cases += [('c60', True, True), ('c55', False, False)]
    print(f'TAP version 13\n1..{len(cases) + 4}', flush=True)
    failed = 0
    for index, (device, traced, disabled) in enumerate(cases, 1):
        try:
            run_case(qemu, artifacts, device, traced, disabled)
            print(f'ok {index} - {device} traced={traced} '
                  f'disabled={disabled}', flush=True)
        except Exception as exc:
            failed += 1
            print(f'not ok {index} - {device} traced={traced} '
                  f'disabled={disabled}', flush=True)
            print(f'# {type(exc).__name__}: {exc}', flush=True)
    for offset, (ringer, xbus) in enumerate(
            ((False, False), (True, False), (False, True), (True, True)), 1):
        try:
            run_routes(qemu, artifacts, ringer, xbus)
            print(f'ok {len(cases) + offset} - '
                  f'ringer={ringer} xbus={xbus}', flush=True)
        except Exception as exc:
            failed += 1
            print(f'not ok {len(cases) + offset} - '
                  f'ringer={ringer} xbus={xbus}', flush=True)
            print(f'# {type(exc).__name__}: {exc}', flush=True)
    return bool(failed)


if __name__ == '__main__':
    sys.exit(main())
