#!/usr/bin/env bash
# Regenerate the tla-rs source-first wrapper from the model sources.
# Only meaning-preserving rewrites are applied:
#   1. concatenate normal/recovery/behavior/invariants (verbatim verus! bodies)
#   2. rename State->LState, Constants->LConstants (the checker keys on these names)
#   3. strip inline `#[trigger]` attributes (proof-only hints; the parser rejects them)
#   4. add LInit/LNext/LInv delegating wrappers
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../../src"
ren() { sed -E 's/\bState\b/LState/g; s/\bConstants\b/LConstants/g; s/#\[trigger\] ?//g'; }
ren < "$src/types.rs" > "$here/types.rs"
{
echo 'use super::types::*;'
echo 'use vstd::prelude::*;'
echo 'verus! {'
for f in normal recovery behavior invariants; do
  echo "// ===== $f.rs ====="
  sed -n '/^verus! {/,/^} \/\/ verus!/p' "$src/$f.rs" | sed '1d;$d' | ren
done
cat <<'EOW'
// ===== wrappers =====
pub open spec fn LInit(s: LState, c: LConstants) -> bool { init(s, c) }
pub open spec fn LNext(s: LState, s_: LState, c: LConstants) -> bool { next(s, s_, c) }
pub open spec fn LInv(s: LState, c: LConstants) -> bool { inv(s, c) }
} // verus!
EOW
} > "$here/mako.rs"
