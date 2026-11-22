//! Successful OCC over the corrected placement protocol. Resolve is the explicit
//! atomic transaction-engine boundary, not a claim about distributed RPC atomicity.
//! The serial completion contains exactly actual commits, including lost replies.
use vstd::prelude::*;
use super::sharding_placement as p;

verus! {

pub enum Op { Read, Put { value: int }, Delete, Add { delta: int } }
pub enum Outcome { Open, Aborted, Committed { index: nat } }
pub enum Result { Aborted, Committed { reads: Map<int, Option<int>> } }
pub struct Record {
    pub body: Map<int, Op>,
    pub reads: Map<int, p::Cell>,
    pub invoked: nat,
    pub outcome: Outcome,
    pub replied: Option<nat>,
}
pub struct Commit {
    pub txn: int,
    pub before: Map<int, p::Cell>,
    pub after: Map<int, p::Cell>,
    pub tick: nat,
}
pub struct State {
    pub placement: p::State,
    pub txns: Map<int, Record>,
    pub order: Seq<Commit>,
    pub tick: nat,
}
pub enum Action {
    Begin { txn: int, body: Map<int, Op> },
    Fetch { txn: int, key: int },
    Commit { txn: int },
    Abort { txn: int },
    Reply { txn: int },
    Retry { txn: int },
    Placement { action: p::Action },
}

pub open spec fn needs_read(op: Op) -> bool { op is Read || op is Add }
pub open spec fn number(value: Option<int>) -> int {
    match value { Some(v) => v, None => 0 }
}
pub open spec fn read_keys(body: Map<int, Op>) -> Set<int> {
    body.dom().filter(|k: int| needs_read(body[k]))
}
pub open spec fn writes(body: Map<int, Op>, reads: Map<int, p::Cell>) -> Map<int, Option<int>> {
    Map::new(body.dom().filter(|k: int| !(body[k] is Read)), |k: int|
        match body[k] {
            Op::Put { value } => Some(value),
            Op::Delete => None,
            Op::Add { delta } => Some(number(reads[k].value) + delta),
            Op::Read => None,
        })
}
pub open spec fn open(s: State, txn: int) -> bool {
    s.txns.dom().contains(txn) && s.txns[txn].outcome is Open
}
pub open spec fn validated(s: State, txn: int) -> bool {
    let r = s.txns[txn];
    &&& r.reads.dom() == read_keys(r.body)
    &&& forall|k: int| r.reads.dom().contains(k) ==>
        p::read(s.placement, txn, k) == Some(#[trigger] r.reads[k])
}
pub open spec fn initial(c: p::Constants) -> State {
    State { placement: p::initial(c), txns: Map::empty(), order: Seq::empty(), tick: 0 }
}
pub open spec fn enabled(c: p::Constants, s: State, a: Action) -> bool {
    match a {
        Action::Begin { txn, body } => !s.txns.dom().contains(txn)
            && body.dom().subset_of(c.keys)
            && p::enabled(c, s.placement, p::Action::Open { txn }),
        Action::Fetch { txn, key } => open(s, txn)
            && read_keys(s.txns[txn].body).contains(key)
            && !s.txns[txn].reads.dom().contains(key)
            && p::read(s.placement, txn, key).is_some(),
        Action::Commit { txn } => open(s, txn)
            && s.txns[txn].body.dom().subset_of(s.placement.sessions[txn].held.dom())
            && validated(s, txn)
            && p::can_resolve(s.placement, txn, writes(s.txns[txn].body, s.txns[txn].reads)),
        Action::Abort { txn } => open(s, txn)
            && p::can_resolve(s.placement, txn, Map::empty()),
        Action::Reply { txn } => s.txns.dom().contains(txn)
            && !(s.txns[txn].outcome is Open) && s.txns[txn].replied.is_none(),
        Action::Retry { txn } => s.txns.dom().contains(txn),
        Action::Placement { action } => p::maintenance(action) && p::enabled(c, s.placement, action),
    }
}
// Rejected, duplicate and stale requests are explicit stuttering steps. A Retry
// returns the retained record; it never opens a new placement session.
pub open spec fn apply(c: p::Constants, s: State, a: Action) -> State {
    if !enabled(c, s, a) { s } else {
        let tick = s.tick + 1;
        match a {
            Action::Begin { txn, body } => State {
                placement: p::apply(c, s.placement, p::Action::Open { txn }),
                txns: s.txns.insert(txn, Record { body, reads: Map::empty(), invoked: tick,
                    outcome: Outcome::Open, replied: None }), tick, ..s
            },
            Action::Fetch { txn, key } => State { txns: s.txns.insert(txn, Record {
                reads: s.txns[txn].reads.insert(key, p::read(s.placement, txn, key).unwrap()),
                ..s.txns[txn] }), tick, ..s },
            Action::Commit { txn } => {
                let w = writes(s.txns[txn].body, s.txns[txn].reads);
                let placement = p::apply(c, s.placement, p::Action::Resolve { txn, writes: w });
                State { placement,
                    txns: s.txns.insert(txn, Record { outcome: Outcome::Committed { index: s.order.len() },
                        ..s.txns[txn] }),
                    order: s.order.push(Commit { txn, before: s.placement.logical,
                        after: placement.logical, tick }), tick }
            },
            Action::Abort { txn } => State {
                placement: p::apply(c, s.placement, p::Action::Resolve { txn, writes: Map::empty() }),
                txns: s.txns.insert(txn, Record { outcome: Outcome::Aborted, ..s.txns[txn] }), tick, ..s
            },
            Action::Reply { txn } => State { txns: s.txns.insert(txn,
                Record { replied: Some(tick), ..s.txns[txn] }), tick, ..s },
            Action::Retry { txn } => State { tick, ..s },
            Action::Placement { action } => State { placement: p::apply(c, s.placement, action), tick, ..s },
        }
    }
}

pub open spec fn record_ok(c: p::Constants, s: State, txn: int) -> bool {
    let r = s.txns[txn];
    &&& txn >= 0
    &&& r.body.dom().subset_of(c.keys)
    &&& r.reads.dom().subset_of(read_keys(r.body))
    &&& 0 < r.invoked <= s.tick
    &&& (r.outcome is Open <==> !s.placement.sessions[txn].resolved)
    &&& (r.outcome is Open ==> r.replied.is_none())
    &&& (r.replied.is_some() ==> r.invoked <= r.replied.unwrap() <= s.tick)
    &&& match r.outcome {
        Outcome::Committed { index } => index < s.order.len() && s.order[index as int].txn == txn
            && (r.replied.is_some() ==> s.order[index as int].tick < r.replied.unwrap()),
        _ => true,
    }
}
pub open spec fn entry_ok(c: p::Constants, s: State, i: int) -> bool {
    let e = s.order[i];
    let r = s.txns[e.txn];
    &&& s.txns.dom().contains(e.txn)
    &&& r.outcome == (Outcome::Committed { index: i as nat })
    &&& r.invoked < e.tick <= s.tick
    &&& e.before.dom() == c.keys && e.after.dom() == c.keys
    &&& r.reads.dom() == read_keys(r.body)
    &&& forall|k: int| r.reads.dom().contains(k) ==> #[trigger] r.reads[k] == e.before[k]
    &&& e.after == p::write_logical(e.before, e.txn, writes(r.body, r.reads))
    &&& (i == 0 ==> e.before == p::initial(c).logical)
    &&& (i > 0 ==> e.before == s.order[i - 1].after && s.order[i - 1].tick < e.tick)
}
pub open spec fn inv(c: p::Constants, s: State) -> bool {
    &&& p::inv(c, s.placement)
    &&& s.txns.dom() == s.placement.sessions.dom()
    &&& forall|txn: int| s.txns.dom().contains(txn) ==> #[trigger] record_ok(c, s, txn)
    &&& forall|i: int| 0 <= i < s.order.len() ==> #[trigger] entry_ok(c, s, i)
    &&& (s.order.len() == 0 ==> s.placement.logical == p::initial(c).logical)
    &&& (s.order.len() > 0 ==> s.placement.logical == s.order.last().after)
}

pub proof fn lemma_init(c: p::Constants)
    requires p::constants_ok(c)
    ensures inv(c, initial(c))
{
    p::lemma_init(c);
}

pub proof fn lemma_validated(c: p::Constants, s: State, txn: int)
    requires inv(c, s), s.txns.dom().contains(txn), validated(s, txn)
    ensures forall|k: int| s.txns[txn].reads.dom().contains(k) ==>
        #[trigger] s.txns[txn].reads[k] == s.placement.logical[k]
{
    assert forall|k: int| s.txns[txn].reads.dom().contains(k) implies
        #[trigger] s.txns[txn].reads[k] == s.placement.logical[k] by {
        assert(p::read(s.placement, txn, k) == Some(s.txns[txn].reads[k]));
        p::lemma_read_matches_logical(c, s.placement, txn, k);
    }
}

pub proof fn lemma_step(c: p::Constants, s: State, a: Action)
    requires inv(c, s)
    ensures inv(c, apply(c, s, a)), s.tick <= apply(c, s, a).tick
{
    hide(p::apply);
    if enabled(c, s, a) {
        let t = apply(c, s, a);
        match a {
            Action::Begin { txn, body } => {
                p::lemma_step(c, s.placement, p::Action::Open { txn });
                p::lemma_open_frame(c, s.placement, txn);
            },
            Action::Commit { txn } => {
                lemma_validated(c, s, txn);
                let w = writes(s.txns[txn].body, s.txns[txn].reads);
                p::lemma_step(c, s.placement, p::Action::Resolve { txn, writes: w });
                p::lemma_resolve_frame(c, s.placement, txn, w);
            },
            Action::Abort { txn } => {
                p::lemma_step(c, s.placement, p::Action::Resolve { txn, writes: Map::empty() });
                p::lemma_resolve_frame(c, s.placement, txn, Map::empty());
                assert(p::write_logical(s.placement.logical, txn, Map::empty()) =~= s.placement.logical);
            },
            Action::Placement { action } => {
                p::lemma_step(c, s.placement, action);
                p::lemma_maintenance_frame(c, s.placement, action);
            },
            _ => {},
        }
        assert forall|id: int| t.txns.dom().contains(id) implies #[trigger] record_ok(c, t, id) by {
            if s.txns.dom().contains(id) { assert(record_ok(c, s, id)); }
            match a {
                Action::Begin { txn, body } => { assert(record_ok(c, t, id)); },
                Action::Commit { txn } => {
                    if id != txn {
                        if let Outcome::Committed { index } = s.txns[id].outcome {
                            assert(t.order[index as int] == s.order[index as int]);
                        }
                    }
                    assert(record_ok(c, t, id));
                },
                Action::Fetch { txn, key } => { assert(record_ok(c, t, id)); },
                Action::Abort { txn } => { assert(record_ok(c, t, id)); },
                Action::Reply { txn } => {
                    if let Outcome::Committed { index } = s.txns[id].outcome {
                        assert(entry_ok(c, s, index as int));
                    }
                    assert(record_ok(c, t, id));
                },
                Action::Retry { txn } => { assert(record_ok(c, t, id)); },
                Action::Placement { action } => { assert(record_ok(c, t, id)); },
            }
        }
        assert forall|i: int| 0 <= i < t.order.len() implies #[trigger] entry_ok(c, t, i) by {
            if i < s.order.len() {
                assert(entry_ok(c, s, i));
                let id = s.order[i].txn;
                assert(record_ok(c, s, id));
                match a {
                    Action::Begin { txn, body } => { assert(id != txn); },
                    Action::Fetch { txn, key } => { assert(id != txn); },
                    Action::Commit { txn } => { assert(id != txn); },
                    Action::Abort { txn } => { assert(id != txn); },
                    _ => {},
                }
                assert(entry_ok(c, t, i));
            } else {
                match a {
                    Action::Commit { txn } => {
                        assert(i == s.order.len());
                        assert(record_ok(c, s, txn));
                        if i > 0 {
                            assert(s.order[i - 1] == s.order.last());
                            assert(entry_ok(c, s, i - 1));
                        }
                        assert(entry_ok(c, t, i));
                    },
                    _ => {},
                }
            }
        }
        if t.order.len() > 0 {
            match a {
                Action::Commit { txn } => {},
                _ => { assert(t.order.last() == s.order.last()); },
            }
        }
        assert(p::inv(c, t.placement));
        assert(t.txns.dom() == t.placement.sessions.dom());
        assert(t.order.len() == 0 ==> t.placement.logical == p::initial(c).logical);
        assert(t.order.len() > 0 ==> t.placement.logical == t.order.last().after);
    }
}

// A sequential execution is defined independently of physical placement and OCC:
// each operation observes the preceding store, then its whole write set takes effect.
pub open spec fn serial_effect(before: Map<int, p::Cell>, txn: int, body: Map<int, Op>) -> Map<int, p::Cell> {
    Map::new(before.dom(), |k: int|
        if !body.dom().contains(k) { before[k] } else {
            match body[k] {
                Op::Read => before[k],
                Op::Put { value } => p::Cell { value: Some(value), writer: txn },
                Op::Delete => p::Cell { value: None, writer: txn },
                Op::Add { delta } => p::Cell { value: Some(number(before[k].value) + delta), writer: txn },
            }
        })
}
pub open spec fn serial_step(before: Map<int, p::Cell>, after: Map<int, p::Cell>,
    txn: int, body: Map<int, Op>, reads: Map<int, p::Cell>) -> bool {
    &&& reads.dom() == read_keys(body)
    &&& forall|k: int| reads.dom().contains(k) ==> #[trigger] reads[k] == before[k]
    &&& after == serial_effect(before, txn, body)
}
pub proof fn lemma_serial_effect(before: Map<int, p::Cell>, txn: int,
    body: Map<int, Op>, reads: Map<int, p::Cell>)
    requires reads.dom() == read_keys(body),
        forall|k: int| reads.dom().contains(k) ==> #[trigger] reads[k] == before[k]
    ensures p::write_logical(before, txn, writes(body, reads)) == serial_effect(before, txn, body)
{
    assert(p::write_logical(before, txn, writes(body, reads)) =~= serial_effect(before, txn, body)) by {
        assert forall|k: int| before.dom().contains(k) implies
            p::write_logical(before, txn, writes(body, reads))[k] == serial_effect(before, txn, body)[k] by {
            if body.dom().contains(k) {
                match body[k] {
                    Op::Add { delta } => { assert(reads.dom().contains(k)); },
                    _ => {},
                }
            }
        }
    }
}
pub open spec fn replay(c: p::Constants, s: State, n: nat) -> Map<int, p::Cell>
    decreases n
{
    if n == 0 { p::initial(c).logical } else {
        let e = s.order[(n - 1) as int];
        let r = s.txns[e.txn];
        serial_effect(replay(c, s, (n - 1) as nat), e.txn, r.body)
    }
}
pub proof fn lemma_replay(c: p::Constants, s: State, n: nat)
    requires inv(c, s), n <= s.order.len()
    ensures
        n > 0 ==> replay(c, s, n) == s.order[(n - 1) as int].after,
        n < s.order.len() ==> replay(c, s, n) == s.order[n as int].before,
        n == s.order.len() ==> replay(c, s, n) == s.placement.logical,
    decreases n
{
    if n > 0 {
        lemma_replay(c, s, (n - 1) as nat);
        assert(entry_ok(c, s, (n - 1) as int));
        let e = s.order[(n - 1) as int];
        lemma_serial_effect(e.before, e.txn, s.txns[e.txn].body, s.txns[e.txn].reads);
    }
    if n < s.order.len() { assert(entry_ok(c, s, n as int)); }
    if n == s.order.len() && n > 0 { assert(s.order[(n - 1) as int] == s.order.last()); }
}

pub open spec fn successful(s: State, txn: int) -> bool {
    s.txns.dom().contains(txn) && s.txns[txn].outcome is Committed
        && s.txns[txn].replied.is_some()
}
pub open spec fn selected(s: State, txn: int) -> bool {
    exists|i: int| 0 <= i < s.order.len() && #[trigger] s.order[i].txn == txn
}
pub open spec fn strict_serializable(c: p::Constants, s: State) -> bool {
    // No aborted/open operation is selected; pending committed operations may finish.
    &&& forall|txn: int| #[trigger] selected(s, txn) <==>
        (s.txns.dom().contains(txn) && s.txns[txn].outcome is Committed)
    &&& forall|txn: int| #[trigger] successful(s, txn) ==> selected(s, txn)
    &&& forall|i: int, j: int| 0 <= i < j < s.order.len() ==>
        (#[trigger] s.order[i]).txn != (#[trigger] s.order[j]).txn
    &&& forall|i: int| 0 <= i < s.order.len() ==> serial_step(
        replay(c, s, i as nat), replay(c, s, (i + 1) as nat), s.order[i].txn,
        s.txns[s.order[i].txn].body, s.txns[s.order[i].txn].reads)
    // Final-before-invoke is the actual client real-time relation, not commit order assumed as a guard.
    &&& forall|i: int, j: int| 0 <= i < s.order.len() && 0 <= j < s.order.len()
        && s.txns[s.order[i].txn].replied.is_some()
        && s.txns[s.order[i].txn].replied.unwrap() < s.txns[s.order[j].txn].invoked ==> i < j
    &&& replay(c, s, s.order.len()) == s.placement.logical
}
pub proof fn lemma_ticks(c: p::Constants, s: State, i: int, j: int)
    requires inv(c, s), 0 <= i <= j < s.order.len()
    ensures s.order[i].tick <= s.order[j].tick
    decreases j - i
{
    assert(entry_ok(c, s, j));
    if i < j { lemma_ticks(c, s, i, j - 1); }
}
pub proof fn theorem_history(c: p::Constants, s: State)
    requires inv(c, s)
    ensures strict_serializable(c, s)
{
    assert forall|txn: int| #[trigger] selected(s, txn) <==>
        (s.txns.dom().contains(txn) && s.txns[txn].outcome is Committed) by {
        if selected(s, txn) {
            let i = choose|i: int| 0 <= i < s.order.len() && s.order[i].txn == txn;
            assert(entry_ok(c, s, i));
        } else if s.txns.dom().contains(txn) && s.txns[txn].outcome is Committed {
            assert(record_ok(c, s, txn));
            let i = s.txns[txn].outcome->index as int;
            assert(s.order[i].txn == txn);
        }
    }
    assert forall|i: int, j: int| 0 <= i < j < s.order.len() implies
        (#[trigger] s.order[i]).txn != (#[trigger] s.order[j]).txn by {
        assert(entry_ok(c, s, i)); assert(entry_ok(c, s, j));
    }
    assert forall|i: int| 0 <= i < s.order.len() implies serial_step(
        replay(c, s, i as nat), replay(c, s, (i + 1) as nat), s.order[i].txn,
        s.txns[s.order[i].txn].body, s.txns[s.order[i].txn].reads) by {
        lemma_replay(c, s, i as nat); lemma_replay(c, s, (i + 1) as nat);
        assert(entry_ok(c, s, i));
        let e = s.order[i];
        lemma_serial_effect(e.before, e.txn, s.txns[e.txn].body, s.txns[e.txn].reads);
    }
    assert forall|i: int, j: int| 0 <= i < s.order.len() && 0 <= j < s.order.len()
        && s.txns[s.order[i].txn].replied.is_some()
        && s.txns[s.order[i].txn].replied.unwrap() < s.txns[s.order[j].txn].invoked implies i < j by {
        assert(entry_ok(c, s, i)); assert(entry_ok(c, s, j));
        assert(record_ok(c, s, s.order[i].txn));
        if j <= i { lemma_ticks(c, s, j, i); }
    }
    lemma_replay(c, s, s.order.len());
}

pub open spec fn behavior(c: p::Constants, states: Seq<State>, actions: Seq<Action>) -> bool {
    &&& states.len() == actions.len() + 1
    &&& states[0] == initial(c)
    &&& forall|i: int| 0 <= i < actions.len() ==> states[i + 1] == apply(c, states[i], actions[i])
}
pub proof fn lemma_prefix(c: p::Constants, states: Seq<State>, actions: Seq<Action>, n: nat)
    requires p::constants_ok(c), behavior(c, states, actions), n < states.len()
    ensures inv(c, states[n as int])
    decreases n
{
    if n == 0 { lemma_init(c); }
    else {
        lemma_prefix(c, states, actions, (n - 1) as nat);
        lemma_step(c, states[(n - 1) as int], actions[(n - 1) as int]);
    }
}
pub proof fn theorem_finite_behavior(c: p::Constants, states: Seq<State>, actions: Seq<Action>)
    requires p::constants_ok(c), behavior(c, states, actions)
    ensures forall|n: int| 0 <= n < states.len() ==> strict_serializable(c, states[n])
{
    assert forall|n: int| 0 <= n < states.len() implies strict_serializable(c, states[n]) by {
        lemma_prefix(c, states, actions, n as nat);
        theorem_history(c, states[n]);
    }
}

// A missing/open ID has no terminal response. For a terminal ID, this is the
// same retained application result used by both the first reply and retries.
pub open spec fn retained_result(s: State, txn: int) -> Option<Result> {
    if !s.txns.dom().contains(txn) { None } else {
        let r = s.txns[txn];
        match r.outcome {
            Outcome::Open => None,
            Outcome::Aborted => Some(Result::Aborted),
            Outcome::Committed { index } => Some(Result::Committed {
                reads: Map::new(r.reads.dom(), |k: int| r.reads[k].value) }),
        }
    }
}

// Terminal application results survive arbitrary migration, delayed replies and
// retries. In particular neither an aborted nor a committed ID can run again.
pub proof fn theorem_terminal_retained(c: p::Constants, s: State, a: Action, txn: int)
    requires s.txns.dom().contains(txn), !(s.txns[txn].outcome is Open)
    ensures
        apply(c, s, a).txns.dom().contains(txn),
        apply(c, s, a).txns[txn].body == s.txns[txn].body,
        apply(c, s, a).txns[txn].reads == s.txns[txn].reads,
        apply(c, s, a).txns[txn].invoked == s.txns[txn].invoked,
        apply(c, s, a).txns[txn].outcome == s.txns[txn].outcome,
        retained_result(apply(c, s, a), txn) == retained_result(s, txn),
        s.txns[txn].replied.is_some() ==> apply(c, s, a).txns[txn].replied == s.txns[txn].replied,
{
    if enabled(c, s, a) {
        match a {
            Action::Begin { txn: id, body } => { assert(id != txn); },
            Action::Fetch { txn: id, key } => { assert(id != txn); },
            Action::Commit { txn: id } => { assert(id != txn); },
            Action::Abort { txn: id } => { assert(id != txn); },
            _ => {},
        }
    }
}

pub proof fn theorem_retry_no_effect(c: p::Constants, s: State, txn: int)
    ensures
        apply(c, s, Action::Retry { txn }).placement == s.placement,
        apply(c, s, Action::Retry { txn }).txns == s.txns,
        apply(c, s, Action::Retry { txn }).order == s.order,
        retained_result(apply(c, s, Action::Retry { txn }), txn) == retained_result(s, txn),
{}

pub proof fn lemma_behavior_begin(c: p::Constants)
    ensures behavior(c, seq![initial(c)], Seq::empty())
{}

pub proof fn lemma_behavior_extend(c: p::Constants, states: Seq<State>, actions: Seq<Action>, a: Action)
    requires behavior(c, states, actions)
    ensures behavior(c, states.push(apply(c, states.last(), a)), actions.push(a))
{
    let more = states.push(apply(c, states.last(), a));
    let acts = actions.push(a);
    assert forall|i: int| 0 <= i < acts.len() implies more[i + 1] == apply(c, more[i], acts[i]) by {
        if i < actions.len() {
            assert(more[i + 1] == states[i + 1]);
        } else {
            assert(states[i] == states.last());
        }
    }
}

} // verus!
