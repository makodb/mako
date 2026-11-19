#!/usr/bin/env python3
"""Negative controls for the runtime transcript comparison."""

import importlib.util
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("runtime_parity", ROOT / "scripts/check_runtime_parity.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class RuntimeParityTests(unittest.TestCase):
    def output(self, record):
        return GATE.PREFIX + json.dumps(record) + "\n"

    def test_accepts_native_observations(self):
        self.assertEqual(GATE.read_transcript("build log\n" + self.output(GATE.EXPECTED), "lane"),
                         GATE.EXPECTED)

    def test_rejects_each_changed_observation_even_when_both_lanes_agree(self):
        for key in GATE.EXPECTED:
            with self.subTest(key=key):
                record = dict(GATE.EXPECTED)
                record[key] = None
                with self.assertRaises(ValueError):
                    GATE.read_transcript(self.output(record), "both lanes")

    def test_rejects_recording_sleep_order_and_foreign_completion(self):
        for changes in [{"timer_order": ["start", "resume", "peer"]},
                        {"completion_on_owner": False}, {"timer_suspended": False},
                        {"rpc_missing_error": 0}, {"timer_suspended": 1}]:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                GATE.read_transcript(self.output(GATE.EXPECTED | changes), "lane")

    def test_rejects_missing_duplicate_malformed_or_extra_records(self):
        for output in ["", self.output(GATE.EXPECTED) * 2, GATE.PREFIX + "{",
                       self.output(GATE.EXPECTED | {"unchecked": 1})]:
            with self.subTest(output=output), self.assertRaises(ValueError):
                GATE.read_transcript(output, "lane")

    def test_rejects_failed_process_even_with_valid_stdout(self):
        command = [sys.executable, "-c", f"print({self.output(GATE.EXPECTED)!r}); raise SystemExit(7)"]
        with self.assertRaisesRegex(ValueError, "fixture exited 7"):
            GATE.run_lane(command, "lane", ROOT, 2)

    def test_rejects_hung_process(self):
        with self.assertRaisesRegex(ValueError, "timed out"):
            GATE.run_lane([sys.executable, "-c", "import time; time.sleep(10)"], "lane", ROOT, 0.05)


if __name__ == "__main__":
    unittest.main()
