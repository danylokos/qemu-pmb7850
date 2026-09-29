#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate raw C166 opcode fixtures below the QEMU build directory."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    manifest_text = args.manifest.read_text(encoding="utf-8")
    if manifest_text.startswith("// SPDX-License-Identifier:"):
        manifest_text = manifest_text.partition("\n")[2]
    manifest = json.loads(manifest_text)
    if manifest.get("schema") != 1:
        parser.error("unsupported manifest schema")

    output = args.output.resolve()
    build = (Path(__file__).resolve().parents[2] / "build").resolve()
    if output != build and build not in output.parents:
        parser.error(f"output must be below {build}")
    output.mkdir(parents=True, exist_ok=True)

    names: set[str] = set()
    for vector in manifest["vectors"]:
        name = vector["name"]
        if name in names or not name.replace("_", "").isalnum():
            parser.error(f"invalid or duplicate vector name: {name}")
        names.add(name)
        try:
            code = bytes.fromhex(vector["code"])
        except ValueError as exc:
            parser.error(f"{name}: invalid code: {exc}")
        if not code or len(code) % 2:
            parser.error(f"{name}: code must contain whole 16-bit words")
        (output / f"{name}.bin").write_bytes(code)

    generated = dict(manifest)
    generated["fixture_dir"] = str(output)
    (output / "manifest.json").write_text(
        json.dumps(generated, indent=2) + "\n", encoding="utf-8"
    )
    print(f"generated {len(names)} fixtures in {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
