#!/usr/bin/env bash

set -euo pipefail

readonly rust_sanitizer_nightly="nightly-2026-08-12"
readonly rust_sanitizer_target="x86_64-unknown-linux-gnu"
readonly rust_linker_environment="CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER"
readonly -a asan_native_quarantine_cases=(
    "tests::post_install_row_count_failure_marks_runtime_indeterminate"
)

usage() {
    cat <<'EOF'
Usage: bash scripts/ci/run_rust_sto_sanitizer.sh <address|undefined|thread>
       bash scripts/ci/run_rust_sto_sanitizer.sh --print-nightly-toolchain

Build and test the Rust STO/Masstree native boundary with one native
sanitizer. The default build directory is build-rust-sto-<sanitizer>.

Environment:
  MAKO_SANITIZER_ALLOW_DIRTY=1  Allow a tracked or untracked source change.
  MAKO_SANITIZER_BUILD_DIR=PATH Override the sanitizer-specific build path.
  MAKO_SANITIZER_JOBS=N         Parallel build jobs. Default: 4.
  CMAKE_GENERATOR=Ninja         This gate audits Ninja's generated commands.

Coverage contract:
  * First-party C and C++ boundary and workload code compiled by the selected
    CMake targets receives the selected sanitizer's compiler instrumentation.
  * rust_sto_integration runs the Rust workspace tests and the Rust tests
    that call the instrumented native Masstree archives.
  * Every registered CTest carrying the exact "rust" label runs. This picks
    up the Rust TPC-C slow-exit and workload smoke tests when CMake registers
    them, without maintaining a second test-name list here.
  * ASan and TSan compiler-instrument first-party Rust code and a rebuilt Rust
    standard library with the pinned nightly. Rust test binaries and native
    executables use the external Clang sanitizer runtime so each mixed-language
    process has one runtime.
  * ASan leak detection remains enabled for the workspace sweep, native runner,
    and every Rust-labeled CTest. Exact intentional-quarantine tests are skipped
    in the sweep and rerun one-by-one with only leak reporting disabled. The
    exact C++ slow-exit lifecycle CTests also disable leak reporting because the
    legacy backend retains native STO/Masstree state for process lifetime. Two exact
    Rust TPC-C lifecycle tests use one checked-in LSan constructor suppression
    for native Masstree roots that the public C ABI retains until process exit.
  * The UBSan job compiler-instruments native C and C++ code. Stable Rust 1.95
    has no UBSan compiler mode, so Rust-owned test executables only link the
    same Clang UBSan runtime to exercise calls into instrumented native code.
  * ASan, UBSan, and TSan use the checked-in, sanitizer-specific Masstree
    suppressions described above. Findings matched by those files qualify the
    affected result and are audited by this gate.
EOF
}

if [[ "${1:-}" == "--print-nightly-toolchain" ]]; then
    if [[ $# -ne 1 ]]; then
        usage >&2
        exit 2
    fi
    printf '%s\n' "${rust_sanitizer_nightly}"
    exit 0
fi

if [[ $# -ne 1 ]]; then
    usage >&2
    exit 2
fi

case "$1" in
    address|asan)
        sanitizer="address"
        short_name="asan"
        cmake_toggle="MAKO_ASAN"
        cmake_asan="ON"
        cmake_ubsan="OFF"
        cmake_tsan="OFF"
        configure_confirmation="MAKO_ASAN=1: building with -fsanitize=address"
        rust_compiler_instrumented=1
        ;;
    undefined|ubsan)
        sanitizer="undefined"
        short_name="ubsan"
        cmake_toggle="MAKO_UBSAN"
        cmake_asan="OFF"
        cmake_ubsan="ON"
        cmake_tsan="OFF"
        configure_confirmation="MAKO_UBSAN=1: building with -fsanitize=undefined"
        rust_compiler_instrumented=0
        ;;
    thread|tsan)
        sanitizer="thread"
        short_name="tsan"
        cmake_toggle="MAKO_TSAN"
        cmake_asan="OFF"
        cmake_ubsan="OFF"
        cmake_tsan="ON"
        configure_confirmation="MAKO_TSAN=1: building with -fsanitize=thread"
        rust_compiler_instrumented=1
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        echo "unknown sanitizer: $1" >&2
        usage >&2
        exit 2
        ;;
esac

for command_name in cmake git ninja python3 sha256sum; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "required command is missing: ${command_name}" >&2
        exit 2
    fi
done

# This is an evidence-producing gate, so ambient compiler and linker flags
# must not be able to add a second sanitizer runtime or silently disable the
# requested instrumentation. CC and CXX remain selectable by the caller.
readonly -a rejected_environment_names=(
    RUSTFLAGS
    RUSTDOCFLAGS
    CARGO_ENCODED_RUSTFLAGS
    CARGO_BUILD_RUSTFLAGS
    CARGO_BUILD_RUSTDOCFLAGS
    CARGO_BUILD_TARGET
    CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_RUSTFLAGS
    CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER
    RUSTC
    RUSTDOC
    RUSTC_WRAPPER
    RUSTC_WORKSPACE_WRAPPER
    CFLAGS
    CXXFLAGS
    CPPFLAGS
    LDFLAGS
    MAKO_FUZZER
    ASAN_OPTIONS
    LSAN_OPTIONS
    UBSAN_OPTIONS
    TSAN_OPTIONS
)
for environment_name in "${rejected_environment_names[@]}"; do
    if [[ -n "${!environment_name:-}" ]]; then
        echo "${environment_name} must be unset for a hermetic sanitizer gate" >&2
        exit 2
    fi
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd -P)"
readonly -a repository_git=(
    git -c "safe.directory=${repo_root}" -C "${repo_root}"
)
cd "${repo_root}"

quarantine_allowlist="${repo_root}/scripts/ci/rust_sto_quarantine_tests.txt"
if [[ ! -r "${quarantine_allowlist}" ]]; then
    echo "Rust STO quarantine allowlist is missing: ${quarantine_allowlist}" >&2
    exit 2
fi
asan_quarantine_cases=()
declare -A quarantine_identities_seen=()
declare -A quarantine_names_seen=()
while IFS= read -r quarantine_case || [[ -n "${quarantine_case}" ]]; do
    [[ -z "${quarantine_case}" || "${quarantine_case}" == \#* ]] && continue
    if [[ ! "${quarantine_case}" =~ ^[A-Za-z0-9_.-]+\|[A-Za-z0-9_.-]+\|[A-Za-z0-9_.:-]+$ ]]; then
        echo "invalid Rust STO quarantine allowlist entry: ${quarantine_case}" >&2
        exit 2
    fi
    IFS='|' read -r quarantine_package quarantine_suite quarantine_name \
        <<<"${quarantine_case}"
    if [[ -n "${quarantine_identities_seen[${quarantine_case}]:-}" ]]; then
        echo "duplicate Rust STO quarantine identity: ${quarantine_case}" >&2
        exit 2
    fi
    if [[ -n "${quarantine_names_seen[${quarantine_name}]:-}" ]]; then
        echo "quarantine test name is not globally unique: ${quarantine_name}" >&2
        exit 2
    fi
    quarantine_identities_seen["${quarantine_case}"]=1
    quarantine_names_seen["${quarantine_name}"]=1
    asan_quarantine_cases+=("${quarantine_case}")
done <"${quarantine_allowlist}"
if [[ "${#asan_quarantine_cases[@]}" -eq 0 ]]; then
    echo "Rust STO quarantine allowlist is empty: ${quarantine_allowlist}" >&2
    exit 2
fi
readonly -a asan_quarantine_cases

head_sha="$("${repository_git[@]}" rev-parse --verify HEAD)"
tree_status="$("${repository_git[@]}" status --porcelain=v1 --untracked-files=normal --ignore-submodules=none)"
if [[ -n "${tree_status}" && "${MAKO_SANITIZER_ALLOW_DIRTY:-0}" != "1" ]]; then
    echo "refusing to attribute sanitizer results to ${head_sha}: the worktree is dirty" >&2
    printf '%s\n' "${tree_status}" >&2
    echo "commit/stash the changes, or set MAKO_SANITIZER_ALLOW_DIRTY=1 for a local diagnostic run" >&2
    exit 2
fi

jobs="${MAKO_SANITIZER_JOBS:-4}"
if [[ ! "${jobs}" =~ ^[1-9][0-9]*$ ]]; then
    echo "MAKO_SANITIZER_JOBS must be a positive integer, got: ${jobs}" >&2
    exit 2
fi

generator="${CMAKE_GENERATOR:-Ninja}"
if [[ "${generator}" != "Ninja" ]]; then
    echo "this gate requires CMAKE_GENERATOR=Ninja so it can audit generated build commands" >&2
    exit 2
fi
build_dir="${MAKO_SANITIZER_BUILD_DIR:-${repo_root}/build-rust-sto-${short_name}}"
if [[ "${build_dir}" != /* ]]; then
    build_dir="${repo_root}/${build_dir}"
fi
mkdir -p "${build_dir}/sanitizer-logs" "${build_dir}/tmp"
export TMPDIR="${build_dir}/tmp"

mode_stamp="${build_dir}/.rust-sto-sanitizer-mode"
if [[ -f "${mode_stamp}" ]]; then
    previous_mode="$(<"${mode_stamp}")"
    if [[ "${previous_mode}" != "${sanitizer}" ]]; then
        echo "build directory was configured for ${previous_mode}, not ${sanitizer}: ${build_dir}" >&2
        echo "use a sanitizer-specific MAKO_SANITIZER_BUILD_DIR" >&2
        exit 2
    fi
fi
printf '%s\n' "${sanitizer}" >"${mode_stamp}"

lsan_suppressions="${repo_root}/src/masstree/lsan_suppressions.txt"
ubsan_suppressions="${repo_root}/src/masstree/ubsan_suppressions.txt"
tsan_suppressions="${repo_root}/src/masstree/tsan_suppressions.txt"
lsan_suppression_sha256=""
lsan_options=""

if [[ "${sanitizer}" == "address" ]]; then
    if [[ ! -f "${lsan_suppressions}" || ! -r "${lsan_suppressions}" ||
          -L "${lsan_suppressions}" ]]; then
        echo "LSan suppression source must be a readable, regular, non-symlink file: ${lsan_suppressions}" >&2
        exit 2
    fi
    python3 - "${lsan_suppressions}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
raw = path.read_bytes()
if b"\0" in raw:
    raise SystemExit(f"LSan suppression source contains NUL: {path}")
if b"\r" in raw:
    raise SystemExit(f"LSan suppression source must use LF line endings: {path}")
try:
    text = raw.decode("utf-8")
except UnicodeDecodeError as error:
    raise SystemExit(f"LSan suppression source is not UTF-8: {path}: {error}")
active = [line for line in text.splitlines() if line and not line.startswith("#")]
expected = ["leak:mt_tree::mt_tree"]
if active != expected:
    raise SystemExit(
        f"LSan suppression source must contain exactly {expected!r}; found {active!r}"
    )
print("verified exact LSan suppression source")
PY
    lsan_suppression_sha256="$(sha256sum "${lsan_suppressions}" | awk '{print $1}')"
    lsan_options="print_suppressions=1:suppressions=${lsan_suppressions}"
fi

unset MAKO_ASAN MAKO_UBSAN MAKO_TSAN MAKO_FUZZER
export "${cmake_toggle}=1"

rust_link_flag="-C link-arg=-fsanitize=${sanitizer}"
rust_target=""
rust_build_std="OFF"
workspace_native_link="OFF"
selected_rustup_toolchain=""
asan_quarantine_names=()
asan_quarantine_cmake=""
asan_native_quarantine_cmake=""

if [[ "${rust_compiler_instrumented}" == "1" ]]; then
    if ! command -v rustup >/dev/null 2>&1; then
        echo "${sanitizer} requires rustup with ${rust_sanitizer_nightly}" >&2
        exit 2
    fi
    if ! selected_cargo="$(rustup which --toolchain "${rust_sanitizer_nightly}" cargo 2>/dev/null)" ||
       ! selected_rustc="$(rustup which --toolchain "${rust_sanitizer_nightly}" rustc 2>/dev/null)"; then
        echo "${sanitizer} requires the pinned ${rust_sanitizer_nightly} toolchain" >&2
        echo "install it with rust-src and clippy before running this gate" >&2
        exit 2
    fi
    selected_rustup_toolchain="${rust_sanitizer_nightly}"
    rust_sysroot="$("${selected_rustc}" --print sysroot)"
    if [[ ! -r "${rust_sysroot}/lib/rustlib/src/rust/library/core/src/marker.rs" ]]; then
        echo "${rust_sanitizer_nightly} is missing the rust-src component" >&2
        exit 2
    fi
    if ! "${selected_cargo}" clippy --version >/dev/null 2>&1; then
        echo "${rust_sanitizer_nightly} is missing the clippy component" >&2
        exit 2
    fi
    rust_target="${rust_sanitizer_target}"
    rust_build_std="ON"
    workspace_native_link="ON"
    rust_sanitizer_flags="-Zsanitizer=${sanitizer} -Zexternal-clangrt"
    rustflags="${rust_sanitizer_flags} -C force-frame-pointers=yes ${rust_link_flag}"
    rustdocflags="${rust_sanitizer_flags} -C force-frame-pointers=yes ${rust_link_flag}"
    sto_tpcc_rustflags="${rust_sanitizer_flags}"
    rust_instrumentation="${sanitizer}-nightly-build-std"
    rust_runtime="external-clang"
else
    for command_name in rustc cargo; do
        if ! command -v "${command_name}" >/dev/null 2>&1; then
            echo "required command is missing: ${command_name}" >&2
            exit 2
        fi
    done
    selected_cargo="$(command -v cargo)"
    selected_rustc="$(command -v rustc)"
    stable_version="$("${selected_rustc}" --version | awk '{print $2}')"
    if [[ "${stable_version}" != "1.95.0" ]]; then
        echo "UBSan requires stable Rust 1.95.0, found ${stable_version}" >&2
        exit 2
    fi
    rustflags="-C force-frame-pointers=yes ${rust_link_flag}"
    rustdocflags="-C force-frame-pointers=yes ${rust_link_flag}"
    sto_tpcc_rustflags="${rust_link_flag}"
    rust_instrumentation="none-stable-1.95"
    rust_runtime="native-link-argument"
fi
if [[ "${sanitizer}" == "address" ]]; then
    for quarantine_case in "${asan_quarantine_cases[@]}"; do
        IFS='|' read -r _quarantine_package _quarantine_suite quarantine_name \
            <<<"${quarantine_case}"
        asan_quarantine_names+=("${quarantine_name}")
    done
    asan_quarantine_cmake="$(IFS=';'; printf '%s' "${asan_quarantine_names[*]}")"
    asan_native_quarantine_cmake="$(IFS=';'; printf '%s' "${asan_native_quarantine_cases[*]}")"
fi
case "${sanitizer}" in
    address)
        export ASAN_OPTIONS="abort_on_error=1:detect_leaks=1:halt_on_error=1:print_suppressions=1:strict_init_order=1"
        ;;
    undefined)
        if [[ ! -r "${ubsan_suppressions}" ]]; then
            echo "UBSan suppression file is missing: ${ubsan_suppressions}" >&2
            exit 2
        fi
        export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:suppressions=${ubsan_suppressions}"
        ;;
    thread)
        if [[ ! -r "${tsan_suppressions}" ]]; then
            echo "TSan suppression file is missing: ${tsan_suppressions}" >&2
            exit 2
        fi
        export TSAN_OPTIONS="halt_on_error=1:print_suppressions=1:second_deadlock_stack=1:suppressions=${tsan_suppressions}"
        ;;
esac

run_logged() {
    local log_name="$1"
    shift
    "$@" 2>&1 | tee "${build_dir}/sanitizer-logs/${log_name}.log"
}

run_asan_quarantine_tests() {
    local asan_quarantine_options
    local quarantine_case
    local quarantine_name
    local quarantine_package
    local quarantine_suite

    asan_quarantine_options="${ASAN_OPTIONS/detect_leaks=1/detect_leaks=0}"
    if [[ "${asan_quarantine_options}" == "${ASAN_OPTIONS}" ]]; then
        echo "ASAN_OPTIONS must contain detect_leaks=1 before quarantine reruns" >&2
        return 1
    fi
    for quarantine_case in "${asan_quarantine_cases[@]}"; do
        IFS='|' read -r quarantine_package quarantine_suite quarantine_name \
            <<<"${quarantine_case}"
        printf 'Running leak-qualified ASan test %s::%s::%s\n' \
            "${quarantine_package}" "${quarantine_suite}" "${quarantine_name}"
        if ! env "${rust_command_environment[@]}" \
            "${workspace_native_environment[@]}" \
            ASAN_OPTIONS="${asan_quarantine_options}" \
            CARGO_TARGET_DIR="${build_dir}/rust-workspace-target" \
                "${selected_cargo}" test \
                --manifest-path "${repo_root}/crates/Cargo.toml" \
                --locked \
                --target "${rust_target}" \
                -Zbuild-std \
                --package "${quarantine_package}" \
                --test "${quarantine_suite}" \
                --all-features \
                "${quarantine_name}" \
                -- \
                --exact \
                --test-threads=1; then
            echo "leak-qualified ASan test failed: ${quarantine_suite}::${quarantine_name}" >&2
            return 1
        fi
    done
}

audit_asan_quarantine_skips() {
    local global_list_log="${build_dir}/sanitizer-logs/asan-workspace-test-list.log"
    local quarantine_case
    local quarantine_package
    local quarantine_suite
    local quarantine_name
    local suite_key
    local suite_log
    local -a suite_cases
    declare -A suites_seen=()

    run_logged asan-workspace-test-list \
        env "${rust_command_environment[@]}" \
            "${workspace_native_environment[@]}" \
            ASAN_OPTIONS="${ASAN_OPTIONS}" \
            CARGO_TARGET_DIR="${build_dir}/rust-workspace-target" \
            "${selected_cargo}" test \
            --manifest-path "${repo_root}/crates/Cargo.toml" \
            --workspace \
            --exclude sto-tpcc-ffi \
            --all-targets \
            --all-features \
            --locked \
            --target "${rust_target}" \
            -Zbuild-std \
            -- \
            --list \
            --format terse

    python3 - "${global_list_log}" "${asan_quarantine_cases[@]}" <<'PY'
import pathlib
import sys

listed = []
for line in pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines():
    if line.endswith(": test"):
        listed.append(line.removesuffix(": test"))

if not listed:
    raise SystemExit("ASan workspace listing returned no tests")
for identity in sys.argv[2:]:
    package, suite, expected = identity.split("|", 2)
    matches = [name for name in listed if expected in name]
    if matches != [expected]:
        raise SystemExit(
            f"ASan --skip {expected!r} from {package}|{suite} matched "
            f"{matches!r}, expected exactly [{expected!r}]"
        )
print(f"verified {len(sys.argv) - 2} ASan workspace quarantine skip matches")
PY

    for quarantine_case in "${asan_quarantine_cases[@]}"; do
        IFS='|' read -r quarantine_package quarantine_suite quarantine_name \
            <<<"${quarantine_case}"
        suite_key="${quarantine_package}|${quarantine_suite}"
        if [[ -n "${suites_seen[${suite_key}]:-}" ]]; then
            continue
        fi
        suites_seen["${suite_key}"]=1
        suite_log="${build_dir}/sanitizer-logs/asan-suite-list-${quarantine_package}-${quarantine_suite}.log"
        run_logged "asan-suite-list-${quarantine_package}-${quarantine_suite}" \
            env "${rust_command_environment[@]}" \
                "${workspace_native_environment[@]}" \
                ASAN_OPTIONS="${ASAN_OPTIONS}" \
                CARGO_TARGET_DIR="${build_dir}/rust-workspace-target" \
                "${selected_cargo}" test \
                --manifest-path "${repo_root}/crates/Cargo.toml" \
                --locked \
                --target "${rust_target}" \
                -Zbuild-std \
                --package "${quarantine_package}" \
                --test "${quarantine_suite}" \
                --all-features \
                -- \
                --list \
                --format terse
        suite_cases=()
        for quarantine_case in "${asan_quarantine_cases[@]}"; do
            if [[ "${quarantine_case}" == "${suite_key}|"* ]]; then
                suite_cases+=("${quarantine_case}")
            fi
        done
        python3 - "${suite_log}" "${suite_cases[@]}" <<'PY'
import pathlib
import sys

listed = [
    line.removesuffix(": test")
    for line in pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
    if line.endswith(": test")
]
for identity in sys.argv[2:]:
    package, suite, expected = identity.split("|", 2)
    exact = [name for name in listed if name == expected]
    if exact != [expected]:
        raise SystemExit(
            f"ASan exact rerun target {identity!r} appears {len(exact)} times "
            "in its declared suite"
        )
print(f"verified {len(sys.argv) - 2} exact ASan rerun targets")
PY
    done
}

run_logged configure \
    cmake -S "${repo_root}" -B "${build_dir}" -G "${generator}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        "-DMAKO_ASAN:BOOL=${cmake_asan}" \
        "-DMAKO_UBSAN:BOOL=${cmake_ubsan}" \
        "-DMAKO_TSAN:BOOL=${cmake_tsan}" \
        "-DMAKO_FUZZER:BOOL=OFF" \
        "-DMAKO_CARGO_EXECUTABLE:FILEPATH=${selected_cargo}" \
        "-DMAKO_RUST_STO_TARGET_TRIPLE:STRING=${rust_target}" \
        "-DMAKO_RUST_STO_BUILD_STD:BOOL=${rust_build_std}" \
        "-DMAKO_RUST_STO_WORKSPACE_NATIVE_LINK:BOOL=${workspace_native_link}" \
        "-DMAKO_RUST_STO_SANITIZER_RUSTFLAGS:STRING=${rustflags}" \
        "-DMAKO_RUST_STO_SANITIZER_RUSTDOCFLAGS:STRING=${rustdocflags}" \
        "-DMAKO_RUST_STO_RUSTUP_TOOLCHAIN:STRING=${selected_rustup_toolchain}" \
        "-DMAKO_RUST_STO_CARGO_LINKER_ENV:STRING=${rust_linker_environment}" \
        "-DMAKO_RUST_STO_ASAN_QUARANTINE_TESTS:STRING=${asan_quarantine_cmake}" \
        "-DMAKO_RUST_STO_ASAN_NATIVE_QUARANTINE_TESTS:STRING=${asan_native_quarantine_cmake}" \
        "-DMAKO_RUST_STO_ASAN_OPTIONS:STRING=${ASAN_OPTIONS:-}" \
        "-DSTO_TPCC_RUST_EXTRA_RUSTFLAGS:STRING=${sto_tpcc_rustflags}"

if ! grep -Fq "${configure_confirmation}" "${build_dir}/sanitizer-logs/configure.log"; then
    echo "CMake did not confirm the requested ${sanitizer} instrumentation" >&2
    exit 1
fi

# CMake may regenerate build files from Ninja after source-list changes. Prove
# the native selector survives without the legacy environment toggle, while
# the cached Rust sanitizer configuration remains in the same build tree.
run_logged configure-regeneration \
    env -u MAKO_ASAN -u MAKO_UBSAN -u MAKO_TSAN -u MAKO_FUZZER \
        cmake -S "${repo_root}" -B "${build_dir}"
if ! grep -Fq "${configure_confirmation}" \
        "${build_dir}/sanitizer-logs/configure-regeneration.log"; then
    echo "CMake regeneration lost the requested ${sanitizer} instrumentation" >&2
    exit 1
fi
for selector_expectation in \
        "MAKO_ASAN:${cmake_asan}" \
        "MAKO_UBSAN:${cmake_ubsan}" \
        "MAKO_TSAN:${cmake_tsan}" \
        "MAKO_FUZZER:OFF"; do
    IFS=: read -r selector expected_selector_value \
        <<<"${selector_expectation}"
    configured_selector_value="$(sed -n \
        "s/^${selector}:BOOL=//p" "${build_dir}/CMakeCache.txt" | head -n 1)"
    if [[ "${configured_selector_value}" != "${expected_selector_value}" ]]; then
        echo "${selector} is ${configured_selector_value:-unset} after regeneration; expected ${expected_selector_value}" >&2
        exit 1
    fi
done

configured_workspace_native_link="$(sed -n \
    's/^MAKO_RUST_STO_WORKSPACE_NATIVE_LINK:BOOL=//p' \
    "${build_dir}/CMakeCache.txt" | head -n 1)"
if [[ "${configured_workspace_native_link}" != "${workspace_native_link}" ]]; then
    echo "CMake workspace native-link mode is ${configured_workspace_native_link:-unset}, expected ${workspace_native_link}" >&2
    exit 1
fi

rust_linker="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "${build_dir}/CMakeCache.txt" | head -n 1)"
cxx_compiler="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "${build_dir}/CMakeCache.txt" | head -n 1)"
if [[ -z "${rust_linker}" || ! -x "${rust_linker}" ]]; then
    echo "could not resolve CMake's C compiler for Rust sanitizer linkage" >&2
    exit 1
fi
if [[ -z "${cxx_compiler}" || ! -x "${cxx_compiler}" ]]; then
    echo "could not resolve CMake's C++ compiler for native sanitizer code" >&2
    exit 1
fi
shopt -s nullglob
c_compiler_metadata=("${build_dir}"/CMakeFiles/*/CMakeCCompiler.cmake)
cxx_compiler_metadata=("${build_dir}"/CMakeFiles/*/CMakeCXXCompiler.cmake)
shopt -u nullglob
if [[ "${#c_compiler_metadata[@]}" -ne 1 ||
      "${#cxx_compiler_metadata[@]}" -ne 1 ]]; then
    echo "could not resolve unique CMake C/C++ compiler metadata files" >&2
    exit 1
fi
c_compiler_id="$(sed -n 's/^set(CMAKE_C_COMPILER_ID "\([^"]*\)")$/\1/p' "${c_compiler_metadata[0]}")"
cxx_compiler_id="$(sed -n 's/^set(CMAKE_CXX_COMPILER_ID "\([^"]*\)")$/\1/p' "${cxx_compiler_metadata[0]}")"
c_compiler_version="$(sed -n 's/^set(CMAKE_C_COMPILER_VERSION "\([^"]*\)")$/\1/p' "${c_compiler_metadata[0]}")"
cxx_compiler_version="$(sed -n 's/^set(CMAKE_CXX_COMPILER_VERSION "\([^"]*\)")$/\1/p' "${cxx_compiler_metadata[0]}")"
if [[ ! "${c_compiler_id}" =~ ^(AppleClang|Clang)$ ||
      ! "${cxx_compiler_id}" =~ ^(AppleClang|Clang)$ ||
      "${c_compiler_id}" != "${cxx_compiler_id}" ]]; then
    echo "sanitizer C/C++ compilers are not from one Clang family: ${c_compiler_id:-unknown}, ${cxx_compiler_id:-unknown}" >&2
    exit 1
fi
c_compiler_major="${c_compiler_version%%.*}"
cxx_compiler_major="${cxx_compiler_version%%.*}"
if [[ ! "${c_compiler_major}" =~ ^[0-9]+$ ||
      ! "${cxx_compiler_major}" =~ ^[0-9]+$ ||
      "${c_compiler_major}" != "${cxx_compiler_major}" ]]; then
    echo "sanitizer C/C++ compiler major versions differ: ${c_compiler_version:-unknown}, ${cxx_compiler_version:-unknown}" >&2
    exit 1
fi
rust_linker_version="$("${rust_linker}" --version | head -n 1)"
cxx_compiler_banner="$("${cxx_compiler}" --version | head -n 1)"
if [[ "${rust_linker_version}" != *"clang version"* ||
      "${cxx_compiler_banner}" != *"clang version"* ]]; then
    echo "sanitizer C/C++ compiler executables do not identify as Clang" >&2
    exit 1
fi
rust_command_environment=(
    "${rust_linker_environment}=${rust_linker}"
    "RUSTFLAGS=${rustflags}"
    "RUSTDOCFLAGS=${rustdocflags}"
)
if [[ -n "${selected_rustup_toolchain}" ]]; then
    rust_command_environment+=("RUSTUP_TOOLCHAIN=${selected_rustup_toolchain}")
fi
workspace_native_environment=()
workspace_native_lib_dirs=""
workspace_native_libs=""
workspace_loader_environment=""
workspace_loader_value=""
if [[ "${workspace_native_link}" == "ON" ]]; then
    cxx_compiler_root="$(cd "$(dirname "${cxx_compiler}")/.." && pwd)"
    workspace_native_lib_dirs="${build_dir}:${build_dir}/src/masstree:${cxx_compiler_root}/lib"
    workspace_native_libs="$(sed -n \
        's/^MAKO_MTREE_RUST_NATIVE_LIBS:STRING=//p' \
        "${build_dir}/CMakeCache.txt" | head -n 1)"
    if [[ -z "${workspace_native_libs}" ]]; then
        echo "could not resolve the CMake native library closure" >&2
        exit 1
    fi
    if [[ "$(uname -s)" == "Darwin" ]]; then
        workspace_loader_environment="DYLD_LIBRARY_PATH"
    else
        workspace_loader_environment="LD_LIBRARY_PATH"
    fi
    workspace_loader_value="${cxx_compiler_root}/lib"
    if [[ -n "${!workspace_loader_environment:-}" ]]; then
        workspace_loader_value+=":${!workspace_loader_environment}"
    fi
    workspace_native_environment=(
        "MAKO_MTREE_NATIVE_INTEGRATION=1"
        "MAKO_MTREE_NATIVE_LIB_DIRS=${workspace_native_lib_dirs}"
        "MAKO_MTREE_NATIVE_LIBS=${workspace_native_libs}"
        "${workspace_loader_environment}=${workspace_loader_value}"
    )
fi

manifest="${build_dir}/rust-sto-sanitizer-manifest.txt"
{
    echo "commit=${head_sha}"
    if [[ -n "${tree_status}" ]]; then
        echo "worktree=dirty-local-diagnostic"
    else
        echo "worktree=clean"
    fi
    echo "sanitizer=${sanitizer}"
    echo "cmake_toggle=${cmake_toggle}=1"
    echo "cmake_generator=${generator}"
    echo "cmake_build_type=Release"
    echo "tmpdir=${TMPDIR}"
    echo "rustc=$("${selected_rustc}" --version)"
    echo "cargo=$("${selected_cargo}" --version)"
    echo "rust_compiler_instrumentation=${rust_instrumentation}"
    echo "rust_target=${rust_target:-host-default}"
    echo "rust_build_std=${rust_build_std}"
    echo "rust_workspace_native_link=${workspace_native_link}"
    echo "rustup_toolchain=${selected_rustup_toolchain:-workspace-pinned-stable}"
    echo "rust_runtime=${rust_runtime}"
    echo "rust_linker=${rust_linker}"
    echo "rust_linker_version=${rust_linker_version}"
    echo "c_compiler=${rust_linker}"
    echo "c_compiler_id=${c_compiler_id}"
    echo "c_compiler_version=${c_compiler_version}"
    echo "cxx_compiler=${cxx_compiler}"
    echo "cxx_compiler_id=${cxx_compiler_id}"
    echo "cxx_compiler_version=${cxx_compiler_version}"
    echo "cxx_compiler_banner=${cxx_compiler_banner}"
    echo "rustflags=${rustflags}"
    echo "rustdocflags=${rustdocflags}"
    echo "sto_tpcc_rust_extra_rustflags=${sto_tpcc_rustflags}"
    echo "ctest_selection=exact-masstree-c11-header;exact-silo-runtime-concurrency-lifecycle;exact-cpp-slow-exit-lifecycle;exact-label-rust"
    if [[ "${sanitizer}" == "undefined" ]]; then
        echo "result_qualification=checked-in-ubsan-suppressions-active"
        echo "suppression_file=${ubsan_suppressions}"
        echo "suppression_sha256=$(sha256sum "${ubsan_suppressions}" | awk '{print $1}')"
    elif [[ "${sanitizer}" == "thread" ]]; then
        echo "result_qualification=checked-in-tsan-suppressions-active"
        echo "suppression_file=${tsan_suppressions}"
        echo "suppression_sha256=$(sha256sum "${tsan_suppressions}" | awk '{print $1}')"
    else
        echo "result_qualification=checked-in-lsan-root-suppression-for-two-exact-tests;listed-intentional-quarantine-tests-leak-qualified"
        echo "suppression_file=${lsan_suppressions}"
        echo "suppression_sha256=${lsan_suppression_sha256}"
        echo "lsan_suppression_rule=leak:mt_tree::mt_tree"
        echo "lsan_suppression_scope=test_sto_tpcc_rust_slow_exit;test_sto_tpcc_rust_concurrent"
        echo "lsan_options=${lsan_options}"
        echo "lsan_expected_test_sto_tpcc_rust_slow_exit=allocations:4,bytes:1280"
        echo "lsan_expected_test_sto_tpcc_rust_concurrent=allocations:16,bytes:5120"
        echo "lsan_expected_total=allocations:20,bytes:6400"
        echo "asan_quarantine_test_count=${#asan_quarantine_cases[@]}"
        echo "asan_quarantine_tests=${asan_quarantine_cmake}"
        echo "asan_quarantine_allowlist=${quarantine_allowlist}"
        echo "asan_quarantine_allowlist_sha256=$(sha256sum "${quarantine_allowlist}" | awk '{print $1}')"
        echo "asan_native_quarantine_test_count=${#asan_native_quarantine_cases[@]}"
        echo "asan_native_quarantine_tests=${asan_native_quarantine_cmake}"
        echo "asan_native_non_quarantine_leak_detection=enabled"
        echo "asan_native_exact_quarantine_leak_detection=disabled"
        echo "asan_workspace_non_quarantine_leak_detection=enabled"
        echo "asan_exact_quarantine_leak_detection=disabled"
        echo "asan_ctest_rust_label_leak_detection=enabled"
        echo "asan_ctest_remaining_rust_label_suppressions=none"
        echo "asan_ctest_cpp_slow_exit_lifecycle_leak_detection=disabled-legacy-process-lifetime-backend"
    fi
} | tee "${manifest}"

run_logged build \
    cmake --build "${build_dir}" --parallel "${jobs}" \
        --target rust_sto_integration dbtest test_silo_runtime -- -k 0

ninja -C "${build_dir}" -t commands rust_sto_integration \
    >"${build_dir}/sanitizer-logs/rust-sto-integration-build-commands.txt"
python3 - "${build_dir}/sanitizer-logs/rust-sto-integration-build-commands.txt" \
    "${rust_compiler_instrumented}" "${sanitizer}" \
    "${rust_sanitizer_target}" "${asan_quarantine_cmake}" \
    "${asan_native_quarantine_cmake}" \
    "${rust_linker_environment}" "${rust_linker}" \
    "${selected_rustup_toolchain}" "${workspace_native_lib_dirs}" \
    "${workspace_native_libs}" "${workspace_loader_environment}" \
    "${workspace_loader_value}" <<'PY'
import pathlib
import sys

commands = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
rust_compiler_instrumented = sys.argv[2] == "1"
sanitizer = sys.argv[3]
rust_target = sys.argv[4]
quarantine_names = [name for name in sys.argv[5].split(";") if name]
native_quarantine_names = [name for name in sys.argv[6].split(";") if name]
linker_environment = sys.argv[7]
rust_linker = sys.argv[8]
rustup_toolchain = sys.argv[9]
native_lib_dirs = sys.argv[10]
native_libs = sys.argv[11]
loader_environment = sys.argv[12]
loader_value = sys.argv[13]

clippy_commands = [
    line
    for line in commands
    if " clippy " in line
    and " --workspace" in line
    and "crates/Cargo.toml" in line
]
test_commands = [
    line
    for line in commands
    if " test " in line
    and " --workspace" in line
    and " --exclude sto-tpcc-ffi" in line
]
if len(clippy_commands) != 1 or len(test_commands) != 1:
    raise SystemExit(
        "expected one workspace Clippy and test command, found "
        f"{len(clippy_commands)} and {len(test_commands)}"
    )

native_runner_commands = [
    line for line in commands if "RunRustMasstreeNativeIntegration.cmake" in line
]
if len(native_runner_commands) != 1:
    raise SystemExit(
        "expected one Rust native runner command, found "
        f"{len(native_runner_commands)}"
    )

for label, command in (
    ("Clippy", clippy_commands[0]),
    ("test", test_commands[0]),
):
    if f"{linker_environment}={rust_linker}" not in command:
        raise SystemExit(
            f"workspace {label} command does not select CMake's Clang linker"
        )
    if "-fno-sanitize" in command:
        raise SystemExit(f"workspace {label} command disables sanitizer coverage")
if f"-DMAKO_RUST_LINKER_ENV_NAME={linker_environment}" not in native_runner_commands[0]:
    raise SystemExit("native Rust runner lacks the Cargo linker environment")
if f"-DMAKO_RUST_LINKER={rust_linker}" not in native_runner_commands[0]:
    raise SystemExit("native Rust runner does not select CMake's Clang linker")
if "-fno-sanitize" in native_runner_commands[0]:
    raise SystemExit("native Rust runner disables sanitizer coverage")

if rust_compiler_instrumented:
    required = (
        "MAKO_MTREE_NATIVE_INTEGRATION=1",
        f"MAKO_MTREE_NATIVE_LIB_DIRS={native_lib_dirs}",
        f"MAKO_MTREE_NATIVE_LIBS={native_libs}",
        f"{loader_environment}={loader_value}",
        f"-Zsanitizer={sanitizer}",
        "-Zexternal-clangrt",
        "-Zbuild-std",
        f"--target {rust_target}",
        f"RUSTUP_TOOLCHAIN={rustup_toolchain}",
    )
    for label, command in (("Clippy", clippy_commands[0]), ("test", test_commands[0])):
        missing = [fragment for fragment in required if fragment not in command]
        if missing:
            raise SystemExit(
                f"workspace {label} command lacks sanitizer/native closure: "
                + ", ".join(missing)
            )
        if "unsafe-allow-abi-mismatch" in command:
            raise SystemExit(f"workspace {label} command waives a sanitizer ABI mismatch")
    if sanitizer == "address":
        if "detect_leaks=1" not in test_commands[0]:
            raise SystemExit("workspace ASan test command does not retain leak detection")
        missing_skips = [
            name for name in quarantine_names if f"--skip {name}" not in test_commands[0]
        ]
        if missing_skips:
            raise SystemExit(
                "workspace ASan test command lacks exact quarantine skips: "
                + ", ".join(missing_skips)
            )
        if "detect_leaks=1" not in native_runner_commands[0]:
            raise SystemExit("native ASan runner does not inherit leak detection")
        missing_native_quarantines = [
            name
            for name in native_quarantine_names
            if name not in native_runner_commands[0]
        ]
        if missing_native_quarantines:
            raise SystemExit(
                "native ASan runner lacks exact quarantine allowlist: "
                + ", ".join(missing_native_quarantines)
            )
else:
    for command in (*clippy_commands, *test_commands):
        if "MAKO_MTREE_NATIVE_INTEGRATION=1" in command:
            raise SystemExit("stable UBSan workspace unexpectedly enables native-gated targets")
        if "-fsanitize=undefined" not in command:
            raise SystemExit("stable UBSan workspace lacks the native runtime link flag")

print("verified Rust workspace sanitizer and native-link environment")
PY

if [[ "${sanitizer}" == "address" ]]; then
    run_logged asan-quarantine-preflight audit_asan_quarantine_skips
    run_logged asan-quarantine-tests run_asan_quarantine_tests
fi

ninja -C "${build_dir}" -t commands rust_sto_integration dbtest \
    >"${build_dir}/sanitizer-logs/sto-tpcc-build-commands.txt"
python3 - "${build_dir}/sanitizer-logs/sto-tpcc-build-commands.txt" \
    "-fsanitize=${sanitizer}" "${rust_compiler_instrumented}" \
    "${rust_sanitizer_target}" "${rust_linker_environment}" \
    "${rust_linker}" "${selected_rustup_toolchain}" <<'PY'
import pathlib
import re
import sys

commands = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
required_flag = sys.argv[2]
rust_compiler_instrumented = sys.argv[3] == "1"
rust_target = sys.argv[4]
linker_environment = sys.argv[5]
rust_linker = sys.argv[6]
rustup_toolchain = sys.argv[7]

rust_builds = [line for line in commands if "--package sto-tpcc-ffi" in line]
if len(rust_builds) != 1:
    raise SystemExit(
        f"expected one sto-tpcc-ffi Rust build command, found {len(rust_builds)}"
    )
rust_build = rust_builds[0]
if f"{linker_environment}={rust_linker}" not in rust_build:
    raise SystemExit("sto-tpcc-ffi Rust build does not select CMake's Clang linker")
if "-fno-sanitize" in rust_build:
    raise SystemExit("sto-tpcc-ffi Rust build disables sanitizer coverage")
if rust_compiler_instrumented:
    required_rust_fragments = (
        f"-Zsanitizer={required_flag.removeprefix('-fsanitize=')}",
        "-Zexternal-clangrt",
        "-Zbuild-std",
        f"--target {rust_target}",
        f"RUSTUP_TOOLCHAIN={rustup_toolchain}",
    )
    missing_rust = [item for item in required_rust_fragments if item not in rust_build]
    if missing_rust:
        raise SystemExit(
            "sto-tpcc-ffi Rust build lacks compiler instrumentation: "
            + ", ".join(missing_rust)
        )
    if "unsafe-allow-abi-mismatch" in rust_build:
        raise SystemExit("sto-tpcc-ffi Rust build waives a sanitizer ABI mismatch")
else:
    if required_flag not in rust_build:
        raise SystemExit(f"sto-tpcc-ffi Rust build lacks linker flag {required_flag}")

boundary_executables = (
    "dbtest",
    "sto_tpcc_bench",
    "sto_tpcc_cpp_wrapper_smoke",
    "sto_tpcc_cpp_wrapper_scan_smoke",
    "test_mako_value_metadata",
    "test_mtree_abi",
    "test_mtree_abi_c11_header",
    "test_silo_varint",
    "test_srpc_epoll_platform",
    "test_sto_tpcc_ffi_c11_header",
)
for executable in boundary_executables:
    output_pattern = re.compile(rf"(?:^| )-o {re.escape(executable)}(?: |$)")
    links = [line for line in commands if output_pattern.search(line)]
    if len(links) != 1:
        raise SystemExit(
            f"expected one {executable} link command, found {len(links)}"
        )
    link = links[0]
    if required_flag not in link.split():
        raise SystemExit(f"{executable} native link lacks {required_flag}")
    if "-fno-sanitize" in link:
        raise SystemExit(f"{executable} native link disables sanitizer coverage")
    if required_flag in ("-fsanitize=address", "-fsanitize=thread"):
        allocator_tokens = ("jemalloc", "tcmalloc")
        forbidden = [token for token in allocator_tokens if token in link]
        if forbidden:
            raise SystemExit(
                f"{executable} sanitizer link retains an incompatible allocator: "
                + ", ".join(forbidden)
            )

print("verified Rust archive sanitizer linkage and all boundary executable links")
PY

if [[ "${rust_compiler_instrumented}" == "1" ]]; then
    rust_target_dir="$(sed -n 's/^STO_TPCC_RUST_TARGET_DIR:[^=]*=//p' \
        "${build_dir}/CMakeCache.txt" | head -n 1)"
    cmake_nm="$(sed -n 's/^CMAKE_NM:[^=]*=//p' \
        "${build_dir}/CMakeCache.txt" | head -n 1)"
    rust_archive="${rust_target_dir}/${rust_target}/release/libsto_tpcc_ffi.a"
    if [[ -z "${rust_target_dir}" || ! -f "${rust_archive}" ]]; then
        echo "instrumented Rust static archive is missing: ${rust_archive}" >&2
        exit 1
    fi
    if [[ -z "${cmake_nm}" || ! -x "${cmake_nm}" ]]; then
        echo "could not resolve CMake's symbol inspector" >&2
        exit 1
    fi

    undefined_symbols="${build_dir}/sanitizer-logs/sto-tpcc-rust-undefined-symbols.txt"
    defined_symbols="${build_dir}/sanitizer-logs/sto-tpcc-rust-defined-symbols.txt"
    "${cmake_nm}" -u "${rust_archive}" >"${undefined_symbols}"
    "${cmake_nm}" --defined-only "${rust_archive}" >"${defined_symbols}"
    if [[ "${sanitizer}" == "address" ]]; then
        hook_pattern='__asan_report_(load|store)'
        runtime_pattern=' [TW] __asan_init$'
    else
        hook_pattern='__tsan_(read|write)'
        runtime_pattern=' [TW] __tsan_init$'
    fi
    if ! grep -Eq "${hook_pattern}" "${undefined_symbols}"; then
        echo "Rust static archive has no ${sanitizer} compiler hooks" >&2
        exit 1
    fi
    if grep -Eq "${runtime_pattern}" "${defined_symbols}"; then
        echo "Rust static archive bundled a conflicting ${sanitizer} runtime" >&2
        exit 1
    fi
    echo "verified Rust ${sanitizer} hooks and external runtime ownership"
fi

# Do not trust the toggle alone. Confirm that every boundary translation unit
# actually received the sanitizer flag in this generated build.
python3 - "${build_dir}/compile_commands.json" "-fsanitize=${sanitizer}" <<'PY'
import json
import pathlib
import sys

database_path = pathlib.Path(sys.argv[1])
required_flag = sys.argv[2]
required_sources = {
    "crates/sto-tpcc-ffi/tests/cpp_wrapper_fixed_read_smoke.cc",
    "crates/sto-tpcc-ffi/tests/cpp_wrapper_scan_smoke.cc",
    "src/mako/benchmarks/bench.cc",
    "src/mako/benchmarks/dbtest.cc",
    "src/mako/benchmarks/rpc_setup.cc",
    "src/mako/benchmarks/sync_util_init.cc",
    "src/mako/benchmarks/tpcc.cc",
    "src/mako/lib/server.cc",
    "src/mako/silo_runtime.cc",
    "src/mako/sto/ReplayDB.cc",
    "src/mako/sto/ThreadPool.cc",
    "src/mako/sto/Transaction.cc",
    "src/mako/storage/mtree_abi.cc",
    "src/mako/storage/rust_sto_tpcc_wrapper.cc",
    "src/srpc/reactor/epoll_platform_linux.cc",
    "tests/mtree_abi_c11_header.c",
    "tests/srpc_epoll_platform_test.cc",
    "tests/sto_tpcc_ffi_c11_header.c",
    "tests/test_mako_value_metadata.cc",
    "tests/test_mtree_abi.cc",
    "tests/test_silo_runtime.cc",
    "tests/test_silo_varint.cc",
}

database = json.loads(database_path.read_text(encoding="utf-8"))
seen = set()
for entry in database:
    source = pathlib.Path(entry["file"])
    try:
        relative_source = source.resolve().relative_to(pathlib.Path.cwd().resolve()).as_posix()
    except ValueError:
        continue
    if relative_source not in required_sources:
        continue
    command = entry.get("command") or " ".join(entry.get("arguments", []))
    if required_flag not in command.split():
        raise SystemExit(f"{relative_source} lacks {required_flag} in compile_commands.json")
    if "-fno-sanitize" in command:
        raise SystemExit(
            f"{relative_source} disables sanitizer coverage in compile_commands.json"
        )
    seen.add(relative_source)

missing = sorted(required_sources - seen)
if missing:
    raise SystemExit("compile_commands.json lacks boundary sources: " + ", ".join(missing))
print("verified native instrumentation: " + ", ".join(sorted(seen)))
PY

ctest_json="${build_dir}/sanitizer-logs/ctest-tests.json"
ctest --test-dir "${build_dir}" --show-only=json-v1 >"${ctest_json}"
python3 - "${ctest_json}" <<'PY'
import json
import pathlib
import sys

tests = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")).get("tests", [])
names = {test["name"] for test in tests}
required = {
    "SiloVarintTests",
    "test_mako_value_metadata",
    "test_mtree_abi",
    "test_mtree_abi_c11_header",
    "test_silo_runtime_concurrency_lifecycle",
    "test_srpc_epoll_platform",
    "test_sto_tpcc_ffi_c11_header",
    "test_sto_tpcc_cpp_wrapper_scan_smoke",
    "test_sto_tpcc_cpp_wrapper_smoke",
    "test_sto_tpcc_cpp_slow_exit",
    "test_sto_tpcc_cpp_multishard_slow_exit",
    "test_sto_tpcc_rust_slow_exit",
    "test_sto_tpcc_rust_concurrent",
    "test_sto_tpcc_rejects_duplicate_local_shards",
    "test_sto_tpcc_rejects_partial_local_shards",
    "test_sto_tpcc_rejects_rust_distributed_shard",
    "test_sto_tpcc_rejects_rust_replication",
}
missing = sorted(required - names)
if missing:
    raise SystemExit("required Rust boundary CTests are not registered: " + ", ".join(missing))

rust_names = set()
for test in tests:
    for prop in test.get("properties", []):
        if prop.get("name") == "LABELS" and "rust" in prop.get("value", []):
            rust_names.add(test["name"])

if not rust_names:
    raise SystemExit('no CTest carries the exact "rust" label')

explicit_non_rust_runs = {
    "test_mtree_abi_c11_header",
    "test_silo_runtime_concurrency_lifecycle",
    "test_sto_tpcc_cpp_slow_exit",
    "test_sto_tpcc_cpp_multishard_slow_exit",
}
missing_rust_labels = sorted((required - explicit_non_rust_runs) - rust_names)
if missing_rust_labels:
    raise SystemExit(
        'required CTests lost the exact "rust" label: ' + ", ".join(missing_rust_labels)
    )

print("registered Rust CTests:")
for name in sorted(rust_names):
    print(f"  {name}")

tpcc = sorted(name for name in rust_names if "tpcc" in name)
if tpcc:
    print("registered Rust TPC-C tests selected by this run:")
    for name in tpcc:
        print(f"  {name}")
else:
    print("no Rust TPC-C CTest is registered at this commit")
PY

# The public Masstree ABI header test predates the Rust label. Keep it in the
# native boundary gate without broadening the dynamic label selection to every
# Masstree stress test in the repository.
run_logged ctest-masstree-c11-header \
    ctest --test-dir "${build_dir}" --output-on-failure \
        --no-tests=error -R '^test_mtree_abi_c11_header$'

# The default runtime is first acquired concurrently in a fresh process.  This
# directly guards its process-lifetime publication contract under each native
# sanitizer, independent of the longer TPC-C lifecycle tests below.
run_logged ctest-silo-runtime-concurrency-lifecycle \
    ctest --test-dir "${build_dir}" --output-on-failure \
        --no-tests=error -R '^test_silo_runtime_concurrency_lifecycle$'

# The C++ reference backend deliberately retains native STO/Masstree state for
# process lifetime, but both its single- and multi-shard graceful teardown must
# still run under each native sanitizer. ASan therefore disables only leak
# reporting for these exact tests; address errors remain fatal and every
# Rust-labeled CTest retains leak detection.
if [[ "${sanitizer}" == "address" ]]; then
    asan_cpp_lifecycle_options="${ASAN_OPTIONS/detect_leaks=1/detect_leaks=0}"
    if [[ "${asan_cpp_lifecycle_options}" == "${ASAN_OPTIONS}" ]]; then
        echo "ASAN_OPTIONS must contain detect_leaks=1 before the C++ lifecycle test" >&2
        exit 1
    fi
    run_logged ctest-cpp-slow-exit-lifecycle \
        env ASAN_OPTIONS="${asan_cpp_lifecycle_options}" \
            ctest --test-dir "${build_dir}" --output-on-failure \
                --no-tests=error \
                -R '^(test_sto_tpcc_cpp_slow_exit|test_sto_tpcc_cpp_multishard_slow_exit)$'
else
    run_logged ctest-cpp-slow-exit-lifecycle \
        ctest --test-dir "${build_dir}" --output-on-failure \
            --no-tests=error \
            -R '^(test_sto_tpcc_cpp_slow_exit|test_sto_tpcc_cpp_multishard_slow_exit)$'
fi

if [[ "${sanitizer}" == "address" ]]; then
    current_lsan_sha256="$(sha256sum "${lsan_suppressions}" | awk '{print $1}')"
    if [[ "${current_lsan_sha256}" != "${lsan_suppression_sha256}" ]]; then
        echo "LSan suppression source changed after sanitizer configuration" >&2
        exit 1
    fi

    # Passing CTests hide child output unless verbose mode is selected. Run the
    # only two qualified tests separately so their suppression tables are
    # retained as independently auditable evidence.
    run_logged ctest-rust-slow-exit-lsan \
        env LSAN_OPTIONS="${lsan_options}" \
            ctest --test-dir "${build_dir}" --verbose \
                --no-tests=error -R '^test_sto_tpcc_rust_slow_exit$'
    run_logged ctest-rust-concurrent-lsan \
        env LSAN_OPTIONS="${lsan_options}" \
            ctest --test-dir "${build_dir}" --verbose \
                --no-tests=error -R '^test_sto_tpcc_rust_concurrent$'

    current_lsan_sha256="$(sha256sum "${lsan_suppressions}" | awk '{print $1}')"
    if [[ "${current_lsan_sha256}" != "${lsan_suppression_sha256}" ]]; then
        echo "LSan suppression source changed while qualified tests ran" >&2
        exit 1
    fi

    python3 - "${manifest}" \
        "${build_dir}/sanitizer-logs/ctest-rust-slow-exit-lsan.log" \
        test_sto_tpcc_rust_slow_exit 4 1280 \
        "${build_dir}/sanitizer-logs/ctest-rust-concurrent-lsan.log" \
        test_sto_tpcc_rust_concurrent 16 5120 <<'PY'
import hashlib
import pathlib
import re
import sys

manifest = pathlib.Path(sys.argv[1])
arguments = sys.argv[2:]
if len(arguments) % 4:
    raise SystemExit("internal error: malformed LSan evidence arguments")

observed = []
for index in range(0, len(arguments), 4):
    log = pathlib.Path(arguments[index])
    test_name = arguments[index + 1]
    expected_count = int(arguments[index + 2])
    expected_bytes = int(arguments[index + 3])
    lines = log.read_text(encoding="utf-8").splitlines()
    clean = [re.sub(r"^\s*\d+:\s?", "", line) for line in lines]
    markers = [i for i, line in enumerate(clean) if line.strip() == "Suppressions used:"]
    if len(markers) != 1:
        raise SystemExit(
            f"{test_name}: expected one LSan suppression block, found {len(markers)}"
        )
    start = markers[0] + 1
    try:
        end = next(
            i
            for i in range(start, len(clean))
            if re.fullmatch(r"-{5,}", clean[i].strip())
        )
    except StopIteration:
        raise SystemExit(f"{test_name}: unterminated LSan suppression block")

    rows = []
    for line in clean[start:end]:
        value = line.strip()
        if not value or re.fullmatch(r"count\s+bytes\s+template", value):
            continue
        match = re.fullmatch(r"(\d+)\s+(\d+)\s+(\S+)", value)
        if match is None:
            raise SystemExit(f"{test_name}: unexpected LSan suppression row: {value!r}")
        rows.append((int(match.group(1)), int(match.group(2)), match.group(3)))

    expected = [(expected_count, expected_bytes, "mt_tree::mt_tree")]
    if rows != expected:
        raise SystemExit(f"{test_name}: LSan rows {rows!r}, expected {expected!r}")
    observed.append((test_name, expected_count, expected_bytes, log.resolve(), hashlib.sha256(log.read_bytes()).hexdigest()))

if (sum(row[1] for row in observed), sum(row[2] for row in observed)) != (20, 6400):
    raise SystemExit("qualified LSan totals differ from 20 allocations / 6400 bytes")

with manifest.open("a", encoding="utf-8") as output:
    for test_name, count, byte_count, log, digest in observed:
        output.write(
            f"lsan_observed_{test_name}=allocations:{count},bytes:{byte_count}\n"
        )
        output.write(f"lsan_evidence_{test_name}={log}\n")
        output.write(f"lsan_evidence_{test_name}_sha256={digest}\n")
    output.write("lsan_observed_total=allocations:20,bytes:6400\n")
print("verified exact LSan evidence: 20 allocations / 6400 bytes")
PY

    # Every other Rust-labeled test runs with leak detection enabled and no
    # LSan suppression environment at all.
    run_logged ctest-rust-boundary \
        ctest --test-dir "${build_dir}" --output-on-failure \
            --no-tests=error -L '^rust$' \
            -E '^(test_sto_tpcc_rust_slow_exit|test_sto_tpcc_rust_concurrent)$'
else
    run_logged ctest-rust-boundary \
        ctest --test-dir "${build_dir}" --output-on-failure \
            --no-tests=error -L '^rust$'
fi

echo "gate_status=passed" >>"${manifest}"
echo "${sanitizer} sanitizer gate passed for ${head_sha}"
