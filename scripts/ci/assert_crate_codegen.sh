#!/usr/bin/env bash
# Assert that a `rrr_goal0_crate_codegen` build regenerated the crate C++ AND
# that the generator reached each named output.
#
# Usage: assert_crate_codegen.sh <rrr.x.cppm> [rrr.y.cppm ...]  < captured-build-output
#
# Why this exists instead of comparing mtimes.
#
# The CI step that touches an input and re-builds used to assert
# `stat -c %Y <output>` had increased. That measures "the file was rewritten",
# which is NOT the invariant anyone wants and is only true by accident. The
# emitters deliberately skip writing byte-identical output (see the
# `unchanged <label>` path in scripts/extract_rrr_rust.py) -- rewriting
# identical files would spuriously invalidate all 38 downstream .cppm
# compilations on every emitter touch. So touching an input regenerates,
# produces identical bytes, skips the write, leaves mtime alone, and the
# mtime assertion fails even though everything behaved correctly. That is
# exactly how it failed in CI.
#
# The real invariant is "changing an input causes the generator to re-run and
# re-confirm every output". So assert on what the generator itself reports:
#   * the CMake comment, proving the codegen edge actually fired;
#   * `Done: N files transpiled, 0 errors`, proving it finished cleanly;
#   * one `<output> (module: ...)` line per output we care about, proving the
#     generator reached that specific file and emitted it.
# That is strictly more meaningful than an mtime, which anything could bump.
#
# NOTE: the `wrote`/`unchanged`/`validated` dispositions belong to
# scripts/extract_rrr_rust.py, which owns `src/rrr/src/*.rs`. The crate codegen
# checked here is the transpiler's crate mode, which owns
# `goal0-crate-cpp/*.cppm` and reports per output with the mapping line above.
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <generated-basename>... < build-output" >&2
    exit 2
fi

output="$(cat)"

if ! grep -Fq 'Generating Goal-0 rrr crate C++ child modules' <<<"${output}"; then
    echo "crate generation did not re-run" >&2
    exit 1
fi

if ! grep -Eq 'Done: [0-9]+ files transpiled, 0 errors' <<<"${output}"; then
    echo "crate generation did not finish cleanly" >&2
    exit 1
fi

for generated in "$@"; do
    # Deliberately not matching the U+2192 arrow the generator prints, so this
    # does not depend on the container's locale handling of non-ASCII.
    if ! grep -Fq "${generated} (module: " <<<"${output}"; then
        echo "generator did not report ${generated}" >&2
        exit 1
    fi
done
