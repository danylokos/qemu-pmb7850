#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Native C166/x55 migration: stopped state and deterministic continuation."""

import contextlib
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from run_vectors import Remote, connect
from test_soc import read_register, write_memory, write_register

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts/pmb7850'))
from run_c55_boot import QMP, terminate


def wait_migration(qmp):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        result = qmp.execute('query-migrate')
        if result.get('status') == 'completed':
            return
        if result.get('status') in ('failed', 'cancelled'):
            raise RuntimeError(result)
        time.sleep(.01)
    raise TimeoutError('migration did not complete')


@contextlib.contextmanager
def machine(binary, root, name, extra=(), incoming=None,
            model="pmb7850-test", shift=0):
    directory = root / name
    directory.mkdir()
    with tempfile.TemporaryDirectory(prefix='c166-snap-') as sockets:
        socket_path = Path(sockets)
        args = [str(binary), '-M', model, '-display', 'none', '-S',
                '-global', 'migration.store-global-state=off',
                '-icount', f'shift={shift},align=off,sleep=off',
                '-global', 'pmb7850-soc.instruction-timing=on',
                '-gdb', f'unix:{socket_path / "gdb"},server=on,wait=off',
                '-qmp', f'unix:{socket_path / "qmp"},server=on,wait=off',
                '-D', str(directory / 'trace.log'),
                '-serial', f'file:{directory / "serial.bin"}',
                '-trace', 'enable=pmb7850_serial',
                '-trace', 'enable=pmb7850_irq',
                '-trace', 'enable=pmb7850_pec',
                '-trace', 'enable=pmb7850_ssc',
                '-trace', 'enable=siemens_xbus_mailbox',
                '-trace', 'enable=x55_nor_*'] + list(extra)
        if incoming:
            args += ['-incoming', 'defer']
        with (directory / 'stderr').open('wb') as err:
            process = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                       stderr=err)
            try:
                with connect(socket_path / 'qmp', time.monotonic() + 5) as qs:
                    qmp = QMP(qs)
                    try:
                        if incoming:
                            qmp.execute('migrate-incoming',
                                        {'uri': f'file:{incoming}'})
                            wait_migration(qmp)
                            assert not qmp.execute('query-status')['running']
                        with connect(socket_path / 'gdb',
                                     time.monotonic() + 5) as gs:
                            remote = Remote(gs)
                            remote.packet('?')
                            yield remote, qmp, directory
                    finally:
                        qmp.close()
            except Exception:
                print((directory / 'stderr').read_text(), flush=True)
                raise
            finally:
                terminate(process)


def save(qmp, path):
    qmp.execute('stop')
    qmp.execute('migrate', {'uri': f'file:{path}'})
    wait_migration(qmp)


def word(remote, address, value):
    write_memory(remote, address, value.to_bytes(2, 'little').hex())


def observe(remote):
    return [remote.packet('g')] + [remote.packet(f'm{addr:x},{size:x}')
        for addr, size in ((0xe100, 64), (0xf200, 64), (0xf400, 64),
                           (0xfc00, 64), (0xfdc0, 64), (0x20000, 64),
                           (0xfe00, 512), (0xf000, 512))]


def setup_core(remote):
    write_memory(remote, 0x2000, 'cc00' * 128)
    write_register(remote, 'pc', '00002000')
    for addr in (0xe100, 0xf200, 0xf400, 0xfc00, 0xfdc0, 0x20000):
        write_memory(remote, addr, bytes(range(64)).hex())
    word(remote, 0xfe42, 0xfff0)  # T3 continuation across overflow
    word(remote, 0xff42, 0x40)
    remote.step()


def setup_window(remote):
    setup_core(remote)
    # Existing C166 vector encoding: EXTS #0x80,#1 followed by a data load.
    write_memory(remote, 0x2000, 'd7008000f2f00010' + 'cc00' * 128)
    write_register(remote, 'pc', '00002000')
    remote.step()
    assert read_register(remote, 'ext_count') == 1


def setup_irq(remote):
    setup_core(remote)
    write_memory(remote, 0xa4, 'cc00' * 64)
    word(remote, 0xff9a, 0xfc)
    write_register(remote, 'psw', '0800')


def physical_effects(trace):
    # Host mapping/flush diagnostics are not guest effects. Old implementations
    # have different mappings, but must produce identical ordered device events.
    return b''.join(line for line in trace.splitlines(keepends=True)
                    if not line.startswith((b'x55_nor_mapping ',
                                            b'x55_nor_flush ')))


def core_case(binary, root, name, setup, model="pmb7850-test", extra=(),
              finish=None, observations=None, shift=0, serial_expected=None,
              setup_qmp=None):
    def state(r, q):
        return observe(r), observations(r, q) if observations else None

    def advance(r):
        if finish:
            finish(r)
        else:
            for unused in range(32):
                r.step()
    path = root / f'{name}.vmstate'
    source = Path(os.environ.get('QEMU_TEST_SOURCE_BINARY', binary))
    with machine(source, root, name + '-source', model=model,
                 extra=extra, shift=shift) as (r, q, directory):
        setup(r)
        if setup_qmp:
            setup_qmp(q)
        before = state(r, q)
        save(q, path)
        offset = (directory / 'trace.log').stat().st_size
        serial_offset = (directory / 'serial.bin').stat().st_size
        advance(r)
        after = state(r, q)
        effects = physical_effects((directory / 'trace.log').read_bytes()[offset:])
        serial_effects = (directory / 'serial.bin').read_bytes()[serial_offset:]
        if serial_expected is not None:
            assert serial_effects == serial_expected, repr(serial_effects)
    with machine(binary, root, name + '-restore', incoming=path,
                 model=model, extra=extra, shift=shift) as (r, q, directory):
        assert state(r, q) == before, 'stopped state changed on load'
        offset = (directory / 'trace.log').stat().st_size
        serial_offset = (directory / 'serial.bin').stat().st_size
        advance(r)
        assert state(r, q) == after, 'continuation diverged'
        assert physical_effects(
            (directory / 'trace.log').read_bytes()[offset:]) == effects
        assert ((directory / 'serial.bin').read_bytes()[serial_offset:] ==
                serial_effects)
        save(q, root / f'{name}-second.vmstate')



def qom(q, path, prop):
    return q.execute('qom-get', {'path': path, 'property': prop})


def panel_state(r, q):
    props = q.execute('qom-list', {'path': '/machine/display'})
    names = {p['name'] for p in props}
    return [qom(q, '/machine/display', p) for p in (
        'memory-digest', 'presentation-digest', 'frame-count', 'frame-digest')
        if p in names]


def nops(r, count):
    while count:
        chunk = min(count, 512)
        if getattr(r, '_snapshot_nops', 0) < chunk + 1:
            # DPRAM is writable in every flash mode, including busy W30.
            write_memory(r, 0xf600, 'cc00' * (chunk + 1))
            r._snapshot_nops = chunk + 1
        write_register(r, 'pc', '0000f600')
        r.continue_to(0xf600 + 2 * chunk)
        count -= chunk


def ssc(r, value):
    word(r, 0xf0b0, value)
    nops(r, 64)


def lcd_case(binary, root, lcd):
    def setup(r):
        setup_core(r)
        word(r, 0xff42, 0)  # no unrelated timer interrupt
        for addr, val in ((0xffc6, 1 << 11), (0xffd2, 1 << 6),
                          (0xffc4, 1 << 11), (0xf0b4, 0),
                          (0xffb2, 0xc0d7), (0xffc4, 0)):
            word(r, addr, val)
        if lcd.startswith('hm17'):
            word(r, 0xffd0, 1 << 6)
            for cmd in (0xa1, 0x81):
                ssc(r, cmd)
            word(r, 0xffd0, 0)
            ssc(r, 0xff)  # partial packed pixel for HM4096
        elif lcd == 'pcf8833-compatible':
            word(r, 0xffd0, 0)
            ssc(r, 0x2a)
            word(r, 0xffd0, 1 << 6)
            ssc(r, 1)  # first byte of column range
        elif lcd == 's6b33bx-compatible':
            word(r, 0xffd0, 0)
            ssc(r, 0x43)
            ssc(r, 1)  # first byte of column range
        else:
            word(r, 0xffd0, 0)
            ssc(r, 0x21)
        word(r, 0xf0b0, 0xff)  # snapshot in-flight SSC transfer

    def finish(r):
        nops(r, 64)
        for byte in (2, 0xff, 0xff, 0x55):
            ssc(r, byte)
        word(r, 0xffc4, 1 << 11)

    core_case(binary, root, lcd, setup,
              model=('siemens-c55' if lcd == 'pcf8813' else
                     'pmb7850-test,display-type=' + lcd),
              finish=finish, observations=panel_state)


def audio_case(binary, root):
    from test_audio import NOTE, SAMPLED, WAVE, command, packet

    def setup(r):
        setup_core(r)
        word(r, 0xff42, 0)
        command(r, 0x16)
        packet(r, NOTE)
        nops(r, 100)
        for unused in range(4):
            packet(r, bytes.fromhex('ccc1'))
            nops(r, 40)

    def finish(r):
        for unused in range(20):
            nops(r, 30000)

    def state(r, q):
        return [qom(q, '/machine/audio', p) for p in (
            'stream-accepted', 'pcm-frames', 'pcm-sha256')]

    core_case(binary, root, 'audio', setup, model='siemens-c55',
              finish=finish, observations=state)

    full = (bytes.fromhex('019100981473') + bytes(20)) * 4 + bytes.fromhex('01a20000ffff') + bytes.fromhex('ccc1') * 9

    def extended_setup(r):
        setup_core(r)
        word(r, 0xff42, 0)
        command(r, 0x16)
        packet(r, full)
        nops(r, 40)
        packet(r, full)  # save a collecting packet, including bytes 48..127

    def extended_finish(r):
        nops(r, 40)
        packet(r, bytes.fromhex('ccc1'))
        nops(r, 40)
        finish(r)

    core_case(binary, root, 'audio-extended-si3', extended_setup,
              model='siemens-c55', finish=extended_finish, observations=state)

    # Preserve the incremental predictor across a save, with no source file.
    def sampled_setup(r):
        setup_core(r)
        word(r, 0xff42, 0)
        packet(r, SAMPLED)
        nops(r, 40)
        command(r, 0x14)
        packet(r, bytes.fromhex('bcadffff1111'))
        nops(r, 40)

    def sampled_finish(r):
        packet(r, bytes.fromhex('bcad01001111640000009999'))
        nops(r, 40)
        command(r, 0x15)
        for unused in range(4):
            nops(r, 30000)

    def sampled_state(r, q):
        result = state(r, q)
        if result[0] == 3:
            assert result[1] == 108, result
        return result

    core_case(binary, root, 'audio-sampled-continuation', sampled_setup,
              model='siemens-c55', finish=sampled_finish,
              observations=sampled_state,
              extra=('-trace', 'enable=x55_audio_*'))


def nor_write(r, address, value):
    # Execute guest writes, since GDB writes to ROMD bypass flash commands.
    code = ('e6f0' + value.to_bytes(2, 'little').hex() +
            'd700' + (address >> 16).to_bytes(2, 'little').hex() +
            ('f7f0' if address & 1 else 'f6f0') +
            (address & 0xffff).to_bytes(2, 'little').hex())
    write_memory(r, 0xf400, code)
    write_register(r, 'pc', '0000f400')
    r.continue_to(0xf40c)


def flash_case(binary, root, model):
    def setup(r):
        setup_core(r)
        word(r, 0xff42, 0)
        if model.startswith('am29'):
            for addr, val in ((0x800aaa, 0xaa), (0x800555, 0x55),
                              (0x840000, 0x25), (0x840000, 1),
                              (0x840000, 0x1234)):
                nor_write(r, addr, val)
        elif model.startswith('m58'):
            for val in (0xe8, 1, 0x1234):
                nor_write(r, 0x840000, val)
        else:
            nor_write(r, 0x840000, 0x60)
            nor_write(r, 0x840000, 0xd0)
            nor_write(r, 0x840000, 0x40)  # pending program datum

    def finish(r):
        nor_write(r, 0x840000 if model.startswith('w30') else 0x840002, 0x5678)
        if not model.startswith('w30'):
            nor_write(r, 0x840000, 0x29 if model.startswith('am29') else 0xd0)
        nops(r, 1000)
        nor_write(r, 0x840000, 0xf0 if model.startswith('am29') else 0xff)
        expected = '7856ffff' if model.startswith('w30') else '34127856'
        assert r.packet('m840000,4') == expected

    def state(r, q):
        return r.packet('m840000,8')

    core_case(binary, root, model, setup,
              model='pmb7850-test,flash-type=' + model,
              finish=finish, observations=state)


def suspended_case(binary, root, model, suspend=True):
    amd = model.startswith('am29')

    def setup(r):
        setup_core(r)
        word(r, 0xff42, 0)
        write_memory(r, 0x840000, '0000')
        if amd:
            for off, val in ((0xaaa, 0xaa), (0x555, 0x55), (0xaaa, 0x80),
                             (0xaaa, 0xaa), (0x555, 0x55), (0x40000, 0x30)):
                nor_write(r, 0x800000 + off, val)
        else:
            for val in (0x60, 0xd0, 0x20, 0xd0):
                nor_write(r, 0x840000, val)
        nops(r, 32)
        if suspend:
            nor_write(r, 0x840000, 0xb0)

    def finish(r):
        if suspend:
            nor_write(r, 0x840000, 0x30 if amd else 0xd0)
        nops(r, 120000)
        nor_write(r, 0x840000, 0xf0 if amd else 0xff)
        assert r.packet('m840000,2') == 'ffff'

    core_case(binary, root, ('suspended-' if suspend else 'busy-') + model, setup,
              model='pmb7850-test,flash-type=' + model,
              extra=('-global', model + '.instruction-timing=on')
                    if amd else (),
              shift=0 if amd else 5, finish=finish,
              observations=lambda r, q: r.packet('m840000,2'))


def sim_case(binary, root):
    def setup(r):
        setup_core(r)
        word(r, 0xff42, 0)
        word(r, 0xef50, 1 << 13)
        nops(r, 20000)
        word(r, 0xf18c, 0)
        nops(r, 20000)  # loaded ATR byte waits for consumption
        assert r.packet('mef5a,1') == '90'

    def finish(r):
        for expected in ('11', '00'):
            word(r, 0xf184, 0)
            nops(r, 20000)
            assert r.packet('mef5a,1') == expected

    core_case(binary, root, 'sim', setup,
              extra=('-global', 'pmb7850-soc.sim-profile=deterministic'),
              finish=finish, observations=lambda r, q: [
                  qom(q, '/machine/soc', 'sim-event-count'),
                  r.packet('mef50,20')])


def mailbox_case(binary, root):
    def setup(r):
        setup_core(r)
        word(r, 0xff42, 0)
        for addr, val in ((0xec10, 0x1704), (0xf140, 0x51), (0xec14, 2),
                          (0xec80, 0x162), (0xec82, 0xa), (0xec84, 8),
                          (0xec12, 1)):
            word(r, addr, val)

    core_case(binary, root, 'mailbox', setup, model='siemens-c55',
              finish=lambda r: nops(r, 100),
              observations=lambda r, q: r.packet('mec10,10'))


def twi_case(binary, root):
    def lines(r, scl, sda):
        word(r, 0xffcc, 0)
        word(r, 0xffce, (scl << 12) | (sda << 13))

    def bits(r, value, first=0, last=8):
        for bit in range(first, last):
            low = not value & (0x80 >> bit)
            lines(r, True, low)
            lines(r, False, low)
            lines(r, True, low)

    def ack(r):
        lines(r, True, False)
        lines(r, False, False)
        pins = int.from_bytes(bytes.fromhex(r.packet('mffcc,2')), 'little')
        assert not pins & (1 << 13)
        lines(r, True, False)

    def setup(r):
        setup_core(r)
        lines(r, False, False)
        lines(r, False, True)
        bits(r, 0x62)
        ack(r)
        bits(r, 1)
        ack(r)
        bits(r, 0xa5, last=4)

    def finish(r):
        bits(r, 0xa5, first=4)
        ack(r)
        lines(r, True, True)
        lines(r, False, True)
        lines(r, False, False)

    core_case(binary, root, 'twi', setup,
              model='pmb7850-test,twi-profile=mc60', finish=finish,
              observations=lambda r, q: qom(
                  q, '/machine/twi', 'register-writes'))


def idle_case(binary, root):
    def setup(r):
        setup_core(r)
        word(r, 0xff42, 0)
        write_memory(r, 0x40, 'cc00' * 64)
        for addr, val in ((0xfe50, 0xff00), (0xfe54, 0xff00),
                          (0xfe80, 0xffff), (0xff52, 4), (0xff78, 0x7c),
                          (0xff50, 0x40)):
            word(r, addr, val)
        write_register(r, 'psw', '0800')
        # Diagnostic setup of an idle core awaiting its scheduled CC0 wake.
        write_register(r, 'idle', '0001')

    def finish(r):
        r.continue_to(0x40)
        assert read_register(r, 'idle') == 0

    core_case(binary, root, 'idle', setup, finish=finish)


def serial_case(binary, root):
    def setup(r):
        setup_core(r)
        word(r, 0xff42, 0)
        word(r, 0xfeb4, 1)
        word(r, 0xffb0, 0x8011)
        word(r, 0xfeb0, 0x41)
        word(r, 0xfeb0, 0x42)

    core_case(binary, root, 'serial', setup, finish=lambda r: nops(r, 2000),
              serial_expected=b'AB', model='siemens-c55')


def inputs_case(binary, root):
    def setup(q):
        for path, prop, value in (('/machine', 'battery-level', 17),
                                  ('/machine', 'battery-charging', True),
                                  ('/machine/keypad', 'ui-buttons', 1)):
            q.execute('qom-set',
                      {'path': path, 'property': prop, 'value': value})

    def state(r, q):
        return (qom(q, '/machine', 'battery-level'),
                qom(q, '/machine', 'battery-charging'),
                qom(q, '/machine/keypad', 'ui-buttons'),
                qom(q, '/machine/keypad', 'sampled-buttons'))

    core_case(binary, root, 'inputs', setup_core, model='siemens-c55',
              setup_qmp=setup, observations=state)

def main():
    binary = Path(os.environ['QEMU_TEST_QEMU_BINARY']).resolve()
    artifacts = Path(os.environ['MESON_BUILD_ROOT']) / 'c166-snapshot'
    artifacts.mkdir(parents=True, exist_ok=True)
    cases = [(name, lambda b, r, n=name, f=setup: core_case(b, r, n, f))
             for name, setup in [('core', setup_core), ('window', setup_window),
                                 ('irq', setup_irq)]]
    cases += [(board, lambda b, r, n=board: core_case(
        b, r, n, setup_core, model='siemens-' + n)) for board in (
        'a52', 'a55', 'a60', 'a62', 'a65', 'c55', 'c60', 'cf62',
        'm55', 'mc60', 's55', 'sl55')]
    cases += [(lcd, lambda b, r, n=lcd: lcd_case(b, r, n)) for lcd in (
        'pcf8813', 'hm17cm256', 'hm17cm4096', 'pcf8833-compatible',
        's6b33bx-compatible')]
    cases += [('audio', audio_case), ('sim', sim_case),
              ('mailbox', mailbox_case),
              ('twi', twi_case), ('idle', idle_case), ('serial', serial_case),
              ('inputs', inputs_case)]
    cases += [('suspended-' + nor, lambda b, r, n=nor: suspended_case(b, r, n))
              for nor in ('am29lv640mh', 'am29lv128mh',
                          'w30-64mbit-top', 'w30-128mbit-top')]
    cases += [('busy-' + nor, lambda b, r, n=nor: suspended_case(b, r, n, False))
              for nor in ('am29lv640mh', 'am29lv128mh',
                          'w30-64mbit-top', 'w30-128mbit-top')]
    cases += [(nor, lambda b, r, n=nor: flash_case(b, r, n)) for nor in (
        'm58lw064d', 'm58-compatible-32mbit', 'am29lv640mh', 'am29lv128mh',
        'w30-64mbit-top', 'w30-128mbit-top')]
    print(f'TAP version 13\n1..{len(cases)}', flush=True)
    for index, (name, setup) in enumerate(cases, 1):
        try:
            with tempfile.TemporaryDirectory(dir=artifacts) as tmp:
                setup(binary, Path(tmp))
            print(f'ok {index} - {name}', flush=True)
        except Exception as exc:
            print(f'not ok {index} - {name}\n# {type(exc).__name__}: {exc}',
                  flush=True)
            raise
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
