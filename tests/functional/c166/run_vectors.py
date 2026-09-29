#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise C166 raw vectors through QEMU's GDB remote protocol."""

from __future__ import annotations

import argparse
import hashlib
import json
import socket
import subprocess
import time
from pathlib import Path


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


def checksum(payload: bytes) -> bytes:
    return f"{sum(payload) & 0xff:02x}".encode()


class Remote:
    def __init__(self, sock: socket.socket):
        self.sock = sock

    def send_command(self, text: str) -> None:
        payload = text.encode()
        self.sock.sendall(b"$" + payload + b"#" + checksum(payload))
        if self.sock.recv(1) != b"+":
            raise RuntimeError(f"remote rejected packet {text!r}")

    def receive(self) -> str:
        while True:
            marker = self.sock.recv(1)
            if marker == b"$":
                break
            if not marker:
                raise EOFError("remote closed before response")
        data = bytearray()
        while True:
            byte = self.sock.recv(1)
            if byte == b"#":
                break
            if not byte:
                raise EOFError("remote closed connection")
            data += byte
        received = self.sock.recv(2)
        if checksum(data) != received.lower():
            self.sock.sendall(b"-")
            raise RuntimeError("bad remote checksum")
        self.sock.sendall(b"+")
        return data.decode()

    def packet(self, text: str) -> str:
        self.send_command(text)
        return self.receive()

    def step(self) -> str:
        self.send_command("s")
        previous_timeout = self.sock.gettimeout()
        self.sock.settimeout(0.05)
        try:
            return self.receive()
        except TimeoutError:
            self.sock.sendall(b"\x03")
            self.sock.settimeout(5)
            return self.receive()
        finally:
            self.sock.settimeout(previous_timeout)

    def continue_to(self, address: int) -> str:
        if self.packet(f"Z0,{address:x},2") != "OK":
            raise AssertionError(f"cannot set breakpoint at {address:06x}")
        try:
            response = self.packet("c")
        finally:
            self.packet(f"z0,{address:x},2")
        return response


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


def expected_remote_hex(value: str) -> str:
    return bytes.fromhex(value)[::-1].hex()


def write_memory(remote: Remote, values: dict, phase: str) -> None:
    for address, value in values.items():
        response = remote.packet(
            f"M{int(address, 16):x},{len(value) // 2:x}:{value}"
        )
        if response != "OK":
            raise AssertionError(
                f"cannot {phase} memory {address}: {response}"
            )


def run_debug_vector(
    qemu: Path, fixture: Path, vector: dict, log: Path
) -> None:
    socket_tag = hashlib.sha256(vector["name"].encode()).hexdigest()[:12]
    sock_path = fixture.parent / f"gdb-{socket_tag}.sock"
    trace_path = log.with_suffix(".trace")
    sock_path.unlink(missing_ok=True)
    command = [
        str(qemu), "-M", vector.get("machine", "c166-test"),
        "-display", "none", "-S",
        "-gdb", f"unix:{sock_path},server=on,wait=off",
        "-trace", "c166_irq_request", "-trace", "c166_irq_check",
        "-trace", "c166_irq_accept", "-D", str(trace_path),
        "-kernel", str(fixture), "-no-reboot",
    ]
    if "icount_shift" in vector:
        command[1:1] = [
            "-icount",
            f"shift={vector['icount_shift']},align=off,sleep=off",
        ]
    with log.open("wb") as stderr:
        proc = subprocess.Popen(
            command, stdout=subprocess.DEVNULL, stderr=stderr
        )
        try:
            with connect(sock_path, time.monotonic() + 5) as sock:
                remote = Remote(sock)
                if not remote.packet("?").startswith(("S", "T")):
                    raise AssertionError("target did not report a stop")
                xml = remote.packet("qXfer:features:read:target.xml:0,1000")
                if "c166-core.xml" not in xml:
                    raise AssertionError(
                        "target description omits c166-core.xml"
                    )
                for name, value in vector.get("initial", {}).items():
                    response = remote.packet(
                        f"P{REGISTERS[name]:x}={expected_remote_hex(value)}"
                    )
                    if response != "OK":
                        raise AssertionError(
                            f"cannot initialize {name}: {response}"
                        )
                write_memory(remote, vector.get("setup_memory", {}),
                             "initialize")
                if "watch" in vector:
                    kind, address, size = vector["watch"]
                    packet = f"Z{kind},{address:x},{size:x}"
                    assert remote.packet(packet) == "OK"
                    sock.settimeout(30)
                    response = remote.packet("c")
                    stop_kind = {2: "watch", 3: "rwatch", 4: "awatch"}[kind]
                    assert f"{stop_kind}:{address:x};" in response, response
                    packet = f"z{kind},{address:x},{size:x}"
                    assert remote.packet(packet) == "OK"
                for _ in range(vector["steps"]):
                    if not remote.step().startswith(("S", "T")):
                        raise AssertionError("single step did not stop")
                write_memory(remote, vector.get("after_steps_memory", {}),
                             "update")
                for _ in range(vector.get("extra_steps", 0)):
                    if not remote.step().startswith(("S", "T")):
                        raise AssertionError("extra single step did not stop")
                if "continue_to" in vector:
                    response = remote.continue_to(
                        int(vector["continue_to"], 16)
                    )
                    if not response.startswith(("S", "T")):
                        raise AssertionError("continue did not stop")
                for name, value in vector.get("expect", {}).items():
                    actual = remote.packet(f"p{REGISTERS[name]:x}")
                    wanted = expected_remote_hex(value)
                    if actual != wanted:
                        raise AssertionError(
                            f"{name}: expected {wanted}, received {actual}"
                        )
                for address, value in vector.get("memory", {}).items():
                    actual = remote.packet(
                        f"m{int(address, 16):x},{len(value) // 2:x}"
                    )
                    if actual != value:
                        raise AssertionError(
                            f"memory {address}: expected {value}, "
                            f"received {actual}"
                        )
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            sock_path.unlink(missing_ok=True)


def run_exit_vector(qemu: Path, fixture: Path, vector: dict, log: Path) -> None:
    command = [
        str(qemu), "-M", "c166-test", "-display", "none",
        "-kernel", str(fixture), "-no-reboot",
    ]
    with log.open("wb") as stderr:
        result = subprocess.run(
            command, stdout=subprocess.DEVNULL, stderr=stderr, timeout=10
        )
    if result.returncode != vector["exit_code"]:
        raise AssertionError(
            f"expected exit {vector['exit_code']}, received {result.returncode}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()

    args.qemu = args.qemu.resolve()
    args.manifest = args.manifest.resolve()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    fixture_dir = Path(manifest["fixture_dir"])
    logs = fixture_dir / "logs"
    logs.mkdir(exist_ok=True)
    failures = []
    for vector in manifest["vectors"]:
        name = vector["name"]
        try:
            if vector.get("run_to_exit"):
                run_exit_vector(args.qemu, fixture_dir / f"{name}.bin", vector,
                                logs / f"{name}.log")
            else:
                run_debug_vector(args.qemu, fixture_dir / f"{name}.bin", vector,
                                 logs / f"{name}.log")
            print(f"PASS {name}")
        except Exception as exc:
            failures.append(name)
            print(f"FAIL {name}: {exc}")
    print(f"{len(manifest['vectors'])} vectors, {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
