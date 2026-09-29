#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare bounded C166 sequences with an externally executed CEMU oracle."""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import time
from pathlib import Path

from run_vectors import REGISTERS, Remote, connect, expected_remote_hex


CASES = (
    {
        "name": "alu_shift_memory",
        "code": "e6f03412e6f1111100015c10f6f00020",
        "steps": 5,
        "touch": {"002000": 2},
    },
    {
        "name": "extended_addressing",
        "code": "dc57f2f41000f2f51200",
        "steps": 3,
        "initial": {"r7": "0010"},
        "memory": {"040010": "1111", "040012": "2222"},
        "touch": {"040010": 4},
    },
    {
        "name": "far_call_return",
        "code": "da010001",
        "steps": 3,
        "memory": {"010100": "e6f2efbedb00"},
        "touch": {"00fbfc": 4},
    },
    {
        "name": "multiply_state",
        "code": "1b01",
        "steps": 1,
        "initial": {"r0": "1000", "r1": "0010"},
        "touch": {},
    },
)

STATE_REGISTERS = (
    "pc", "csp", "ip", "cp", "sp", "psw", "mdh", "mdl", "mdc",
    "ext_kind", "ext_value", "ext_count", "extr", "guest_icount",
)


def remote_value(remote: Remote, name: str) -> int:
    encoded = remote.packet(f"p{REGISTERS[name]:x}")
    return int.from_bytes(bytes.fromhex(encoded), "little")


def run_qemu(qemu: Path, case: dict, artifacts: Path) -> dict:
    fixture = artifacts / f"{case['name']}.bin"
    fixture.write_bytes(bytes.fromhex(case["code"]))
    sock_path = artifacts / f"gdb-{case['name']}.sock"
    sock_path.unlink(missing_ok=True)
    command = [
        str(qemu), "-M", "c166-test", "-display", "none", "-S",
        "-gdb", f"unix:{sock_path},server=on,wait=off",
        "-kernel", str(fixture), "-no-reboot",
    ]
    log = artifacts / f"{case['name']}.qemu.log"
    with log.open("wb") as stderr:
        proc = subprocess.Popen(
            command, stdout=subprocess.DEVNULL, stderr=stderr
        )
        try:
            with connect(sock_path, time.monotonic() + 5) as sock:
                remote = Remote(sock)
                remote.packet("?")
                for name, value in case.get("initial", {}).items():
                    response = remote.packet(
                        f"P{REGISTERS[name]:x}={expected_remote_hex(value)}"
                    )
                    if response != "OK":
                        raise RuntimeError(f"cannot initialize {name}")
                for address, value in case.get("memory", {}).items():
                    response = remote.packet(
                        f"M{int(address, 16):x},{len(value) // 2:x}:{value}"
                    )
                    if response != "OK":
                        raise RuntimeError(f"cannot initialize {address}")
                for _ in range(case["steps"]):
                    remote.step()
                state = {name: remote_value(remote, name)
                         for name in STATE_REGISTERS}
                state["gpr"] = [remote_value(remote, f"r{i}")
                                for i in range(16)]
                state["dpp"] = [remote_value(remote, f"dpp{i}")
                                for i in range(4)]
                state["memory"] = {}
                for address, length in case["touch"].items():
                    state["memory"][address] = remote.packet(
                        f"m{int(address, 16):x},{length:x}"
                    )
                return state
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            sock_path.unlink(missing_ok=True)


def run_cemu(oracle: Path, case: dict) -> dict:
    command = [str(oracle), case["code"], str(case["steps"])]
    command.extend(
        f"{name}={value}" for name, value in case.get("initial", {}).items()
    )
    command.extend(
        f"mem{address}={value}"
        for address, value in case.get("memory", {}).items()
    )
    command.extend(
        f"touch={address}:{length}"
        for address, length in case["touch"].items()
    )
    completed = subprocess.run(
        command, check=True, text=True, stdout=subprocess.PIPE
    )
    return json.loads(completed.stdout)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True, type=Path)
    parser.add_argument("--cemu", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    args = parser.parse_args()
    args.artifacts.mkdir(parents=True, exist_ok=True)

    failures = 0
    for case in CASES:
        qemu_state = run_qemu(args.qemu.resolve(), case, args.artifacts)
        cemu_state = run_cemu(args.cemu.resolve(), case)
        result = {
            "case": case["name"],
            "qemu": qemu_state,
            "cemu": cemu_state,
        }
        (args.artifacts / f"{case['name']}.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        if qemu_state == cemu_state:
            print(f"PASS {case['name']}")
        else:
            failures += 1
            differing = sorted(
                key for key in qemu_state if qemu_state[key] != cemu_state[key]
            )
            print(f"FAIL {case['name']}: {', '.join(differing)}")
    print(f"{len(CASES)} sequences, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
