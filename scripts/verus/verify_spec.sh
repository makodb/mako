#!/usr/bin/env bash
# verify_spec.sh [--no-run]
#
# Checks that the group's spec checkout ($GLR, branch mako-spec) is exactly the
# frozen spec version recorded in src/deptran/raft/verus/spec/SPEC_VERSION.toml
# (docs/verus/modification-plan.md §4.5), then runs the full-crate Verus check
# and compares its result with the recorded one:
#
#   1. $GLR is clean and on mako-spec, at the commit of the manifest's tag;
#   2. the commits base..mako-spec are, in order, the committed patch series
#      (compared by `git patch-id --stable`, which ignores dates and hashes);
#   3. the sha256 of every statement-bearing file matches the manifest;
#   4. $VERUS_PIN reports the manifest's Verus version;
#   5. (unless --no-run) the full crate verifies with 0 errors and at least
#      the recorded number of verified items.
#
# Exits 0 when all hold, 1 otherwise, 2 on a usage or setup error. The run's
# log goes to $RESULTS/spec/verify_spec.<tag>.<timestamp>.log.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SPEC_DIR="$REPO_ROOT/src/deptran/raft/verus/spec"
RUN=1
[ "${1:-}" = "--no-run" ] && RUN=0
: "${GLR:?source ~/mako-verus-env.sh first}"
: "${VERUS_PIN:?source ~/mako-verus-env.sh first}"
: "${RESULTS:?source ~/mako-verus-env.sh first}"
exec python3 - "$SPEC_DIR" "$GLR" "$VERUS_PIN" "$RESULTS" "$RUN" <<'PY'
import hashlib, os, re, subprocess, sys, time, tomllib
spec_dir, glr, verus, results, run = sys.argv[1:6]
run = run == "1"
with open(os.path.join(spec_dir, "SPEC_VERSION.toml"), "rb") as f:
    m = tomllib.load(f)
fails = []

def git(*args, text=True):
    return subprocess.run(["git", "-C", glr, *args], check=True, capture_output=True, text=text).stdout

def check(cond, msg):
    print(("ok    " if cond else "FAIL  ") + msg)
    if not cond:
        fails.append(msg)

branch = git("rev-parse", "--abbrev-ref", "HEAD").strip()
check(branch == m["branch"], f"$GLR on branch {m['branch']} (is {branch})")
check(git("status", "--porcelain").strip() == "", "$GLR working tree clean")
head = git("rev-parse", "HEAD").strip()
tag = git("rev-parse", m["tag"] + "^{commit}").strip()
check(head == tag, f"HEAD is tag {m['tag']} ({tag[:10]})")
check(git("rev-parse", m["base_commit"] + "^{commit}").strip().startswith(m["base_commit"]),
      f"base commit {m['base_commit']} present")

def patch_ids(text):
    p = subprocess.run(["git", "-C", glr, "patch-id", "--stable"], input=text, check=True,
                       capture_output=True, text=True)
    return [line.split()[0] for line in p.stdout.splitlines() if line.strip()]

commits = git("rev-list", "--reverse", f"{m['base_commit']}..{m['tag']}").split()
branch_ids = []
for c in commits:
    branch_ids += patch_ids(git("format-patch", "-1", "--stdout", c))
committed_ids = []
for name in m["patches"]:
    with open(os.path.join(spec_dir, "patches", name)) as f:
        committed_ids += patch_ids(f.read())
check(branch_ids == committed_ids,
      f"patch series {m['patches']} == {m['base_commit'][:8]}..{m['tag']} ({len(commits)} commit(s))")

for path, want in sorted(m["sha256"].items()):
    blob = subprocess.run(["git", "-C", glr, "show", f"{m['tag']}:{path}"], check=True,
                          capture_output=True).stdout
    got = hashlib.sha256(blob).hexdigest()
    check(got == want, f"sha256 {path}")

ver = subprocess.run([verus, "--version"], capture_output=True, text=True).stdout
check(m["verus_version"] in ver, f"Verus version {m['verus_version']}")

if run:
    os.makedirs(os.path.join(results, "spec"), exist_ok=True)
    log = os.path.join(results, "spec", f"verify_spec.{m['tag']}.{time.strftime('%Y%m%dT%H%M%S')}.log")
    t0 = time.time()
    with open(log, "w") as f:
        p = subprocess.run([verus, "--crate-type=dylib", "--expand-errors", "src/lib.rs"],
                           cwd=glr, stdout=f, stderr=subprocess.STDOUT)
    txt = open(log).read()
    r = re.findall(r"verification results:: (\d+) verified, (\d+) errors", txt)
    if not r:
        check(False, f"no verification result line (exit {p.returncode}); log {log}")
    else:
        v, e = map(int, r[-1])
        print(f"      {v} verified, {e} errors in {time.time() - t0:.0f} s; log {log}")
        check(e == 0, "0 errors")
        check(v >= m["verified"], f"verified {v} >= recorded {m['verified']}")
print("verify_spec:", "FAIL" if fails else "OK")
sys.exit(1 if fails else 0)
PY
