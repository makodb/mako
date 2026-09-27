#!/usr/bin/env bash
# Reproduce the tla-rs model-check construct probes. Each probe evaluates one
# expression as an invariant on a trivial 2-state model and prints the result.
set -u
B=/home/shuai/tmp/claude-3000/-home-users-shuai-mako/800b988f-cadf-4cf3-9746-5ada68c42ba2/scratchpad/tla-rs/transpiler/target/release/verus-transpile
d="$(mktemp -d)"
cat > "$d/t.rs" <<'EOT'
use vstd::prelude::*;
verus! {
pub struct LConstants { pub n: int }
pub struct LState { pub x: int }
pub enum Wm { Fin(int), Inf }
pub enum Entry { Log { txn: int, epoch: nat, clock: int }, Inf { epoch: nat } }
}
EOT
cat > "$d/m.toml" <<'EOT'
[constants.assignments]
n = 1
[quantifiers.int]
min = 0
max = 2
[search]
max_depth = 3
max_states = 100
timeout_ms = 10000
[properties]
check_deadlock = false
successor_semantics = "deadlock"
EOT
probe() {
cat > "$d/p.rs" <<EOT
use vstd::prelude::*;
verus! {
pub open spec fn LInit(s: LState, c: LConstants) -> bool { s == LState { x: 0 } }
pub open spec fn LNext(s: LState, s_: LState, c: LConstants) -> bool { s.x < 1 && s_ == LState { x: s.x + 1 } }
pub open spec fn rec(n: int) -> int decreases n { if n <= 0 { 0 } else { 1 + rec(n - 1) } }
pub open spec fn Inv(s: LState, c: LConstants) -> bool { $1 }
}
EOT
r=$(timeout 60 "$B" model-check --input "$d/p.rs" --types "$d/t.rs" --model "$d/m.toml" --search bfs --invariant Inv 2>&1 | tr '\n' ' ' | tr -s ' ' | grep -o 'result: [a-z_]*\|Error.*' | head -c 200)
printf '%-75s => %s\n' "$1" "$r"
}
probe "forall|i: int| 0 <= i < 3 ==> #[trigger] (i + 0) >= 0"
probe "forall|i: int| #![trigger (i + 0)] 0 <= i < 3 ==> (i + 0) >= 0"
probe "let q = seq![7int]; q.len() == 1"
probe "let q = Seq::<int>::empty(); q.len() == 0"
probe "let q = seq![7int]; q.len() as int == 1"
probe "let q = seq![1int, 2int]; q.last() == 2"
probe "let q = seq![1int, 2int]; q.drop_last() == seq![1int]"
probe "let q = seq![1int, 2int]; q.drop_first() == seq![2int]"
probe "let q = seq![1int, 2int]; q.take(1) == seq![1int]"
probe "let q = seq![1int, 2int]; q.update(0, 5) == seq![5int, 2int]"
probe "let q = seq![1int, 2int]; q.no_duplicates()"
probe "seq![1int] + seq![2int] == seq![1int, 2int]"
probe "Seq::new(2, |i: int| 0int) == seq![0int, 0int]"
probe "let q = seq![1int, 2int]; q.filter(|v: int| v > 1) == seq![2int]"
probe "let t = set![1int, 2int]; t.filter(|v: int| v > 1) == set![2int]"
probe "Set::<int>::empty().is_empty()"
probe "let m = map![1int => 2int]; m.map_entries(|k: int, v: int| v + 1) == map![1int => 3int]"
probe "let m = map![1int => 2int]; m.filter_keys(|k: int| k > 0) == m"
probe "let m = map![1int => 2int]; m.union_prefer_right(map![1int => 3int]) == map![1int => 3int]"
probe "let m = map![1int => 2int]; m.remove_keys(set![1int]) == Map::<int, int>::empty()"
probe "Map::new(set![1int], |k: int| k + 1) == map![1int => 2int]"
probe "(Entry::Log { txn: 1, epoch: 0, clock: 2 })->Log_clock == 2"
probe "(Entry::Log { txn: 1, epoch: 0, clock: 2 })->clock == 2"
probe "(Wm::Fin(3))->0 == 3"
probe "(Entry::Inf { epoch: 1 }) is Inf"
probe "rec(3) == 3"
probe "(choose|i: int| 0 <= i && i < 2 && i == 1) == 1"
rm -rf "$d"
