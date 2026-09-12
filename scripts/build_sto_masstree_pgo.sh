#!/usr/bin/env bash
# Build a workload-trained STO/Masstree benchmark with LLVM instrumentation PGO.
#
# Required environment:
#   MAKO_MTREE_NATIVE_LIB_DIRS  Native library search path list.
#   MAKO_MTREE_NATIVE_LIBS      Rust build-script link specification.
#
# Optional environment:
#   CARGO, RUSTC, LLVM_PROFDATA, TASKSET, LDD, READELF, SHA256SUM
#   PGO_TRAIN_CPU (default 0), PGO_TRAIN_WARMUP_MS (500),
#   PGO_TRAIN_DURATION_MS (3000), PGO_TRAIN_KEYSPACE (100000),
#   PGO_TRAIN_OPS_PER_TXN (10), PGO_TRAIN_SEED (1),
#   PGO_TRAIN_WRITE_PERCENTS ("0 5 50")
#   PGO_VALUE_MODE ("binary"; also accepts "fixed-u64")
#   STO_PGO_COMMON_RUSTFLAGS, STO_PGO_NATIVE_RPATH
#   CARGO_PROFILE_RELEASE_{OPT_LEVEL,DEBUG,LTO,CODEGEN_UNITS}
#
# Usage: scripts/build_sto_masstree_pgo.sh OUTPUT_DIRECTORY
set -euo pipefail

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

extract_llvm_version() {
  local text=$1
  local line
  local expression='LLVM version:?[[:space:]]+([0-9]+([.][0-9]+)*)'
  while IFS= read -r line; do
    if [[ "$line" =~ $expression ]]; then
      printf '%s\n' "${BASH_REMATCH[1]}"
      return 0
    fi
  done <<< "$text"
  return 1
}

validate_dynamic_links() {
  local binary=$1
  local log=$2
  if ! env -u LD_LIBRARY_PATH "$ldd_bin" "$binary" >"$log" 2>&1; then
    sed -n '1,120p' "$log" >&2
    fail "could not inspect dynamic linkage for $binary"
  fi
  if grep -q 'not found' "$log"; then
    sed -n '1,120p' "$log" >&2
    fail "unresolved dynamic dependency in $binary; set STO_PGO_NATIVE_RPATH"
  fi
}

record_elf_notes() {
  local binary=$1
  local log=$2
  if [[ -n "$readelf_bin" ]]; then
    "$readelf_bin" -n "$binary" >"$log"
  else
    printf 'readelf unavailable; build ID not recorded\n' >"$log"
  fi
}

require_result_number() {
  local json=$1
  local field=$2
  local expected=$3
  local expression="\"${field}\"[[:space:]]*:[[:space:]]*${expected}([,}])"
  [[ "$json" =~ $expression ]] ||
    fail "training result field $field did not equal $expected"
}

validate_training_result() {
  local log=$1
  local write_percent=$2
  local json
  local commits
  local engine_expression="\"engine\"[[:space:]]*:[[:space:]]*\"${expected_engine}\"([,}])"
  local commits_expression='"commits"[[:space:]]*:[[:space:]]*([0-9]+)([,}])'
  local -a result_lines=()

  mapfile -t result_lines < <(grep '^BENCH_RESULT=' "$log" || true)
  [[ ${#result_lines[@]} -eq 1 ]] ||
    fail "expected exactly one BENCH_RESULT in $log, found ${#result_lines[@]}"
  json=${result_lines[0]#BENCH_RESULT=}
  [[ "$json" =~ $engine_expression ]] || fail "unexpected training engine in $log"
  require_result_number "$json" threads 1
  require_result_number "$json" keyspace "$train_keyspace"
  require_result_number "$json" ops_per_txn "$train_ops"
  require_result_number "$json" write_percent "$write_percent"
  require_result_number "$json" warmup_ms "$train_warmup_ms"
  require_result_number "$json" duration_ms "$train_duration_ms"
  require_result_number "$json" seed "$train_seed"
  [[ "$json" =~ $commits_expression ]] || fail "training result has no commit count"
  commits=${BASH_REMATCH[1]}
  [[ "$commits" != 0 ]] || fail "training run committed no transactions"
}

if [[ $# -ne 1 ]]; then
  printf 'usage: %s OUTPUT_DIRECTORY\n' "$0" >&2
  exit 2
fi

: "${MAKO_MTREE_NATIVE_LIB_DIRS:?set MAKO_MTREE_NATIVE_LIB_DIRS}"
: "${MAKO_MTREE_NATIVE_LIBS:?set MAKO_MTREE_NATIVE_LIBS}"

output_argument=$1
[[ -n "$output_argument" ]] || die "PGO output path must not be empty"
if [[ "$output_argument" =~ [[:space:]] ]]; then
  die "PGO output path must not contain whitespace: $output_argument"
fi

script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
script_path="$script_directory/$(basename -- "${BASH_SOURCE[0]}")"
repo_root="$(cd -- "$script_directory/.." && pwd -P)"
workspace="$repo_root/crates"

cargo_bin="$(resolve_tool "${CARGO:-cargo}")"
rustc_bin="$(resolve_tool "${RUSTC:-rustc}")"
profdata_bin="$(resolve_tool "${LLVM_PROFDATA:-llvm-profdata}")"
taskset_bin="$(resolve_tool "${TASKSET:-taskset}")"
ldd_bin="$(resolve_tool "${LDD:-ldd}")"
sha256_bin="$(resolve_tool "${SHA256SUM:-sha256sum}")"
if [[ -n "${READELF:-}" ]]; then
  readelf_bin="$(resolve_tool "$READELF")"
elif command -v -- readelf >/dev/null; then
  readelf_bin="$(resolve_tool readelf)"
else
  readelf_bin=
fi

if [[ -n "${CARGO_ENCODED_RUSTFLAGS:-}" ]]; then
  die "CARGO_ENCODED_RUSTFLAGS would override the controlled PGO RUSTFLAGS"
fi
inherited_cargo_profile_environment="$(
  env | LC_ALL=C sort | grep '^CARGO_PROFILE_' || true
)"
inherited_rustc_wrapper=${RUSTC_WRAPPER:-}
inherited_rustc_workspace_wrapper=${RUSTC_WORKSPACE_WRAPPER:-}
unset CARGO_ENCODED_RUSTFLAGS LLVM_PROFILE_FILE

train_cpu=${PGO_TRAIN_CPU:-0}
train_warmup_ms=${PGO_TRAIN_WARMUP_MS:-500}
train_duration_ms=${PGO_TRAIN_DURATION_MS:-3000}
train_keyspace=${PGO_TRAIN_KEYSPACE:-100000}
train_ops=${PGO_TRAIN_OPS_PER_TXN:-10}
train_seed=${PGO_TRAIN_SEED:-1}
train_write_percents=${PGO_TRAIN_WRITE_PERCENTS:-"0 5 50"}
value_mode=${PGO_VALUE_MODE:-binary}

require_unsigned PGO_TRAIN_CPU "$train_cpu"
require_unsigned PGO_TRAIN_WARMUP_MS "$train_warmup_ms"
require_positive PGO_TRAIN_DURATION_MS "$train_duration_ms"
require_positive PGO_TRAIN_KEYSPACE "$train_keyspace"
require_positive PGO_TRAIN_OPS_PER_TXN "$train_ops"
require_unsigned PGO_TRAIN_SEED "$train_seed"
read -r -a train_write_percent_values <<< "$train_write_percents"
[[ ${#train_write_percent_values[@]} -ne 0 ]] ||
  die "PGO_TRAIN_WRITE_PERCENTS must not be empty"
for write_percent in "${train_write_percent_values[@]}"; do
  require_unsigned PGO_TRAIN_WRITE_PERCENTS "$write_percent"
  (( 10#$write_percent <= 100 )) ||
    die "PGO_TRAIN_WRITE_PERCENTS values must be at most 100: $write_percent"
done
case "$value_mode" in
  binary)
    expected_engine=rust-sto-masstree
    cargo_feature_arguments=()
    benchmark_mode_arguments=()
    ;;
  fixed-u64)
    expected_engine=rust-sto-masstree-fixed-u64
    cargo_feature_arguments=(--features fixed-u64)
    benchmark_mode_arguments=(--value-mode fixed-u64)
    ;;
  *)
    die "PGO_VALUE_MODE must be binary or fixed-u64: $value_mode"
    ;;
esac
"$taskset_bin" -c "$train_cpu" /bin/true >/dev/null ||
  die "PGO_TRAIN_CPU is not available to this process: $train_cpu"

common_rustflags=${STO_PGO_COMMON_RUSTFLAGS:-"-Dwarnings -C target-cpu=native -C force-frame-pointers=yes -C link-arg=-Wl,--defsym=__rust_alloc_error_handler_should_panic=0"}
if [[ -n "${STO_PGO_NATIVE_RPATH:-}" ]]; then
  if [[ "$STO_PGO_NATIVE_RPATH" =~ [[:space:]] ]]; then
    die "STO_PGO_NATIVE_RPATH must not contain whitespace"
  fi
  common_rustflags+=" -C link-arg=-Wl,-rpath,${STO_PGO_NATIVE_RPATH}"
fi

IFS=: read -r -a native_dir_values <<< "$MAKO_MTREE_NATIVE_LIB_DIRS"
[[ ${#native_dir_values[@]} -ne 0 ]] || die "MAKO_MTREE_NATIVE_LIB_DIRS is empty"
native_dirs=()
for native_dir in "${native_dir_values[@]}"; do
  [[ -n "$native_dir" ]] || die "MAKO_MTREE_NATIVE_LIB_DIRS contains an empty entry"
  [[ -d "$native_dir" ]] || die "native library directory does not exist: $native_dir"
  native_dirs+=("$(cd -- "$native_dir" && pwd -P)")
done
MAKO_MTREE_NATIVE_LIB_DIRS="$(IFS=:; printf '%s' "${native_dirs[*]}")"
export MAKO_MTREE_NATIVE_LIB_DIRS MAKO_MTREE_NATIVE_LIBS

# Record every explicit linker-search directory in order and every matching
# library candidate in it. This makes duplicate/stale archive selection
# diagnosable without pretending to reproduce the linker's system defaults.
shopt -s nullglob
IFS=, read -r -a native_lib_specs <<< "$MAKO_MTREE_NATIVE_LIBS"
native_search_records=()
native_candidate_files=()
declare -A seen_native_candidates=()
native_directory_index=0
for native_dir in "${native_dirs[@]}"; do
  native_search_records+=("directory[$native_directory_index]=$native_dir")
  native_library_index=0
  for native_spec in "${native_lib_specs[@]}"; do
    native_spec=${native_spec#"${native_spec%%[![:space:]]*}"}
    native_spec=${native_spec%"${native_spec##*[![:space:]]}"}
    [[ -n "$native_spec" ]] || continue
    if [[ "$native_spec" == *=* ]]; then
      native_kind=${native_spec%%=*}
      native_name=${native_spec##*=}
    else
      native_kind=
      native_name=$native_spec
    fi
    native_name=${native_name%%:*}
    native_search_records+=(
      "directory[$native_directory_index].library[$native_library_index].spec=$native_spec"
    )
    native_candidate_patterns=()
    case "$native_kind" in
      *+verbatim*)
        native_candidate_patterns+=("$native_dir/$native_name")
        ;;
      static*)
        native_candidate_patterns+=("$native_dir/lib${native_name}.a")
        ;;
      dylib*)
        native_candidate_patterns+=(
          "$native_dir/lib${native_name}.so"
          "$native_dir/lib${native_name}.so."*
          "$native_dir/lib${native_name}.dylib"
        )
        ;;
      *)
        native_candidate_patterns+=(
          "$native_dir/lib${native_name}.a"
          "$native_dir/lib${native_name}.so"
          "$native_dir/lib${native_name}.so."*
          "$native_dir/lib${native_name}.dylib"
        )
        ;;
    esac
    native_match_found=0
    for native_candidate in "${native_candidate_patterns[@]}"; do
      [[ -f "$native_candidate" ]] || continue
      native_match_found=1
      native_search_records+=(
        "directory[$native_directory_index].library[$native_library_index].candidate=$native_candidate"
      )
      if [[ -z "${seen_native_candidates[$native_candidate]+present}" ]]; then
        seen_native_candidates[$native_candidate]=1
        native_candidate_files+=("$native_candidate")
      fi
    done
    if (( native_match_found == 0 )); then
      native_search_records+=(
        "directory[$native_directory_index].library[$native_library_index].candidate=(none)"
      )
    fi
    native_library_index=$((native_library_index + 1))
  done
  native_directory_index=$((native_directory_index + 1))
done

cargo_version="$($cargo_bin -Vv 2>&1)"
rustc_version="$($rustc_bin -vV 2>&1)"
profdata_version="$($profdata_bin --version 2>&1)"
taskset_version="$($taskset_bin --version 2>&1 | sed -n '1p')"
rustc_llvm_version="$(extract_llvm_version "$rustc_version")" ||
  die "could not determine rustc's LLVM version"
profdata_llvm_version="$(extract_llvm_version "$profdata_version")" ||
  die "could not determine llvm-profdata's LLVM version"
if [[ "${rustc_llvm_version%%.*}" != "${profdata_llvm_version%%.*}" ]]; then
  die "rustc LLVM $rustc_llvm_version is incompatible with llvm-profdata $profdata_llvm_version"
fi

# Snapshot source state before creating the output, so an output directory
# inside the repository cannot contaminate its own provenance.
git_head="$(git -C "$repo_root" rev-parse HEAD)"
git_branch="$(git -C "$repo_root" symbolic-ref --short -q HEAD || printf '(detached)')"
git_status="$(git -C "$repo_root" status --short)"
git_diff_hash_line="$({
  git -C "$repo_root" diff --binary
  git -C "$repo_root" diff --cached --binary
} | "$sha256_bin")"
git_diff_sha=${git_diff_hash_line%% *}
mapfile -d '' -t untracked_crate_files < <(
  git -C "$repo_root" ls-files --others --exclude-standard -z -- crates
)

output_parent_argument="$(dirname -- "$output_argument")"
output_name="$(basename -- "$output_argument")"
[[ "$output_name" != . && "$output_name" != .. ]] ||
  die "invalid PGO output path: $output_argument"
[[ -d "$output_parent_argument" ]] ||
  die "PGO output parent directory does not exist: $output_parent_argument"
output_parent="$(cd -- "$output_parent_argument" && pwd -P)"
output="$output_parent/$output_name"
if [[ -e "$output" || -L "$output" ]]; then
  die "refusing to reuse PGO output path: $output"
fi
mkdir -- "$output" || die "could not create PGO output path: $output"
status_file="$output/STATUS"
status_complete=0
printf 'building started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$status_file"
record_exit() {
  local result=$?
  if (( status_complete == 0 )); then
    printf 'failed exit_status=%d finished_utc=%s\n' \
      "$result" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$status_file" || true
  fi
  return "$result"
}
trap record_exit EXIT
mkdir -- "$output/profile-raw" "$output/build-profile-raw"

export RUSTC="$rustc_bin"
export MAKO_MTREE_NATIVE_INTEGRATION=1
export CARGO_INCREMENTAL=0
export CARGO_PROFILE_RELEASE_OPT_LEVEL=${CARGO_PROFILE_RELEASE_OPT_LEVEL:-2}
export CARGO_PROFILE_RELEASE_DEBUG=${CARGO_PROFILE_RELEASE_DEBUG:-1}
export CARGO_PROFILE_RELEASE_LTO=${CARGO_PROFILE_RELEASE_LTO:-fat}
export CARGO_PROFILE_RELEASE_CODEGEN_UNITS=${CARGO_PROFILE_RELEASE_CODEGEN_UNITS:-1}

generate_target="$output/generate-target"
profile_raw="$output/profile-raw"
build_profile_raw="$output/build-profile-raw"
merged_profile="$output/merged.profdata"
optimized_target="$output/optimized-target"
training_log="$output/training.log"
generate_rustflags="$common_rustflags -C profile-generate=$profile_raw"
optimized_rustflags="$common_rustflags -C profile-use=$merged_profile -C llvm-args=-pgo-warn-missing-function"

cp -- "$script_path" "$output/build-script.sh"
printf '%s\n' "$git_status" >"$output/source-status.txt"
{
  git -C "$repo_root" diff --binary
  git -C "$repo_root" diff --cached --binary
} >"$output/source.patch"

input_files=("$script_path" "$workspace/Cargo.lock")
for relative_path in "${untracked_crate_files[@]}"; do
  input_files+=("$repo_root/$relative_path")
  destination="$output/untracked-source/$relative_path"
  mkdir -p -- "$(dirname -- "$destination")"
  cp -- "$repo_root/$relative_path" "$destination"
done
printf '%s\n' "${native_search_records[@]}" >"$output/native-search-order.txt"
if [[ ${#native_candidate_files[@]} -ne 0 ]]; then
  "$sha256_bin" "${native_candidate_files[@]}" >"$output/native-candidates-sha256.txt"
  input_files+=("${native_candidate_files[@]}")
else
  : >"$output/native-candidates-sha256.txt"
fi
"$sha256_bin" "${input_files[@]}" >"$output/input-sha256.txt"

{
  printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'host=%s\n' "$(hostname)"
  printf 'kernel=%s\n' "$(uname -srvmo)"
  printf 'repo_root=%s\n' "$repo_root"
  printf 'git_branch=%s\n' "$git_branch"
  printf 'git_head=%s\n' "$git_head"
  printf 'git_tracked_diff_sha256=%s\n' "$git_diff_sha"
  printf 'cargo=%s\n' "$cargo_bin"
  printf 'rustc=%s\n' "$rustc_bin"
  printf 'llvm_profdata=%s\n' "$profdata_bin"
  printf 'taskset=%s\n' "$taskset_bin"
  printf 'readelf=%s\n' "${readelf_bin:-unavailable}"
  printf 'taskset_version=%s\n' "$taskset_version"
  printf 'rustc_llvm_version=%s\n' "$rustc_llvm_version"
  printf 'llvm_profdata_version=%s\n' "$profdata_llvm_version"
  printf 'native_lib_dirs=%q\n' "$MAKO_MTREE_NATIVE_LIB_DIRS"
  printf 'native_libs=%q\n' "$MAKO_MTREE_NATIVE_LIBS"
  printf 'native_rpath=%q\n' "${STO_PGO_NATIVE_RPATH:-}"
  printf 'inherited_ld_library_path=%q\n' "${LD_LIBRARY_PATH:-}"
  printf 'release_opt_level=%s\n' "$CARGO_PROFILE_RELEASE_OPT_LEVEL"
  printf 'release_debug=%s\n' "$CARGO_PROFILE_RELEASE_DEBUG"
  printf 'release_lto=%s\n' "$CARGO_PROFILE_RELEASE_LTO"
  printf 'release_codegen_units=%s\n' "$CARGO_PROFILE_RELEASE_CODEGEN_UNITS"
  printf 'cargo_incremental=%s\n' "$CARGO_INCREMENTAL"
  printf 'rustup_toolchain=%q\n' "${RUSTUP_TOOLCHAIN:-}"
  printf 'rustc_wrapper=%q\n' "$inherited_rustc_wrapper"
  printf 'rustc_workspace_wrapper=%q\n' "$inherited_rustc_workspace_wrapper"
  printf 'common_rustflags=%q\n' "$common_rustflags"
  printf 'generate_rustflags=%q\n' "$generate_rustflags"
  printf 'optimized_rustflags=%q\n' "$optimized_rustflags"
  printf 'train_cpu=%s\n' "$train_cpu"
  printf 'train_warmup_ms=%s\n' "$train_warmup_ms"
  printf 'train_duration_ms=%s\n' "$train_duration_ms"
  printf 'train_keyspace=%s\n' "$train_keyspace"
  printf 'train_ops_per_txn=%s\n' "$train_ops"
  printf 'train_seed=%s\n' "$train_seed"
  printf 'train_write_percents=%q\n' "$train_write_percents"
  printf 'value_mode=%s\n' "$value_mode"
  printf 'expected_engine=%s\n' "$expected_engine"
  printf '%s\n' '--- cargo -Vv ---'
  printf '%s\n' "$cargo_version"
  printf '%s\n' '--- rustc -vV ---'
  printf '%s\n' "$rustc_version"
  printf '%s\n' '--- llvm-profdata --version ---'
  printf '%s\n' "$profdata_version"
  printf '%s\n' '--- inherited CARGO_PROFILE_* ---'
  if [[ -n "$inherited_cargo_profile_environment" ]]; then
    printf '%s\n' "$inherited_cargo_profile_environment"
  else
    printf '%s\n' '(none)'
  fi
} >"$output/provenance.txt"

(
  cd "$workspace"
  CARGO_TARGET_DIR="$generate_target" \
    RUSTFLAGS="$generate_rustflags" \
    "$cargo_bin" build --locked --release -p sto-masstree --bin sto_masstree_compare \
      "${cargo_feature_arguments[@]}"
) 2>&1 | tee "$output/generate-build.log"

instrumented="$generate_target/release/sto_masstree_compare"
[[ -x "$instrumented" ]] || fail "instrumented benchmark was not produced"
validate_dynamic_links "$instrumented" "$output/instrumented.ldd"
record_elf_notes "$instrumented" "$output/instrumented-elf-notes.txt"

shopt -s nullglob
build_profiles=("$profile_raw"/*.profraw)
if [[ ${#build_profiles[@]} -ne 0 ]]; then
  mv -- "${build_profiles[@]}" "$build_profile_raw/"
fi

: >"$training_log"
train_index=0
for write_percent in "${train_write_percent_values[@]}"; do
  scenario_log="$output/training-${train_index}-w${write_percent}.log"
  profile_stem="$profile_raw/train-${train_index}-w${write_percent}"
  profile_pattern="$profile_stem-%m-%p.profraw"
  command=(
    "$taskset_bin" -c "$train_cpu" "$instrumented"
    --threads 1
    --keyspace "$train_keyspace"
    --ops-per-txn "$train_ops"
    --write-percent "$write_percent"
    --warmup-ms "$train_warmup_ms"
    --duration-ms "$train_duration_ms"
    --seed "$train_seed"
    "${benchmark_mode_arguments[@]}"
  )
  : >"$scenario_log"
  {
    printf 'training_index=%s write_percent=%s profile_pattern=%s\n' \
      "$train_index" "$write_percent" "$profile_pattern"
    printf 'command=LLVM_PROFILE_FILE=%q ' "$profile_pattern"
    printf '%q ' "${command[@]}"
    printf '\n'
  } | tee -a "$scenario_log" "$training_log"
  LLVM_PROFILE_FILE="$profile_pattern" \
    "${command[@]}" 2>&1 | tee -a "$scenario_log" | tee -a "$training_log"
  validate_training_result "$scenario_log" "$write_percent"
  scenario_profiles=("${profile_stem}-"*.profraw)
  [[ ${#scenario_profiles[@]} -ne 0 ]] ||
    fail "training run $train_index produced no profile"
  for scenario_profile in "${scenario_profiles[@]}"; do
    [[ -s "$scenario_profile" ]] || fail "training profile is empty: $scenario_profile"
  done
  train_index=$((train_index + 1))
done

raw_profiles=("$profile_raw"/train-*.profraw)
[[ ${#raw_profiles[@]} -ne 0 ]] || fail "instrumented runs produced no training profiles"
"$sha256_bin" "${raw_profiles[@]}" >"$output/training-profiles-sha256.txt"
nontraining_profiles=("$build_profile_raw"/*.profraw)
if [[ ${#nontraining_profiles[@]} -ne 0 ]]; then
  "$sha256_bin" "${nontraining_profiles[@]}" >"$output/build-profiles-sha256.txt"
else
  : >"$output/build-profiles-sha256.txt"
fi

{
  printf 'command=%q merge -sparse' "$profdata_bin"
  printf ' %q' "${raw_profiles[@]}"
  printf ' -o %q\n' "$merged_profile"
} >"$output/profdata.log"
"$profdata_bin" merge -sparse "${raw_profiles[@]}" -o "$merged_profile" \
  2>&1 | tee -a "$output/profdata.log"
"$profdata_bin" show "$merged_profile" >"$output/merged-profile-summary.txt" 2>&1

(
  cd "$workspace"
  CARGO_TARGET_DIR="$optimized_target" \
    RUSTFLAGS="$optimized_rustflags" \
    "$cargo_bin" build --locked --release -p sto-masstree --bin sto_masstree_compare \
      "${cargo_feature_arguments[@]}"
) 2>&1 | tee "$output/optimized-build.log"

optimized="$optimized_target/release/sto_masstree_compare"
[[ -x "$optimized" ]] || fail "optimized benchmark was not produced"
validate_dynamic_links "$optimized" "$output/optimized.ldd"
record_elf_notes "$optimized" "$output/optimized-elf-notes.txt"
instrumented_build_id="$(
  sed -n 's/^.*Build ID: //p' "$output/instrumented-elf-notes.txt"
)"
optimized_build_id="$(
  sed -n 's/^.*Build ID: //p' "$output/optimized-elf-notes.txt"
)"

"$sha256_bin" "$instrumented" "$merged_profile" "$optimized" \
  "${raw_profiles[@]}" >"$output/artifacts-sha256.txt"
{
  printf 'completed_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'training_profile_count=%s\n' "${#raw_profiles[@]}"
  printf 'nontraining_profile_count=%s\n' "${#nontraining_profiles[@]}"
  printf 'instrumented_build_id=%s\n' "${instrumented_build_id:-unavailable}"
  printf 'optimized_build_id=%s\n' "${optimized_build_id:-unavailable}"
} >>"$output/provenance.txt"

printf 'complete finished_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$status_file"
status_complete=1
printf 'PGO benchmark: %s\n' "$optimized"
printf 'Merged profile: %s\n' "$merged_profile"
cat "$output/artifacts-sha256.txt"
