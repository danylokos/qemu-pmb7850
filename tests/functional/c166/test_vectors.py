#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Meson/TAP entry point for the compiler-free C166 vector suite."""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path


def diagnostic(output: str) -> None:
    for line in output.splitlines():
        print(f"# {line}")


def main() -> int:
    source = Path(__file__).resolve().parents[3]
    build = Path(os.environ["MESON_BUILD_ROOT"]).resolve()
    qemu = Path(os.environ["QEMU_TEST_QEMU_BINARY"]).resolve()
    fixture_dir = build / "c166-fixtures"
    manifest = fixture_dir / "manifest.json"

    generate = subprocess.run(
        [
            sys.executable,
            str(source / "scripts/c166/gen_opcode_fixtures.py"),
            str(Path(__file__).with_name("opcodes.json")),
            str(fixture_dir),
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    vectors = subprocess.run(
        [
            sys.executable,
            str(Path(__file__).with_name("run_vectors.py")),
            "--qemu",
            str(qemu),
            "--manifest",
            str(manifest),
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    ) if generate.returncode == 0 else None

    disassembly_log = fixture_dir / "disassembly.log"
    disassembly = subprocess.run(
        [
            str(qemu), "-M", "c166-test", "-display", "none",
            "-kernel", str(fixture_dir / "disassembly_coverage.bin"),
            "-d", "in_asm", "-D", str(disassembly_log), "-no-reboot",
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=10,
    ) if vectors is not None and vectors.returncode == 0 else None

    expected_disassembly = (
        "mov r0, #0x1",
        "add r0, r1",
        "shl r0, #1",
        "atomic #1",
        "mov 0xe000, r4",
    )
    expected_disassembly += tuple(
        text
        for i in range(16)
        for reg in [f"r{'h' if i % 2 else 'l'}{i // 2}"]
        for text in (f"movb {reg}, #0x5", f"movb {reg}, #0x5a",
                     f"movb {reg}, {reg}", f"movb 0x3000, {reg}",
                     f"movb {reg}, 0x3000",
                     f"movbz r15, {reg}", f"movbs r15, {reg}")
    )
    disassembly_text = (
        disassembly_log.read_text(encoding="utf-8")
        if disassembly is not None and disassembly.returncode == 0
        else ""
    )
    passed = (
        generate.returncode == 0
        and vectors is not None
        and vectors.returncode == 0
        and disassembly is not None
        and disassembly.returncode == 0
        and all(re.search(r"^0x[0-9a-f]+:  " + re.escape(text) + r"$",
                          disassembly_text, re.MULTILINE)
                for text in expected_disassembly)
    )

    print("TAP version 13")
    print("1..1")
    if passed:
        print("ok 1 - C166 reset, GDB, execution, MMIO, "
              "and disassembly vectors")
        return 0
    print("not ok 1 - C166 reset, GDB, execution, MMIO, "
          "and disassembly vectors")
    diagnostic(generate.stdout)
    if vectors is not None:
        diagnostic(vectors.stdout)
    if disassembly is not None:
        diagnostic(disassembly.stdout)
    diagnostic(disassembly_text)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
