#!/usr/bin/env bash

set -euo pipefail

# The RocksDB the thread-sanitizer gate links and loads
# (scripts/ci/run_rust_sto_sanitizer.sh thread). The pin is the upstream
# release of the version the CI image installs as librocksdb-dev: Mako is
# compiled against the image's headers, so the rebuilt library must be that
# version. This script refuses to run when the image and the pin disagree;
# to follow a new image, change rocksdb_version and rocksdb_commit together.
readonly rocksdb_version="8.9.1"
# The commit upstream tag v8.9.1 points at.
readonly rocksdb_commit="49ce8a1064dd1ad89117899839bf136365e49e79"
readonly rocksdb_repository="https://github.com/facebook/rocksdb.git"
readonly rocksdb_system_package="librocksdb-dev"
# The compiler's default C++ standard library on purpose (libstdc++ on
# Linux, as in the system package): Mako reaches RocksDB only through its C
# API. PORTABLE keeps a cached build usable on another runner's CPU.
# WITH_LIBURING is pinned off, which is what the image without liburing-dev
# gives the default anyway.
readonly -a rocksdb_cmake_arguments=(
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DWITH_TSAN=ON
    -DROCKSDB_BUILD_SHARED=ON
    -DWITH_TESTS=OFF
    -DWITH_BENCHMARK_TOOLS=OFF
    -DWITH_TOOLS=OFF
    -DWITH_CORE_TOOLS=OFF
    -DWITH_GFLAGS=OFF
    -DWITH_SNAPPY=ON
    -DWITH_LZ4=ON
    -DWITH_ZLIB=ON
    -DWITH_ZSTD=ON
    -DWITH_BZ2=ON
    -DWITH_LIBURING=OFF
    -DFAIL_ON_WARNINGS=OFF
    -DPORTABLE=1
    -DUSE_RTTI=1
)

usage() {
    cat <<'EOF'
Usage: bash scripts/ci/build_tsan_rocksdb.sh --prefix DIR [options]
       bash scripts/ci/build_tsan_rocksdb.sh --print-cache-key [--cc CC] [--cxx CXX]

Build RocksDB with -fsanitize=thread as a shared library into DIR/lib, or
reuse the one already there. DIR/tsan-rocksdb-stamp.txt records what was
built: the pinned commit and version, the CMake arguments, the compiler
banners, the machine and this script's hash. A directory is reused only when
its stamp and library hash match, the library carries the thread-sanitizer
hooks and every library it depends on resolves in this image; otherwise it is
rebuilt. Any edit to this script changes the identity.

The image's RocksDB must be the pinned version: the version macros of the
<rocksdb/version.h> the compiler finds, and the installed librocksdb-dev
package where dpkg-query knows one, are compared with the pin.

Options:
  --prefix DIR      Absolute directory to build into or reuse. It may hold
                    only this script's own output. Nothing locks it: one
                    run per directory at a time.
  --cc CC           C compiler. Default: clang-22.
  --cxx CXX         C++ compiler. Default: clang++-22.
  --nm NM           Symbol inspector. Default: nm.
  --readelf READELF ELF inspector. Default: readelf.
  --jobs N          Parallel build jobs. Default: MAKO_SANITIZER_JOBS, or 4.
  --source DIR      An existing checkout of the pinned commit, used instead
                    of fetching it from GitHub (three attempts, each given
                    up when it stalls). It is not modified.
  --report FILE     Also write the key=value facts printed at the end to FILE.
  --print-cache-key Print the cache key for this recipe and compiler, then
                    exit. Nothing is built.
EOF
}

fail() {
    echo "build_tsan_rocksdb: $*" >&2
    exit 1
}

prefix=""
cc="clang-22"
cxx="clang++-22"
nm="nm"
readelf="readelf"
jobs="${MAKO_SANITIZER_JOBS:-4}"
source_dir=""
report=""
print_cache_key=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix|--cc|--cxx|--nm|--readelf|--jobs|--source|--report)
            if [[ $# -lt 2 || -z "$2" ]]; then
                echo "$1 requires a value" >&2
                usage >&2
                exit 2
            fi
            case "$1" in
                --prefix) prefix="$2" ;;
                --cc) cc="$2" ;;
                --cxx) cxx="$2" ;;
                --nm) nm="$2" ;;
                --readelf) readelf="$2" ;;
                --jobs) jobs="$2" ;;
                --source) source_dir="$2" ;;
                --report) report="$2" ;;
            esac
            shift 2
            ;;
        --print-cache-key)
            print_cache_key=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$(uname -s)" != "Linux" ]]; then
    fail "the instrumented RocksDB is built for Linux only"
fi
for command_name in "${cc}" "${cxx}" sha256sum; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        fail "required command is missing: ${command_name}"
    fi
done
cc="$(command -v "${cc}")"
cxx="$(command -v "${cxx}")"

compiler_banner() {
    # The version and target lines; the installation directory is not part
    # of what a compiler produces.
    local banner
    if ! banner="$("$1" --version 2>/dev/null)"; then
        fail "could not run $1 --version"
    fi
    printf '%s\n' "${banner}" |
        awk 'NR <= 2 { printf "%s%s", (NR > 1 ? "; " : ""), $0 } END { print "" }'
}
c_banner="$(compiler_banner "${cc}")"
cxx_banner="$(compiler_banner "${cxx}")"
if [[ "${c_banner}" != *"clang version"* || "${cxx_banner}" != *"clang version"* ]]; then
    fail "the compilers do not identify as Clang: ${c_banner} / ${cxx_banner}"
fi
script_sha256="$(sha256sum "${BASH_SOURCE[0]}")"
script_sha256="${script_sha256%% *}"

identity_text() {
    printf '%s\n' \
        "format=mako-tsan-rocksdb-1" \
        "repository=${rocksdb_repository}" \
        "commit=${rocksdb_commit}" \
        "version=${rocksdb_version}" \
        "cmake_arguments=${rocksdb_cmake_arguments[*]}" \
        "c_compiler=${c_banner}" \
        "cxx_compiler=${cxx_banner}" \
        "machine=$(uname -m)" \
        "script_sha256=${script_sha256}"
}
identity_sha256="$(identity_text | sha256sum)"
identity_sha256="${identity_sha256%% *}"

if [[ "${print_cache_key}" == "1" ]]; then
    printf 'mako-tsan-rocksdb-%s-%s\n' "${rocksdb_version}" "${identity_sha256}"
    exit 0
fi

if [[ -z "${prefix}" ]]; then
    usage >&2
    exit 2
fi
# A trailing slash would give the prefix's own lib directory a spelling that
# the foreign-content check below does not recognise.
while [[ "${prefix}" == */ && "${prefix}" != "/" ]]; do
    prefix="${prefix%/}"
done
# The directory is spliced into -L, -Wl,-rpath,<dir> and RUSTFLAGS.
if [[ ! "${prefix}" =~ ^/[A-Za-z0-9._/+@=-]+$ || "${prefix}" == *//* ]]; then
    fail "--prefix must be an absolute path of [A-Za-z0-9._/+@=-] characters with no empty component: ${prefix}"
fi
if [[ ! "${jobs}" =~ ^[1-9][0-9]*$ ]]; then
    fail "--jobs must be a positive integer, got: ${jobs}"
fi
for command_name in "${nm}" "${readelf}" cmake git ldd ninja realpath; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        fail "required command is missing: ${command_name}"
    fi
done

lib_dir="${prefix}/lib"
stamp="${prefix}/tsan-rocksdb-stamp.txt"
work="$(mktemp -d "${TMPDIR:-/tmp}/mako-tsan-rocksdb.XXXXXX")"
trap 'rm -rf "${work}"' EXIT

soname_of() {
    "${readelf}" -dW "$1" | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p'
}

# The image's RocksDB, as the compiler resolves it, must be the pinned
# version. Mako compiles against these headers and would otherwise call a
# library of another version.
if ! header_version="$(
        printf '%s\n' '#include <rocksdb/version.h>' \
            'mako_tsan_rocksdb_version ROCKSDB_MAJOR.ROCKSDB_MINOR.ROCKSDB_PATCH' |
            "${cxx}" -E -P -x c++ - 2>"${work}/header-version.stderr" |
            sed -n 's/^mako_tsan_rocksdb_version //p' | tr -d ' ')"; then
    cat "${work}/header-version.stderr" >&2
    fail "${cxx} could not preprocess <rocksdb/version.h>; is ${rocksdb_system_package} installed?"
fi
if [[ "${header_version}" != "${rocksdb_version}" ]]; then
    fail "the installed RocksDB headers are version ${header_version:-unknown}, but this script rebuilds ${rocksdb_version} (${rocksdb_commit}); change rocksdb_version and rocksdb_commit together"
fi
package_version="unavailable"
if command -v dpkg-query >/dev/null 2>&1 &&
   package_state="$(dpkg-query -W -f='${db:Status-Status} ${Version}' \
       "${rocksdb_system_package}" 2>/dev/null)" &&
   [[ "${package_state}" == "installed "* ]]; then
    package_version="${package_state#installed }"
    package_upstream="${package_version#*:}"
    if [[ "${package_upstream}" != "${rocksdb_version}" &&
          "${package_upstream}" != "${rocksdb_version}"[-+~]* ]]; then
        fail "${rocksdb_system_package} ${package_version} is installed, but this script rebuilds ${rocksdb_version} (${rocksdb_commit}); change rocksdb_version and rocksdb_commit together"
    fi
fi
system_library="not-found"
system_soname="not-found"
system_candidate="$("${cc}" --print-file-name=librocksdb.so)"
if [[ "${system_candidate}" == /* && -e "${system_candidate}" ]]; then
    system_library="$(realpath -e "${system_candidate}")"
    system_soname="$(soname_of "${system_library}")"
fi
echo "image RocksDB: headers ${header_version}, ${rocksdb_system_package} ${package_version}, library ${system_library} (${system_soname})"

soname=""
library_name=""
library_sha256=""
tsan_symbols=""

# Why the directory cannot be used as it is; set by the two checks below.
reason=""

# What the gate needs from the library, whether it was just built or reused.
inspect_library() {
    local library="${lib_dir}/${library_name}"
    local link
    local loader_report
    if [[ -L "${library}" || ! -f "${library}" ]]; then
        reason="${library} is not a regular file"
        return 1
    fi
    for link in "${lib_dir}/librocksdb.so" "${lib_dir}/${soname}"; do
        if [[ "$(realpath -e "${link}" 2>/dev/null)" != "$(realpath -e "${library}")" ]]; then
            reason="${link} does not resolve to ${library}"
            return 1
        fi
    done
    if [[ -z "${soname}" || "$(soname_of "${library}")" != "${soname}" ]]; then
        reason="${library} does not carry the recorded SONAME ${soname:-(none)}"
        return 1
    fi
    if ! "${nm}" -D --undefined-only "${library}" >"${work}/undefined-symbols.txt" ||
       ! "${nm}" -D --defined-only "${library}" >"${work}/defined-symbols.txt"; then
        reason="${nm} could not read ${library}"
        return 1
    fi
    if ! grep -Eq ' U __tsan_(read|write|func_entry)' "${work}/undefined-symbols.txt"; then
        reason="${library} has no thread-sanitizer compiler hooks"
        return 1
    fi
    if grep -Eq ' [TW] __tsan_init$' "${work}/defined-symbols.txt"; then
        reason="${library} bundles a thread-sanitizer runtime of its own"
        return 1
    fi
    if ! grep -Eq ' T rocksdb_open$' "${work}/defined-symbols.txt"; then
        reason="${library} does not export the RocksDB C API"
        return 1
    fi
    # The identity names the recipe and the compiler, not the rest of the
    # image. A directory restored into an image whose snappy, lz4, zstd, bz2,
    # zlib or libstdc++ no longer satisfies the library cannot be loaded:
    # ask the loader, and rebuild on a missing library or symbol version.
    if ! loader_report="$(ldd "${library}" 2>&1)"; then
        reason="ldd could not read ${library}: $(
            tr -s '[:space:]' ' ' <<<"${loader_report}" | sed 's/^ //; s/ $//')"
        return 1
    fi
    if grep -Fq 'not found' <<<"${loader_report}"; then
        reason="${library} has dependencies this image does not satisfy: $(
            grep -F 'not found' <<<"${loader_report}" |
                tr -s '[:space:]' ' ' | sed 's/^ //; s/ $//')"
        return 1
    fi
    tsan_symbols="$(grep -c ' U __tsan_' "${work}/undefined-symbols.txt")"
}

reusable() {
    local actual_sha256
    if [[ ! -f "${stamp}" ]]; then
        reason="no stamp at ${stamp}"
        return 1
    fi
    if [[ "$(sed -n 's/^identity_sha256=//p' "${stamp}")" != "${identity_sha256}" ]]; then
        reason="the stamp records another recipe or compiler"
        return 1
    fi
    soname="$(sed -n 's/^soname=//p' "${stamp}")"
    library_name="$(sed -n 's/^library=//p' "${stamp}")"
    library_sha256="$(sed -n 's/^library_sha256=//p' "${stamp}")"
    if [[ ! "${library_name}" =~ ^librocksdb\.so\.[0-9.]+$ || -z "${library_sha256}" ]]; then
        reason="the stamp is incomplete"
        return 1
    fi
    if [[ ! -f "${lib_dir}/${library_name}" ]]; then
        reason="${lib_dir}/${library_name} is missing"
        return 1
    fi
    actual_sha256="$(sha256sum "${lib_dir}/${library_name}")"
    if [[ "${actual_sha256%% *}" != "${library_sha256}" ]]; then
        reason="${lib_dir}/${library_name} does not match the stamp's hash"
        return 1
    fi
    inspect_library
}

build_library() {
    local source_tree
    local build_tree="${work}/build"
    local actual_commit
    local attempt
    local fetched=0
    local source_version
    local -a built
    local -a source_git

    if [[ -n "${source_dir}" ]]; then
        if ! source_tree="$(cd "${source_dir}" 2>/dev/null && pwd -P)"; then
            fail "--source is not a directory: ${source_dir}"
        fi
        source_git=(git -c "safe.directory=${source_tree}" -C "${source_tree}")
        if [[ -n "$("${source_git[@]}" status --porcelain=v1 --untracked-files=no)" ]]; then
            fail "--source has tracked changes: ${source_tree}"
        fi
    else
        source_tree="${work}/src"
        source_git=(git -C "${source_tree}")
        git -c init.defaultBranch=main init --quiet "${source_tree}"
        # A connection slower than 1000 bytes a second for 60 seconds is
        # given up, so a stalled fetch cannot hold the job until its
        # timeout; a transient failure gets two more attempts.
        for attempt in 1 2 3; do
            if "${source_git[@]}" \
                    -c http.lowSpeedLimit=1000 -c http.lowSpeedTime=60 \
                    fetch --quiet --depth 1 \
                    "${rocksdb_repository}" "${rocksdb_commit}"; then
                fetched=1
                break
            fi
            echo "build_tsan_rocksdb: fetch attempt ${attempt} of 3 failed" >&2
            if [[ "${attempt}" -lt 3 ]]; then
                sleep "$((attempt * 10))"
            fi
        done
        if [[ "${fetched}" != "1" ]]; then
            fail "could not fetch ${rocksdb_commit} from ${rocksdb_repository}"
        fi
        "${source_git[@]}" -c advice.detachedHead=false \
            checkout --quiet --detach FETCH_HEAD
    fi
    actual_commit="$("${source_git[@]}" rev-parse --verify HEAD)"
    if [[ "${actual_commit}" != "${rocksdb_commit}" ]]; then
        fail "the RocksDB source is at ${actual_commit}, expected ${rocksdb_commit}"
    fi
    source_version="$(sed -n \
        's/^#define ROCKSDB_\(MAJOR\|MINOR\|PATCH\) \([0-9][0-9]*\)$/\2/p' \
        "${source_tree}/include/rocksdb/version.h" | paste -sd.)"
    if [[ "${source_version}" != "${rocksdb_version}" ]]; then
        fail "${rocksdb_commit} is RocksDB ${source_version:-unknown}, not ${rocksdb_version}"
    fi

    # Only this script's own output is ever removed from the prefix.
    rm -f "${stamp}" "${lib_dir}"/librocksdb.so*
    env -u CC -u CXX -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
        cmake -S "${source_tree}" -B "${build_tree}" -G Ninja \
            "-DCMAKE_C_COMPILER=${cc}" "-DCMAKE_CXX_COMPILER=${cxx}" \
            "${rocksdb_cmake_arguments[@]}"
    env -u CC -u CXX -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
        cmake --build "${build_tree}" --parallel "${jobs}" --target rocksdb-shared

    mapfile -t built < <(find "${build_tree}" -maxdepth 1 -type f -name 'librocksdb.so.*')
    if [[ "${#built[@]}" -ne 1 ]]; then
        fail "expected one built librocksdb.so.<version>, found ${#built[@]}"
    fi
    library_name="$(basename "${built[0]}")"
    soname="$(soname_of "${built[0]}")"
    if [[ -z "${soname}" ]]; then
        fail "${built[0]} has no SONAME"
    fi
    # The symbolic links (librocksdb.so, the SONAME) and the library itself.
    # `cmake --install` is not usable: it also wants the static archive.
    mkdir -p "${lib_dir}"
    cp -a "${build_tree}"/librocksdb.so* "${lib_dir}/"
    library_sha256="$(sha256sum "${lib_dir}/${library_name}")"
    library_sha256="${library_sha256%% *}"
    if ! inspect_library; then
        fail "the built library is not usable by the thread gate: ${reason}"
    fi
    {
        identity_text
        echo "identity_sha256=${identity_sha256}"
        echo "soname=${soname}"
        echo "library=${library_name}"
        echo "library_sha256=${library_sha256}"
    } >"${stamp}"
}

mkdir -p "${prefix}"
foreign="$(find "${prefix}" -mindepth 1 \
    ! -path "${lib_dir}" ! -path "${lib_dir}/librocksdb.so*" ! -path "${stamp}" \
    -print -quit)"
if [[ -n "${foreign}" ]]; then
    fail "--prefix holds something this script did not write (${foreign}); choose an empty or new directory"
fi

if reusable; then
    provenance="reused"
    echo "reusing the instrumented RocksDB in ${prefix}"
else
    echo "building the instrumented RocksDB into ${prefix}: ${reason}"
    build_library
    provenance="built"
fi

facts="$(printf '%s\n' \
    "tsan_rocksdb_provenance=${provenance}" \
    "tsan_rocksdb_version=${rocksdb_version}" \
    "tsan_rocksdb_commit=${rocksdb_commit}" \
    "tsan_rocksdb_cmake_arguments=${rocksdb_cmake_arguments[*]}" \
    "tsan_rocksdb_compiler=${cxx_banner}" \
    "tsan_rocksdb_identity_sha256=${identity_sha256}" \
    "tsan_rocksdb_soname=${soname}" \
    "tsan_rocksdb_tsan_symbols=${tsan_symbols}" \
    "system_rocksdb_header_version=${header_version}" \
    "system_rocksdb_package=${rocksdb_system_package} ${package_version}" \
    "system_rocksdb_library=${system_library}" \
    "system_rocksdb_soname=${system_soname}")"
printf '%s\n' "${facts}"
if [[ -n "${report}" ]]; then
    printf '%s\n' "${facts}" >"${report}"
fi
