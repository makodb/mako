#[allow(non_snake_case)]
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct RaftData {
    // ballot_t is SIGNED int64_t (constants.h:6). These five fields decode
    // ballots/terms written by RaftServer, so they must be read back with the
    // same signedness the writer used -- otherwise a ballot >= 2^63 prints as
    // a huge positive here and as negative in the server. slot_id is
    // genuinely unsigned: slotid_t is uint64_t (constants.h:19).
    pub max_ballot_seen_: i64,
    pub max_ballot_accepted_: i64,
    pub term: i64,
    pub prevTerm: i64,
    pub slot_id: u64,
    pub ballot: i64,
}
