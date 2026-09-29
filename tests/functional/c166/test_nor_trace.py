#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Physical NOR reads through CPU ROMD, EBC routing, and PEC."""

import os
import subprocess
import tempfile
import time
from pathlib import Path

from run_vectors import Remote, connect
from test_soc import read_register, write_memory, write_register


def set_mode(remote, mode, model):
    # Execute an actual NOR command; GDB writes can bypass ROMD callbacks.
    writes = [(0, mode)]
    if model == "am29lv640mh" and mode == "9000":
        writes = [(0xAAA, "aa00"), (0x555, "5500"), (0xAAA, mode)]
    elif model == "am29lv640mh":
        writes = [(0, "f000")]
    for offset, value in writes:
        address = offset.to_bytes(2, 'little').hex()
        write_memory(remote, 0xF400, "e6f0" + value + "d7008000f7f0" + address)
        write_register(remote, "pc", "0000f400")
        remote.continue_to(0xF40C)


def run_case(qemu, artifacts, enabled, router, model):
    with tempfile.TemporaryDirectory(prefix="nor-trace-", dir=artifacts) as tmp:
        directory = Path(tmp)
        sock_path = directory / "gdb.sock"
        trace_path = directory / "native.log"
        flash_path = directory / "flash.bin"
        small = model == "m58-compatible-32mbit"
        flash = bytearray(b"\xff" * ((4 if small else 8) * 1024 * 1024))
        for offset, code in ((0x00, "d7008000f2f00010"),
                             (0x100, "d7000000f2f00010"),
                             (0x200, "d7008000f2f00000"),
                             (0x300, "d700c000f2f00010")):
            # A data load inside a multi-instruction TB must not attempt a
            # forbidden virtual-clock read (single-step would hide this).
            data = bytes.fromhex(code + "cc00" * 28)
            flash[offset:offset + len(data)] = data
        flash[0x1000:0x1002] = bytes.fromhex("3412")
        flash[-1] = 0xa5
        if not small:
            flash[0x401000:0x401002] = bytes.fromhex("7856")
        flash_path.write_bytes(flash)
        command = [str(qemu), "-M",
                   f"pmb7850-test,flash-type={model}",
                   "-display", "none", "-S", "-icount",
                   "shift=0,align=off,sleep=off", "-drive",
                   f"if=pflash,format=raw,file={flash_path}", "-gdb",
                   f"unix:{sock_path},server=on,wait=off",
                   "-trace", "enable=x55_nor_*", "-D", str(trace_path)]
        if enabled:
            command += ["-global", "pmb7850-soc.observe-flash-reads=on",
                        "-global", "pmb7850-soc.instruction-timing=on"]
        if router:
            command += ["-trace", "enable=pmb7850_bus"]
        if model == "w30-64mbit-top":
            command += ["-global", f"{model}.factory-uid=0001020304050607"]
        if model == "am29lv640mh":
            command += ["-global", f"{model}.factory-secsi=" + "ff" * 256]
        with (directory / "stderr.log").open("wb") as stderr:
            proc = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                                    stderr=stderr)
            try:
                with connect(sock_path, time.monotonic() + 5) as sock:
                    remote = Remote(sock)
                    remote.packet("?")
                    # EXTS #segment,#1; MOV R0,[offset], from existing vectors.
                    identity = (0x20 if small else
                                (1 if model == "am29lv640mh" else 0x89))
                    folded_value = 0x1234 if small else 0x5678
                    for pc, mode, expected in ((0x00, "ff00", 0x1234),
                                                (0x100, "9000", 0x1234),
                                                (0x200, "9000", identity),
                                                (0x300, "ff00", folded_value)):
                        set_mode(remote, mode, model)
                        for unused in range(4):
                            remote.packet("m801000,2")
                            remote.packet("m1000,2")
                        write_register(remote, "pc", f"{pc:08x}")
                        remote.continue_to(pc + 64)
                        actual = read_register(remote, "r0")
                        assert actual == expected, (pc, mode, actual, expected)

                    # PEC0 copies a native-ROMD flash word into DPRAM.
                    write_memory(remote, 0xF400, "cc00")
                    write_register(remote, "pc", "0000f400")
                    write_memory(remote, 0xFCE0, "0010")
                    write_memory(remote, 0xFCE2, "00f6")
                    write_memory(remote, 0xFED0, "8000")
                    write_memory(remote, 0xFEC0, "0140")
                    write_memory(remote, 0xFF9A, "f800")
                    write_register(remote, "psw", "0800")
                    remote.continue_to(0xF402)
                    assert remote.packet("mf600,2") == "3412"
                    # The real PEC transaction crosses into unmapped physical
                    # space. Observation must not invent a wrap to flash zero.
                    write_register(remote, "pc", "0000f400")
                    write_memory(remote, 0xFCE0, "ffff")
                    write_memory(remote, 0xFED0, "ff00")
                    write_memory(remote, 0xFEC0, "0140")
                    write_memory(remote, 0xFF9A, "f800")
                    write_register(remote, "psw", "0800")
                    remote.continue_to(0xF402)
                    assert remote.packet("mf600,2").startswith("a5")
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()

        reads = []
        for line in trace_path.read_text().splitlines():
            if line.startswith("x55_nor_read "):
                fields = dict(word.split("=", 1) for word in line.split()[1:])
                reads.append((int(fields["chip"]), int(fields["offset"], 0),
                              int(fields["addr"], 0), int(fields["value"], 0),
                              int(fields["size"]), int(fields["array"]),
                              fields["source"]))
        expected = [(0, 0x1000, 0x801000, 0x1234, 2, 1, "cpu"),
                    (0, 0x1000, 0x001000, 0x1234, 2, 1, "cpu"),
                    (0, 0x0000, 0x800000, identity, 2, 0, "cpu"),
                    (0, 0x1000 if small else 0x401000,
                     0xC01000, folded_value, 2, 1, "cpu"),
                    (0, 0x1000, 0x801000, 0x1234, 2, 1, "pec"),
                    (0, len(flash) - 1, 0xFFFFFF, 0xA5, 1, 1, "pec")]
        assert reads == (expected if enabled else []), reads


def run_dual_chip(qemu, artifacts):
    with tempfile.TemporaryDirectory(prefix="nor-dual-", dir=artifacts) as tmp:
        directory = Path(tmp)
        sock_path = directory / "gdb.sock"
        trace_path = directory / "native.log"
        command = [str(qemu), "-M", "siemens-s55", "-display", "none",
                   "-S", "-icount", "shift=0,align=off,sleep=off",
                   "-global", "pmb7850-soc.observe-flash-reads=on",
                   "-trace", "enable=x55_nor_*", "-D", str(trace_path),
                   "-gdb", f"unix:{sock_path},server=on,wait=off"]
        for chip, size in enumerate((8, 4)):
            flash = bytearray(b"\xff" * (size * 1024 * 1024))
            flash[0x1000:0x1002] = bytes([0xa0 + chip, 0])
            path = directory / f"chip{chip}.bin"
            path.write_bytes(flash)
            command += ["-drive",
                        f"if=pflash,unit={chip},format=raw,file={path}"]
        with (directory / "stderr.log").open("wb") as stderr:
            proc = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                                    stderr=stderr)
            try:
                with connect(sock_path, time.monotonic() + 5) as sock:
                    remote = Remote(sock)
                    remote.packet("?")
                    for segment, expected in ((0, 0xa0), (0x40, 0xa1),
                                              (0x80, 0xa0)):
                        code = f"d700{segment:02x}00f2f00010" + "cc00" * 28
                        write_memory(remote, 0xF400, code)
                        write_register(remote, "pc", "0000f400")
                        remote.continue_to(0xF440)
                        assert read_register(remote, "r0") == expected
                    # Secondary boot view is its command-capable M58 alias.
                    # Program can only clear the original 0x00a1 bits.
                    code = ("e6f04000d7004000f7f00010"
                            "e6f03012d7004000f6f00010"
                            "e6f0ff00d7004000f7f00000"
                            "d7004000f2f00010" + "cc00" * 10)
                    write_memory(remote, 0xF400, code)
                    write_register(remote, "pc", "0000f400")
                    remote.continue_to(0xF440)
                    assert read_register(remote, "r0") == 0x20
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        events = []
        for line in trace_path.read_text().splitlines():
            if line.startswith(("x55_nor_read ", "x55_nor_mutation ")):
                kind, *words = line.split()
                fields = dict(word.split("=", 1) for word in words)
                assert int(fields['tick']) > 0
                assert int(fields['pc'], 0) >= 0xf400
                events.append((kind, int(fields['chip']),
                               int(fields['offset'], 0),
                               int(fields['value'], 0)))
        assert events == [('x55_nor_read', 0, 0x1000, 0xa0),
                          ('x55_nor_read', 1, 0x1000, 0xa1),
                          ('x55_nor_read', 0, 0x1000, 0xa0),
                          ('x55_nor_mutation', 1, 0x1000, 0x20),
                          ('x55_nor_read', 1, 0x1000, 0x20)], events


def reject_unsupported_timing(qemu):
    for extra in ([], ['-icount', 'shift=0'],
                  ['-global', 'pmb7850-soc.instruction-timing=on']):
        result = subprocess.run(
            [str(qemu), '-M', 'pmb7850-test', '-display', 'none',
             '-global', 'pmb7850-soc.observe-flash-reads=on'] + extra,
            capture_output=True, text=True, timeout=5)
        assert result.returncode != 0
        assert 'requires instruction timing and -icount' in result.stderr


def main():
    qemu = Path(os.environ["QEMU_TEST_QEMU_BINARY"])
    artifacts = Path(os.environ["MESON_BUILD_ROOT"])
    cases = [(False, False, "m58-compatible-32mbit")]
    cases += [(True, router, model) for router in (False, True)
              for model in ("m58-compatible-32mbit", "am29lv640mh",
                            "w30-64mbit-top")]
    print(f"TAP version 13\n1..{len(cases) + 2}", flush=True)
    failures = 0
    for index, (enabled, router, model) in enumerate(cases, 1):
        try:
            run_case(qemu, artifacts, enabled, router, model)
            print(f"ok {index} - {model} observer={enabled} router={router}",
                  flush=True)
        except Exception as exc:
            failures += 1
            print(f"not ok {index} - {model} "
                  f"observer={enabled} router={router}")
            print(f"# {type(exc).__name__}: {exc}", flush=True)
    for index, (name, probe) in enumerate((
            ('dual physical chips', lambda: run_dual_chip(qemu, artifacts)),
            ('unsupported timing', lambda: reject_unsupported_timing(qemu))),
            len(cases) + 1):
        try:
            probe()
            print(f'ok {index} - {name}', flush=True)
        except Exception as exc:
            failures += 1
            print(f'not ok {index} - {name}')
            print(f'# {type(exc).__name__}: {exc}', flush=True)
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
