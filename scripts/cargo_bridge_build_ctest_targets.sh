#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-build}"
BUILD_DIR_ABS="$(realpath "$BUILD_DIR")"

if [[ ! -f "$BUILD_DIR_ABS/compile_commands.json" ]]; then
  echo "missing compile_commands.json in $BUILD_DIR_ABS" >&2
  echo "configure CMake with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON first" >&2
  exit 1
fi

mapfile -t TARGET_ROWS < <(python3 - "$BUILD_DIR_ABS" <<'PY'
import glob
import json
import os
import shlex
import subprocess
import sys
from collections import defaultdict

build_dir = os.path.realpath(sys.argv[1])
repo_root = os.path.dirname(build_dir)

show = subprocess.check_output(["ctest", "--show-only=json-v1"], cwd=build_dir, text=True)
data = json.loads(show)

def resolve_test_token(token: str) -> str:
    if os.path.isabs(token):
        return os.path.realpath(token)
    candidates = [
        os.path.realpath(os.path.join(build_dir, token)),
        os.path.realpath(os.path.join(repo_root, token)),
    ]
    for cand in candidates:
        if os.path.exists(cand):
            return cand
    return candidates[0]

required_execs = []
for test in data.get("tests", []):
    cmd = test.get("command") or []
    if not cmd:
        continue

    exe = None
    launcher = os.path.basename(cmd[0])
    if launcher == "cmake":
        for arg in cmd[1:]:
            if arg.startswith("-DTEST_COMMAND="):
                exe = resolve_test_token(arg.split("=", 1)[1])
                break
    elif launcher in {"bash", "sh"} and "-c" in cmd:
        script = cmd[cmd.index("-c") + 1]
        for tok in shlex.split(script):
            if tok.endswith("/rpcbench") or tok == "./build/rpcbench":
                exe = resolve_test_token(tok)
                break
    else:
        exe = resolve_test_token(cmd[0])

    if exe:
        required_execs.append(exe)

required_execs = sorted(set(required_execs))

# Build mapping from linked executable path -> (target_name, link_txt, output_abs)
output_to_entries = defaultdict(list)
for link_txt in glob.glob(os.path.join(build_dir, "**", "CMakeFiles", "*.dir", "link.txt"), recursive=True):
    try:
        line = open(link_txt, "r", encoding="utf-8").read().strip()
    except OSError:
        continue
    if not line:
        continue

    tokens = shlex.split(line)
    out_token = ""
    for i, tok in enumerate(tokens):
        if tok == "-o" and i + 1 < len(tokens):
            out_token = tokens[i + 1]
            break
        if tok.startswith("-o") and tok != "-o":
            out_token = tok[2:]
            break
    if not out_token:
        continue

    target_dir = os.path.dirname(link_txt)
    cmake_dir = os.path.dirname(target_dir)
    if os.path.basename(cmake_dir) == "CMakeFiles":
        link_work_dir = os.path.dirname(cmake_dir)
    else:
        link_work_dir = build_dir

    output_abs = os.path.realpath(os.path.join(link_work_dir, out_token))
    target_name = os.path.basename(target_dir)
    if target_name.endswith(".dir"):
        target_name = target_name[:-4]

    output_to_entries[output_abs].append((target_name, link_txt, output_abs))

def is_under(path: str, root: str) -> bool:
    try:
        return os.path.commonpath([root, path]) == root
    except ValueError:
        return False

selected = {}
missing = []
for exe in required_execs:
    entries = output_to_entries.get(exe)
    if not entries:
        # Fallback: unique basename match
        basename = os.path.basename(exe)
        by_name = []
        for out, pairs in output_to_entries.items():
            if os.path.basename(out) == basename:
                by_name.extend(pairs)
        if len(by_name) == 1:
            entries = by_name

    if not entries:
        missing.append(exe)
        continue

    # Prefer deterministic first item (there should normally be one)
    target_name, link_txt, mapped_output = sorted(entries, key=lambda p: (p[0], p[1]))[0]

    if link_txt not in selected:
        selected[link_txt] = {
            "target_name": target_name,
            "build_output_path": mapped_output,
            "extra_copy_paths": set(),
        }

    if not is_under(exe, build_dir):
        selected[link_txt]["extra_copy_paths"].add(exe)

if missing:
    print("ERROR: missing link.txt mapping for required executables:", file=sys.stderr)
    for m in missing:
        print(f"  {m}", file=sys.stderr)
    sys.exit(2)

for link_txt, payload in sorted(selected.items(), key=lambda item: item[1]["target_name"]):
    extras = "|".join(sorted(payload["extra_copy_paths"]))
    print(
        f"{payload['target_name']}\t{link_txt}\t{payload['build_output_path']}\t{extras}"
    )
PY
)

if [[ ${#TARGET_ROWS[@]} -eq 0 ]]; then
  echo "no ctest executable targets were discovered in $BUILD_DIR_ABS" >&2
  exit 1
fi

echo "ctest executable targets to build: ${#TARGET_ROWS[@]}"

idx=0
for row in "${TARGET_ROWS[@]}"; do
  idx=$((idx + 1))
  IFS=$'\t' read -r target_name link_txt build_output_path extra_copy_paths <<< "$row"
  echo "==> [$idx/${#TARGET_ROWS[@]}] building target: $target_name"

  MAKO_BUILD_DIR="$BUILD_DIR_ABS" \
  MAKO_CMAKE_TARGET="$target_name" \
  MAKO_CMAKE_LINK_TXT="$link_txt" \
  MAKO_CMAKE_STAGE_TO_BUILD="${MAKO_CMAKE_STAGE_TO_BUILD:-1}" \
  cargo build -p mako-cmake-bridge

  if [[ -n "${extra_copy_paths:-}" ]]; then
    IFS='|' read -r -a extra_paths <<< "$extra_copy_paths"
    for dst_path in "${extra_paths[@]}"; do
      if [[ -z "$dst_path" || "$dst_path" == "$build_output_path" ]]; then
        continue
      fi
      mkdir -p "$(dirname "$dst_path")"
      cp "$build_output_path" "$dst_path"
      echo "    staged extra copy: $dst_path"
    done
  fi

done

echo "built ctest executable targets: ${#TARGET_ROWS[@]}"
