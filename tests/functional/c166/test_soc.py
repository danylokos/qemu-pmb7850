#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""End-to-end PMB7850 interrupt and PEC checks through the C166 CPU."""

from __future__ import annotations

import os
import subprocess
import time
from pathlib import Path

from run_vectors import REGISTERS, Remote, connect, expected_remote_hex


def write_memory(remote: Remote, address: int, data: str) -> None:
    print(f"# write memory 0x{address:06x}", flush=True)
    response = remote.packet(f"M{address:x},{len(data) // 2:x}:{data}")
    if response != "OK":
        raise RuntimeError(f"cannot write 0x{address:06x}: {response}")


def write_register(remote: Remote, name: str, value: str) -> None:
    print(f"# write register {name}", flush=True)
    response = remote.packet(
        f"P{REGISTERS[name]:x}={expected_remote_hex(value)}"
    )
    if response != "OK":
        raise RuntimeError(f"cannot write {name}: {response}")


def read_register(remote: Remote, name: str) -> int:
    encoded = remote.packet(f"p{REGISTERS[name]:x}")
    return int.from_bytes(bytes.fromhex(encoded), "little")


def run_case(qemu: Path, artifacts: Path, name: str, body) -> None:
    sock_path = artifacts / f"gdb-{name}.sock"
    sock_path.unlink(missing_ok=True)
    log = artifacts / f"{name}.log"
    command = [
        str(qemu), "-M", "pmb7850-test", "-display", "none", "-S",
        "-gdb", f"unix:{sock_path},server=on,wait=off", "-no-reboot",
        "-trace", "enable=pmb7850_irq", "-trace", "enable=c166_irq_*",
        "-trace", "enable=pmb7850_fast_lm",
    ]
    with log.open("wb") as stderr:
        proc = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                                stderr=stderr)
        try:
            with connect(sock_path, time.monotonic() + 5) as sock:
                remote = Remote(sock)
                print(f"# connected {name}", flush=True)
                remote.packet("?")
                print(f"# stopped {name}", flush=True)
                body(remote)
        finally:
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            print(f"# qemu {name} exit={proc.returncode}", flush=True)
            sock_path.unlink(missing_ok=True)


def normal_interrupt(remote: Remote) -> None:
    write_memory(remote, 0x000000, "cc00")
    write_memory(remote, 0x0000A4, "cc00")
    write_memory(remote, 0x00FF9A, "fc00")  # ADEIC: IR, IE, ILVL 15
    write_register(remote, "psw", "0800")
    assert remote.packet("mff9a,2") == "fc00"
    assert read_register(remote, "psw") == 0x0800
    remote.continue_to(0xA4)
    pc = read_register(remote, "pc")
    sp = read_register(remote, "sp")
    ic = remote.packet("mff9a,2")
    assert pc == 0xA4, f"pc=0x{pc:06x}"
    assert sp == 0xFBFA, f"sp=0x{sp:04x}"
    assert ic == "7c00", f"ADEIC={ic}"


def interrupt_arbitration(remote: Remote) -> None:
    write_memory(remote, 0x000000, "cc00")
    write_memory(remote, 0x000040, "cc00")  # CC0 vector
    write_memory(remote, 0x0000A4, "cc00")  # ADE vector
    write_memory(remote, 0x00FF78, "fc00")  # CC0IC: ILVL 15, xGLVL 0
    write_memory(remote, 0x00FF9A, "fc01")  # ADEIC: ILVL 15, xGLVL 4
    write_register(remote, "psw", "0800")
    remote.continue_to(0xA4)
    cc0ic = remote.packet("mff78,2")
    adeic = remote.packet("mff9a,2")
    sp = read_register(remote, "sp")
    assert cc0ic == "fc00", f"CC0IC={cc0ic}"
    assert adeic == "7c01", f"ADEIC={adeic}"
    assert sp == 0xFBFA, f"sp=0x{sp:04x}"


def prepare_pec_interrupt(remote: Remote, pecc: str) -> None:
    write_memory(remote, 0x000000, "cc00")
    write_memory(remote, 0x000200, "3412")
    write_memory(remote, 0x00FCE0, "0002")  # SRCP0 = 0x0200
    write_memory(remote, 0x00FCE2, "0003")  # DSTP0 = 0x0300
    write_memory(remote, 0x00FED0, "0000")  # PECSN0
    write_memory(remote, 0x00FEC0, pecc)
    write_memory(remote, 0x00FF9A, "f800")  # ADEIC: ILVL 14, xGLVL 0
    write_register(remote, "psw", "0800")
    assert remote.packet("mff9a,2") == "f800"
    assert read_register(remote, "psw") == 0x0800


def pec_interrupt(remote: Remote) -> None:
    prepare_pec_interrupt(remote, "0140")  # one word, EOP interrupt
    remote.continue_to(2)
    destination = remote.packet("m300,2")
    pecc = remote.packet("mfec0,2")
    ic = remote.packet("mff9a,2")
    subnode = remote.packet("mffa8,2")
    sp = read_register(remote, "sp")
    pc = read_register(remote, "pc")
    assert destination == "3412", f"destination={destination}"
    assert pecc == "0040", f"PECC0={pecc}"
    assert ic == "7800", f"ADEIC={ic}"
    assert subnode == "0200", f"PECISNC={subnode}"
    assert sp == 0xFC00, f"sp=0x{sp:04x}"
    assert pc == 2, f"pc=0x{pc:06x}"


def pec_final_interrupt(remote: Remote) -> None:
    prepare_pec_interrupt(remote, "0100")  # one word, source ISR follows
    write_memory(remote, 0x0000A4, "cc00")
    remote.continue_to(0xA4)
    destination = remote.packet("m300,2")
    pecc = remote.packet("mfec0,2")
    sp = read_register(remote, "sp")
    assert destination == "3412", f"destination={destination}"
    assert pecc == "0000", f"PECC0={pecc}"
    assert sp == 0xFBFA, f"sp=0x{sp:04x}"


def software_reset(remote: Remote) -> None:
    write_memory(remote, 0x000000, "b748b7b7")
    write_memory(remote, 0x00E100, "3412")
    write_memory(remote, 0x00FC00, "7856")
    remote.step()
    pc = read_register(remote, "pc")
    assert pc == 0, f"pc=0x{pc:06x}"
    assert remote.packet("me100,2") == "0000"
    assert remote.packet("mfc00,2") == "0000"


def debugger_syscon_mapping(remote: Remote) -> None:
    """RSP overrides notify mappings; mapped writes retain EINIT protection."""
    write_memory(remote, 0x200, "1111")
    write_memory(remote, 0xff12, "0004")
    write_memory(remote, 0x200, "2222")
    for register in (False, True):
        if register:
            write_register(remote, "syscon", "0000")
        else:
            write_memory(remote, 0xff12, "0000")
        assert read_register(remote, "syscon") == 0
        assert remote.packet("m200,2") == "1111"
        write_register(remote, "syscon", "1400")
        assert remote.packet("m10200,2") == "2222"
        assert remote.packet("m200,2") == "1111"
        write_register(remote, "syscon", "0400")
        assert remote.packet("m200,2") == "2222"
    # C166S V1 manual 3.3.5 (PDF p.53): EINIT protects guest SYSCON writes.
    write_memory(remote, 0xf600, "b54ab5b5")
    write_register(remote, "pc", "0000f600")
    assert remote.packet("s").startswith("T05")
    assert read_register(remote, "pc") == 0xf604
    assert read_register(remote, "guest_icount") == 1
    write_memory(remote, 0xff12, "0000")
    assert read_register(remote, "syscon") == 0x400
    assert remote.packet("m200,2") == "2222"
    write_register(remote, "syscon", "0000")
    assert read_register(remote, "syscon") == 0
    assert remote.packet("mff12,2") == "0000"
    assert remote.packet("m200,2") == "1111"


def pending_irq_after_psw(remote: Remote, kind: str) -> None:
    # Warm the continuation before raising a masked level-1 request. No debug
    # stop/patch is allowed between warming and the eligibility change.
    # C166S V1 PDF pp.55,59-60 and M166 p.99 define PSW priority and RETI.
    tail = {"reti": "e60900fbfb88",       # SP=FB00; RETI
            "register": "e6880008da000020",  # MOV PSW,#0800; CALLS
            "memory": "e6f00800f7f011ffda000020"}[kind]
    code = "da000020e6f10000e6f0c400f6f09aff" + tail
    write_memory(remote, 0xf400, code)
    write_memory(remote, 0x2000, "e6f13412db00")
    write_memory(remote, 0xa4, "cc00")
    write_memory(remote, 0xfb00, "002000000008")
    write_register(remote, "sp", "fbfc")
    write_register(remote, "psw", "7800")
    write_register(remote, "pc", "0000f400")
    remote.continue_to(0xa4)
    assert read_register(remote, "r1") == 0
    expected = 7 if kind == "register" else 8
    assert read_register(remote, "guest_icount") == expected


def main() -> int:
    qemu = Path(os.environ["QEMU_TEST_QEMU_BINARY"]).resolve()
    artifacts = Path(os.environ["MESON_BUILD_ROOT"]) / "pmb7850-functional"
    artifacts.mkdir(parents=True, exist_ok=True)
    cases = (("normal-interrupt", normal_interrupt),
             ("reti-cached-target", lambda r: pending_irq_after_psw(r, "reti")),
             ("psw-register", lambda r: pending_irq_after_psw(r, "register")),
             ("psw-memory", lambda r: pending_irq_after_psw(r, "memory")),
             ("interrupt-arbitration", interrupt_arbitration),
             ("pec-interrupt", pec_interrupt),
             ("pec-final-interrupt", pec_final_interrupt),
             ("software-reset", software_reset),
             ("debugger-syscon-mapping", debugger_syscon_mapping))

    print("TAP version 13")
    print(f"1..{len(cases)}")
    failed = 0
    for index, (name, body) in enumerate(cases, 1):
        try:
            run_case(qemu, artifacts, name, body)
            print(f"ok {index} - {name}")
        except Exception as exc:  # TAP must report the failed assertion.
            failed += 1
            print(f"not ok {index} - {name}")
            print(f"# {type(exc).__name__}: {exc}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
