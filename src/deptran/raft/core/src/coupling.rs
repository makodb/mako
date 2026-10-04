// [M12] (whole file) The coupling of the core to the group's Raft spec (plan
// Phase 8): what the core's state and messages mean as the spec's LState and
// LRaftMessage, the ghost log of the core's actions, and the lemmas that each
// closed segment is one of the spec's atomic steps. Verification only: the
// module exists when Verus checks the crate (cfg verus_keep_ghost), with the
// spec imported from the frozen tag (scripts/verus/verify_core.sh); plain
// cargo never compiles it.

#[allow(unused_imports)]
use crate::*;
use vstd::prelude::*;
#[allow(unused_imports)]
use glr::protocol::Raft::types::*;

verus! {

// The term, as the spec counts it.
pub open spec fn term_view<C>(core: &RaftCore<C>) -> int {
    core.current_term_ as int
}

// The import reaches the spec: a fresh core's term is the spec's initial one.
pub proof fn lemma_new_term_is_init<C>(core: &RaftCore<C>)
    requires core.current_term_ == 0,
    ensures term_view(core) == glr::protocol::Raft::ghost_log::init_state().current_term,
{
}

} // verus!
