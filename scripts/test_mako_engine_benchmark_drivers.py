#!/usr/bin/env python3
"""Fail-closed launcher checks with simulated children, not benchmark evidence."""

import builtins
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import run_mako_engine_compare as compare
import run_mako_engine_soak as soak
import run_mako_engine_variants as variants


class BenchmarkDriverTests(unittest.TestCase):
    def exercise(self, module, defect=None, arguments=(), resume=False):
        launches = []
        baseline = {"boost": "0", "governors": {"0": "performance"}}
        real_open = builtins.open
        real_read_text = Path.read_text
        driver = soak if module is soak else compare
        tick_reads = 0

        def read_text(path, *args, **kwargs):
            if str(path) == "/sys/devices/system/cpu/online":
                return "0-127\n"
            if str(path).startswith("/sys/devices/system/cpu/") and path.name == "thread_siblings_list":
                cpu = int(path.parent.parent.name[3:])
                # Deliberately unlike cpu+64: topology, not numbering, defines SMT.
                return f"{cpu},{cpu ^ 16}\n"
            return real_read_text(path, *args, **kwargs)

        def cpu_snapshot(cpus):
            nonlocal tick_reads
            tick_reads += 1
            counters = {}
            for cpu in cpus:
                busy = 20 if defect == "sibling" and cpu == 16 else 0
                counters[str(cpu)] = dict.fromkeys(compare.accounting.COUNTER_FIELDS, 0)
                counters[str(cpu)].update(user=tick_reads * busy, idle=tick_reads * (1000 - busy))
            if defect in ("accounting", "count-and-accounting") and tick_reads % 2 == 0:
                del counters["16"]["iowait"]
            return {"schema": compare.accounting.SCHEMA,
                    "boot_id": "00000000-0000-4000-8000-000000000001",
                    "clock_ticks_per_second": 100,
                    "before_ns": tick_reads * 100, "after_ns": tick_reads * 100 + 1,
                    "cpus": counters}
        with tempfile.TemporaryDirectory(prefix="mako-benchmark-driver-test-") as temporary, tempfile.TemporaryFile() as lock:
            root = Path(temporary)
            output = root / "output"
            output.mkdir()
            binaries = {name: root / name for name in ("baseline", "candidate")}
            for binary in binaries.values():
                binary.touch()
                binary.chmod(0o755)
            sentinel = output / "sentinel"
            if defect == "nonempty":
                sentinel.touch()
            argv = ["driver", "--output", str(output), "--workers", "1", "--repetitions", "1"]
            if module is variants:
                argv += ["--variant", f"first={binaries['baseline']}", "--variant", f"second={binaries['candidate']}", "--modes", "cache-ack"]
            else:
                argv += ["--baseline", str(binaries["baseline"]), "--candidate", str(binaries["candidate"])]
                argv += (["--seconds", "1", "--run-timeout", "5"] if module is soak else
                         ["--iterations", "1", "--warmup", "256", "--modes", "cache-ack", "--perf", "1"])
            argv += list(arguments)

            def open_lock(path, *args, **kwargs):
                if str(path) == "/var/tmp/mako-zoo2-perf-shuai.lock":
                    return contextlib.nullcontext(lock)
                return real_open(path, *args, **kwargs)

            def engine(command):
                return "sto"

            def fake_sample(command, **kwargs):
                launches.append(command)
                if "--path" in command:
                    database = Path(command[command.index("--path") + 1])
                    database.mkdir()
                def number(flag):
                    return int(command[command.index(flag) + 1])
                iterations, warmup = number("--iterations"), number("--warmup")
                workers = number("--workers")
                cpu_start = number("--worker-cpu-start") if "--worker-cpu-start" in command else None
                worker_cpus = (list(map(int, command[command.index("--worker-cpus") + 1].split(",")))
                               if cpu_start is None else list(range(cpu_start, cpu_start + workers)))
                record = {
                    "engine": engine(command), "workers": workers, "mode": command[4],
                    "iterations_per_worker": iterations, "warmup_per_worker": warmup,
                    "commits": iterations * workers, "acknowledged": (iterations + warmup) * workers,
                    "verified_keys": 256 * workers, "pmu_min_running_ratio": 1.0,
                    "keys_per_worker": 256, "value_bytes": 128, "checksum": "none",
                    "perf_enabled": bool(number("--perf")), "items_per_transaction": number("--items"),
                    "worker_cpu_start": cpu_start, "worker_cpus": worker_cpus,
                    "writeback_cpu": number("--writeback-cpu"),
                    "creation_delay_ms": number("--creation-delay-ms"), "verified": True, "drained": False,
                    "queue_capacity_per_worker": 1 << (iterations + warmup - 1).bit_length(),
                    "max_worker_nanoseconds": 1000000, "transactions_per_second": iterations * workers * 1000,
                }
                changes = {"engine": ("engine", "unexpected"), "workers": ("workers", 2),
                           "mode": ("mode", "engine"), "count": ("commits", 0),
                           "keys": ("verified_keys", 255), "pmu": ("pmu_min_running_ratio", 0.994),
                           "pmu-nan": ("pmu_min_running_ratio", float("nan"))}
                if defect == "count-and-accounting":
                    record["commits"] = 0
                if defect == "pmu-first" and len(launches) == 1:
                    record["pmu_min_running_ratio"] = 0.994
                if defect == "baseline":
                    record["engine"] = "mako-local/sto-masstrans"
                if defect in changes:
                    key, value = changes[defect]
                    record[key] = value
                if defect == "reordered-cpus":
                    record["worker_cpus"] = list(reversed(worker_cpus))
                if defect == "noninteger-cpus":
                    record["worker_cpus"] = [float(cpu) for cpu in worker_cpus]
                if defect == "explicit-cpu-base":
                    record["worker_cpu_start"] = 0
                if defect == "missing-cpu-base":
                    del record["worker_cpu_start"]
                kwargs["stdout"].write(json.dumps(record) + "\n")
                kwargs["stdout"].flush()
                for worker in range(workers):
                    stats = {"worker": worker, "cpu": worker_cpus[worker],
                             "wall_ns": 1000000, "diagnostic_wall_ns": 1000200,
                             "cpu_ns": 980000 if defect == "off-cpu" else 1000100,
                             "cycles": 1000000, "instructions": 1000000,
                             "pmu_time_enabled_ns": 1000000, "pmu_time_running_ns": 1000000}
                    if defect == "wrong-worker-cpu":
                        stats["cpu"] = worker
                    if defect == "noninteger-worker-cpu":
                        stats["cpu"] = float(stats["cpu"])
                    if defect != "missing-worker":
                        kwargs["stderr"].write("WORKER " + json.dumps(stats) + "\n")
                kwargs["stderr"].flush()
                if defect == "interrupt":
                    raise KeyboardInterrupt
                return subprocess.CompletedProcess(command, 7 if defect == "child-failure" else 0)

            def fake_soak(command, directory, phase, timeout, machine):
                launches.append(command)
                database = Path(command[command.index("--path") + 1])
                manifest = Path(command[command.index("--manifest") + 1])
                identity = engine(command)
                if phase == "run":
                    database.mkdir()
                    manifest.write_text("gc-soak-v1 1 256 128\n256\n")
                    protocol = {"event": "protocol", "version": 2, "engine": identity,
                                "workers": 1, "worker_cpus": [0], "seconds": 1, "keys_per_worker": 256,
                                "value_bytes": 128, "rate_per_worker": 0,
                                "queue_capacity_per_worker": 1024, "max_batch_records": 64,
                                "writeback_cpu": 32, "checksum": "crc32c", "wal": True, "sync": False}
                    if defect == "engine":
                        protocol["engine"] = "unexpected"
                    if defect == "workers":
                        protocol["workers"] = 2
                    if defect == "protocol":
                        protocol["checksum"] = "none"
                    drained = {"event": "drained", "engine": identity, "commits": 256,
                               "ack_seconds": 1.0, "applied_seconds": 2.0}
                    if defect == "elapsed-nan":
                        drained["ack_seconds"] = float("nan")
                    records = [protocol, drained]
                else:
                    records = [{"event": "verified", "engine": identity,
                                "commits": 257 if defect == "count" else 256,
                                "workers": 2 if defect == "verified-workers" else 1,
                                "keys": 255 if defect == "keys" else 256, "open_seconds": 1.0}]
                if defect == "duplicate-terminal" and phase == "verify":
                    records.append(records[0])
                with (directory / f"{phase}.jsonl").open("w") as stream:
                    for record in records:
                        stream.write(json.dumps(record, separators=(",", ":")) + "\n")
                return {"returncode": 0, "command": command}

            error = None
            try:
                with contextlib.ExitStack() as stack:
                    stack.enter_context(patch.object(sys, "argv", argv))
                    stack.enter_context(patch.object(driver.socket, "gethostname", return_value="other-host" if defect == "host" else "zoo-002"))
                    stack.enter_context(patch.object(driver, "machine", return_value=baseline))
                    stack.enter_context(patch.object(driver.fcntl, "flock"))
                    stack.enter_context(patch("builtins.open", side_effect=open_lock))
                    stack.enter_context(patch.object(driver.subprocess, "Popen", side_effect=AssertionError("real child execution forbidden")))
                    stack.enter_context(patch.object(Path, "read_text", read_text))
                    stack.enter_context(patch.object(driver.os, "sched_getaffinity", return_value=set(range(128))))
                    if module is not soak:
                        stack.enter_context(patch.object(compare.accounting, "snapshot", side_effect=cpu_snapshot))
                    stack.enter_context(patch.object(soak, "execute", side_effect=fake_soak) if module is soak else
                                        patch.object(compare.subprocess, "run", side_effect=fake_sample))
                    stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                    stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
                    module.main()
                    if resume:
                        self.assertIs(module, soak)
                        argv.append("--resume")
                        module.main()
            except (RuntimeError, SystemExit, KeyboardInterrupt) as failure:
                error = failure
            rows = json.loads((output / "results.json").read_text()) if (output / "results.json").exists() else []
            blocks = json.loads((output / "blocks.json").read_text()) if (output / "blocks.json").exists() else []
            return {"error": error, "launches": len(launches),
                    "retained_databases": len(list(output.glob("*/rocks"))),
                    "sentinel_preserved": sentinel.exists(), "rows": rows, "blocks": blocks,
                    "sample_records": [json.loads((Path(row["directory"]) / "result.json").read_text())
                                       for row in rows] if module is not soak else [],
                    "block_journal": [json.loads(line) for line in (output / "blocks.jsonl").read_text().splitlines()]
                                     if (output / "blocks.jsonl").exists() else [],
                    "raw_evidence": [
                        all((Path(row["directory"]) / name).is_file()
                            for name in ("stdout.txt", "stderr.txt", "before.json", "after.json",
                                         "cpu-before.json", "cpu-after.json", "result.json"))
                        for row in rows] if module is not soak else []}

    def test_mismatched_compare_and_variant_results_fail_and_retain_scratch(self):
        for module in (compare, variants):
            for defect in ("mode", "workers", "count", "engine", "keys", "pmu-nan", "child-failure", "missing-worker", "noninteger-worker-cpu"):
                with self.subTest(module=module.__name__, defect=defect):
                    result = self.exercise(module, defect)
                    self.assertIsNotNone(result["error"])
                    self.assertEqual(result["launches"], 1)
                    self.assertEqual(result["retained_databases"], 1)

    def test_invalid_worker_cpu_pools_fail_before_workload(self):
        cases = [
            ("--worker-cpus", ""),
            ("--worker-cpus", "0,0"),
            ("--worker-cpus", "0,"),
            ("--worker-cpus", "-1"),
            ("--worker-cpus", "+1"),
            ("--worker-cpus", "0-3"),
            ("--worker-cpus", "1024"),
            ("--worker-cpus", "128"),
            ("--workers", "2", "--worker-cpus", "0"),
            ("--worker-cpus", "0,2", "--worker-cpu-start", "0"),
            ("--worker-cpu-start", "0", "--worker-cpus", "0,2"),
            ("--worker-cpus", "0,2", "--helper-affinity", "2,3"),
            ("--worker-cpus", "0,2", "--writeback-cpu", "2"),
        ]
        for module in (compare, variants):
            for arguments in cases:
                with self.subTest(module=module.__name__, arguments=arguments):
                    result = self.exercise(module, arguments=arguments)
                    self.assertIsInstance(result["error"], SystemExit)
                    self.assertEqual(result["launches"], 0)

    def test_sparse_mapping_result_mismatches_are_fatal_not_quality_retries(self):
        for module in (compare, variants):
            for defect in ("reordered-cpus", "noninteger-cpus", "explicit-cpu-base",
                           "missing-cpu-base", "wrong-worker-cpu", "noninteger-worker-cpu"):
                with self.subTest(module=module.__name__, defect=defect):
                    result = self.exercise(module, defect, arguments=(
                        "--workers", "2", "--worker-cpus", "2,0,3", "--max-block-attempts", "2"))
                    self.assertIsInstance(result["error"], RuntimeError)
                    self.assertEqual(result["launches"], 1)
                    self.assertEqual(result["retained_databases"], 1)
                    self.assertIn("fatal_error", result["rows"][0])
                    self.assertFalse(result["blocks"][0]["accepted"])

    def test_rust_variants_reject_c_abi_backend(self):
        result = self.exercise(variants, "baseline")
        self.assertIsNotNone(result["error"])
        self.assertEqual(result["launches"], 1)
        self.assertEqual(result["retained_databases"], 1)

    def test_interrupted_block_keeps_partial_sample_and_manifest(self):
        for module in (compare, variants):
            with self.subTest(module=module.__name__):
                result = self.exercise(module, "interrupt", arguments=("--balanced-blocks",))
                self.assertIsInstance(result["error"], KeyboardInterrupt)
                self.assertEqual(result["launches"], 1)
                self.assertEqual(len(result["rows"]), 1)
                self.assertFalse(result["rows"][0]["completed"])
                self.assertFalse(result["rows"][0]["block_accepted"])
                self.assertEqual(result["sample_records"], result["rows"])
                self.assertEqual(len(result["blocks"]), 1)
                self.assertFalse(result["blocks"][0]["accepted"])
                self.assertEqual(result["blocks"][0]["samples"], [result["rows"][0]["directory"]])
                self.assertEqual(result["block_journal"], result["blocks"])
                self.assertEqual(result["retained_databases"], 1)

    def test_rejected_balanced_block_retains_all_arms_then_retries_cleanly(self):
        for module in (compare, variants):
            with self.subTest(module=module.__name__):
                result = self.exercise(module, "pmu-first",
                                       arguments=("--balanced-blocks", "--max-block-attempts", "2"))
                self.assertIsNone(result["error"])
                self.assertEqual(result["launches"], 8)
                self.assertEqual([block["accepted"] for block in result["blocks"]], [False, True])
                self.assertEqual([row["block_accepted"] for row in result["rows"]], [False] * 4 + [True] * 4)
                self.assertEqual([row["quality_valid"] for row in result["rows"][:4]], [False, True, True, True])
                self.assertEqual(result["retained_databases"], 4)
                self.assertEqual(result["raw_evidence"], [True] * 8)

    def test_quality_retry_budget_exhaustion_preserves_all_rejected_evidence(self):
        for module in (compare, variants):
            with self.subTest(module=module.__name__):
                result = self.exercise(module, "pmu",
                                       arguments=("--balanced-blocks", "--max-block-attempts", "2"))
                self.assertIsInstance(result["error"], RuntimeError)
                self.assertEqual(result["launches"], 8)
                self.assertEqual([block["accepted"] for block in result["blocks"]], [False, False])
                self.assertTrue(all(not row["block_accepted"] for row in result["rows"]))
                self.assertEqual(result["retained_databases"], 8)
                self.assertEqual(result["raw_evidence"], [True] * 8)

    def test_off_cpu_and_topology_sibling_quality_gates_reject_complete_blocks(self):
        for defect, flag in (("off-cpu", "--max-worker-off-cpu-fraction"),
                             ("sibling", "--max-sibling-contention")):
            with self.subTest(defect=defect):
                result = self.exercise(compare, defect, arguments=(flag, ".01"))
                self.assertIsInstance(result["error"], RuntimeError)
                self.assertEqual(result["launches"], 2)
                self.assertEqual(result["retained_databases"], 2)
                self.assertEqual([block["accepted"] for block in result["blocks"]], [False])

    def test_missing_accounting_rejects_quality_but_never_masks_bad_counts(self):
        for module in (compare, variants):
            with self.subTest(module=module.__name__):
                rejected = self.exercise(module, "accounting", arguments=("--max-sibling-contention", ".01"))
                self.assertIsInstance(rejected["error"], RuntimeError)
                self.assertEqual(rejected["launches"], 2)
                self.assertTrue(all(not row["quality_valid"] for row in rejected["rows"]))
                self.assertTrue(all("16" in row["cpu_accounting_errors"] for row in rejected["rows"]))
                fatal = self.exercise(module, "count-and-accounting",
                                      arguments=("--max-sibling-contention", ".01"))
                self.assertIsInstance(fatal["error"], RuntimeError)
                self.assertEqual(fatal["launches"], 1)
                self.assertIn("commits", fatal["rows"][0]["fatal_error"])
                self.assertFalse(fatal["rows"][0]["environment_and_counts_valid"])

    def test_multi_item_logged_and_ack_rejected_before_workload(self):
        for module in (compare, variants):
            for mode in ("engine-logged", "cache-ack"):
                with self.subTest(module=module.__name__, mode=mode):
                    result = self.exercise(module, arguments=("--items", "4", "--modes", mode))
                    self.assertIsInstance(result["error"], SystemExit)
                    self.assertEqual(result["launches"], 0)

    def test_mismatched_soak_results_fail_and_retain_scratch(self):
        for defect in ("protocol", "workers", "verified-workers", "count", "engine", "keys", "elapsed-nan", "duplicate-terminal"):
            with self.subTest(defect=defect):
                result = self.exercise(soak, defect)
                self.assertIsNotNone(result["error"])
                self.assertEqual(result["retained_databases"], 1)

    def test_nonempty_output_and_wrong_host_fail_before_child_execution(self):
        for module in (compare, soak, variants):
            for defect in ("nonempty", "host"):
                with self.subTest(module=module.__name__, defect=defect):
                    result = self.exercise(module, defect)
                    self.assertIsInstance(result["error"], SystemExit)
                    self.assertEqual(result["launches"], 0)
                    if defect == "nonempty":
                        self.assertTrue(result["sentinel_preserved"])

    def test_invalid_workers_and_repetitions_fail_before_child_execution(self):
        for module in (compare, soak, variants):
            for arguments in (("--workers", "0"), ("--workers", "33"), ("--repetitions", "0")):
                with self.subTest(module=module.__name__, arguments=arguments):
                    result = self.exercise(module, arguments=arguments)
                    self.assertIsInstance(result["error"], SystemExit)
                    self.assertEqual(result["launches"], 0)

    def test_invalid_duration_iteration_and_timeout_fail_before_child_execution(self):
        cases = [(compare, ("--iterations", "0")), (compare, ("--warmup", "255")),
                 (soak, ("--seconds", "0")), (soak, ("--run-timeout", "1")),
                 (soak, ("--reopen-timeout", "0"))]
        for module, arguments in cases:
            with self.subTest(module=module.__name__, arguments=arguments):
                result = self.exercise(module, arguments=arguments)
                self.assertIsInstance(result["error"], SystemExit)
                self.assertEqual(result["launches"], 0)

    def test_only_valid_samples_remove_their_owned_scratch_databases(self):
        for module in (compare, soak, variants):
            with self.subTest(module=module.__name__):
                result = self.exercise(module)
                self.assertIsNone(result["error"])
                self.assertEqual(result["launches"], 4 if module is soak else 2)
                self.assertEqual(result["retained_databases"], 0)

    def test_resuming_completed_soak_does_not_launch_duplicate_children(self):
        result = self.exercise(soak, resume=True)
        self.assertIsNone(result["error"])
        self.assertEqual(result["launches"], 4)
        self.assertEqual(result["retained_databases"], 0)


class ResumeTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="mako-soak-resume-test-")
        self.addCleanup(self.scratch.cleanup)
        self.output = Path(self.scratch.name).resolve()
        self.machine = {"boost": "0", "governors": {"0": "schedutil"}}
        self.protocol = {"schema": "mako-paired-engine-soak-v1", "seconds": 120,
                         "workers": [1], "repetitions": 1,
                         "worker_cpus": [0], "writeback_cpu": 32,
                         "binary_sha256": {"baseline": "a" * 64, "candidate": "b" * 64},
                         "binary_paths": {"baseline": "/frozen/c-abi", "candidate": "/frozen/sto"},
                         "initial_machine": self.machine}
        self.planned = [(1, 1, "baseline"), (1, 1, "candidate")]
        self.directory = self.output / "r1-w1-baseline.example"
        self.directory.mkdir()
        (self.directory / "expected.txt").write_text("gc-soak-v1 1 256 128\n256\n")
        identity = "sto"
        cell_protocol = {"event": "protocol", "version": 2, "engine": identity,
                         "workers": 1, "worker_cpus": [0], "seconds": 120, "keys_per_worker": 256,
                         "value_bytes": 128, "rate_per_worker": 0,
                         "queue_capacity_per_worker": 1024, "max_batch_records": 64,
                         "writeback_cpu": 32, "checksum": "crc32c", "wal": True, "sync": False}
        drained = {"event": "drained", "engine": identity, "commits": 256,
                   "ack_seconds": 120.1, "applied_seconds": 120.2}
        verified = {"event": "verified", "engine": identity, "commits": 256,
                    "workers": 1, "keys": 256, "open_seconds": 1.0}
        self.result = {"arm": "baseline", "workers": 1, "repetition": 1,
                       "directory": str(self.directory), "run": {"returncode": 0},
                       "verify": {"returncode": 0}, "protocol": cell_protocol,
                       "drained": drained, "verified": verified,
                       "drained_and_verified": True, "scratch_cleaned": True}
        soak.write_json(self.output / "protocol.json", self.protocol)
        self.save_result()
        for name in ("before.json", "after.json"):
            soak.write_json(self.directory / name, self.machine)
        (self.directory / "run.jsonl").write_text("".join(
            json.dumps(event, separators=(",", ":")) + "\n" for event in (cell_protocol, drained)))
        (self.directory / "verify.jsonl").write_text(json.dumps(verified, separators=(",", ":")) + "\n")

    def save_result(self):
        soak.write_json(self.output / "results.json", [self.result])
        soak.write_json(self.directory / "result.json", self.result)

    def load(self, protocol=None):
        return soak.load_resume(self.output, protocol or self.protocol, self.planned)

    def test_valid_prefix_reused_without_touching_incomplete_attempt(self):
        failed = self.output / "r1-w1-sto.interrupted" / "rocks"
        failed.mkdir(parents=True)
        marker = failed / "preserve"
        marker.touch()
        self.assertEqual(self.load(), [self.result])
        self.assertTrue(marker.exists())

    def test_changed_binary_protocol_and_governor_rejected(self):
        for change in ({"binary_sha256": {"baseline": "changed", "candidate": "b" * 64}},
                       {"binary_paths": {"baseline": "/different", "candidate": "/frozen/sto"}},
                       {"seconds": 960}, {"workers": [1, 4]}, {"repetitions": 2},
                       {"initial_machine": {"boost": "1", "governors": {"0": "schedutil"}}},
                       {"initial_machine": {"boost": "0", "governors": {"0": "performance"}}}):
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                self.load({**self.protocol, **change})

    def test_duplicate_or_reordered_cells_rejected(self):
        soak.write_json(self.output / "results.json", [self.result, self.result])
        with self.assertRaises(RuntimeError):
            self.load()
        self.result["arm"] = "candidate"
        self.save_result()
        with self.assertRaises(RuntimeError):
            self.load()

    def test_corrupt_or_missing_records_rejected(self):
        (self.directory / "result.json").write_text("{}")
        with self.assertRaises(RuntimeError):
            self.load()
        self.save_result()
        (self.directory / "verify.jsonl").write_text("")
        with self.assertRaises(RuntimeError):
            self.load()
        (self.directory / "expected.txt").unlink()
        with self.assertRaises(RuntimeError):
            self.load()

    def test_wrong_manifest_count_or_unfinished_database_rejected(self):
        (self.directory / "expected.txt").write_text("gc-soak-v1 1 256 128\n257\n")
        with self.assertRaises(RuntimeError):
            self.load()
        (self.directory / "expected.txt").write_text("gc-soak-v1 1 256 128\n256\n")
        database = self.directory / "rocks"
        database.mkdir()
        with self.assertRaises(RuntimeError):
            self.load()
        self.assertTrue(database.exists())


if __name__ == "__main__":
    unittest.main()
