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
  MAKO_SANITIZER_BUILD_DIR=PATH Override the fresh sanitizer build path. The
                                selected path must not already exist.
  MAKO_SANITIZER_JOBS=N         Parallel build jobs. Default: 4.
  MAKO_SANITIZER_FOCUS=cache    Run only the native Rust adapter/cache gate.
  CMAKE_GENERATOR=Ninja         This gate audits Ninja's generated commands.

Coverage contract:
  * Production Masstree storage and SRPC's C/assembly platform code receive
    the selected compiler instrumentation. Rust applications use native SRPC.
  * Transaction-manager tests have no native linkage. Tree, adapter, runtime
    and executable-process tests run separately, one test per fresh process.
  * ASan/TSan instrument Rust and one pinned, rebuilt standard library in a
    private sysroot. Final binaries use the external Clang sanitizer runtime;
    the gate audits native/Rust hooks and the single std identity.
  * UBSan instruments native C/C++; stable Rust has no UBSan compiler mode.
  * ASan retains leak detection. Only exact inventoried intentional-quarantine
    cases disable leak reporting; all remain compiler-instrumented and execute.
  * The cache gate retains its allocation-root suppressions, leak tripwire,
    exact quarantine inventories and separately executed Loom models.
  * Native Masstree TSan/UBSan suppressions remain explicit and recorded.
    Cache tests remove them and use their own narrower qualification.

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
if [[ -n "${MAKO_SANITIZER_FOCUS:-}" && "${MAKO_SANITIZER_FOCUS}" != "cache" ]]; then
    echo "MAKO_SANITIZER_FOCUS must be unset or cache" >&2
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
if [[ -e "${build_dir}" || -L "${build_dir}" ]]; then
    echo "sanitizer evidence requires a fresh, nonexistent build path: ${build_dir}" >&2
    echo "set MAKO_SANITIZER_BUILD_DIR to a new sanitizer-specific path" >&2
    exit 2
fi
mkdir -p "${build_dir}/sanitizer-logs" "${build_dir}/tmp"
export TMPDIR="${build_dir}/tmp"

mode_stamp="${build_dir}/.rust-sto-sanitizer-mode"
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
native_unwind_cmake_arguments=()
unwind_backend="platform-default"
unwind_config=""
unwind_config_sha256=""
if [[ "$(uname -s)" == "Linux" ]]; then
    # LLVM #226351: slow unwinding can scan beyond Ubuntu's unterminated loader
    # .eh_frame before main. Select the GNU unwinder without disabling LSan or
    # slow unwinding. Clang prepends config options, so gcc_s precedes libc++'s
    # linker-script dependency on LLVM libunwind even in Rust's final link.
    unwind_backend="libgcc"
    unwind_config="${build_dir}/clang-gnu-unwind.cfg"
    printf '%s\n' \
        '--unwindlib=libgcc' \
        '-Wl,--push-state,--no-as-needed' \
        '-lgcc_s' \
        '-Wl,--pop-state' >"${unwind_config}"
    unwind_config_sha256="$(sha256sum "${unwind_config}")"
    unwind_config_sha256="${unwind_config_sha256%% *}"
    rust_link_flag+=" -C link-arg=--config=${unwind_config}"
    native_unwind_cmake_arguments=(
        "-DCMAKE_EXE_LINKER_FLAGS:STRING=--config=${unwind_config}"
        "-DCMAKE_SHARED_LINKER_FLAGS:STRING=--config=${unwind_config}"
        "-DCMAKE_MODULE_LINKER_FLAGS:STRING=--config=${unwind_config}"
    )
fi

verify_unwind_config() {
    if [[ -n "${unwind_config}" ]]; then
        printf '%s  %s\n' "${unwind_config_sha256}" "${unwind_config}" |
            sha256sum --check --strict
    fi
}

rust_target=""
rust_std_source="toolchain-prebuilt"
rust_std_sysroot=""
rust_std_rustflags=""
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
    if ! RUSTUP_TOOLCHAIN="${rust_sanitizer_nightly}" \
            "${selected_cargo}" clippy --version >/dev/null 2>&1; then
        echo "${rust_sanitizer_nightly} is missing the clippy component" >&2
        exit 2
    fi
    rust_target="${rust_sanitizer_target}"
    workspace_native_link="ON"
    if [[ "${build_dir}" =~ [[:space:]] ]]; then
        echo "the Rust sanitizer sysroot path must not contain whitespace: ${build_dir}" >&2
        exit 2
    fi
    rust_std_source="instrumented-sysroot"
    rust_std_sysroot="${build_dir}/rust-sanitizer-sysroot"
    rust_sanitizer_flags="-Zsanitizer=${sanitizer} -Zexternal-clangrt"
    # The flags std itself is compiled with: those of every first-party crate,
    # without link arguments and the sysroot it is about to become.
    rust_std_rustflags="${rust_sanitizer_flags} -C force-frame-pointers=yes"
    rust_sanitizer_flags+=" --sysroot=${rust_std_sysroot}"
    rustflags="${rust_sanitizer_flags} -C force-frame-pointers=yes ${rust_link_flag}"
    rustdocflags="${rust_sanitizer_flags} -C force-frame-pointers=yes ${rust_link_flag}"
    sto_tpcc_rustflags="${rust_sanitizer_flags}"
    rust_instrumentation="${sanitizer}-nightly-sysroot"
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
    if [[ "${stable_version}" != "1.97.1" ]]; then
        echo "UBSan requires stable Rust 1.97.1, found ${stable_version}" >&2
        exit 2
    fi
    rustflags="-C force-frame-pointers=yes ${rust_link_flag}"
    rustdocflags="-C force-frame-pointers=yes ${rust_link_flag}"
    sto_tpcc_rustflags="${rust_link_flag}"
    rust_instrumentation="none-stable-1.97"
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

# Rebuild one instrumented standard library for every Cargo invocation in the
# gate. Build scripts and proc-macros still use the host toolchain's library.
build_rust_sanitizer_sysroot() {
    local seed="${build_dir}/rust-sanitizer-sysroot-seed"
    local wrapper="${build_dir}/rust-sanitizer-sysroot-rustc"
    local artifacts="${build_dir}/sanitizer-logs/rust-sanitizer-sysroot-artifacts.json"
    local build_log="${build_dir}/sanitizer-logs/rust-sanitizer-sysroot-cargo.log"
    local lib_dir="${rust_std_sysroot}/lib/rustlib/${rust_target}/lib"

    mkdir -p "${seed}/src" "${lib_dir}"
    cat >"${seed}/Cargo.toml" <<'EOF'
[package]
name = "mako-sanitizer-sysroot-seed"
version = "0.0.0"
edition = "2021"
publish = false

[lib]
path = "src/lib.rs"

[workspace]
EOF
    printf '#![no_std]\n' >"${seed}/src/lib.rs"
    cat >"${wrapper}" <<'EOF'
#!/bin/sh
# RUSTC_WRAPPER for the sanitizer sysroot build only.
rustc="$1"
shift
previous=""
for argument in "$@"; do
    if [ "${previous}" = "--crate-name" ] && [ "${argument}" = "panic_abort" ]; then
        exec "${rustc}" "$@" -C panic=abort
    fi
    previous="${argument}"
done
exec "${rustc}" "$@"
EOF
    chmod +x "${wrapper}"

    echo "Building the instrumented Rust std once into ${rust_std_sysroot}"
    if ! env "RUSTUP_TOOLCHAIN=${selected_rustup_toolchain}" \
            "RUSTC_WRAPPER=${wrapper}" \
            "RUSTFLAGS=${rust_std_rustflags}" \
            CARGO_INCREMENTAL=0 \
            CARGO_PROFILE_RELEASE_OPT_LEVEL=3 \
            CARGO_PROFILE_RELEASE_DEBUG=1 \
            CARGO_PROFILE_RELEASE_DEBUG_ASSERTIONS=true \
            CARGO_PROFILE_RELEASE_OVERFLOW_CHECKS=true \
            CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1 \
            CARGO_PROFILE_RELEASE_LTO=false \
            CARGO_PROFILE_RELEASE_PANIC=unwind \
            "${selected_cargo}" build --release \
                --manifest-path "${seed}/Cargo.toml" \
                --target "${rust_target}" \
                --target-dir "${build_dir}/rust-sanitizer-sysroot-target" \
                -Zbuild-std=std,panic_abort,panic_unwind,test \
                --message-format=json-render-diagnostics \
                >"${artifacts}" 2>"${build_log}"; then
        cat "${build_log}" >&2
        echo "could not build the instrumented Rust std" >&2
        return 1
    fi
    cat "${build_log}"
    python3 - "${artifacts}" "${lib_dir}" <<'PY'
import json
import pathlib
import shutil
import sys

artifacts = pathlib.Path(sys.argv[1])
lib_dir = pathlib.Path(sys.argv[2])
rlibs = {}
for line in artifacts.read_text(encoding="utf-8").splitlines():
    message = json.loads(line)
    if message.get("reason") != "compiler-artifact":
        continue
    if "mako-sanitizer-sysroot-seed" in message["package_id"]:
        continue
    for filename in message["filenames"]:
        if filename.endswith(".rlib"):
            rlibs.setdefault(message["target"]["name"], []).append(pathlib.Path(filename))
required = {"core", "alloc", "std", "compiler_builtins", "panic_abort",
            "panic_unwind", "unwind", "test", "proc_macro"}
missing = sorted(required - set(rlibs))
if missing:
    raise SystemExit(f"sanitizer sysroot build lacks {missing}")
repeated = sorted(name for name, paths in rlibs.items() if len(paths) != 1)
if repeated:
    raise SystemExit(f"sanitizer sysroot build produced several rlibs for {repeated}")
if any(lib_dir.iterdir()):
    raise SystemExit(f"sanitizer sysroot library directory is not fresh: {lib_dir}")
for name, (path,) in sorted(rlibs.items()):
    shutil.copy2(path, lib_dir / path.name)
print(f"sanitizer sysroot holds {len(rlibs)} crates: " + " ".join(sorted(rlibs)))
PY
    # rustc finds its self-contained linker (rust-lld, gcc-ld) under the
    # sysroot it compiles against.
    ln -s "${rust_sysroot}/lib/rustlib/${rust_target}/bin" \
        "${rust_std_sysroot}/lib/rustlib/${rust_target}/bin"
}

if [[ -n "${rust_std_sysroot}" ]]; then
    run_logged rust-sanitizer-sysroot build_rust_sanitizer_sysroot
fi

audit_one_rust_std() {
    local label="$1"
    shift
    python3 - "${cmake_nm}" "${rust_std_libdir}" "${jobs}" \
        "${build_dir}/sanitizer-logs/rust-std-${label}.txt" "$@" <<'PY' |
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile

nm, libdir, jobs, report = sys.argv[1], pathlib.Path(sys.argv[2]), int(sys.argv[3]), pathlib.Path(sys.argv[4])
std_pattern = re.compile(r"Cs[0-9A-Za-z]*_3std")


def inspect(path):
    stds = set()
    personalities = 0
    with tempfile.TemporaryFile(mode="w+") as errors:
        with subprocess.Popen([nm, "--defined-only", str(path)], stdout=subprocess.PIPE,
                              stderr=errors, text=True) as process:
            for line in process.stdout:
                stds.update(std_pattern.findall(line))
                if line.rstrip("\n").endswith((" T rust_eh_personality", " t rust_eh_personality")):
                    personalities += 1
        if process.returncode != 0:
            errors.seek(0)
            raise SystemExit(f"{nm} failed on {path}: {errors.read().strip()}")
    return sorted(stds), personalities


def is_executable(path):
    if path.is_symlink() or not path.is_file():
        return False
    if path.suffix in (".so", ".rlib", ".a", ".rmeta", ".d", ".o"):
        return False
    if path.name.startswith(("build-script", "build_script")) or not path.stat().st_mode & 0o111:
        return False
    with path.open("rb") as stream:
        return stream.read(4) == b"\x7fELF"


std_rlibs = sorted(libdir.glob("libstd-*.rlib"))
if len(std_rlibs) != 1:
    raise SystemExit(f"expected one libstd rlib in {libdir}, found {std_rlibs}")
expected, _ = inspect(std_rlibs[0])
if len(expected) != 1:
    raise SystemExit(f"the job's std defines several crate identities: {expected}")
expected_std = expected[0]

artifacts = []
trees = {}
mode = None
for argument in sys.argv[5:]:
    if argument in ("--must", "--tree", "--may-dir"):
        mode = argument
        continue
    path = pathlib.Path(argument)
    if mode == "--must":
        if not path.is_file():
            raise SystemExit(f"Rust artifact is missing: {path}")
        artifacts.append((path, True, None))
    elif mode == "--tree":
        if not path.is_dir():
            raise SystemExit(f"Rust target directory is missing: {path}")
        trees[path] = 0
        artifacts.extend((candidate, False, path) for candidate in sorted(path.rglob("*"))
                         if is_executable(candidate))
    elif mode == "--may-dir":
        artifacts.extend((candidate, False, None) for candidate in sorted(path.iterdir())
                         if is_executable(candidate))
    else:
        raise SystemExit(f"internal error: artifact {argument} has no mode")

failures = []
lines = []
with_rust = 0
with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
    results = pool.map(lambda item: (item, inspect(item[0])), artifacts)
    for (path, must, tree), (stds, personalities) in results:
        archive = path.suffix == ".a"
        lines.append(f"{path} std={','.join(stds) or '-'} "
                     f"rust_eh_personality={'n/a' if archive else personalities}")
        if not stds:
            if must:
                failures.append(f"{path}: carries no Rust std")
            continue
        with_rust += 1
        if tree is not None:
            trees[tree] += 1
        if stds != [expected_std]:
            failures.append(f"{path}: std {stds}, expected [{expected_std}]")
        elif not archive and personalities != 1:
            failures.append(f"{path}: {personalities} rust_eh_personality definitions")
failures.extend(f"{tree}: holds no Rust executable" for tree, count in trees.items() if not count)
report.write_text("\n".join(lines) + "\n", encoding="utf-8")
if failures:
    raise SystemExit("artifacts do not carry exactly the job's Rust std:\n  " + "\n  ".join(failures))
print(f"{len(artifacts)} artifacts, {with_rust} with Rust, all std {expected_std}")
PY
        tee "${build_dir}/sanitizer-logs/rust-std-${label}.log"
    echo "rust_one_std_${label}=$(tail -n 1 "${build_dir}/sanitizer-logs/rust-std-${label}.log")" >>"${manifest}"
}

run_logged configure \
    cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DUSE_MALLOC_MODE=0 \
        "${native_unwind_cmake_arguments[@]}" \
        "-DMAKO_ASAN:BOOL=${cmake_asan}" "-DMAKO_UBSAN:BOOL=${cmake_ubsan}" \
        "-DMAKO_TSAN:BOOL=${cmake_tsan}" \
        "-DMAKO_NATIVE_CARGO:FILEPATH=${selected_cargo}"
grep -Fq "Native storage sanitizer: ${sanitizer}" "${build_dir}/sanitizer-logs/configure.log"
run_logged regenerate cmake -S "${repo_root}" -B "${build_dir}"
grep -Fq "Native storage sanitizer: ${sanitizer}" "${build_dir}/sanitizer-logs/regenerate.log"
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
cmake_nm="$(sed -n 's/^CMAKE_NM:[^=]*=//p' "${build_dir}/CMakeCache.txt" | head -n 1)"
if [[ -z "${cmake_nm}" || ! -x "${cmake_nm}" ]]; then
    echo "could not resolve CMake's symbol inspector" >&2
    exit 1
fi
if [[ -n "${rust_std_sysroot}" ]]; then
    rust_std_libdir="${rust_std_sysroot}/lib/rustlib/${rust_target}/lib"
    # std's crate hash does not see RUSTFLAGS, so only its symbols show that
    # the sysroot's std is instrumented.
    if [[ "${sanitizer}" == "address" ]]; then
        std_hook_pattern=' U __asan_report_(load|store)'
    else
        std_hook_pattern=' U __tsan_(read|write|func_entry)'
    fi
    std_undefined_symbols="${build_dir}/sanitizer-logs/rust-sysroot-std-undefined-symbols.txt"
    if ! "${cmake_nm}" -u "${rust_std_libdir}"/libstd-*.rlib \
            >"${std_undefined_symbols}" 2>"${std_undefined_symbols}.stderr"; then
        cat "${std_undefined_symbols}.stderr" >&2
        echo "could not inspect the Rust sanitizer sysroot's std" >&2
        exit 1
    fi
    if ! grep -Eq "${std_hook_pattern}" "${std_undefined_symbols}"; then
        echo "the Rust sanitizer sysroot's std lacks ${sanitizer} instrumentation" >&2
        exit 1
    fi
    echo "verified ${sanitizer} instrumentation in the sanitizer sysroot's std"
else
    rust_std_libdir="$("${selected_rustc}" --print target-libdir)"
fi
rust_command_environment=(
    "${rust_linker_environment}=${rust_linker}"
    "RUSTFLAGS=${rustflags}"
    "RUSTDOCFLAGS=${rustdocflags}"
)
if [[ -n "${selected_rustup_toolchain}" ]]; then
    rust_command_environment+=("RUSTUP_TOOLCHAIN=${selected_rustup_toolchain}")
fi

# Cargo owns the Rust graph. CMake builds only the native storage dependency.
run_logged native-build cmake --build "${build_dir}" --parallel "${jobs}" --target native_storage_source_audit
python3 - "${build_dir}" "${sanitizer}" <<'PY'
import json
from pathlib import Path
import shlex
import sys
build, sanitizer = Path(sys.argv[1]), sys.argv[2]
root = Path.cwd()
required = {line for line in (root / 'cmake/native-storage-sources.txt').read_text().splitlines() if line and not line.startswith('#')}
seen = set()
for entry in json.loads((build / 'compile_commands.json').read_text()):
    source = Path(entry['file']).resolve()
    if not source.is_relative_to(root):
        continue
    name = source.relative_to(root).as_posix()
    if name not in required:
        raise SystemExit(f'unexpected native compiler input: {name}')
    args = entry.get('arguments') or shlex.split(entry['command'])
    if f'-fsanitize={sanitizer}' not in args or any(arg.startswith('-fno-sanitize') for arg in args):
        raise SystemExit(f'native source lacks complete instrumentation: {name}')
    seen.add(name)
if seen != required:
    raise SystemExit(f'missing native instrumentation: {required - seen}')
print(f'verified all {len(seen)} native storage compilation commands')
PY

# Use the same Clang runtime for SRPC's C kernels and for the Rust executables.
# This wrapper is retained with the evidence, so the platform code is not an
# uninstrumented hole in the application tests.
native_cc="${build_dir}/sanitizer-cc"
printf '#!/usr/bin/env bash\nexec %q -fsanitize=%q -fno-omit-frame-pointer "$@"\n' \
    "${rust_linker}" "${sanitizer}" >"${native_cc}"
chmod +x "${native_cc}"
libcxx_dir="$(dirname "$(dirname "$(readlink -f "${cxx_compiler}")")")/lib"
export CC="${native_cc}" CXX="${cxx_compiler}"
export MAKO_BUILD_DIR="${build_dir}" LIBCXX_DIR="${libcxx_dir}"
export MAKO_MTREE_NATIVE_INTEGRATION=1
export MAKO_MTREE_NATIVE_LIB_DIRS="${build_dir}:${libcxx_dir}"
export MAKO_MTREE_NATIVE_LIBS="static:-bundle=mako_native_storage,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread"
export CARGO_TARGET_DIR="${build_dir}/rust-workspace-target"
export CARGO_BUILD_JOBS="${jobs}"
# Explicit --target keeps the private sysroot and instrumentation off host
# proc-macros/build scripts, even when host and target triples are identical.
rust_target="${rust_target:-${rust_sanitizer_target}}"
export CARGO_BUILD_TARGET="${rust_target}"
for setting in "${rust_command_environment[@]}"; do export "${setting}"; done
manifest="${build_dir}/rust-sto-sanitizer-manifest.txt"
{
    echo "commit=${head_sha}"
    echo "sanitizer=${sanitizer}"
    echo "native_scope=Masstree-storage-and-SRPC-platform-kernels"
    echo "rust_instrumentation=${rust_instrumentation}"
    echo "rust_std_source=${rust_std_source}"
    echo "rust_std_sysroot=${rust_std_sysroot}"
    echo "rust_runtime=${rust_runtime}"
    echo "rustflags=${rustflags}"
    echo "target=${rust_target}"
    echo "unwind_backend=${unwind_backend}"
    echo "unwind_config_sha256=${unwind_config_sha256}"
    echo "asan_options=${ASAN_OPTIONS:-}"
    echo "ubsan_options=${UBSAN_OPTIONS:-}"
    echo "tsan_options=${TSAN_OPTIONS:-}"
    echo "gate_status=running"
} >"${manifest}"
trap 'status=$?; if (( status != 0 )); then echo "gate_status=failed:${status}" >>"${manifest}"; fi' EXIT

runner=(python3 "${repo_root}/scripts/ci/run_native_sanitizer_tests.py"
    --cargo "${selected_cargo}" --manifest "${repo_root}/crates/Cargo.toml" --target "${rust_target}"
    --quarantine "${quarantine_allowlist}"
    --quarantine "${repo_root}/scripts/ci/native-adapter-quarantine-tests.txt")
if [[ "${MAKO_SANITIZER_FOCUS:-}" != cache ]]; then
    # No native environment in the transaction-manager gate. The following
    # process groups explicitly exercise Masstree and the application adapter.
    run_logged transactions env --unset=MAKO_MTREE_NATIVE_INTEGRATION \
        --unset=MAKO_MTREE_NATIVE_LIB_DIRS --unset=MAKO_MTREE_NATIVE_LIBS \
        "${runner[@]}" --package sto-core --package sto-test-datatypes --all-features \
        --report "${build_dir}/sanitizer-logs/transactions.json"
    run_logged tree "${runner[@]}" --package masstree --package sto-masstree --all-features \
        --report "${build_dir}/sanitizer-logs/tree.json"
    run_logged adapter "${runner[@]}" --package sto-tpcc-ffi --all-features \
        --report "${build_dir}/sanitizer-logs/adapter.json"
    run_logged runtime "${runner[@]}" --package mako-runtime --features replication \
        --report "${build_dir}/sanitizer-logs/runtime.json"
fi
# Retains its separate unsanitized Loom executions, exact quarantine reruns,
# two checked startup allocation roots and the unrelated-lost-buffer tripwire.
run_logged cache python3 "${repo_root}/scripts/ci/run_rust_sto_cache_tests.py" \
    --cargo "${selected_cargo}" --manifest-path "${repo_root}/crates/Cargo.toml" \
    --target "${rust_target}" --report "${build_dir}/sanitizer-logs/cache.json"

# Inspect instrumented objects and every target executable, including binaries
# Cargo built for the native process tests. Host build scripts are separate.
case "${sanitizer}" in
    address) native_hook=' U __asan_(report|load|store|init)' ;;
    thread) native_hook=' U __tsan_(read|write|func_entry|init)' ;;
    undefined) native_hook=' U __ubsan_handle_' ;;
esac
"${cmake_nm}" -u "${build_dir}/libmako_native_storage.a" >"${build_dir}/sanitizer-logs/native-hooks.txt"
grep -Eq "${native_hook}" "${build_dir}/sanitizer-logs/native-hooks.txt"
if [[ "${rust_compiler_instrumented}" == 1 ]]; then
    python3 - "${cmake_nm}" "${CARGO_TARGET_DIR}/${rust_target}" "${native_hook}" "${MAKO_SANITIZER_FOCUS:-}" <<'PY'
from pathlib import Path
import re
import subprocess
import sys
nm, target, pattern, focus = sys.argv[1:]
required = ['sto_core', 'sto_masstree', 'masstree', 'mako_engine_sto', 'mako_cache']
if focus != 'cache':
    required += ['sto_tpcc_ffi', 'mako_runtime']
for crate in required:
    archives = sorted(Path(target).glob(f'debug/deps/lib{crate}-*.rlib'))
    if not archives:
        raise SystemExit(f'missing instrumented crate: {crate}')
    for archive in archives:
        symbols = subprocess.check_output([nm, '-u', archive], text=True, stderr=subprocess.DEVNULL)
        if not re.search(pattern, symbols):
            raise SystemExit(f'no compiler sanitizer hooks in {archive}')
print('verified first-party Rust sanitizer hooks')
PY
fi
audit_one_rust_std native --tree "${CARGO_TARGET_DIR}/${rust_target}"
verify_unwind_config
final_head_sha="$("${repository_git[@]}" rev-parse --verify HEAD)"
final_tree_status="$("${repository_git[@]}" status --porcelain=v1 --untracked-files=normal --ignore-submodules=none)"
if [[ "${final_head_sha}" != "${head_sha}" || "${final_tree_status}" != "${tree_status}" ]]; then
    echo 'sanitizer evidence is stale: the checkout changed while the gate ran' >&2
    exit 2
fi
printf 'final_commit=%s\ngate_status=passed\n' "${final_head_sha}" >>"${manifest}"
echo "${sanitizer} native Rust/storage sanitizer gate passed for ${head_sha}"
