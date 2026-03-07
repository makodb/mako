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
import os
import shlex
import sys

build_dir = os.path.realpath(sys.argv[1])

rows = []
for link_txt in glob.glob(os.path.join(build_dir, "**", "CMakeFiles", "*.dir", "link.txt"), recursive=True):
    try:
        line = open(link_txt, "r", encoding="utf-8").read().splitlines()
    except OSError:
        continue

    out_token = ""
    first_cmd = []
    for raw in line:
        raw = raw.strip()
        if not raw:
            continue
        first_cmd = shlex.split(raw)
        break

    if not first_cmd:
        continue

    for i, tok in enumerate(first_cmd):
        if tok == "-o" and i + 1 < len(first_cmd):
            out_token = first_cmd[i + 1]
            break
        if tok.startswith("-o") and tok != "-o":
            out_token = tok[2:]
            break

    if not out_token:
        # likely ar command: ar qc <out.a> ...
        if len(first_cmd) >= 3 and (first_cmd[1].startswith("-") or first_cmd[1].isalpha()):
            out_token = first_cmd[2]

    if not out_token:
        continue

    out_name = os.path.basename(out_token)
    if out_name.endswith(".a"):
        kind = "static"
        order = 0
    elif out_name.endswith(".so") or ".so." in out_name or out_name.endswith(".dylib"):
        kind = "shared"
        order = 1
    else:
        kind = "executable"
        order = 2

    target_dir_name = os.path.basename(os.path.dirname(link_txt))
    target_name = target_dir_name[:-4] if target_dir_name.endswith(".dir") else target_dir_name
    rows.append((order, target_name, kind, link_txt, out_name))

rows.sort(key=lambda item: (item[0], item[1], item[3]))
for _, target_name, kind, link_txt, out_name in rows:
    print(f"{target_name}\t{kind}\t{out_name}\t{link_txt}")
PY
)

if [[ ${#TARGET_ROWS[@]} -eq 0 ]]; then
  echo "no link.txt targets found under $BUILD_DIR_ABS" >&2
  exit 1
fi

echo "targets discovered: ${#TARGET_ROWS[@]}"

idx=0
for row in "${TARGET_ROWS[@]}"; do
  idx=$((idx + 1))
  IFS=$'\t' read -r target_name kind out_name link_txt <<< "$row"
  echo "==> [$idx/${#TARGET_ROWS[@]}] building $kind target: $target_name ($out_name)"

  MAKO_BUILD_DIR="$BUILD_DIR_ABS" \
  MAKO_CMAKE_TARGET="$target_name" \
  MAKO_CMAKE_LINK_TXT="$link_txt" \
  MAKO_CMAKE_STAGE_TO_BUILD="${MAKO_CMAKE_STAGE_TO_BUILD:-1}" \
  cargo build -p mako-cmake-bridge

done

echo "built targets: ${#TARGET_ROWS[@]}"
