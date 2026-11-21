import hashlib
import json
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
proof = json.loads((HERE / "proof-final/report.json").read_text())
test = "db::native_health_tests::native_poison_is_diagnostic_before_point_mapping"
role = "MAKO_NATIVE_RUNTIME_HEALTH_PROOF_ROLE"
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
results = []
for mutant in ("core-only", "cleanup-health"):
    root = HERE / "mutants" / mutant
    before = {str(path.relative_to(root)): digest(path) for path in root.rglob("*") if path.is_file()}
    env = os.environ.copy()
    for name in list(env):
        if name.startswith(("CARGO_ENCODED_", "CARGO_BUILD_", "CARGO_TARGET_")):
            env.pop(name)
    env.update(proof["environment"])
    env["CARGO_TARGET_DIR"] = str(HERE / f"target-{mutant}")
    build = ["cargo", "test", "--manifest-path", str(root / "crates/Cargo.toml"),
             "--locked", "-p", "mako-engine-sto", "--all-features", "--lib", "--no-run",
             "--message-format=json"]
    with (HERE / f"{mutant}-build.log").open("w") as stream:
        status = subprocess.run(build, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=900)
    assert status.returncode == 0, mutant
    executables = []
    for line in (HERE / f"{mutant}-build.log").read_text().splitlines():
        if line.startswith("{"):
            message = json.loads(line)
            if message.get("reason") == "compiler-artifact" and message.get("executable"):
                executables.append(Path(message["executable"]))
    assert len(executables) == 1
    executable = executables[0]
    cases = []
    for scenario in ("abort", "drop"):
        env[role] = scenario
        command = [str(executable), test, "--exact", "--nocapture", "--test-threads=1"]
        result = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=30)
        (HERE / f"{mutant}-{scenario}.log").write_text(result.stdout)
        assert result.returncode != 0, (mutant, scenario, result.stdout)
        if mutant == "core-only":
            assert "assertion failed: !origin.runtime_is_healthy()" in result.stdout, result.stdout
        else:
            assert f"NATIVE_POISON_DIAGNOSTIC_WITH_HEALTHY_CORES {scenario}" in result.stdout
            assert "WorkerPoisoned" in result.stdout
            assert f"NATIVE_POISON_KNOWN_CORE_CLEANUP {scenario}" not in result.stdout
        cases.append({"scenario": scenario, "command": command, "exit_code": result.returncode,
                      "log_sha256": digest(HERE / f"{mutant}-{scenario}.log")})
    after = {str(path.relative_to(root)): digest(path) for path in root.rglob("*") if path.is_file()}
    assert before == after
    results.append({"mutant": mutant, "source_root": str(root), "build_command": build,
                    "environment": {name: env[name] for name in proof["environment"]},
                    "source_sha256": before, "inputs_unchanged": True,
                    "executable_sha256": digest(executable), "cases": cases})
    print(f"{mutant}: both abort/drop negative controls rejected", flush=True)
production_after = {path: digest(Path(path)) for path in proof["inputs_before"]}
assert production_after == proof["inputs_before"]
report = {"passed": True, "results": results, "production_inputs_unchanged": True,
          "positive_proof_report_sha256": digest(HERE / "proof-final/report.json")}
(HERE / "negative-controls.json").write_text(json.dumps(report, indent=2) + "\n")
