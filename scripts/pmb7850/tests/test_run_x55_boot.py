#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import importlib.util
import hashlib
import json
import re
import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "run_x55_boot.py"
SPEC = importlib.util.spec_from_file_location("run_x55_boot", SCRIPT)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class MatrixRunnerTest(unittest.TestCase):
    def test_preflight_checks_only_selected_variants(self):
        root = RUNNER.repository_root()
        with mock.patch.object(RUNNER, "validate_image_entry") as validate:
            RUNNER.load_manifest(RUNNER.default_manifest(), root, ["sl55"])
            self.assertEqual([call.args[0] for call in validate.call_args_list],
                             ["sl55:canonical"])
            validate.reset_mock()
            RUNNER.load_manifest(RUNNER.default_manifest(), root, ["sl55"],
                                 include_alternates=True)
            self.assertEqual([call.args[0] for call in validate.call_args_list],
                             ["sl55:canonical", "sl55:sw20-aircheck"])

    def test_selected_image_provenance_is_required(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            entry = {"image": "image.bin", "sha256": "wrong",
                     "topology": [{"size": 4}]}
            with self.assertRaisesRegex(RUNNER.MatrixError, "missing"):
                RUNNER.validate_image_entry("selected", entry, root)
            (root / "image.bin").write_bytes(b"test")
            with self.assertRaisesRegex(RUNNER.MatrixError, "SHA-256"):
                RUNNER.validate_image_entry("selected", entry, root)
            entry["sha256"] = hashlib.sha256(b"test").hexdigest()
            RUNNER.validate_image_entry("selected", entry, root)
            entry["topology"][0]["size"] = 3
            with self.assertRaisesRegex(RUNNER.MatrixError, "topology"):
                RUNNER.validate_image_entry("selected", entry, root)

    def test_manifest_preflight_and_policies(self):
        root = RUNNER.repository_root()
        manifest = RUNNER.load_manifest(RUNNER.default_manifest(), root)
        self.assertEqual(tuple(manifest["devices"]), RUNNER.DEVICE_ORDER)
        self.assertEqual(manifest["synchronization_unit"], "soc_ticks")
        self.assertEqual(
            manifest["devices"]["c55"]["identity"],
            {"kind": "fsn", "value": "A35F2F28"},
        )
        self.assertEqual(
            manifest["devices"]["c60"]["image"],
            "fw/Fullflash/C60/v.27/"
            "C60_SW27_LG95_00432911_flash-intel_fullflash.bin",
        )
        self.assertEqual(
            [item["name"] for item in manifest["devices"]["a65"]["alternates"]],
            ["sw07-amd", "sw07-intel"],
        )
        self.assertEqual(
            [item["name"] for item in
             manifest["devices"]["cf62"]["alternates"]],
            ["sw12-intel", "sw21-amd"],
        )
        self.assertEqual(manifest["devices"]["c60"]["topology"][0]["model"],
                         "w30-128mbit-top")
        self.assertIn("perf", RUNNER.GATE_ORDER)
        self.assertEqual(manifest["devices"]["s55"]["sim"]["minimum_ticks"],
                         75_800_000)
        self.assertEqual(manifest["devices"]["sl55"]["sim"]["minimum_ticks"],
                         43_300_000)
        sw20 = manifest["devices"]["sl55"]["alternates"][0]
        self.assertEqual(sw20["name"], "sw20-aircheck")
        self.assertEqual(sw20["confirm"], {
            "minimum_ticks": 130_000_000,
            "trigger_frame": 10,
            "key": "soft-left",
            "key_presses": [[100_000_000, "soft-left"]],
            "required_frame_sha256":
                "27e9e2a723d336a9256c63affe30ff4210de3829f3a5aa0ad9af81ba4fa2f388",
        })

    def test_confirmation_passes_frame_trigger_to_protocol(self):
        root = RUNNER.repository_root()
        manifest = RUNNER.load_manifest(RUNNER.default_manifest(), root)
        canonical = manifest["devices"]["sl55"]
        _, entry = RUNNER.variants_for("sl55", canonical, True)[1]
        args = SimpleNamespace(
            root=root, emu=root / "emu/bin/emu",
            qemu=root / "qemu/build/release/qemu-system-c166",
            timeout=300.0,
        )
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(RUNNER, "run_protocol_process",
                               return_value={}) as protocol:
            RUNNER.run_qemu(args, "sl55", entry, "confirm",
                            Path(directory), 130_000_000)
        kwargs = protocol.call_args.kwargs
        self.assertEqual(kwargs["trigger_frame"], 10)
        self.assertEqual(kwargs["key"], "soft-left")
        self.assertEqual(kwargs["key_presses"],
                         [[100_000_000, "soft-left"]])

    def test_selection_is_ordered_and_deduplicated(self):
        self.assertEqual(
            RUNNER.parse_selection(["a60,c55", "a60"],
                                   RUNNER.DEVICE_ORDER, "device"),
            ["a60", "c55"],
        )
        with self.assertRaises(RUNNER.MatrixError):
            RUNNER.parse_selection(["a70"], RUNNER.DEVICE_ORDER, "device")

    def test_protocol_round_trip_and_hello(self):
        header = RUNNER.repository_root() / "emu/include/emu_ui.h"
        if header.exists():
            version = int(re.search(r"#define EMU_UI_VERSION (\d+)u",
                                    header.read_text())[1])
            self.assertEqual(RUNNER.UI_VERSION, version)
        payload = struct.pack("<HHHHB", 101, 80, 2, 0, 3) + b"a60"[:3]
        payload += bytes((1,)) + b"1" + bytes((9,)) + b"soft-left"
        packet = RUNNER.encode_ui(RUNNER.UI_HELLO, payload, 4, 5)
        packets, remainder = RUNNER.decode_ui(packet)
        self.assertFalse(remainder)
        self.assertEqual(packets[0][2:], (4, 5))
        hello = RUNNER.decode_hello(packets[0][1])
        self.assertEqual((hello["width"], hello["height"]), (101, 80))
        self.assertEqual(hello["keys"], ["1", "soft-left"])

    def test_protocol_rejects_old_version(self):
        packet = RUNNER.UI_HEADER.pack(RUNNER.UI_MAGIC, 7,
                                      RUNNER.UI_STATS, 0, 0, 0, 0, bytes(16))
        with self.assertRaises(RUNNER.MatrixError):
            RUNNER.decode_ui(packet)

    def test_supervised_identity_and_shared_validation(self):
        hello = struct.pack("<HHHHB", 1, 1, 1, 3, 3) + b"c55\x05power"
        identity = {}
        packet = RUNNER.encode_ui(RUNNER.UI_HELLO, hello, 1, run_id=b"a" * 16)
        packets, tail = RUNNER.decode_ui(packet[:20], identity)
        self.assertEqual(packets, [])
        self.assertEqual(identity, {})
        packets, tail = RUNNER.decode_ui(tail + packet[20:], identity)
        self.assertFalse(tail)
        self.assertEqual(identity["run_id"], b"a" * 16)
        with self.assertRaises(RUNNER.MatrixError):
            RUNNER.decode_ui(packet, identity)
        with self.assertRaises(RUNNER.MatrixError):
            RUNNER.decode_ui(RUNNER.encode_ui(RUNNER.UI_STATS, b"", 2,
                                              run_id=b"b" * 16), identity)
        with self.assertRaises(RUNNER.MatrixError):
            RUNNER.decode_ui(RUNNER.encode_ui(RUNNER.UI_STATS, b"", 1), {})
        with self.assertRaises(RUNNER.MatrixError):
            RUNNER.decode_hello(b"\0\0" + hello[2:])
        subscribe = RUNNER.encode_ui(
            20, struct.pack("<QQ", 7, 10), 2, run_id=b"a" * 16)
        RUNNER.decode_ui(subscribe, identity)
        live = RUNNER.encode_ui(
            11, struct.pack("<QQ", 7, 10) + b"abc", 3, run_id=b"a" * 16)
        RUNNER.decode_ui(live, identity)
        with self.assertRaises(RUNNER.MatrixError):
            RUNNER.decode_ui(RUNNER.encode_ui(
                11, struct.pack("<QQ", 7, 12) + b"x", 4, run_id=b"a" * 16),
                identity)

    def test_summary_and_difference(self):
        summary = RUNNER.parse_summary(
            "status: interrupted  ticks: 123  icount: 120  pc: 0x01a9c4\n"
        )
        self.assertEqual(summary["pc"], 0x01A9C4)
        self.assertEqual(RUNNER.first_difference([1, 2], [1, 3]), 1)
        self.assertEqual(RUNNER.first_difference([1], [1, 2]), 1)
        self.assertIsNone(RUNNER.first_difference([1], [1]))

    def test_effects_compare_lcd_transaction_disposition(self):
        columns = [
            "kind", "addr", "size", "value", "info_bank_i64",
            "info_command_str", "info_data_bytes_i64",
            "info_disposition_str", "info_sequence_i64", "info_x_i64",
            "info_y_i64", "info_bits_i64", "info_rx_i64", "info_tx_i64",
        ]
        values = [
            "lcd_transaction", None, None, 7, None, None, 102, "held",
            7, 0, 6, None, None, None,
        ]

        class Connection:
            description = [(column,) for column in columns]

            def execute(self, query, parameters):
                self.assert_query = (query, parameters)
                return self

            def fetchall(self):
                return [values]

            def close(self):
                pass

        fake_duckdb = SimpleNamespace(connect=lambda: Connection())
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.dict("sys.modules", {"duckdb": fake_duckdb}):
            root = Path(directory)
            partition = root / "kind=lcd_transaction"
            partition.mkdir()
            (partition / "part.parquet").touch()
            effects = RUNNER.read_effects(root)
        self.assertEqual(effects["display"], [])
        self.assertEqual(len(effects["lcd_transaction"]), 1)
        self.assertIn("held", effects["lcd_transaction"][0])

    def test_perf_baseline_policy(self):
        samples = [20_000_000.0, 22_000_000.0, 21_000_000.0]
        median = RUNNER.statistics.median(samples)
        self.assertEqual(median, 21_000_000.0)
        self.assertGreaterEqual(median, 20_000_000.0 * 0.9)

    def test_legacy_c55_ui_parity_is_scoped_to_ui(self):
        result = {
            "parity": False,
            "ui": {"first_differences": {
                "20m": None, "100m": None, "softkey": None,
            }},
        }
        self.assertTrue(RUNNER.legacy_c55_gate_parity(result, "ui"))
        self.assertFalse(RUNNER.legacy_c55_gate_parity(result, "no-sim"))
        result["ui"]["first_differences"]["softkey"] = 38
        self.assertFalse(RUNNER.legacy_c55_gate_parity(result, "ui"))

    def test_merge_refreshes_legacy_c55_ui_status(self):
        report = {"devices": {"c55": {"canonical": {"ui": {
            "status": "fail", "parity": False,
            "legacy_result": {"parity": False, "ui": {
                "first_differences": {
                    "20m": None, "100m": None, "softkey": None,
                },
            }},
        }}}}}
        RUNNER.refresh_legacy_c55_ui(report)
        ui = report["devices"]["c55"]["canonical"]["ui"]
        self.assertEqual(ui["status"], "pass")
        self.assertTrue(ui["parity"])
        self.assertFalse(RUNNER.report_has_failed_gate(report))

    def test_preflight_report(self):
        with tempfile.TemporaryDirectory() as directory:
            code = RUNNER.matrix_main([
                "--preflight-only", "--device", "a60,c55",
                "--gate", "no-sim", "--artifacts", directory,
            ])
            self.assertEqual(code, 0)
            report = json.loads((Path(directory) / "report.json").read_text())
            self.assertEqual(report["status"], "pass")
            self.assertEqual(report["selection"]["devices"], ["a60", "c55"])

    def test_merge_reports(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            common = {
                "schema": RUNNER.REPORT_SCHEMA,
                "schema_version": RUNNER.REPORT_SCHEMA_VERSION,
                "comparison_mode": "probe",
                "manifest": {"sha256": "manifest"},
                "provenance": {"qemu_commit": "qemu"},
                "status": "pass",
            }
            paths = []
            for device in ("a60", "c55"):
                path = root / f"{device}.json"
                path.write_text(json.dumps({
                    **common,
                    "selection": {"devices": [device],
                                  "gates": ["supervised"],
                                  "include_alternates": False},
                    "devices": {device: {"canonical": {}}},
                }), encoding="utf-8")
                paths.append(path)
            output = root / "merged"
            self.assertEqual(RUNNER.merge_reports(paths, output), 0)
            report = json.loads((output / "report.json").read_text())
            self.assertEqual(report["selection"]["devices"], ["a60", "c55"])

    def test_merge_report_replaces_a_focused_rerun(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            common = {
                "schema": RUNNER.REPORT_SCHEMA,
                "schema_version": RUNNER.REPORT_SCHEMA_VERSION,
                "comparison_mode": "probe",
                "manifest": {"sha256": "manifest"},
                "provenance": {"qemu_commit": "qemu"},
            }
            paths = []
            for index, status in enumerate(("fail", "pass")):
                path = root / f"report-{index}.json"
                path.write_text(json.dumps({
                    **common, "status": status,
                    "selection": {"devices": ["c55"], "gates": ["ui"],
                                  "include_alternates": False},
                    "devices": {"c55": {"canonical": {
                        "ui": {"status": status, "attempt": index},
                    }}},
                }), encoding="utf-8")
                paths.append(path)
            output = root / "merged"
            self.assertEqual(RUNNER.merge_reports(paths, output), 0)
            report = json.loads((output / "report.json").read_text())
            ui = report["devices"]["c55"]["canonical"]["ui"]
            self.assertEqual(ui, {"status": "pass", "attempt": 1})


if __name__ == "__main__":
    unittest.main()
