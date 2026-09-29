#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Canonical Siemens C55 SW24 boot parity against the external CEMU oracle."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


def skip(reason: str) -> int:
    print("TAP version 13")
    print("1..1")
    print(f"ok 1 - Siemens C55 canonical boot # SKIP {reason}")
    return 0


def main() -> int:
    firmware = os.environ.get("PMB7850_C55_FIRMWARE")
    cemu = os.environ.get("PMB7850_CEMU")
    if not firmware or not cemu:
        return skip("set PMB7850_C55_FIRMWARE and PMB7850_CEMU")

    source = Path(__file__).resolve().parents[3]
    build = Path(os.environ["MESON_BUILD_ROOT"]).resolve()
    qemu = Path(os.environ["QEMU_TEST_QEMU_BINARY"]).resolve()
    python = os.environ.get("PMB7850_C55_PYTHON", sys.executable)
    artifacts = build / "c55-boot-parity"
    command = [
        python, str(source / "scripts/pmb7850/run_c55_boot.py"),
        "--qemu", str(qemu), "--cemu", cemu,
        "--firmware", firmware, "--artifacts", str(artifacts),
        "--timeout", "900", "--phase4",
    ]

    try:
        completed = subprocess.run(
            command, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=1800,
        )
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        completed = None
    else:
        output = completed.stdout

    print("TAP version 13")
    print("1..1")
    if completed is not None and completed.returncode == 0:
        print("ok 1 - Siemens C55 canonical boot and Phase 4 parity")
        return 0
    print("not ok 1 - Siemens C55 canonical boot and Phase 4 parity")
    for line in output.splitlines():
        print(f"# {line}")
    if completed is None:
        print("# host timeout after 1800 seconds")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
