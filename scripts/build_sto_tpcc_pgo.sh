#!/usr/bin/env bash
# Build a Rust-PGO-trained sto_tpcc_bench without profiling its C++ workload.
#
# Usage:
#   scripts/build_sto_tpcc_pgo.sh OUTPUT_DIRECTORY [-- CMAKE_CONFIGURE_ARG ...]
#   STO_TPCC_PGO_NATIVE_BUILD=/validated/ninja-build \
#     scripts/build_sto_tpcc_pgo.sh OUTPUT_DIRECTORY
#
# OUTPUT_DIRECTORY must be a new, whitespace-free directory outside the source
# tree. By default, the script builds an instrumented Rust static library in one
# CMake/Cargo tree, trains it with the one-worker default TPC-C mix, then builds
# the profile-use library and final executable in a second tree.
#
# STO_TPCC_PGO_NATIVE_BUILD selects the recommended reuse-native mode. It must
# name an existing, validated Ninja/CMake build of sto_tpcc_bench from this
# checkout. The script never builds or changes that native object graph. It
# builds both Rust archives in isolated Cargo target directories and relinks
# copies of the benchmark from the recorded Ninja link edge. CMake arguments
# are not accepted in this mode.
#
# Common overrides:
#   STO_TPCC_PGO_TRAIN_CPU       first allowed CPU by default
#   STO_TPCC_PGO_TRAIN_SECONDS   60
#   STO_TPCC_PGO_TRAIN_MIX       45,43,4,4,4
#   STO_TPCC_PGO_TRAIN_ALLOCATOR_MEMORY  1G
#   STO_TPCC_PGO_CONFIG          config/mako_sto_tpcc_local.yml
#   STO_TPCC_PGO_SITE            local_s0
#   STO_TPCC_PGO_BUILD_JOBS      8
#   STO_TPCC_PGO_NATIVE_BUILD    validated native Ninja build (recommended)
#   CMAKE, CARGO, RUSTC, LLVM_PROFDATA, TASKSET, PYTHON3
set -euo pipefail

usage() {
  sed -n '2,29s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 2
}

fail() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

resolve_tool() {
  local candidate=$1
  local directory
  local resolved
  if [[ "$candidate" == */* ]]; then
    [[ -x "$candidate" ]] || die "tool is not executable: $candidate"
    directory="$(cd -- "$(dirname -- "$candidate")" && pwd -P)"
    resolved="$directory/$(basename -- "$candidate")"
  else
    resolved="$(command -v -- "$candidate")" || die "tool not found: $candidate"
    [[ -x "$resolved" ]] || die "tool is not executable: $resolved"
  fi
  printf '%s\n' "$resolved"
}

resolve_optional_tool() {
  local candidate=$1
  if [[ "$candidate" == */* ]]; then
    [[ -x "$candidate" ]] || return 1
  elif ! command -v -- "$candidate" >/dev/null 2>&1; then
    return 1
  fi
  resolve_tool "$candidate"
}

require_unsigned() {
  local name=$1
  local value=$2
  [[ "$value" =~ ^[0-9]+$ ]] || die "$name must be an unsigned integer: $value"
}

require_positive() {
  local name=$1
  local value=$2
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || die "$name must be a positive integer: $value"
}

require_memory_spec() {
  local name=$1
  local value=$2
  local amount
  local suffix
  [[ "$value" =~ ^([1-9][0-9]*)([KMG]?)$ ]] || die \
    "$name must be a positive integer with an optional uppercase K, M, or G suffix: $value"
  amount=${BASH_REMATCH[1]}
  suffix=${BASH_REMATCH[2]}
  "$python_bin" - "$amount" "$suffix" <<'PY' || die "$name exceeds size_t: $value"
import struct
import sys

amount = int(sys.argv[1])
suffix = sys.argv[2]
multiplier = {"": 1, "K": 1 << 10, "M": 1 << 20, "G": 1 << 30}[suffix]
maximum = (1 << (8 * struct.calcsize("P"))) - 1
raise SystemExit(amount > maximum // multiplier)
PY
}

extract_llvm_version() {
  local text=$1
  local line
  local expression='[Ll][Ll][Vv][Mm].*[Vv]ersion:?[[:space:]]+([0-9]+([.][0-9]+)*)'
  while IFS= read -r line; do
    if [[ "$line" =~ $expression ]]; then
      printf '%s\n' "${BASH_REMATCH[1]}"
      return 0
    fi
  done <<< "$text"
  return 1
}

first_allowed_cpu() {
  local allowed
  local first_range
  allowed="$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' /proc/self/status 2>/dev/null)"
  [[ -n "$allowed" ]] || {
    printf '0\n'
    return
  }
  first_range=${allowed%%,*}
  printf '%s\n' "${first_range%%-*}"
}

record_command() {
  local destination=$1
  shift
  {
    printf 'command='
    printf '%q ' "$@"
    printf '\n'
  } >"$destination"
}

source_state_digest() {
  local digest_line
  digest_line="$({
    "$git_bin" -C "$repo_root" rev-parse HEAD
    "$git_bin" -C "$repo_root" diff --binary --no-ext-diff --no-textconv
    "$git_bin" -C "$repo_root" diff --cached --binary --no-ext-diff --no-textconv
    while IFS= read -r -d '' relative_path; do
      [[ -f "$repo_root/$relative_path" ]] || continue
      "$sha256_bin" "$repo_root/$relative_path"
    done < <(
      "$git_bin" -C "$repo_root" ls-files --others --exclude-standard -z -- \
        CMakeLists.txt cmake config crates scripts src
    )
  } | "$sha256_bin")"
  printf '%s\n' "${digest_line%% *}"
}

toolchain_digest() {
  local digest_line
  digest_line="$({
    "$cargo_bin" -Vv
    "$rustc_bin" -vV
    "$profdata_bin" --version
    "$sha256_bin" "$cargo_bin" "$rustc_bin" "$profdata_bin" "$profiler_archive"
  } 2>&1 | "$sha256_bin")"
  printf '%s\n' "${digest_line%% *}"
}

validate_dynamic_links() {
  local binary=$1
  local log=$2
  if [[ -z "$ldd_bin" ]]; then
    printf 'ldd unavailable; dynamic linkage not recorded\n' >"$log"
    return
  fi
  if ! "$ldd_bin" "$binary" >"$log" 2>&1; then
    sed -n '1,120p' "$log" >&2
    fail "could not inspect dynamic linkage for $binary"
  fi
  if grep -q 'not found' "$log"; then
    sed -n '1,120p' "$log" >&2
    fail "unresolved dynamic dependency in $binary"
  fi
}

record_elf_notes() {
  local binary=$1
  local log=$2
  if [[ -n "$readelf_bin" ]]; then
    "$readelf_bin" -n "$binary" >"$log"
  else
    printf 'readelf unavailable; ELF notes not recorded\n' >"$log"
  fi
}

validate_profile_runtime_link() {
  local binary=$1
  local log=$2
  local runtime_symbols
  if [[ -z "$nm_bin" ]]; then
    printf 'nm unavailable; profile runtime symbols not inspected\n' >"$log"
    return
  fi
  "$nm_bin" -u "$binary" >"$log" 2>&1 ||
    fail "could not inspect unresolved symbols in $binary"
  if grep -q '__llvm_profile' "$log"; then
    sed -n '1,120p' "$log" >&2
    fail "instrumented executable has unresolved LLVM profile symbols"
  fi
  runtime_symbols="$("$nm_bin" "$binary" 2>/dev/null | \
    grep '__llvm_profile_runtime' || true)"
  [[ -n "$runtime_symbols" ]] ||
    fail "instrumented executable does not retain __llvm_profile_runtime"
  {
    printf '%s\n' '--- retained profile runtime symbols ---'
    printf '%s\n' "$runtime_symbols"
  } >>"$log"
}

validate_training_result() {
  local log=$1
  local output_json=$2
  "$python_bin" - "$log" "$train_seconds" "$train_mix" \
    "$train_allocator_memory" >"$output_json" <<'PY'
import json
import pathlib
import sys

prefix = "TPCC_BENCH_RESULT "
path = pathlib.Path(sys.argv[1])
expected_seconds = int(sys.argv[2])
configured_mix = [int(value) for value in sys.argv[3].split(",")]
allocator_memory = sys.argv[4]
records = [
    line[len(prefix):]
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
    if line.startswith(prefix)
]
if len(records) != 1:
    raise SystemExit(f"expected exactly one {prefix!r} record, found {len(records)}")
result = json.loads(records[0])

expected = {
    "schema_version": 1,
    "engine": "rust",
    "threads": 1,
    "warehouses": 1,
    "configured_seconds": expected_seconds,
}
for field, value in expected.items():
    if result.get(field) != value:
        raise SystemExit(
            f"unexpected {field}: {result.get(field)!r}; expected {value!r}"
        )
for field in ("commits", "aborts", "attempts"):
    if type(result.get(field)) is not int or result[field] < 0:
        raise SystemExit(f"{field} must be a nonnegative integer")
if result["commits"] == 0:
    raise SystemExit("training committed no transactions")
if result["aborts"] != 0:
    raise SystemExit(f"training must have zero aborts; found {result['aborts']}")
if result["attempts"] != result["commits"] + result["aborts"]:
    raise SystemExit("attempts != commits + aborts")

mix_names = ("NewOrder", "Payment", "Delivery", "OrderStatus", "StockLevel")
mix_keys = set(mix_names)
mix = result.get("mix")
if not isinstance(mix, dict) or set(mix) != mix_keys:
    raise SystemExit("training result has an invalid TPC-C mix record")
if any(type(value) is not int or value < 0 for value in mix.values()):
    raise SystemExit("TPC-C mix counters must be nonnegative integers")
if sum(mix.values()) != result["commits"]:
    raise SystemExit("sum(mix counters) != commits")
for name, weight in zip(mix_names, configured_mix, strict=True):
    if weight > 0 and mix[name] == 0:
        raise SystemExit(f"training committed no {name} transactions")

result["environment_overrides"] = {
    "MAKO_TPCC_ALLOCATOR_MEMORY": allocator_memory,
    "MAKO_TPCC_WORKLOAD_MIX": ",".join(str(value) for value in configured_mix),
}
json.dump(result, sys.stdout, indent=2, sort_keys=True)
sys.stdout.write("\n")
PY
}

cmake_cache_value() {
  local cache=$1
  local key=$2
  sed -n "s/^${key}:[^=]*=//p" "$cache" | head -n 1
}

compare_cmake_toolchains() {
  local generate_cache=$1
  local use_cache=$2
  local key
  local generate_value
  local use_value
  local -a keys=(
    CMAKE_AR
    CMAKE_BUILD_TYPE
    CMAKE_C_COMPILER
    CMAKE_CXX_COMPILER
    CMAKE_CXX_FLAGS
    CMAKE_GENERATOR
    CMAKE_LINKER
    CMAKE_MAKE_PROGRAM
    CMAKE_RANLIB
    MAKO_CARGO_EXECUTABLE
    MAKO_USE_RAFT
  )
  for key in "${keys[@]}"; do
    generate_value="$(cmake_cache_value "$generate_cache" "$key")"
    use_value="$(cmake_cache_value "$use_cache" "$key")"
    [[ "$generate_value" == "$use_value" ]] ||
      fail "CMake toolchain mismatch for $key: $generate_value != $use_value"
  done
}

record_native_archive_hashes() {
  local build_directory=$1
  local destination=$2
  local -a archives=()
  mapfile -d '' -t archives < <(
    find "$build_directory" -type f \
      \( -name 'libmako.a' -o -name 'libmasstree.a' \) -print0 | sort -z
  )
  if [[ ${#archives[@]} -eq 0 ]]; then
    : >"$destination"
  else
    "$sha256_bin" "${archives[@]}" >"$destination"
  fi
}

native_graph_digest() {
  local input_list=$1
  local digest_line
  local path
  local -a inputs=()
  while IFS= read -r path; do
    [[ -n "$path" ]] || continue
    [[ -f "$path" ]] || fail "native link input disappeared: $path"
    inputs+=("$path")
  done <"$input_list"
  [[ ${#inputs[@]} -gt 0 ]] || fail "native link input manifest is empty"
  digest_line="$({
    "$sha256_bin" "${inputs[@]}"
  } | "$sha256_bin")"
  printf '%s\n' "${digest_line%% *}"
}

record_native_graph_hashes() {
  local input_list=$1
  local destination=$2
  local path
  local -a inputs=()
  while IFS= read -r path; do
    [[ -n "$path" ]] || continue
    [[ -f "$path" ]] || fail "native link input disappeared: $path"
    inputs+=("$path")
  done <"$input_list"
  [[ ${#inputs[@]} -gt 0 ]] || fail "native link input manifest is empty"
  "$sha256_bin" "${inputs[@]}" >"$destination"
}

# Extracts the one benchmark link edge emitted by Ninja, replaces only the
# Rust archive and output, removes Ninja's dependency-file side effect, and
# emits a directly executable argv-preserving shell script. Parsing with shlex
# deliberately rejects response-file/archive layouts that cannot be audited.
prepare_native_relink() {
  local replacement_archive=$1
  local output_binary=$2
  local profile_runtime=$3
  local command_script=$4
  local metadata_file=$5
  local input_list=$6
  local commands_file=$7
  local original_command_file=$8

  "$native_ninja_bin" -C "$native_build" -t commands sto_tpcc_bench \
    >"$commands_file"
  "$python_bin" - \
    "$native_build" "$native_cxx_compiler" "$native_rust_archive" \
    "$replacement_archive" "$output_binary" "$profile_runtime" \
    "$command_script" "$metadata_file" "$input_list" \
    "$original_command_file" "$commands_file" <<'PY'
import os
import pathlib
import shlex
import shutil
import sys

(
    build_text,
    expected_compiler_text,
    expected_archive_text,
    replacement_text,
    output_text,
    runtime_text,
    script_text,
    metadata_text,
    inputs_text,
    original_text,
    commands_text,
) = sys.argv[1:]

build = pathlib.Path(build_text).resolve()
expected_compiler = pathlib.Path(expected_compiler_text).resolve()
expected_archive = (
    pathlib.Path(expected_archive_text).resolve() if expected_archive_text else None
)
replacement = pathlib.Path(replacement_text).resolve()
output = pathlib.Path(output_text).resolve()
runtime = pathlib.Path(runtime_text).resolve() if runtime_text else None

if not replacement.is_file():
    raise SystemExit(f"replacement Rust archive does not exist: {replacement}")
if runtime is not None and not runtime.is_file():
    raise SystemExit(f"Rust profile runtime does not exist: {runtime}")


def unwrap(command: str) -> str:
    command = command.strip()
    if command.startswith(": && ") and command.endswith(" && :"):
        return command[5:-5].strip()
    return command


matches = []
parse_failures = []
for raw in pathlib.Path(commands_text).read_text(
    encoding="utf-8", errors="strict"
).splitlines():
    if "libsto_tpcc_ffi.a" not in raw or "sto_tpcc_bench" not in raw:
        continue
    command = unwrap(raw)
    try:
        argv = shlex.split(command, posix=True)
    except ValueError as error:
        parse_failures.append(str(error))
        continue
    if any(token in {"&&", "||", ";", "|"} for token in argv):
        continue
    archives = [
        index
        for index, token in enumerate(argv)
        if pathlib.Path(token).name == "libsto_tpcc_ffi.a"
    ]
    outputs = [
        index
        for index in range(len(argv) - 1)
        if argv[index] == "-o"
        and pathlib.Path(argv[index + 1]).name == "sto_tpcc_bench"
    ]
    if len(archives) == 1 and len(outputs) == 1:
        matches.append((raw, argv, archives[0], outputs[0] + 1))

if len(matches) != 1:
    detail = f"; parse failures: {parse_failures}" if parse_failures else ""
    raise SystemExit(
        "expected exactly one auditable sto_tpcc_bench Ninja link command, "
        f"found {len(matches)}{detail}"
    )

raw, argv, archive_index, output_index = matches[0]
if any(token.startswith("@") for token in argv):
    raise SystemExit("Ninja benchmark link command uses an unsupported response file")

compiler_token = argv[0]
if "/" in compiler_token:
    actual_compiler = pathlib.Path(compiler_token)
    if not actual_compiler.is_absolute():
        actual_compiler = build / actual_compiler
    actual_compiler = actual_compiler.resolve()
else:
    found = shutil.which(compiler_token)
    if found is None:
        raise SystemExit(f"link compiler is not executable: {compiler_token}")
    actual_compiler = pathlib.Path(found).resolve()
if actual_compiler != expected_compiler:
    raise SystemExit(
        f"Ninja link compiler differs from CMakeCache: "
        f"{actual_compiler} != {expected_compiler}"
    )

original_archive = pathlib.Path(argv[archive_index])
if not original_archive.is_absolute():
    original_archive = build / original_archive
original_archive = original_archive.resolve()
if expected_archive is not None and original_archive != expected_archive:
    raise SystemExit(
        f"Ninja Rust archive differs from CMakeCache: "
        f"{original_archive} != {expected_archive}"
    )

original_output = pathlib.Path(argv[output_index])
if not original_output.is_absolute():
    original_output = build / original_output
if original_output.resolve() != (build / "sto_tpcc_bench").resolve():
    raise SystemExit(f"unexpected Ninja benchmark output: {original_output}")

transformed = []
index = 0
while index < len(argv):
    token = argv[index]
    if (
        token == "-Xlinker"
        and index + 1 < len(argv)
        and argv[index + 1].startswith("--dependency-file=")
    ):
        index += 2
        continue
    if token.startswith("-Wl,--dependency-file="):
        index += 1
        continue
    if index == archive_index:
        transformed.append(str(replacement))
        if runtime is not None:
            transformed.extend(["-Wl,-u,__llvm_profile_runtime", str(runtime)])
    elif index == output_index:
        transformed.append(str(output))
    else:
        transformed.append(token)
    index += 1

native_inputs = {
    (build / "CMakeCache.txt").resolve(),
    (build / "build.ninja").resolve(),
}
rules = build / "CMakeFiles" / "rules.ninja"
if rules.is_file():
    native_inputs.add(rules.resolve())

for index, token in enumerate(argv):
    if index in {archive_index, output_index} or token.startswith("-"):
        continue
    candidate = pathlib.Path(token)
    if not candidate.is_absolute():
        candidate = build / candidate
    if candidate.is_file():
        native_inputs.add(candidate.resolve())
        continue
    name = candidate.name
    looks_like_link_input = (
        name.endswith((".o", ".a", ".rlib"))
        or ".so" in name
        or ".dylib" in name
    )
    if looks_like_link_input:
        raise SystemExit(f"native link input is missing: {candidate}")

script = pathlib.Path(script_text)
script.write_text(
    "#!/usr/bin/env bash\nset -euo pipefail\nexec "
    + shlex.join(transformed)
    + "\n",
    encoding="utf-8",
)
script.chmod(0o700)
pathlib.Path(original_text).write_text(raw + "\n", encoding="utf-8")
pathlib.Path(inputs_text).write_text(
    "".join(f"{path}\n" for path in sorted(native_inputs)), encoding="utf-8"
)
pathlib.Path(metadata_text).write_text(
    "".join(
        [
            f"native_build={build}\n",
            f"compiler={actual_compiler}\n",
            f"original_archive={original_archive}\n",
            f"replacement_archive={replacement}\n",
            f"original_output={original_output.resolve()}\n",
            f"replacement_output={output}\n",
            f"profile_runtime={runtime or ''}\n",
        ]
    ),
    encoding="utf-8",
)
PY
}

build_rust_archive() {
  local target_directory=$1
  local rustflags=$2
  local build_profile_pattern=$3
  local command_file=$4
  local log_file=$5
  local -a cargo_command=(
    "$cargo_bin" build --release --locked
    --manifest-path "$repo_root/crates/Cargo.toml"
    --package sto-tpcc-ffi
  )
  local -a cargo_environment=(
    "CARGO_TARGET_DIR=$target_directory"
    CARGO_INCREMENTAL=0
    CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1
    CARGO_PROFILE_RELEASE_DEBUG=1
    CARGO_PROFILE_RELEASE_LTO=fat
    CARGO_PROFILE_RELEASE_OPT_LEVEL=3
    CARGO_PROFILE_RELEASE_PANIC=unwind
    "RUSTFLAGS=$rustflags"
  )
  if [[ -n "$build_profile_pattern" ]]; then
    record_command "$command_file" env \
      "${cargo_environment[@]}" "LLVM_PROFILE_FILE=$build_profile_pattern" \
      "${cargo_command[@]}"
    env "${cargo_environment[@]}" "LLVM_PROFILE_FILE=$build_profile_pattern" \
      "${cargo_command[@]}" 2>&1 | "$tee_bin" "$log_file"
  else
    record_command "$command_file" env -u LLVM_PROFILE_FILE \
      "${cargo_environment[@]}" "${cargo_command[@]}"
    env -u LLVM_PROFILE_FILE "${cargo_environment[@]}" \
      "${cargo_command[@]}" 2>&1 | "$tee_bin" "$log_file"
  fi
}

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
  usage
  exit 0
fi
[[ $# -ge 1 ]] || {
  usage >&2
  exit 2
}

output_argument=$1
shift
if [[ ${1:-} == -- ]]; then
  shift
fi
cmake_user_arguments=("$@")

for argument in "${cmake_user_arguments[@]}"; do
  case "$argument" in
    -S|-S*|-B|-B*|--source|--source=*|--build|--build=*)
      die "the script controls CMake source/build directories: $argument"
      ;;
    -DCMAKE_BUILD_TYPE=*|-DCMAKE_BUILD_TYPE:*=*|\
    -DCMAKE_EXE_LINKER_FLAGS=*|-DCMAKE_EXE_LINKER_FLAGS:*=*|\
    -DMAKO_CARGO_EXECUTABLE=*|-DMAKO_CARGO_EXECUTABLE:*=*|\
    -DMAKO_USE_RAFT=*|-DMAKO_USE_RAFT:*=*|\
    -DSTO_TPCC_RUST_TARGET_DIR=*|-DSTO_TPCC_RUST_TARGET_DIR:*=*|\
    -DSTO_TPCC_RUST_EXTRA_RUSTFLAGS=*|-DSTO_TPCC_RUST_EXTRA_RUSTFLAGS:*=*)
      die "the script controls this CMake definition: $argument"
      ;;
  esac
done

[[ -n "$output_argument" ]] || die "PGO output path must not be empty"
if [[ "$output_argument" =~ [[:space:]] ]]; then
  die "PGO output path must not contain whitespace: $output_argument"
fi

script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
script_path="$script_directory/$(basename -- "${BASH_SOURCE[0]}")"
repo_root="$(cd -- "$script_directory/.." && pwd -P)"

build_mode=clean
native_build=
if [[ -n ${STO_TPCC_PGO_NATIVE_BUILD:-} ]]; then
  [[ -d "$STO_TPCC_PGO_NATIVE_BUILD" ]] ||
    die "STO_TPCC_PGO_NATIVE_BUILD is not a directory: $STO_TPCC_PGO_NATIVE_BUILD"
  native_build="$(cd -- "$STO_TPCC_PGO_NATIVE_BUILD" && pwd -P)"
  build_mode=reuse-native
  [[ ${#cmake_user_arguments[@]} -eq 0 ]] ||
    die "CMake configure arguments are unavailable with STO_TPCC_PGO_NATIVE_BUILD"
fi

cmake_bin="$(resolve_tool "${CMAKE:-cmake}")"
cargo_bin="$(resolve_tool "${CARGO:-cargo}")"
rustc_bin="$(resolve_tool "${RUSTC:-rustc}")"
taskset_bin="$(resolve_tool "${TASKSET:-taskset}")"
python_bin="$(resolve_tool "${PYTHON3:-python3}")"
git_bin="$(resolve_tool "${GIT:-git}")"
sha256_bin="$(resolve_tool "${SHA256SUM:-sha256sum}")"
tee_bin="$(resolve_tool "${TEE:-tee}")"
ldd_bin="$(resolve_optional_tool "${LDD:-ldd}" || true)"
readelf_bin="$(resolve_optional_tool "${READELF:-readelf}" || true)"
nm_bin="$(resolve_optional_tool "${NM:-nm}" || true)"
lscpu_bin="$(resolve_optional_tool "${LSCPU:-lscpu}" || true)"
rustup_bin="$(resolve_optional_tool "${RUSTUP:-rustup}" || true)"

for variable in \
  RUSTFLAGS CARGO_ENCODED_RUSTFLAGS CARGO_BUILD_RUSTFLAGS CARGO_BUILD_TARGET \
  CARGO_TARGET_DIR LLVM_PROFILE_FILE RUSTC_WRAPPER RUSTC_WORKSPACE_WRAPPER; do
  if [[ -n "${!variable:-}" ]]; then
    die "$variable must be unset so the PGO build is controlled"
  fi
done
tpcc_fallback_variables=(
  MAKO_STO_TPCC_DISABLE_PAYMENT_PREFIX
  MAKO_STO_TPCC_DISABLE_PAYMENT_FULL
  MAKO_STO_TPCC_DISABLE_NEW_ORDER_FULL
  MAKO_STO_TPCC_DISABLE_DELIVERY_FULL
  MAKO_STO_TPCC_DISABLE_STOCK_LEVEL_FULL
)
for variable in "${tpcc_fallback_variables[@]}"; do
  if [[ -v "$variable" ]]; then
    die "$variable must be unset so PGO trains the complete Rust transaction paths"
  fi
done
unset "${tpcc_fallback_variables[@]}"
inherited_environment="$(env)"
if grep -q '^CARGO_TARGET_.*_RUSTFLAGS=' <<< "$inherited_environment"; then
  die "target-specific Cargo RUSTFLAGS must be unset"
fi
if grep -q '^CARGO_PROFILE_RELEASE_' <<< "$inherited_environment"; then
  die "inherited CARGO_PROFILE_RELEASE_* variables must be unset"
fi
unset RUSTFLAGS CARGO_ENCODED_RUSTFLAGS CARGO_BUILD_RUSTFLAGS
unset CARGO_BUILD_TARGET CARGO_TARGET_DIR LLVM_PROFILE_FILE
export RUSTC="$rustc_bin"

rustup_toolchain_origin=inherited
if [[ -z ${RUSTUP_TOOLCHAIN:-} && -n "$rustup_bin" ]]; then
  rustup_real="$(readlink -f -- "$rustup_bin")"
  cargo_real="$(readlink -f -- "$cargo_bin")"
  rustc_real="$(readlink -f -- "$rustc_bin")"
  if [[ "$cargo_real" == "$rustup_real" || "$rustc_real" == "$rustup_real" ]]; then
    active_toolchain="$($rustup_bin show active-toolchain | awk 'NR == 1 { print $1 }')"
    [[ -n "$active_toolchain" ]] || die "could not determine the active rustup toolchain"
    export RUSTUP_TOOLCHAIN="$active_toolchain"
    rustup_toolchain_origin=active-toolchain
  fi
fi

train_cpu=${STO_TPCC_PGO_TRAIN_CPU:-$(first_allowed_cpu)}
train_seconds=${STO_TPCC_PGO_TRAIN_SECONDS:-60}
train_mix=${STO_TPCC_PGO_TRAIN_MIX:-45,43,4,4,4}
train_allocator_memory=${STO_TPCC_PGO_TRAIN_ALLOCATOR_MEMORY:-1G}
train_config=${STO_TPCC_PGO_CONFIG:-$repo_root/config/mako_sto_tpcc_local.yml}
train_site=${STO_TPCC_PGO_SITE:-local_s0}
build_jobs=${STO_TPCC_PGO_BUILD_JOBS:-8}

require_unsigned STO_TPCC_PGO_TRAIN_CPU "$train_cpu"
require_positive STO_TPCC_PGO_TRAIN_SECONDS "$train_seconds"
require_memory_spec STO_TPCC_PGO_TRAIN_ALLOCATOR_MEMORY "$train_allocator_memory"
require_positive STO_TPCC_PGO_BUILD_JOBS "$build_jobs"
[[ -f "$train_config" ]] || die "TPC-C shard config does not exist: $train_config"
[[ -n "$train_site" ]] || die "STO_TPCC_PGO_SITE must not be empty"
"$taskset_bin" -c "$train_cpu" /bin/true >/dev/null ||
  die "training CPU is unavailable to this process: $train_cpu"

IFS=, read -r -a mix_values <<< "$train_mix"
[[ ${#mix_values[@]} -eq 5 ]] ||
  die "STO_TPCC_PGO_TRAIN_MIX must contain five comma-separated integers"
mix_total=0
for mix_value in "${mix_values[@]}"; do
  require_unsigned STO_TPCC_PGO_TRAIN_MIX "$mix_value"
  mix_total=$((mix_total + 10#$mix_value))
done
[[ $mix_total -eq 100 ]] || die "STO_TPCC_PGO_TRAIN_MIX must sum to 100"

rustc_version="$($rustc_bin -vV 2>&1)"
cargo_version="$($cargo_bin -Vv 2>&1)"
rust_host="$(sed -n 's/^host: //p' <<< "$rustc_version")"
rust_sysroot="$($rustc_bin --print sysroot)"
[[ -n "$rust_host" && -n "$rust_sysroot" ]] ||
  die "could not determine the Rust host and sysroot"
bundled_profdata="$rust_sysroot/lib/rustlib/$rust_host/bin/llvm-profdata"
if [[ -n ${LLVM_PROFDATA:-} ]]; then
  profdata_bin="$(resolve_tool "$LLVM_PROFDATA")"
else
  [[ -x "$bundled_profdata" ]] || die \
    "Rust llvm-profdata is missing; run: rustup component add llvm-tools-preview"
  profdata_bin="$bundled_profdata"
fi
profdata_version="$($profdata_bin --version 2>&1)"
rustc_llvm_version="$(extract_llvm_version "$rustc_version")" ||
  die "could not determine rustc's LLVM version"
profdata_llvm_version="$(extract_llvm_version "$profdata_version")" ||
  die "could not determine llvm-profdata's LLVM version"
if [[ ${rustc_llvm_version%%.*} != "${profdata_llvm_version%%.*}" ]]; then
  die "rustc LLVM $rustc_llvm_version is incompatible with llvm-profdata $profdata_llvm_version"
fi

rust_target_lib="$rust_sysroot/lib/rustlib/$rust_host/lib"
shopt -s nullglob
profiler_archives=("$rust_target_lib"/libprofiler_builtins-*.rlib)
shopt -u nullglob
[[ ${#profiler_archives[@]} -eq 1 ]] || die \
  "expected one Rust libprofiler_builtins archive in $rust_target_lib; found ${#profiler_archives[@]}"
profiler_archive=${profiler_archives[0]}
profile_runtime_link_flags="-Wl,-u,__llvm_profile_runtime -Wl,--whole-archive $profiler_archive -Wl,--no-whole-archive"

native_cache=
native_ninja_bin=
native_cxx_compiler=
native_cxx_version=
native_rust_archive=
if [[ "$build_mode" == reuse-native ]]; then
  native_cache="$native_build/CMakeCache.txt"
  [[ -f "$native_cache" && -f "$native_build/build.ninja" ]] || die \
    "STO_TPCC_PGO_NATIVE_BUILD must contain CMakeCache.txt and build.ninja"
  native_generator="$(cmake_cache_value "$native_cache" CMAKE_GENERATOR)"
  native_build_type="$(cmake_cache_value "$native_cache" CMAKE_BUILD_TYPE)"
  native_source="$(cmake_cache_value "$native_cache" CMAKE_HOME_DIRECTORY)"
  native_cxx_candidate="$(cmake_cache_value "$native_cache" CMAKE_CXX_COMPILER)"
  native_ninja_candidate="$(cmake_cache_value "$native_cache" CMAKE_MAKE_PROGRAM)"
  [[ "$native_generator" == Ninja ]] || die \
    "native build must use the Ninja generator: ${native_generator:-unset}"
  [[ "$native_build_type" == Release ]] || die \
    "native build must use CMAKE_BUILD_TYPE=Release: ${native_build_type:-unset}"
  [[ -d "$native_source" ]] || die \
    "native CMake source directory does not exist: ${native_source:-unset}"
  native_source="$(cd -- "$native_source" && pwd -P)"
  [[ "$native_source" == "$repo_root" ]] || die \
    "native build belongs to a different source tree: $native_source != $repo_root"
  [[ -n "$native_cxx_candidate" ]] || die \
    "native CMake cache does not identify CMAKE_CXX_COMPILER"
  native_cxx_compiler="$(resolve_tool "$native_cxx_candidate")"
  if [[ -n ${NINJA:-} ]]; then
    native_ninja_bin="$(resolve_tool "$NINJA")"
  else
    [[ -n "$native_ninja_candidate" ]] || die \
      "native CMake cache does not identify CMAKE_MAKE_PROGRAM"
    native_ninja_bin="$(resolve_tool "$native_ninja_candidate")"
  fi
  native_target_dir="$(cmake_cache_value "$native_cache" STO_TPCC_RUST_TARGET_DIR)"
  if [[ -n "$native_target_dir" ]]; then
    native_rust_archive="$native_target_dir/release/libsto_tpcc_ffi.a"
  fi
  native_cxx_version="$($native_cxx_compiler --version 2>&1)"
fi

initial_source_digest="$(source_state_digest)"
initial_toolchain_digest="$(toolchain_digest)"
initial_config_hash_line="$($sha256_bin "$train_config")"
initial_config_hash=${initial_config_hash_line%% *}

output_parent_argument="$(dirname -- "$output_argument")"
output_name="$(basename -- "$output_argument")"
[[ "$output_name" != . && "$output_name" != .. ]] ||
  die "invalid PGO output path: $output_argument"
[[ -d "$output_parent_argument" ]] ||
  die "PGO output parent directory does not exist: $output_parent_argument"
output_parent="$(cd -- "$output_parent_argument" && pwd -P)"
output="$output_parent/$output_name"
[[ "$output" != "$repo_root" && "$output" != "$repo_root/"* ]] ||
  die "PGO output path must be outside the source tree: $output"
if [[ -e "$output" || -L "$output" ]]; then
  die "refusing to reuse PGO output path: $output"
fi

mkdir -- "$output"
status_file="$output/STATUS"
status_complete=0
printf 'building started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$status_file"
record_exit() {
  local result=$?
  if ((status_complete == 0)); then
    printf 'failed exit_status=%d finished_utc=%s\n' \
      "$result" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$status_file" || true
  fi
  return "$result"
}
trap record_exit EXIT

generate_cmake="$output/cmake-generate"
generate_cargo="$output/cargo-generate"
use_cmake="$output/cmake-use"
use_cargo="$output/cargo-use"
profile_raw="$output/profile-raw"
build_profile_raw="$output/build-profile-raw"
merged_profile="$output/merged.profdata"
mkdir -- "$generate_cargo" "$use_cargo" "$profile_raw" "$build_profile_raw"

"$git_bin" -C "$repo_root" status --porcelain=v2 >"$output/source-status.txt"
{
  "$git_bin" -C "$repo_root" diff --binary --no-ext-diff --no-textconv
  "$git_bin" -C "$repo_root" diff --cached --binary --no-ext-diff --no-textconv
} >"$output/source.patch"
mkdir -- "$output/source-untracked"
: >"$output/source-untracked-files.txt"
while IFS= read -r -d '' relative_path; do
  [[ -f "$repo_root/$relative_path" ]] || continue
  printf '%q\n' "$relative_path" >>"$output/source-untracked-files.txt"
  mkdir -p -- "$output/source-untracked/$(dirname -- "$relative_path")"
  cp -- "$repo_root/$relative_path" "$output/source-untracked/$relative_path"
done < <(
  "$git_bin" -C "$repo_root" ls-files --others --exclude-standard -z -- \
    CMakeLists.txt cmake config crates scripts src
)
cp -- "$script_path" "$output/build-script.sh"

common_rustflags="-C target-cpu=native -C force-frame-pointers=yes"
generate_flags="-Cprofile-generate=$profile_raw"
use_flags="-Cprofile-use=$merged_profile -Cllvm-args=-pgo-warn-missing-function"
# A clean build passes the two PGO-only strings through CMake, whose ordinary
# Rust archive rule prepends `common_rustflags`.  Reuse-native mode bypasses
# that rule and invokes Cargo directly, so it must prepend the same flags here.
# Otherwise the reused C++ graph is `-march=native` while the replacement Rust
# archive silently falls back to rustc's generic x86-64 target.
if [[ "$build_mode" == reuse-native ]]; then
  generate_flags="$common_rustflags $generate_flags"
  use_flags="$common_rustflags $use_flags"
fi
selected_pkg_config=
if [[ "$build_mode" == clean ]]; then
  auto_generator_arguments=()
  generator_was_supplied=0
  pkg_config_was_supplied=0
  for argument in "${cmake_user_arguments[@]}"; do
    case "$argument" in
      -G|-G*|--generator|--generator=*) generator_was_supplied=1 ;;
      -DPKG_CONFIG_EXECUTABLE=*|-DPKG_CONFIG_EXECUTABLE:*=*)
        pkg_config_was_supplied=1
        ;;
    esac
  done
  if ((generator_was_supplied == 0)) && [[ -z ${CMAKE_GENERATOR:-} ]]; then
    if [[ -n ${NINJA:-} ]]; then
      ninja_bin="$(resolve_tool "$NINJA")"
    else
      ninja_bin="$(resolve_optional_tool ninja || true)"
    fi
    if [[ -n "$ninja_bin" ]]; then
      auto_generator_arguments=(-G Ninja "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninja_bin")
    fi
  fi

  # Linuxbrew's pkg-config deliberately omits the system pkg-config directory
  # on some zoo hosts, even though the DPDK development package is present.
  auto_dependency_arguments=()
  if ((pkg_config_was_supplied == 0)); then
    pkg_config_candidates=("${PKG_CONFIG:-pkg-config}")
    if [[ ${PKG_CONFIG:-pkg-config} != /usr/bin/pkg-config ]]; then
      pkg_config_candidates+=(/usr/bin/pkg-config)
    fi
    for pkg_config_candidate in "${pkg_config_candidates[@]}"; do
      resolved_pkg_config="$(resolve_optional_tool "$pkg_config_candidate" || true)"
      [[ -n "$resolved_pkg_config" ]] || continue
      if "$resolved_pkg_config" --exists libdpdk; then
        selected_pkg_config=$resolved_pkg_config
        break
      fi
    done
    [[ -n "$selected_pkg_config" ]] || die \
      "no pkg-config candidate can resolve required package libdpdk"
    auto_dependency_arguments=(
      "-DPKG_CONFIG_EXECUTABLE:FILEPATH=$selected_pkg_config"
    )
  fi

  common_cmake_arguments=(
    "${auto_generator_arguments[@]}"
    "${auto_dependency_arguments[@]}"
    "${cmake_user_arguments[@]}"
    -DCMAKE_BUILD_TYPE:STRING=Release
    -DMAKO_USE_RAFT:BOOL=OFF
    "-DMAKO_CARGO_EXECUTABLE:FILEPATH=$cargo_bin"
  )
  generate_configure_command=(
    "$cmake_bin" -S "$repo_root" -B "$generate_cmake"
    "${common_cmake_arguments[@]}"
    "-DCMAKE_EXE_LINKER_FLAGS:STRING=$profile_runtime_link_flags"
    "-DSTO_TPCC_RUST_TARGET_DIR:PATH=$generate_cargo"
    "-DSTO_TPCC_RUST_EXTRA_RUSTFLAGS:STRING=$generate_flags"
  )
  generate_build_command=(
    "$cmake_bin" --build "$generate_cmake" --target sto_tpcc_bench
    --parallel "$build_jobs"
  )
fi

{
  printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'host=%s\n' "$(hostname)"
  printf 'kernel=%s\n' "$(uname -srvmo)"
  printf 'repo_root=%s\n' "$repo_root"
  printf 'git_head=%s\n' "$($git_bin -C "$repo_root" rev-parse HEAD)"
  printf 'git_branch=%s\n' \
    "$($git_bin -C "$repo_root" symbolic-ref --short -q HEAD || printf '(detached)')"
  printf 'build_mode=%s\n' "$build_mode"
  printf 'source_state_sha256=%s\n' "$initial_source_digest"
  printf 'toolchain_sha256=%s\n' "$initial_toolchain_digest"
  printf 'config_sha256=%s\n' "$initial_config_hash"
  printf 'cmake=%s\n' "$cmake_bin"
  printf 'cargo=%s\n' "$cargo_bin"
  printf 'rustc=%s\n' "$rustc_bin"
  printf 'llvm_profdata=%s\n' "$profdata_bin"
  printf 'rustc_llvm_version=%s\n' "$rustc_llvm_version"
  printf 'llvm_profdata_version=%s\n' "$profdata_llvm_version"
  printf 'profiler_builtins=%s\n' "$profiler_archive"
  printf 'profiler_builtins_sha256=%s\n' \
    "$("$sha256_bin" "$profiler_archive" | awk '{ print $1 }')"
  printf 'taskset=%s\n' "$taskset_bin"
  printf 'python=%s\n' "$python_bin"
  printf 'path=%q\n' "$PATH"
  printf 'pythonpath=%q\n' "${PYTHONPATH:-}"
  printf 'ld_library_path=%q\n' "${LD_LIBRARY_PATH:-}"
  printf 'rustup_toolchain=%q\n' "${RUSTUP_TOOLCHAIN:-}"
  printf 'rustup_toolchain_origin=%s\n' "$rustup_toolchain_origin"
  printf 'rustup=%q\n' "${rustup_bin:-unavailable}"
  printf 'cargo_home=%q\n' "${CARGO_HOME:-}"
  printf 'rustup_home=%q\n' "${RUSTUP_HOME:-}"
  printf 'cc=%q\n' "${CC:-}"
  printf 'cxx=%q\n' "${CXX:-}"
  printf 'cmake_generator=%q\n' "${CMAKE_GENERATOR:-}"
  printf 'pkg_config=%q\n' "${selected_pkg_config:-caller-selected}"
  printf 'pkg_config_path=%q\n' "${PKG_CONFIG_PATH:-}"
  printf 'pkg_config_libdir=%q\n' "${PKG_CONFIG_LIBDIR:-}"
  printf 'pkg_config_sysroot_dir=%q\n' "${PKG_CONFIG_SYSROOT_DIR:-}"
  printf 'train_cpu=%s\n' "$train_cpu"
  printf 'train_seconds=%s\n' "$train_seconds"
  printf 'train_mix=%s\n' "$train_mix"
  printf 'train_allocator_memory=%s\n' "$train_allocator_memory"
  printf 'tpcc_fallback_overrides=all-unset\n'
  printf 'train_config=%s\n' "$train_config"
  printf 'train_site=%s\n' "$train_site"
  printf 'build_jobs=%s\n' "$build_jobs"
  printf 'common_rustflags=%q\n' "$common_rustflags"
  printf 'generate_rustflags=%q\n' "$generate_flags"
  printf 'use_rustflags=%q\n' "$use_flags"
  printf 'generate_profile_runtime_link_flags=%q\n' "$profile_runtime_link_flags"
  if [[ "$build_mode" == reuse-native ]]; then
    printf 'native_build=%s\n' "$native_build"
    printf 'native_source=%s\n' "$native_source"
    printf 'native_generator=%s\n' "$native_generator"
    printf 'native_build_type=%s\n' "$native_build_type"
    printf 'native_ninja=%s\n' "$native_ninja_bin"
    printf 'native_cxx=%s\n' "$native_cxx_compiler"
    printf 'native_cached_rust_archive=%s\n' "$native_rust_archive"
    printf '%s\n' '--- native C++ compiler --version ---'
    printf '%s\n' "$native_cxx_version"
  fi
  printf '%s\n' '--- cmake --version ---'
  "$cmake_bin" --version
  printf '%s\n' '--- cargo -Vv ---'
  printf '%s\n' "$cargo_version"
  printf '%s\n' '--- rustc -vV ---'
  printf '%s\n' "$rustc_version"
  printf '%s\n' '--- llvm-profdata --version ---'
  printf '%s\n' "$profdata_version"
  if [[ -n "$lscpu_bin" ]]; then
    printf '%s\n' '--- lscpu ---'
    "$lscpu_bin"
  fi
  printf '%s\n' '--- user CMake arguments ---'
  printf '%q\n' "${cmake_user_arguments[@]}"
} >"$output/provenance.txt"

instrumented_archive="$generate_cargo/release/libsto_tpcc_ffi.a"
native_graph_digest_initial=
if [[ "$build_mode" == clean ]]; then
  record_command "$output/configure-generate.command" "${generate_configure_command[@]}"
  "${generate_configure_command[@]}" 2>&1 |
    "$tee_bin" "$output/configure-generate.log"
  cp -- "$generate_cmake/CMakeCache.txt" "$output/CMakeCache-generate.txt"

  record_command "$output/build-generate.command" \
    env "LLVM_PROFILE_FILE=$build_profile_raw/build-%m-%p.profraw" \
    "${generate_build_command[@]}"
  env "LLVM_PROFILE_FILE=$build_profile_raw/build-%m-%p.profraw" \
    "${generate_build_command[@]}" 2>&1 |
    "$tee_bin" "$output/build-generate.log"
  instrumented_binary="$generate_cmake/sto_tpcc_bench"
  record_native_archive_hashes \
    "$generate_cmake" "$output/native-archives-generate-sha256.txt"
else
  # A dry run is diagnostic only: Rust source changes are expected to make the
  # original Cargo edge dirty. The immutable explicit-input manifest below is
  # the authoritative graph check for the two relinks.
  "$native_ninja_bin" -C "$native_build" -n sto_tpcc_bench \
    >"$output/native-ninja-dry-run.txt" 2>&1 || fail \
    "Ninja cannot evaluate sto_tpcc_bench in the supplied native build"
  build_rust_archive \
    "$generate_cargo" "$generate_flags" \
    "$build_profile_raw/build-%m-%p.profraw" \
    "$output/build-generate.command" "$output/build-generate.log"
  instrumented_binary="$output/sto_tpcc_bench-generate"
  prepare_native_relink \
    "$instrumented_archive" "$instrumented_binary" "$profiler_archive" \
    "$output/relink-generate.sh" "$output/relink-generate-metadata.txt" \
    "$output/native-link-inputs.txt" "$output/native-ninja-commands.txt" \
    "$output/native-link-original.command"
  native_graph_digest_initial="$(native_graph_digest "$output/native-link-inputs.txt")"
  record_native_graph_hashes \
    "$output/native-link-inputs.txt" "$output/native-link-inputs-sha256.txt"
  printf 'native_graph_sha256=%s\n' "$native_graph_digest_initial" \
    >>"$output/provenance.txt"
  (
    cd -- "$native_build"
    "$output/relink-generate.sh"
  ) 2>&1 | "$tee_bin" "$output/relink-generate.log"
  [[ "$(native_graph_digest "$output/native-link-inputs.txt")" == \
      "$native_graph_digest_initial" ]] ||
    fail "native object graph changed during the instrumented relink"
fi

[[ -s "$instrumented_archive" ]] || fail "instrumented Rust archive was not produced"
[[ -x "$instrumented_binary" ]] || fail "instrumented TPC-C benchmark was not produced"
validate_dynamic_links "$instrumented_binary" "$output/instrumented.ldd"
record_elf_notes "$instrumented_binary" "$output/instrumented-elf-notes.txt"
validate_profile_runtime_link "$instrumented_binary" "$output/instrumented-undefined.txt"

shopt -s nullglob
unexpected_training_profiles=("$profile_raw"/*.profraw)
[[ ${#unexpected_training_profiles[@]} -eq 0 ]] ||
  fail "training profile directory was polluted during the build"
build_profiles=("$build_profile_raw"/*.profraw)
if [[ ${#build_profiles[@]} -gt 0 ]]; then
  "$sha256_bin" "${build_profiles[@]}" >"$output/build-profiles-sha256.txt"
else
  : >"$output/build-profiles-sha256.txt"
fi

[[ "$(source_state_digest)" == "$initial_source_digest" ]] ||
  fail "source state changed during the instrumented build"
[[ "$(toolchain_digest)" == "$initial_toolchain_digest" ]] ||
  fail "Rust toolchain changed during the instrumented build"
current_config_hash_line="$($sha256_bin "$train_config")"
[[ ${current_config_hash_line%% *} == "$initial_config_hash" ]] ||
  fail "TPC-C shard config changed during the instrumented build"

training_profile_pattern="$profile_raw/train-%m-%p.profraw"
training_command=(
  "$taskset_bin" -c "$train_cpu" "$instrumented_binary"
  --num-threads 1
  --shard-config "$train_config"
  --site-name "$train_site"
  --runtime "$train_seconds"
  --storage-engine rust
)
{
  printf 'environment=MAKO_TPCC_ALLOCATOR_MEMORY=%q MAKO_TPCC_WORKLOAD_MIX=%q LLVM_PROFILE_FILE=%q' \
    "$train_allocator_memory" "$train_mix" "$training_profile_pattern"
  for variable in "${tpcc_fallback_variables[@]}"; do
    printf ' %s=<unset>' "$variable"
  done
  printf '\n'
  printf 'command='
  printf '%q ' "${training_command[@]}"
  printf '\n'
} >"$output/training.log"
MAKO_TPCC_ALLOCATOR_MEMORY="$train_allocator_memory" \
MAKO_TPCC_WORKLOAD_MIX="$train_mix" \
LLVM_PROFILE_FILE="$training_profile_pattern" \
  "${training_command[@]}" 2>&1 |
  "$tee_bin" -a "$output/training.log"
validate_training_result "$output/training.log" "$output/training-result.json"

raw_profiles=("$profile_raw"/train-*.profraw)
[[ ${#raw_profiles[@]} -gt 0 ]] || fail "training produced no raw profiles"
for raw_profile in "${raw_profiles[@]}"; do
  [[ -s "$raw_profile" ]] || fail "training profile is empty: $raw_profile"
done
"$sha256_bin" "${raw_profiles[@]}" >"$output/training-profiles-sha256.txt"

profdata_command=(
  "$profdata_bin" merge -sparse "${raw_profiles[@]}"
  -o "$output/merged.profdata.tmp"
)
record_command "$output/profdata.command" "${profdata_command[@]}"
"${profdata_command[@]}" 2>&1 | "$tee_bin" "$output/profdata.log"
[[ -s "$output/merged.profdata.tmp" ]] || fail "llvm-profdata produced no merged profile"
mv -- "$output/merged.profdata.tmp" "$merged_profile"
"$profdata_bin" show "$merged_profile" >"$output/merged-profile-summary.txt" 2>&1

[[ "$(source_state_digest)" == "$initial_source_digest" ]] ||
  fail "source state changed between PGO generation and use"
[[ "$(toolchain_digest)" == "$initial_toolchain_digest" ]] ||
  fail "Rust toolchain changed between PGO generation and use"
current_config_hash_line="$($sha256_bin "$train_config")"
[[ ${current_config_hash_line%% *} == "$initial_config_hash" ]] ||
  fail "TPC-C shard config changed between PGO generation and use"

optimized_archive="$use_cargo/release/libsto_tpcc_ffi.a"
if [[ "$build_mode" == clean ]]; then
  use_configure_command=(
    "$cmake_bin" -S "$repo_root" -B "$use_cmake"
    "${common_cmake_arguments[@]}"
    "-DSTO_TPCC_RUST_TARGET_DIR:PATH=$use_cargo"
    "-DSTO_TPCC_RUST_EXTRA_RUSTFLAGS:STRING=$use_flags"
  )
  use_build_command=(
    "$cmake_bin" --build "$use_cmake" --target sto_tpcc_bench
    --parallel "$build_jobs"
  )
  record_command "$output/configure-use.command" "${use_configure_command[@]}"
  "${use_configure_command[@]}" 2>&1 | "$tee_bin" "$output/configure-use.log"
  cp -- "$use_cmake/CMakeCache.txt" "$output/CMakeCache-use.txt"
  compare_cmake_toolchains \
    "$generate_cmake/CMakeCache.txt" "$use_cmake/CMakeCache.txt"

  record_command "$output/build-use.command" \
    env -u LLVM_PROFILE_FILE "${use_build_command[@]}"
  env -u LLVM_PROFILE_FILE "${use_build_command[@]}" 2>&1 |
    "$tee_bin" "$output/build-use.log"
  optimized_binary="$use_cmake/sto_tpcc_bench"
  record_native_archive_hashes \
    "$use_cmake" "$output/native-archives-use-sha256.txt"
else
  [[ "$(native_graph_digest "$output/native-link-inputs.txt")" == \
      "$native_graph_digest_initial" ]] ||
    fail "native object graph changed between PGO generation and use"
  build_rust_archive \
    "$use_cargo" "$use_flags" "" \
    "$output/build-use.command" "$output/build-use.log"
  optimized_binary="$output/sto_tpcc_bench"
  prepare_native_relink \
    "$optimized_archive" "$optimized_binary" "" \
    "$output/relink-use.sh" "$output/relink-use-metadata.txt" \
    "$output/native-link-inputs-use.txt" "$output/native-ninja-commands-use.txt" \
    "$output/native-link-original-use.command"
  [[ "$(native_graph_digest "$output/native-link-inputs-use.txt")" == \
      "$native_graph_digest_initial" ]] ||
    fail "Ninja link inputs changed between the generate and use relinks"
  record_native_graph_hashes \
    "$output/native-link-inputs-use.txt" \
    "$output/native-link-inputs-use-sha256.txt"
  (
    cd -- "$native_build"
    "$output/relink-use.sh"
  ) 2>&1 | "$tee_bin" "$output/relink-use.log"
  [[ "$(native_graph_digest "$output/native-link-inputs.txt")" == \
      "$native_graph_digest_initial" ]] ||
    fail "native object graph changed during the profile-use relink"
fi

[[ -s "$optimized_archive" ]] || fail "profile-use Rust archive was not produced"
[[ -x "$optimized_binary" ]] || fail "profile-use TPC-C benchmark was not produced"
validate_dynamic_links "$optimized_binary" "$output/optimized.ldd"
record_elf_notes "$optimized_binary" "$output/optimized-elf-notes.txt"

if [[ "$build_mode" == clean ]]; then
  profile_use_search_roots=("$use_cmake" "$use_cargo")
else
  profile_use_search_roots=("$use_cargo")
fi
mapfile -d '' -t unexpected_use_profiles < <(
  find "${profile_use_search_roots[@]}" -type f -name '*.profraw' -print0
)
[[ ${#unexpected_use_profiles[@]} -eq 0 ]] ||
  fail "profile-use build unexpectedly emitted raw profiles"
[[ "$(source_state_digest)" == "$initial_source_digest" ]] ||
  fail "source state changed during the profile-use build"
[[ "$(toolchain_digest)" == "$initial_toolchain_digest" ]] ||
  fail "Rust toolchain changed during the profile-use build"

"$sha256_bin" \
  "$instrumented_archive" "$instrumented_binary" "$merged_profile" \
  "$optimized_archive" "$optimized_binary" "${raw_profiles[@]}" \
  >"$output/artifacts-sha256.txt"
{
  printf 'completed_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'training_profile_count=%s\n' "${#raw_profiles[@]}"
  printf 'build_profile_count=%s\n' "${#build_profiles[@]}"
  printf 'instrumented_archive=%s\n' "$instrumented_archive"
  printf 'instrumented_binary=%s\n' "$instrumented_binary"
  printf 'merged_profile=%s\n' "$merged_profile"
  printf 'optimized_archive=%s\n' "$optimized_archive"
  printf 'optimized_binary=%s\n' "$optimized_binary"
} >>"$output/provenance.txt"

printf 'complete finished_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$status_file"
status_complete=1
printf 'PGO TPC-C benchmark: %s\n' "$optimized_binary"
printf 'PGO Rust archive: %s\n' "$optimized_archive"
printf 'Merged Rust profile: %s\n' "$merged_profile"
cat "$output/artifacts-sha256.txt"
