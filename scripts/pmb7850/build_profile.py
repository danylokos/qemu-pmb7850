#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Configure, build, and validate the supported C166 QEMU profiles."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


QEMU_ROOT = Path(__file__).resolve().parents[2]
BUILD_ROOT = QEMU_ROOT / "build"
PROFILES = ("debug", "release")
COMMON_CONFIGURE = (
    "--target-list=c166-softmmu",
    "--disable-download",
    "--disable-docs",
    "--enable-werror",
)
DEBUG_OPTIONS = {
    "optimization": "0",
    "debug": True,
    "b_lto": False,
    "b_ndebug": "false",
    "qom_cast_debug": True,
    "debug_tcg": True,
    "debug_mutex": True,
    "debug_graph_lock": True,
}
RELEASE_OPTIONS = {
    "optimization": "3",
    "debug": False,
    "b_lto": True,
    "b_ndebug": "false",
    "qom_cast_debug": False,
    "debug_tcg": False,
    "debug_mutex": False,
    "debug_graph_lock": False,
    "b_pgo": "off",
    "c_args": [],
}
TARGETS = {
    "debug": (
        "qemu-system-c166",
        "tests/qtest/pmb7850-soc-test",
        "tests/qtest/siemens-c55-test",
        "tests/qtest/c166-x55-test",
    ),
    "release": ("qemu-system-c166",),
}


class ProfileError(RuntimeError):
    pass


def check_layout() -> None:
    if not BUILD_ROOT.exists():
        raise ProfileError(f"missing build root: {BUILD_ROOT}")
    unexpected = sorted(
        path.name for path in BUILD_ROOT.iterdir() if path.name not in PROFILES
    )
    missing = [name for name in PROFILES if not (BUILD_ROOT / name).is_dir()]
    if unexpected or missing:
        details = []
        if unexpected:
            details.append("unexpected entries: " + ", ".join(unexpected))
        if missing:
            details.append("missing directories: " + ", ".join(missing))
        raise ProfileError(
            "invalid qemu/build layout (" + "; ".join(details) + ")"
        )


def meson_command(build_dir: Path) -> Path:
    command = build_dir / "pyvenv" / "bin" / "meson"
    if not command.is_file():
        raise ProfileError(f"Meson is missing from configured tree: {command}")
    return command


def configure(profile: str) -> None:
    build_dir = BUILD_ROOT / profile
    build_dir.mkdir(parents=True, exist_ok=True)
    command = [str(QEMU_ROOT / "configure"), *COMMON_CONFIGURE]
    if os.environ.get("QEMU_PROFILE_PYTHON"):
        command.append("--python=" + os.environ["QEMU_PROFILE_PYTHON"])
    if profile == "debug":
        command.append("--enable-debug")
    subprocess.run(command, cwd=build_dir, check=True)

    options = DEBUG_OPTIONS if profile == "debug" else RELEASE_OPTIONS
    meson = meson_command(build_dir)
    values = []
    for name, value in options.items():
        if isinstance(value, bool):
            value = "true" if value else "false"
        elif isinstance(value, list):
            value = ""
        values.append(f"-D{name}={value}")
    subprocess.run([str(meson), "configure", ".", *values],
                   cwd=build_dir, check=True)
    validate(profile)


def load_options(build_dir: Path) -> dict[str, object]:
    path = build_dir / "meson-info" / "intro-buildoptions.json"
    try:
        options = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ProfileError(
            f"cannot read Meson options from {path}: {exc}"
        ) from exc
    return {option["name"]: option["value"] for option in options}


def validate(profile: str) -> None:
    build_dir = BUILD_ROOT / profile
    actual = load_options(build_dir)
    expected = DEBUG_OPTIONS if profile == "debug" else RELEASE_OPTIONS
    mismatches = []
    for name, wanted in expected.items():
        got = actual.get(name)
        if got != wanted:
            mismatches.append(f"{name}={got!r} (expected {wanted!r})")
    if mismatches:
        raise ProfileError(
            f"invalid {profile} profile: " + ", ".join(mismatches)
        )


def build(profile: str) -> None:
    validate(profile)
    subprocess.run(["ninja", *TARGETS[profile]], cwd=BUILD_ROOT / profile,
                   check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("configure", "build", "check"))
    parser.add_argument("profile", nargs="?", choices=PROFILES)
    args = parser.parse_args()

    try:
        check_layout()
        if args.action == "check":
            if args.profile:
                validate(args.profile)
        elif not args.profile:
            parser.error(f"{args.action} requires a profile")
        elif args.action == "configure":
            configure(args.profile)
        else:
            build(args.profile)
    except (ProfileError, subprocess.CalledProcessError) as exc:
        print(f"build-profile: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
