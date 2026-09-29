#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the canonical Siemens C55 image in QEMU and the frozen CEMU oracle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path


IMAGE_SIZE = 8 * 1024 * 1024
IMAGE_SHA256 = (
    "bcb893b6b3ae94031f319e2d387e34679055c02e9cd6024d218a514f09954d75"
)
FACTORY_UID = "000000004079A7E9"
CEMU_FSN = "A35F2F28"
IDLE_PC = 0x012A06
EXPECTED = {
    "pc": IDLE_PC,
    "csp": 0x01,
    "ip": 0x2A06,
    "psw": 0x0841,
    "sp": 0xFBF8,
    "cp": 0xFC00,
    "dpp0": 0x0000,
    "dpp1": 0x0005,
    "dpp2": 0x0040,
    "dpp3": 0x0003,
}
REGISTERS = {
    **{f"r{i}": i for i in range(16)},
    "dpp0": 16,
    "dpp1": 17,
    "dpp2": 18,
    "dpp3": 19,
    "cp": 20,
    "sp": 21,
    "stkov": 22,
    "stkun": 23,
    "mdh": 24,
    "mdl": 25,
    "mdc": 26,
    "psw": 27,
    "csp": 28,
    "ip": 29,
    "pc": 30,
    "ext_kind": 31,
    "ext_value": 32,
    "ext_count": 33,
    "extr": 34,
    "guest_icount": 35,
    "tfr": 36,
    "syscon": 37,
    "idle": 38,
}
STATE_NAMES = (
    *(f"r{i}" for i in range(16)),
    "pc", "csp", "ip", "psw", "sp", "cp",
    "dpp0", "dpp1", "dpp2", "dpp3", "idle", "guest_icount",
)
MEMORY_RANGES = {
    "dpram": (0x00F200, 0x0C00),
    "local-memory": (0x010000, 0x40000),
    "sram": (0x100000, 0x80000),
    "flash": (0x800000, IMAGE_SIZE),
}
CEMU_MEMORY = {
    "dpram": "snapshot/full-bins/00f200_00fdff_dpram.bin",
    "local-memory": "snapshot/full-bins/010000_04ffff_lm_segment1_window.bin",
    "sram": "snapshot/xram0_000000.bin",
    "flash": "flash.bin",
}

# These bytes are firmware-maintained time/entropy samples whose values depend
# on the calibrated instruction-to-peripheral timing ratio.  The ranges were
# measured at the first 0x012a06 stop; bytes outside them remain exact gates.
# The DPRAM interval is released stack below SP rather than live CPU state.
TIMING_CALIBRATED_RANGES = {
    "dpram": ((0x09B8, 0x09F8),),
    "local-memory": (
        (0x0C2C0, 0x0C2C2), (0x0FAD6, 0x0FAD9),
        (0x1726C, 0x1729C), (0x172A0, 0x172A2),
        (0x172A6, 0x172A7), (0x172AA, 0x172AB),
        (0x172AC, 0x172AD), (0x172B2, 0x172B4),
        (0x17326, 0x17327), (0x1732A, 0x1732E),
        (0x17332, 0x17333), (0x182C6, 0x182C7),
        (0x1A56C, 0x1A56E), (0x1A5D8, 0x1A5D9),
        (0x1A696, 0x1A697), (0x1A6B6, 0x1A6B8),
        (0x1B0D0, 0x1B0D2), (0x1B0DA, 0x1B0E0),
        (0x2BFFE, 0x2C000),
    ),
    "sram": (
        (0x1FFD0, 0x1FFD4), (0x1FFE8, 0x1FFEB),
        (0x31174, 0x31177), (0x34FDC, 0x34FE0),
        (0x37928, 0x3792F), (0x57364, 0x57367),
        (0x57946, 0x57949), (0x58054, 0x58057),
        (0x58058, 0x5805B), (0x58D3E, 0x58D41),
        (0x595A8, 0x595AA), (0x5A29C, 0x5A29E),
        (0x5A2BE, 0x5A2C0), (0x5A2E0, 0x5A2E2),
        (0x5A302, 0x5A304), (0x5BE7A, 0x5BE7D),
        (0x5F372, 0x5F373), (0x5F37E, 0x5F37F),
        (0x5F380, 0x5F381), (0x5F591, 0x5F592),
    ),
}


def checksum(payload: bytes) -> bytes:
    return f"{sum(payload) & 0xff:02x}".encode()


class Remote:
    def __init__(self, sock: socket.socket):
        self.sock = sock

    def receive(self) -> str:
        while True:
            marker = self.sock.recv(1)
            if marker == b"$":
                break
            if not marker:
                raise EOFError("GDB remote closed before a response")
        data = bytearray()
        while True:
            byte = self.sock.recv(1)
            if byte == b"#":
                break
            if not byte:
                raise EOFError("GDB remote closed in a response")
            data += byte
        received = self.sock.recv(2)
        if checksum(data) != received.lower():
            self.sock.sendall(b"-")
            raise RuntimeError("bad GDB remote checksum")
        self.sock.sendall(b"+")
        return data.decode()

    def packet(self, command: str) -> str:
        payload = command.encode()
        self.sock.sendall(b"$" + payload + b"#" + checksum(payload))
        if self.sock.recv(1) != b"+":
            raise RuntimeError(f"GDB remote rejected {command!r}")
        return self.receive()

    def continue_to(self, address: int, timeout: float) -> str:
        if self.packet(f"Z0,{address:x},2") != "OK":
            raise RuntimeError(f"cannot set breakpoint at 0x{address:06x}")
        old_timeout = self.sock.gettimeout()
        self.sock.settimeout(timeout)
        try:
            response = self.packet("c")
        finally:
            self.sock.settimeout(old_timeout)
        if self.packet(f"z0,{address:x},2") != "OK":
            raise RuntimeError(f"cannot remove breakpoint at 0x{address:06x}")
        return response

    def read_register(self, name: str) -> int:
        raw = bytes.fromhex(self.packet(f"p{REGISTERS[name]:x}"))
        return int.from_bytes(raw, "little")

    def read_memory(self, address: int, size: int) -> bytes:
        chunks = []
        while size:
            count = min(size, 4096)
            chunks.append(bytes.fromhex(self.packet(f"m{address:x},{count:x}")))
            address += count
            size -= count
        return b"".join(chunks)


class QMP:
    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.file = sock.makefile("rwb", buffering=0)
        greeting = self._receive()
        if "QMP" not in greeting:
            raise RuntimeError("QMP greeting is missing")
        self.execute("qmp_capabilities")

    def _receive(self) -> dict:
        while True:
            line = self.file.readline()
            if not line:
                raise EOFError("QMP socket closed")
            message = json.loads(line)
            if "event" not in message:
                return message

    def execute(self, command: str, arguments: dict | None = None):
        request = {"execute": command}
        if arguments:
            request["arguments"] = arguments
        self.file.write(json.dumps(request).encode() + b"\n")
        response = self._receive()
        if "error" in response:
            raise RuntimeError(f"QMP {command} failed: {response['error']}")
        return response.get("return")

    def close(self) -> None:
        self.file.close()


def connect(path: Path, deadline: float) -> socket.socket:
    while time.monotonic() < deadline:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.connect(str(path))
            sock.settimeout(5)
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            sock.close()
            time.sleep(0.01)
    raise TimeoutError(f"GDB socket did not appear: {path}")


def terminate(proc: subprocess.Popen) -> None:
    if proc.poll() is None:
        proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def png_rgb(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError(f"not a PNG: {path}")
    pos = 8
    payload = bytearray()
    width = height = depth = color = interlace = 0
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(
                ">IIBBBBB", chunk
            )
        elif kind == b"IDAT":
            payload += chunk
        elif kind == b"IEND":
            break
    if depth != 8 or color not in (2, 6) or interlace:
        raise RuntimeError(
            f"unsupported PNG format depth={depth} color={color} "
            f"interlace={interlace}: {path}"
        )
    channels = 3 if color == 2 else 4
    stride = width * channels
    packed = zlib.decompress(payload)
    rows = bytearray(height * stride)
    offset = 0
    previous = bytearray(stride)

    def paeth(a: int, b: int, c: int) -> int:
        estimate = a + b - c
        distances = abs(estimate - a), abs(estimate - b), abs(estimate - c)
        if distances[0] <= distances[1] and distances[0] <= distances[2]:
            return a
        return b if distances[1] <= distances[2] else c

    for y in range(height):
        method = packed[offset]
        raw = packed[offset + 1:offset + 1 + stride]
        offset += stride + 1
        row = bytearray(stride)
        for x, value in enumerate(raw):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            predictor = (0 if method == 0 else left if method == 1 else up
                         if method == 2 else (left + up) // 2 if method == 3
                         else paeth(left, up, upper_left) if method == 4
                         else None)
            if predictor is None:
                raise RuntimeError(f"invalid PNG filter {method}: {path}")
            row[x] = (value + predictor) & 0xff
        rows[y * stride:(y + 1) * stride] = row
        previous = row
    if channels == 3:
        return width, height, bytes(rows)
    rgb = bytearray(width * height * 3)
    for pixel in range(width * height):
        rgb[3 * pixel:3 * pixel + 3] = rows[4 * pixel:4 * pixel + 3]
    return width, height, bytes(rgb)


def visible_frame_digest(path: Path) -> int:
    width, height, rgb = png_rgb(path)
    if (width, height) != (404, 256):
        raise RuntimeError(f"strict C55 frame is {width}x{height}, not 404x256")
    digest = 14695981039346656037
    for y in range(64):
        for x in range(101):
            at = ((y * 4) * width + x * 4) * 3
            for value in rgb[at:at + 3]:
                digest ^= value
                digest = (digest * 1099511628211) & 0xffffffffffffffff
    return digest


def frame_digests(directory: Path) -> list[int]:
    frames = sorted(directory.glob("frame-*.png"))
    return [visible_frame_digest(path) for path in frames]


def preflight(path: Path) -> dict:
    data = path.read_bytes()
    actual = hashlib.sha256(data).hexdigest()
    if len(data) != IMAGE_SIZE:
        raise RuntimeError(
            f"firmware must be exactly 8 MiB, got {len(data)} bytes"
        )
    if actual != IMAGE_SHA256:
        raise RuntimeError(
            f"firmware SHA-256 {actual} != canonical {IMAGE_SHA256}"
        )
    return {"path": str(path), "size": len(data), "sha256": actual,
            "factory_uid": FACTORY_UID, "cemu_fsn": CEMU_FSN}


def parse_cemu_state(text: str) -> dict:
    state: dict[str, int] = {}
    patterns = {
        "pc": r"pc=0x([0-9a-fA-F]+)",
        "csp": r"csp=0x([0-9a-fA-F]+)",
        "ip": r"ip=0x([0-9a-fA-F]+)",
        "guest_icount": r"icount=([0-9]+)",
        "psw": r"psw=0x([0-9a-fA-F]+)",
        "sp": r"\bsp=0x([0-9a-fA-F]+)",
        "cp": r"\bcp=0x([0-9a-fA-F]+)",
    }
    for name, pattern in patterns.items():
        match = re.search(pattern, text)
        if not match:
            raise RuntimeError(f"CEMU state omits {name}")
        state[name] = int(match.group(1), 10 if name == "guest_icount" else 16)
    dpp = re.search(
        r"dpp=0x([0-9a-fA-F]+) 0x([0-9a-fA-F]+) "
        r"0x([0-9a-fA-F]+) 0x([0-9a-fA-F]+)", text
    )
    if not dpp:
        raise RuntimeError("CEMU state omits DPP registers")
    for index, value in enumerate(dpp.groups()):
        state[f"dpp{index}"] = int(value, 16)
    for name, value in re.findall(r"r(\d+)\s*=0x([0-9a-fA-F]+)", text):
        state[f"r{int(name)}"] = int(value, 16)
    state["idle"] = int(bool(re.search(r"\bidle=1\b", text)))
    missing = set(STATE_NAMES) - set(state)
    if missing:
        raise RuntimeError(f"CEMU state omits {', '.join(sorted(missing))}")
    return state


def run_cemu_state(cemu: Path, firmware: Path, artifacts: Path,
                   timeout: float, sim: bool = False) -> dict:
    command = [
        str(cemu), str(firmware), "--device", "c55", "--fsn", CEMU_FSN,
        "--limit", "9m" if sim else "8m", "-c",
        f"break 0x{IDLE_PC:06x}; cont; regs",
    ]
    if sim:
        command.insert(6, "--sim")
    completed = subprocess.run(command, cwd=artifacts, text=True,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=timeout)
    (artifacts / "cemu-state.log").write_text(completed.stdout,
                                               encoding="utf-8")
    if completed.returncode:
        raise RuntimeError(f"CEMU state run exited {completed.returncode}")
    if "[break]" not in completed.stdout:
        raise RuntimeError("CEMU did not reach the idle breakpoint")
    return parse_cemu_state(completed.stdout)


def run_cemu_artifacts(cemu: Path, firmware: Path, artifacts: Path,
                       stop_icount: int, timeout: float,
                       sim: bool = False) -> Path:
    command = [
        str(cemu), str(firmware), "--device", "c55", "--fsn", CEMU_FSN,
        "--limit", str(stop_icount), "--label", "oracle",
        "--trace", "flash,pec,tdma,xbus,serial,lifecycle" +
        (",sim" if sim else ""),
        "--snapshot-full-bins", "--dump-flash",
    ]
    if sim:
        command.insert(6, "--sim")
    completed = subprocess.run(command, cwd=artifacts, text=True,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=timeout)
    (artifacts / "cemu.log").write_text(completed.stdout, encoding="utf-8")
    if completed.returncode:
        raise RuntimeError(f"CEMU artifact run exited {completed.returncode}")
    run = artifacts / "shots" / "oracle"
    if not (run / "snapshot" / "snapshot.json").is_file():
        raise RuntimeError("CEMU did not produce its stable-idle snapshot")
    return run


def run_cemu_ui(cemu: Path, firmware: Path, artifacts: Path,
                label: str, limit: str, timeout: float,
                commands: str | None = None) -> list[int]:
    command = [
        str(cemu), str(firmware), "--device", "c55", "--fsn", CEMU_FSN,
        "--limit", limit, "--label", label, "--lcd-frames",
    ]
    if commands:
        command += ["-c", commands]
    completed = subprocess.run(command, cwd=artifacts, text=True,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=timeout)
    (artifacts / f"cemu-{label}.log").write_text(completed.stdout,
                                                  encoding="utf-8")
    if completed.returncode:
        raise RuntimeError(f"CEMU UI run {label} exited {completed.returncode}")
    lcd = artifacts / "shots" / label / "lcd"
    if not lcd.is_dir():
        raise RuntimeError(f"CEMU UI run {label} produced no LCD directory")
    return frame_digests(lcd)


def qmp_key(qmp: QMP, qcode: str, down: bool) -> None:
    qmp.execute("input-send-event", {"events": [{
        "type": "key",
        "data": {"down": down,
                 "key": {"type": "qcode", "data": qcode}},
    }]})


def run_qemu_ui(qemu: Path, firmware: Path, artifacts: Path,
                label: str, target_frames: int, timeout: float,
                icount_shift: int,
                softkey: bool = False) -> list[tuple[int, int]]:
    trace_path = artifacts / f"qemu-{label}.trace"
    log_path = artifacts / f"qemu-{label}.log"
    serial_path = artifacts / f"qemu-{label}.serial"
    for path in (trace_path, log_path, serial_path):
        path.unlink(missing_ok=True)
    deadline = time.monotonic() + timeout
    # AF_UNIX paths are limited to 107 bytes on Linux.  Retained matrix roots
    # are intentionally descriptive, so keep the transient control socket in
    # the system temporary directory and all evidence under artifacts.
    with tempfile.TemporaryDirectory(prefix="c55-qmp-") as socket_dir:
        qmp_path = Path(socket_dir) / "qmp.sock"
        command = [
            str(qemu), "-M", "siemens-c55", "-drive",
            f"if=pflash,format=raw,file={firmware}", "-snapshot", "-global",
            f"m58lw064d.factory-uid={FACTORY_UID}", "-icount",
            f"shift={icount_shift},align=off,sleep=off", "-display", "none",
            "-serial",
            f"file:{serial_path}", "-S", "-qmp",
            f"unix:{qmp_path},server=on,wait=off", "-no-reboot",
            "-trace", "enable=pmb7850_ssc",
            "-trace", "enable=pmb7850_pec",
            "-trace", "enable=pmb7850_adc_conversion",
            "-trace", "enable=pmb7850_irq",
            "-trace", "enable=pcf8813_command",
            "-trace", "enable=pcf8813_data",
            "-trace", "enable=pcf8813_transaction",
            "-trace", "enable=pcf8813_frame",
            "-trace", "enable=siemens_keypad_access",
            "-trace", "enable=siemens_keypad_input",
            "-D", str(trace_path),
        ]
        with log_path.open("wb") as log:
            proc = subprocess.Popen(command, cwd=artifacts,
                                    stdout=subprocess.DEVNULL, stderr=log)
            try:
                with connect(qmp_path, time.monotonic() + 5) as qmp_sock:
                    qmp = QMP(qmp_sock)
                    observed_count = 0

                    def wait_for_count(count: int, prop: str,
                                       precise: bool = False) -> None:
                        nonlocal observed_count
                        while observed_count < count:
                            if time.monotonic() >= deadline:
                                raise TimeoutError(
                                    f"QEMU {label} reached {observed_count} "
                                    f"for {prop}"
                                )
                            observed_count = qmp.execute("qom-get", {
                                "path": "/machine/display",
                                "property": prop,
                            })
                            # Long gates use sparse polling so QMP does not
                            # starve the vCPU.  Key injection needs the exact
                            # logical repaint boundary, so poll that short
                            # interval tightly.
                            time.sleep(0.001 if precise else 0.1)

                    qmp.execute("cont")
                    if softkey:
                        wait_for_count(23, "transaction-count", precise=True)
                        qmp_key(qmp, "f1", True)
                        wait_for_count(39, "transaction-count", precise=True)
                        qmp_key(qmp, "f1", False)
                        # The release does not itself redraw.  Let the vCPU
                        # consume the edge, then require its trace sample.
                        time.sleep(1)
                    else:
                        wait_for_count(target_frames, "frame-count")
                    qmp.execute("stop")
                    qmp.close()
            finally:
                terminate(proc)
    if serial_path.read_bytes():
        raise RuntimeError(f"QEMU {label} emitted unexpected ASC0 output")
    pattern = re.compile(
        r"pcf8813_frame sequence=(\d+) data-bytes=(\d+) "
        r"digest=0x([0-9a-f]+)"
    )
    frames = []
    for match in pattern.finditer(trace_path.read_text(encoding="utf-8")):
        sequence, data_bytes, digest = match.groups()
        data_bytes = int(data_bytes)
        if (int(sequence) != len(frames) + 1 or
                data_bytes not in (714, 816)):
            raise RuntimeError(
                f"QEMU {label} malformed frame {match.group(0)}"
            )
        frames.append((int(digest, 16), data_bytes))
    if len(frames) < target_frames:
        raise RuntimeError(
            f"QEMU {label} trace has {len(frames)}, expected {target_frames}"
        )
    return frames[:target_frames]


def run_qemu(qemu: Path, firmware: Path, artifacts: Path,
             timeout: float, icount_shift: int,
             one_insn_per_tb: bool,
             sim: bool = False) -> tuple[dict, dict[str, Path]]:
    # Linux limits AF_UNIX paths to 107 bytes.  Matrix artifact roots are
    # deliberately descriptive, so keep this transient control socket short.
    sock_path = Path("/tmp") / (
        f"pmb7850-c55-gdb-{os.getpid()}-{time.monotonic_ns()}.sock"
    )
    trace_path = artifacts / "qemu.trace"
    serial_path = artifacts / "qemu.serial"
    log_path = artifacts / "qemu.log"
    for path in (sock_path, trace_path, serial_path, log_path):
        path.unlink(missing_ok=True)
    command = [
        str(qemu), "-M", "siemens-c55", "-drive",
        f"if=pflash,format=raw,file={firmware}", "-snapshot", "-global",
        f"m58lw064d.factory-uid={FACTORY_UID}", "-icount",
        f"shift={icount_shift},align=off,sleep=off", "-display", "none",
        "-serial",
        f"file:{serial_path}", "-S", "-gdb",
        f"unix:{sock_path},server=on,wait=off", "-no-reboot",
        "-trace", "enable=m58lw064d_command",
        "-trace", "enable=m58lw064d_program",
        "-trace", "enable=m58lw064d_erase",
        "-trace", "enable=pmb7850_fast_cs0",
        "-trace", "enable=pmb7850_pec",
        "-trace", "enable=pmb7850_tdma_irq",
        "-trace", "enable=pmb7850_serial",
        "-trace", "enable=pmb7850_ssc",
        "-trace", "enable=pmb7850_adc_conversion",
        "-trace", "enable=pcf8813_command",
        "-trace", "enable=pcf8813_data",
        "-trace", "enable=pcf8813_frame",
        "-trace", "enable=siemens_keypad_access",
        "-trace", "enable=siemens_keypad_input",
        "-trace", "enable=siemens_xbus_mailbox",
        "-D", str(trace_path),
    ]
    if sim:
        command[1:1] = [
            "-global", "pmb7850-soc.sim-profile=deterministic",
            "-trace", "enable=pmb7850_sim_*",
        ]
    if one_insn_per_tb:
        command[1:1] = ["-accel", "tcg,one-insn-per-tb=on"]
    memory_paths: dict[str, Path] = {}
    with log_path.open("wb") as log:
        proc = subprocess.Popen(command, cwd=artifacts,
                                stdout=subprocess.DEVNULL, stderr=log)
        try:
            with connect(sock_path, time.monotonic() + 5) as sock:
                remote = Remote(sock)
                if not remote.packet("?").startswith(("S", "T")):
                    raise RuntimeError("QEMU did not report its initial stop")
                stop = remote.continue_to(IDLE_PC, timeout)
                if not stop.startswith(("S", "T")):
                    raise RuntimeError(f"unexpected QEMU stop: {stop}")
                state = {name: remote.read_register(name)
                         for name in STATE_NAMES}
                if state["pc"] != IDLE_PC:
                    raise RuntimeError(
                        f"QEMU stopped at 0x{state['pc']:06x}, not sync PC"
                    )
                for name, (address, size) in MEMORY_RANGES.items():
                    path = artifacts / f"qemu-{name}.bin"
                    path.write_bytes(remote.read_memory(address, size))
                    memory_paths[name] = path
        finally:
            terminate(proc)
            sock_path.unlink(missing_ok=True)
    if serial_path.read_bytes():
        raise RuntimeError("QEMU emitted unexpected ASC0 serial output")
    return state, memory_paths


def first_difference(left: bytes, right: bytes) -> int | None:
    for index, (a, b) in enumerate(zip(left, right)):
        if a != b:
            return index
    return min(len(left), len(right)) if len(left) != len(right) else None


def compare_memory(qemu_paths: dict[str, Path], cemu_run: Path,
                   strict: bool = True) -> dict:
    result = {}
    for name, qemu_path in qemu_paths.items():
        cemu_path = cemu_run / CEMU_MEMORY[name]
        qemu_data = qemu_path.read_bytes()
        cemu_data = cemu_path.read_bytes()
        raw_differences = [
            offset for offset, values in enumerate(zip(qemu_data, cemu_data))
            if values[0] != values[1]
        ]
        calibrated = TIMING_CALIBRATED_RANGES.get(name, ())
        unexpected = [
            offset for offset in raw_differences
            if not any(start <= offset < end for start, end in calibrated)
        ]
        result[name] = {
            "qemu_sha256": hashlib.sha256(qemu_data).hexdigest(),
            "cemu_sha256": hashlib.sha256(cemu_data).hexdigest(),
            "bytes": len(qemu_data),
            "raw_difference_count": len(raw_differences),
            "first_raw_difference": raw_differences[0]
                if raw_differences else None,
            "calibrated_difference_count":
                len(raw_differences) - len(unexpected),
            "first_difference": unexpected[0] if unexpected else None,
            "first_qemu_value": qemu_data[unexpected[0]]
                if unexpected else None,
            "first_cemu_value": cemu_data[unexpected[0]]
                if unexpected else None,
        }
        if unexpected and strict:
            difference = unexpected[0]
            raise RuntimeError(
                f"{name} first differs at offset 0x{difference:x}: "
                f"QEMU=0x{qemu_data[difference]:02x} "
                f"CEMU=0x{cemu_data[difference]:02x}"
            )
    return result


def parse_qemu_effects(path: Path) -> dict[str, list]:
    effects: dict[str, list] = {
        "flash": [], "pec": [], "tdma": [], "xbus": [],
        "sim_control": [], "sim_irq": [], "sim_byte": [], "sim_apdu": [],
    }
    command = re.compile(
        r"m58lw064d_command .* view=native offset=0x([0-9a-f]+) "
        r"size=(\d+) value=0x([0-9a-f]+)"
    )
    pec = re.compile(
        r"pmb7850_pec .* channel=(\d+) src=0x([0-9a-f]+) "
        r"dst=0x([0-9a-f]+) size=(\d+) value=0x([0-9a-f]+) "
        r"count=(\d+)->(\d+)"
    )
    tdma = re.compile(
        r"pmb7850_tdma_irq .* (XP[01]) compare=0x([0-9a-f]+) "
        r"counter=0x([0-9a-f]+) already-pending=(\d+)"
    )
    xbus = re.compile(
        r"siemens_xbus_mailbox .* phase=([^ ]+) transaction=(\d+) "
        r"status=0x([0-9a-f]+) result=0x([0-9a-f]+) irq=(\d+)"
    )
    sim_control = re.compile(
        r"pmb7850_sim_control .* before=0x([0-9a-f]+) "
        r"control=0x([0-9a-f]+)"
    )
    sim_irq = re.compile(
        r"pmb7850_sim_irq .* addr=0x([0-9a-f]+) already-pending=(\d+)"
    )
    sim_byte = re.compile(
        r"pmb7850_sim_byte .* direction=([^ ]+) value=0x([0-9a-f]+) "
        r"sequence=(\d+)"
    )
    sim_apdu = re.compile(
        r"pmb7850_sim_apdu .* command=0x([0-9a-f]+) "
        r"selection=0x([0-9a-f]+) status=0x([0-9a-f]+) response=(\d+)"
    )
    for line in path.read_text(encoding="utf-8").splitlines():
        match = command.search(line)
        if match:
            effects["flash"].append(tuple(
                int(value, 16 if index != 1 else 10)
                for index, value in enumerate(match.groups())
            ))
            continue
        match = pec.search(line)
        if match:
            values = match.groups()
            effects["pec"].append((int(values[0]), *(int(v, 16)
                for v in values[1:5]), int(values[5]), int(values[6])))
            continue
        match = tdma.search(line)
        if match:
            effects["tdma"].append((64 if match.group(1) == "XP0" else 65,
                                    int(match.group(2), 16),
                                    int(match.group(3), 16),
                                    int(match.group(4))))
            continue
        match = xbus.search(line)
        if match:
            effects["xbus"].append((match.group(1), int(match.group(2)),
                                    int(match.group(3), 16),
                                    int(match.group(4), 16),
                                    int(match.group(5))))
            continue
        match = sim_control.search(line)
        if match:
            effects["sim_control"].append(tuple(
                int(value, 16) for value in match.groups()
            ))
            continue
        match = sim_irq.search(line)
        if match:
            effects["sim_irq"].append((int(match.group(1), 16),
                                       int(match.group(2))))
            continue
        match = sim_byte.search(line)
        if match:
            effects["sim_byte"].append((match.group(1),
                                        int(match.group(2), 16),
                                        int(match.group(3))))
            continue
        match = sim_apdu.search(line)
        if match:
            apdu_command = int(match.group(1), 16)
            selection = int(match.group(2), 16)
            requested = ((apdu_command >> 40) & 0xffff
                         if apdu_command & (1 << 56) else None)
            effects["sim_apdu"].append((
                (apdu_command >> 32) & 0xff,
                (apdu_command >> 24) & 0xff,
                (apdu_command >> 16) & 0xff,
                (apdu_command >> 8) & 0xff,
                apdu_command & 0xff, requested,
                (selection >> 48) & 0xffff,
                (selection >> 32) & 0xffff,
                (selection >> 16) & 0xffff, selection & 0xffff,
                int(match.group(3), 16), int(match.group(4)),
            ))
    return effects


def read_cemu_effects(cemu_run: Path) -> dict[str, list]:
    try:
        import duckdb
    except ImportError as exc:
        raise RuntimeError(
            "ordered effect comparison requires the Python duckdb package"
        ) from exc
    root = cemu_run / "trace" / "trace.parquet"
    con = duckdb.connect()

    def rows(kind: str, columns: str) -> list:
        path = root / f"kind={kind}" / "part-00000.parquet"
        if not path.exists():
            return []
        return [tuple(row) for row in con.execute(
            f"SELECT {columns} FROM read_parquet(?) ORDER BY seq", [str(path)]
        ).fetchall()]

    result = {
        "flash": rows("flash_command", "info_chip_offset_i64,size,value"),
        "pec": rows("pec_transfer",
                    "info_channel_i64,info_src_i64,info_dst_i64,"
                    "size,value,info_count_before_i64,info_count_after_i64"),
        "tdma": rows("tdma_irq",
                     "info_trap_i64,info_compare_i64,info_counter_i64,"
                     "CAST(info_already_pending_bool AS INTEGER)"),
        "xbus": rows("xbus_mailbox_complete",
                     "info_phase_str,info_transaction_id_i64,value,"
                     "info_result_i64,CAST(info_irq_asserted_bool AS INTEGER)"),
        "sim_control": rows("sim_control", "info_before_i64,value"),
        "sim_irq": rows(
            "sim_irq", "addr,CAST(info_already_pending_bool AS INTEGER)"),
        "sim_byte": rows(
            "sim_byte", "info_direction_str,value,info_sequence_i64"),
        "sim_apdu": rows(
            "sim_apdu", "info_cla_i64,info_ins_i64,info_p1_i64,"
            "info_p2_i64,info_p3_i64,info_requested_file_i64,"
            "info_selected_df_before_i64,info_selected_ef_before_i64,"
            "info_selected_df_i64,info_selected_ef_i64,"
            "info_status_word_i64,info_response_length_i64"),
    }
    con.close()
    return result


def compare_effects(qemu: dict[str, list], cemu: dict[str, list],
                    strict: bool = True) -> dict:
    result = {}
    for name in qemu:
        raw_left = qemu[name]
        raw_right = cemu[name]
        # XP1/XP0 form one periodic TDMA cycle.  A breakpoint may split that
        # pair, so compare complete cycles and report the trailing event.
        if name == "tdma":
            left = raw_left[:len(raw_left) // 2 * 2]
            right = raw_right[:len(raw_right) // 2 * 2]
        else:
            left = raw_left
            right = raw_right
        difference = next((index for index, pair in enumerate(zip(left, right))
                           if pair[0] != pair[1]), None)
        if difference is None and len(left) != len(right):
            difference = min(len(left), len(right))
        result[name] = {
            "qemu_count": len(left), "cemu_count": len(right),
            "qemu_raw_count": len(raw_left), "cemu_raw_count": len(raw_right),
            "first_difference": difference,
            "qemu_value": left[difference]
                if difference is not None and difference < len(left) else None,
            "cemu_value": right[difference]
                if difference is not None and difference < len(right) else None,
        }
        if difference is not None and strict:
            qvalue = left[difference] if difference < len(left) else None
            cvalue = right[difference] if difference < len(right) else None
            raise RuntimeError(
                f"{name} effect {difference} differs: QEMU={qvalue!r} "
                f"CEMU={cvalue!r}"
            )
    return result


def sim_pec_effects(effects: list[tuple]) -> list[tuple]:
    """Retain PEC3 transfers whose source or destination is the SIM FIFO."""
    return [effect for effect in effects
            if effect[0] == 3 and
            ((effect[1] & 0xffff) in (0xef58, 0xef5a) or
             (effect[2] & 0xffff) in (0xef58, 0xef5a))]


def compare_state(qemu: dict, cemu: dict, strict: bool = True) -> dict:
    result = {}
    for name in (*EXPECTED, *(f"r{i}" for i in range(16))):
        if qemu[name] != cemu[name]:
            result[name] = {"qemu": qemu[name], "cemu": cemu[name]}
            if strict:
                raise RuntimeError(
                    f"CPU {name} differs: QEMU=0x{qemu[name]:x} "
                    f"CEMU=0x{cemu[name]:x}"
                )
    for name, expected in EXPECTED.items():
        if qemu[name] != expected:
            result.setdefault(name, {})["expected"] = expected
            if strict:
                raise RuntimeError(
                    f"CPU {name}=0x{qemu[name]:x}, expected 0x{expected:x}"
                )
    return result


def validate_softkey_trace(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    edges = re.findall(
        r"siemens_keypad_input .* name=soft-left raw=0x010e "
        r"logical=0x01 pressed=(\d)", text
    )
    matrix = re.findall(
        r"siemens_keypad_access .* command=0x00ed result=0x000e", text
    )
    irq = re.findall(r"pmb7850_irq .* vector=0x37 ", text)
    if edges[:2] != ["1", "0"]:
        raise RuntimeError(f"soft-left edges differ: {edges[:2]}")
    if not matrix:
        raise RuntimeError("soft-left never produced ED -> 000E")
    if not irq:
        raise RuntimeError("soft-left never selected IRQ55/vector 0x37")
    return {"edges": len(edges), "matrix_ed_000e": len(matrix),
            "irq55": len(irq)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True, type=Path)
    parser.add_argument("--cemu", required=True, type=Path)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--icount-shift", type=int, choices=range(11),
                        default=0,
                        help="fixed QEMU icount shift (0..10)")
    parser.add_argument("--comparison-mode", choices=("strict", "probe"),
                        default="strict",
                        help="report parity differences instead of failing")
    parser.add_argument("--one-insn-per-tb", action="store_true",
                        help="diagnostic TCG mode for translation comparison")
    parser.add_argument("--phase4", action="store_true",
                        help="also run display and left-softkey parity gates")
    parser.add_argument("--sim", action="store_true",
                        help="run the deterministic SIM controller/card gate")
    args = parser.parse_args()
    if args.phase4 and args.sim:
        parser.error("--phase4 and --sim are separate bounded gates")
    args.qemu = args.qemu.resolve()
    args.cemu = args.cemu.resolve()
    args.firmware = args.firmware.resolve()
    args.artifacts = args.artifacts.resolve()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    for name in ("shots",):
        path = args.artifacts / name
        if path.exists():
            shutil.rmtree(path)

    strict = args.comparison_mode == "strict"
    result = {
        "preflight": preflight(args.firmware),
        "configuration": {
            "icount_shift": args.icount_shift,
            "comparison_mode": args.comparison_mode,
            "one_insn_per_tb": args.one_insn_per_tb,
            "sim": args.sim,
        },
    }
    try:
        cemu_state = run_cemu_state(args.cemu, args.firmware,
                                    args.artifacts, args.timeout, args.sim)
        cemu_run = run_cemu_artifacts(
            args.cemu, args.firmware, args.artifacts,
            cemu_state["guest_icount"], args.timeout, args.sim,
        )
        qemu_state, qemu_memory = run_qemu(args.qemu, args.firmware,
                                           args.artifacts, args.timeout,
                                           args.icount_shift,
                                           args.one_insn_per_tb, args.sim)
        result["cpu"] = {"qemu": qemu_state, "cemu": cemu_state}
        result["cpu"]["differences"] = compare_state(
            qemu_state, cemu_state, strict)
        result["memory"] = compare_memory(
            qemu_memory, cemu_run, strict and not args.sim)
        qemu_effects = parse_qemu_effects(args.artifacts / "qemu.trace")
        cemu_effects = read_cemu_effects(cemu_run)
        if args.sim:
            qemu_effects["sim_pec"] = sim_pec_effects(qemu_effects["pec"])
            cemu_effects["sim_pec"] = sim_pec_effects(cemu_effects["pec"])
            sim_gate = (
                "flash", "xbus", "sim_control", "sim_irq", "sim_byte",
                "sim_apdu", "sim_pec",
            )
            qemu_effects = {name: qemu_effects[name] for name in sim_gate}
            cemu_effects = {name: cemu_effects[name] for name in sim_gate}
        result["effects"] = compare_effects(
            qemu_effects, cemu_effects, strict)
        if args.phase4:
            cemu_20 = run_cemu_ui(args.cemu, args.firmware, args.artifacts,
                                  "oracle-ui-20m", "20m", args.timeout)
            cemu_100 = run_cemu_ui(args.cemu, args.firmware, args.artifacts,
                                   "oracle-ui-100m", "100m", args.timeout)
            cemu_key = run_cemu_ui(
                args.cemu, args.firmware, args.artifacts,
                "oracle-ui-key", "24m", args.timeout,
                "c 20000000; key soft-left down; c 2000000; "
                "key soft-left up; c 2000000",
            )
            qemu_100 = run_qemu_ui(args.qemu, args.firmware, args.artifacts,
                                   "ui-100m", 12, args.timeout,
                                   args.icount_shift)
            qemu_key = run_qemu_ui(args.qemu, args.firmware, args.artifacts,
                                   "ui-key", 5, args.timeout,
                                   args.icount_shift, softkey=True)
            cemu_counts = (len(cemu_20), len(cemu_100), len(cemu_key))
            if cemu_counts != (2, 12, 5):
                raise RuntimeError(
                    "CEMU UI frame counts differ: "
                    f"20M={cemu_counts[0]} 100M={cemu_counts[1]} "
                    f"key={cemu_counts[2]}"
                )
            qemu_100_digests = [digest for digest, _ in qemu_100]
            qemu_100_bytes = [data_bytes for _, data_bytes in qemu_100]
            qemu_key_digests = [digest for digest, _ in qemu_key]
            qemu_key_bytes = [data_bytes for _, data_bytes in qemu_key]
            frame_differences = {
                "20m": first_difference(qemu_100_digests[:len(cemu_20)],
                                         cemu_20),
                "100m": first_difference(qemu_100_digests, cemu_100),
                "softkey": first_difference(
                    qemu_key_digests[:len(cemu_key)], cemu_key),
            }
            for name, index in frame_differences.items():
                if index is not None and strict:
                    raise RuntimeError(f"{name} frame {index + 1} differs")
            result["ui"] = {
                "frames_20m": len(cemu_20),
                "frames_100m": len(qemu_100),
                "softkey_frames": len(qemu_key),
                "frame_data_bytes_100m": qemu_100_bytes,
                "frame_data_bytes_softkey": qemu_key_bytes,
                "cemu_logical_frames_20m": len(cemu_20),
                "cemu_logical_frames_100m": len(cemu_100),
                "cemu_logical_frames_softkey": len(cemu_key),
                "softkey": validate_softkey_trace(
                    args.artifacts / "qemu-ui-key.trace"
                ),
                "emergency_dialog_digest": f"{qemu_key[-1][0]:016x}",
                "first_differences": frame_differences,
            }
        parity = (
            not result["cpu"]["differences"] and
            (args.sim or all(item["first_difference"] is None
                             for item in result["memory"].values())) and
            all(item["first_difference"] is None
                for item in result["effects"].values()) and
            (not args.phase4 or
             all(value is None
                 for value in result["ui"]["first_differences"].values()))
        )
        result["parity"] = parity
        result["status"] = "pass" if strict else "probe"
    except Exception as exc:
        result["status"] = "fail"
        result["error"] = f"{type(exc).__name__}: {exc}"
        (args.artifacts / "result.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        raise
    (args.artifacts / "result.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"{result['status'].upper()} siemens-c55 sync PC 0x{IDLE_PC:06x}")
    print(f"artifacts: {args.artifacts}")
    return 0


if __name__ == "__main__":
    from run_x55_boot import legacy_c55_main

    raise SystemExit(legacy_c55_main())
