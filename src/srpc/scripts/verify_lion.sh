#!/usr/bin/env bash
#
# Re-verify the pinned Lion crates by running Lion's own CI (`./ci.sh`).
#
# SRPC's claim about Lion is identity with Lion@pin (docs/dev/lion-runtime-plan.md,
# D2): the Rust lane compiles the third-party/lion gitlink's bytes unmodified, and
# Lion's CI is the proof of record. This lane re-runs that CI on exactly those
# bytes. It is optional and non-gating, like scripts/verify_srpc.sh: nothing in
# CMake, ctest or the source gate calls it.
#
# It verifies the commit the gitlink records, not the submodule's working tree:
# the tree is extracted with `git archive` into $VERIFY_LION_DIR/<sha>/, so a
# dirty or re-checked-out submodule cannot change what is verified. Lion's
# ci.sh then writes its build output next to that copy, never into the
# submodule. VERIFY_LION_DIR defaults to ${XDG_CACHE_HOME:-~/.cache}/srpc/verify-lion
# and must lie outside this checkout: Cargo looks upward for a workspace, and
# a Lion crate found under SRPC's root (target/ included) is rejected as a
# package that "believes it's in a workspace when it's not". The extraction is
# kept, so a second run reuses Lion's build output.
#
# Requirements (Lion's REQUIREMENTS.md pins both; Verus refuses other versions):
#   - a Verus 0.2025.11.15.db81a74 dist (bundles Z3 4.12.5; the directory that
#     holds `cargo-verus`, `verus` and `version.txt`), given as VERUS_PATH
#     (Lion's name) or VERUS_HOME (verify_srpc.sh's name);
#   - the Rust 1.91.0 toolchain installed through rustup (Verus runs it itself;
#     SRPC's own toolchain is unaffected);
#   - the third-party/lion submodule initialized, with the gitlink commit present;
#   - Cargo able to resolve Lion's full dependency set: ci.sh also verifies
#     lion-utility and lion-liveness, which pull mio, tokio, socket2 and more
#     from crates.io (SRPC's own graph excludes them). Run once with network
#     access, or with a warm Cargo registry.
#
# A cold run takes about nine minutes.
#
# Usage:  VERUS_PATH=/path/to/verus-x86-linux [VERIFY_LION_DIR=/scratch] scripts/verify_lion.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPECTED_VERUS_VERSION="0.2025.11.15.db81a74"
EXPECTED_RUST_TOOLCHAIN="1.91.0"

verus_path="${VERUS_PATH:-${VERUS_HOME:-}}"
if [ -z "$verus_path" ]; then
    echo "verify_lion: set VERUS_PATH (or VERUS_HOME) to a Verus $EXPECTED_VERUS_VERSION dist" >&2
    exit 2
fi
verus_path="$(cd "$verus_path" && pwd)"
if [ ! -x "$verus_path/cargo-verus" ]; then
    echo "verify_lion: no cargo-verus in $verus_path" >&2
    exit 2
fi
verus_version="$(tr -d '[:space:]' < "$verus_path/version.txt" 2>/dev/null || true)"
if [ "$verus_version" != "$EXPECTED_VERUS_VERSION" ]; then
    echo "verify_lion: Verus at $verus_path is '${verus_version:-unknown}'," \
         "Lion@pin needs $EXPECTED_VERUS_VERSION" >&2
    exit 2
fi
if ! rustup toolchain list | grep -q "^${EXPECTED_RUST_TOOLCHAIN}-"; then
    echo "verify_lion: Verus $EXPECTED_VERUS_VERSION needs the Rust $EXPECTED_RUST_TOOLCHAIN" \
         "toolchain (rustup toolchain install $EXPECTED_RUST_TOOLCHAIN)" >&2
    exit 2
fi

# The commit SRPC pins: the gitlink in the index, not the submodule's HEAD.
read -r mode sha _ < <(git -C "$HERE" ls-files --stage -- third-party/lion)
if [ "${mode:-}" != "160000" ] || [ -z "${sha:-}" ]; then
    echo "verify_lion: third-party/lion is not a gitlink" >&2
    exit 2
fi
if ! git -C "$HERE/third-party/lion" cat-file -e "$sha^{commit}" 2>/dev/null; then
    echo "verify_lion: Lion commit $sha is not in third-party/lion" \
         "(git submodule update --init third-party/lion)" >&2
    exit 2
fi

work="${VERIFY_LION_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/srpc/verify-lion}"
mkdir -p "$work"
work="$(cd "$work" && pwd)"
case "$work/" in
    "$HERE/"*)
        echo "verify_lion: VERIFY_LION_DIR ($work) must lie outside $HERE" >&2
        exit 2
        ;;
esac
tree="$work/$sha"
if [ ! -f "$tree/.extracted" ]; then
    rm -rf "$tree" "$tree.partial"
    mkdir -p "$tree.partial"
    git -C "$HERE/third-party/lion" archive --format=tar "$sha" | tar -x -C "$tree.partial"
    touch "$tree.partial/.extracted"
    mv "$tree.partial" "$tree"
fi
printf 'VERUS_PATH=%s\n' "$verus_path" > "$tree/verus.config"

echo "verify_lion: Lion $sha, Verus $verus_version, in $tree"
start=$(date +%s)
status=0
(cd "$tree" && ./ci.sh) || status=$?
echo "verify_lion: ci.sh exited $status after $(( $(date +%s) - start ))s"
exit "$status"
