#include <array>
#include <cstdint>
#include <map>
#include <set>

#include <gtest/gtest.h>

#include "deptran/raft/server.h"
#include "deptran/raft/memory_log_storage.hpp"

namespace {

TEST(RaftDurabilityAccountingTest,
     ElectionCampaignRequiresLatchedGenerationAndStrictDeadline) {
  EXPECT_TRUE(janus::raft_server_timer_campaign_is_current(
      /*is_leader=*/false,
      /*observed_generation=*/17,
      /*current_generation=*/17,
      /*elapsed=*/750001,
      /*timeout=*/750000));
  EXPECT_FALSE(janus::raft_server_timer_campaign_is_current(
      /*is_leader=*/false,
      /*observed_generation=*/16,
      /*current_generation=*/17,
      /*elapsed=*/750001,
      /*timeout=*/750000));
  EXPECT_FALSE(janus::raft_server_timer_campaign_is_current(
      /*is_leader=*/false,
      /*observed_generation=*/17,
      /*current_generation=*/17,
      /*elapsed=*/750000,
      /*timeout=*/750000));
  EXPECT_FALSE(janus::raft_server_timer_campaign_is_current(
      /*is_leader=*/true,
      /*observed_generation=*/17,
      /*current_generation=*/17,
      /*elapsed=*/750001,
      /*timeout=*/750000));
  EXPECT_TRUE(janus::raft_server_campaign_can_start(
      /*is_leader=*/false, /*election_in_progress=*/false));
  EXPECT_FALSE(janus::raft_server_campaign_can_start(
      /*is_leader=*/true, /*election_in_progress=*/false));
  EXPECT_FALSE(janus::raft_server_campaign_can_start(
      /*is_leader=*/false, /*election_in_progress=*/true));
}

TEST(RaftDurabilityAccountingTest,
     DelayedElectionCompletionCannotMutateANewerCampaign) {
  using janus::ElectionCompletionAction;

  struct Case {
    bool election_in_progress;
    uint64_t election_term;
    uint64_t campaign_term;
    uint64_t current_term;
    int64_t observed_response_term;
    ElectionCompletionAction expected;
  };

  // The completion gate precedes the YES/NO/TIMEOUT branch, so each row
  // applies identically to all three ordinary quorum outcomes.
  const std::array<Case, 8> cases{{
      {true, 11, 10, 11, 0,
       ElectionCompletionAction::IGNORE_STALE},
      {true, 11, 10, 11, 10,
       ElectionCompletionAction::IGNORE_STALE},
      {true, 11, 10, 11, 11,
       ElectionCompletionAction::IGNORE_STALE},
      {false, 11, 10, 11, 0,
       ElectionCompletionAction::IGNORE_STALE},
      {false, 11, 10, 11, 10,
       ElectionCompletionAction::IGNORE_STALE},
      {false, 11, 10, 11, 11,
       ElectionCompletionAction::IGNORE_STALE},
      {true, 11, 11, 11, 11,
       ElectionCompletionAction::APPLY_CURRENT},
      {true, 11, 10, 11, 12,
       ElectionCompletionAction::ADVANCE_HIGHER_TERM},
  }};

  for (const auto& test_case : cases) {
    EXPECT_EQ(static_cast<ElectionCompletionAction>(
                  janus::raft_server_election_completion_action(
                      test_case.election_in_progress,
                      test_case.election_term,
                      test_case.campaign_term,
                      test_case.current_term,
                      test_case.observed_response_term)),
              test_case.expected);
  }
}

TEST(RaftDurabilityAccountingTest,
     LocalAppendAdmissionPreservesIndeterminateOutcome) {
  EXPECT_TRUE(janus::raft_server_start_was_rejected(
      janus::RaftStartResult::REJECTED));
  EXPECT_FALSE(janus::raft_server_start_was_rejected(
      janus::RaftStartResult::INDETERMINATE));
  EXPECT_TRUE(janus::raft_server_start_was_appended(
      janus::RaftStartResult::APPENDED));
  EXPECT_FALSE(janus::raft_server_start_was_appended(
      janus::RaftStartResult::INDETERMINATE));
  EXPECT_TRUE(janus::raft_server_start_is_indeterminate(
      janus::RaftStartResult::INDETERMINATE));
  EXPECT_FALSE(janus::raft_server_start_is_indeterminate(
      janus::RaftStartResult::REJECTED));
}

TEST(RaftDurabilityAccountingTest,
     SubmissionWaitsForDefinitiveCommittedSlotResolution) {
  // A different term or an uncommitted local overwrite is intentionally not
  // an input: neither proves the old entry's outcome.
  EXPECT_FALSE(janus::raft_server_submission_is_committed(40, 41, true));
  EXPECT_FALSE(janus::raft_server_submission_is_superseded(
      40, 41, /*entry_known_conflict=*/true,
      /*committed_newer_prefix=*/false));

  EXPECT_TRUE(janus::raft_server_submission_is_committed(41, 41, true));
  EXPECT_FALSE(janus::raft_server_submission_is_superseded(
      41, 41, /*entry_known_conflict=*/false,
      /*committed_newer_prefix=*/false));

  EXPECT_FALSE(janus::raft_server_submission_is_committed(41, 41, false));
  EXPECT_TRUE(janus::raft_server_submission_is_superseded(
      41, 41, /*entry_known_conflict=*/true,
      /*committed_newer_prefix=*/false));

  // A new leader can have a much shorter log. Its committed current-term
  // no-op makes the old private suffix terminal without growing commitIndex
  // all the way to the old submitted slot.
  EXPECT_TRUE(janus::raft_server_submission_is_superseded(
      2, 41, /*entry_known_conflict=*/true,
      /*committed_newer_prefix=*/true));

  // Missing because the slot was compacted is not a known conflict. Its exact
  // identity must be retained by the active-submission compaction guard.
  EXPECT_FALSE(janus::raft_server_submission_is_superseded(
      42, 41, /*entry_known_conflict=*/false,
      /*committed_newer_prefix=*/true));
}

TEST(RaftDurabilityAccountingTest,
     SnapshotClassifiesEveryCoveredSubmissionBeforeErasure) {
  // A snapshot below the submission does not resolve it.
  EXPECT_FALSE(janus::raft_server_snapshot_resolves_submission(
      /*snapshot_index=*/40, /*submitted_index=*/41));
  EXPECT_FALSE(janus::raft_server_snapshot_submission_is_committed(
      /*snapshot_index=*/40, /*snapshot_term=*/9,
      /*submitted_index=*/41, /*submitted_term=*/7,
      /*local_entry_matches=*/true, /*local_commit_crossed=*/false,
      /*snapshot_prefix_matches=*/false));
  EXPECT_FALSE(janus::raft_server_snapshot_submission_is_superseded(
      /*snapshot_index=*/40, /*snapshot_term=*/9,
      /*submitted_index=*/41, /*submitted_term=*/7,
      /*local_entry_known_conflict=*/false,
      /*local_commit_crossed=*/false,
      /*snapshot_prefix_matches=*/false));
  EXPECT_FALSE(janus::raft_server_snapshot_submission_is_indeterminate(
      /*snapshot_index=*/40, /*submitted_index=*/41,
      /*committed=*/false, /*superseded=*/false));

  // The snapshot's exact boundary identity is authoritative even when no
  // matching local prefix survives.
  EXPECT_TRUE(janus::raft_server_snapshot_resolves_submission(
      /*snapshot_index=*/41, /*submitted_index=*/41));
  EXPECT_TRUE(janus::raft_server_snapshot_submission_is_committed(
      /*snapshot_index=*/41, /*snapshot_term=*/7,
      /*submitted_index=*/41, /*submitted_term=*/7,
      /*local_entry_matches=*/false, /*local_commit_crossed=*/false,
      /*snapshot_prefix_matches=*/false));
  EXPECT_FALSE(janus::raft_server_snapshot_submission_is_superseded(
      /*snapshot_index=*/41, /*snapshot_term=*/7,
      /*submitted_index=*/41, /*submitted_term=*/7,
      /*local_entry_known_conflict=*/false,
      /*local_commit_crossed=*/false,
      /*snapshot_prefix_matches=*/false));

  EXPECT_FALSE(janus::raft_server_snapshot_submission_is_committed(
      /*snapshot_index=*/41, /*snapshot_term=*/8,
      /*submitted_index=*/41, /*submitted_term=*/7,
      /*local_entry_matches=*/false, /*local_commit_crossed=*/false,
      /*snapshot_prefix_matches=*/false));
  EXPECT_TRUE(janus::raft_server_snapshot_submission_is_superseded(
      /*snapshot_index=*/41, /*snapshot_term=*/8,
      /*submitted_index=*/41, /*submitted_term=*/7,
      /*local_entry_known_conflict=*/false,
      /*local_commit_crossed=*/false,
      /*snapshot_prefix_matches=*/false));

  // A matching boundary proves the entire local prefix, so a matching local
  // entry committed and a known local conflict was superseded.
  EXPECT_TRUE(janus::raft_server_snapshot_submission_is_committed(
      /*snapshot_index=*/99, /*snapshot_term=*/10,
      /*submitted_index=*/41, /*submitted_term=*/7,
      /*local_entry_matches=*/true, /*local_commit_crossed=*/false,
      /*snapshot_prefix_matches=*/true));
  EXPECT_TRUE(janus::raft_server_snapshot_submission_is_superseded(
      /*snapshot_index=*/99, /*snapshot_term=*/10,
      /*submitted_index=*/41, /*submitted_term=*/7,
      /*local_entry_known_conflict=*/true,
      /*local_commit_crossed=*/false,
      /*snapshot_prefix_matches=*/true));

  // A divergent snapshot carries no identity for earlier slots. Even a local
  // match or conflict could differ from the incoming committed prefix, so the
  // safe terminal result is retryable ambiguity.
  const bool divergent_committed =
      janus::raft_server_snapshot_submission_is_committed(
          /*snapshot_index=*/99, /*snapshot_term=*/10,
          /*submitted_index=*/41, /*submitted_term=*/7,
          /*local_entry_matches=*/true, /*local_commit_crossed=*/false,
          /*snapshot_prefix_matches=*/false);
  const bool divergent_superseded =
      janus::raft_server_snapshot_submission_is_superseded(
          /*snapshot_index=*/99, /*snapshot_term=*/10,
          /*submitted_index=*/41, /*submitted_term=*/7,
          /*local_entry_known_conflict=*/false,
          /*local_commit_crossed=*/false,
          /*snapshot_prefix_matches=*/false);
  EXPECT_FALSE(divergent_committed);
  EXPECT_FALSE(divergent_superseded);
  EXPECT_TRUE(janus::raft_server_snapshot_submission_is_indeterminate(
      /*snapshot_index=*/99, /*submitted_index=*/41,
      divergent_committed, divergent_superseded));

  // Locally committed identity remains definitive even if the incoming
  // boundary diverges above it.
  EXPECT_TRUE(janus::raft_server_snapshot_submission_is_committed(
      /*snapshot_index=*/99, /*snapshot_term=*/10,
      /*submitted_index=*/41, /*submitted_term=*/7,
      /*local_entry_matches=*/true, /*local_commit_crossed=*/true,
      /*snapshot_prefix_matches=*/false));
  EXPECT_TRUE(janus::raft_server_snapshot_submission_is_superseded(
      /*snapshot_index=*/99, /*snapshot_term=*/10,
      /*submitted_index=*/41, /*submitted_term=*/7,
      /*local_entry_known_conflict=*/true,
      /*local_commit_crossed=*/true,
      /*snapshot_prefix_matches=*/false));
}

TEST(RaftDurabilityAccountingTest,
     SnapshotSubmissionLedgerIsExactKeyedAndOneShot) {
  janus::RaftResolvedSubmissionLedger ledger;
  const janus::RaftResolvedSubmissionLedger::Key committed_key{41, 7};
  const janus::RaftResolvedSubmissionLedger::Key superseded_key{42, 7};
  const janus::RaftResolvedSubmissionLedger::Key indeterminate_key{43, 7};

  EXPECT_FALSE(ledger.Record(committed_key, {}));
  EXPECT_FALSE(ledger.Record(
      committed_key, {/*committed=*/true, /*superseded=*/true,
                      /*indeterminate=*/false}));
  EXPECT_FALSE(ledger.Record(
      committed_key, {/*committed=*/true, /*superseded=*/false,
                      /*indeterminate=*/true}));
  EXPECT_EQ(ledger.size(), 0u);

  EXPECT_TRUE(ledger.Record(
      committed_key, {/*committed=*/true, /*superseded=*/false,
                      /*indeterminate=*/false}));
  EXPECT_TRUE(ledger.Record(
      superseded_key, {/*committed=*/false, /*superseded=*/true,
                       /*indeterminate=*/false}));
  EXPECT_TRUE(ledger.Record(
      indeterminate_key, {/*committed=*/false, /*superseded=*/false,
                          /*indeterminate=*/true}));
  EXPECT_FALSE(ledger.Record(
      committed_key, {/*committed=*/true, /*superseded=*/false,
                      /*indeterminate=*/false}));
  EXPECT_EQ(ledger.size(), 3u);

  // A different term at the same index is a different submission identity.
  const auto wrong_term = ledger.Consume({41, 8});
  EXPECT_FALSE(wrong_term.first);
  EXPECT_EQ(ledger.size(), 3u);

  const auto committed = ledger.Consume(committed_key);
  ASSERT_TRUE(committed.first);
  EXPECT_TRUE(committed.second.committed);
  EXPECT_FALSE(committed.second.superseded);
  EXPECT_FALSE(committed.second.indeterminate);
  EXPECT_EQ(ledger.size(), 2u);

  const auto consumed_again = ledger.Consume(committed_key);
  EXPECT_FALSE(consumed_again.first);
  EXPECT_EQ(ledger.size(), 2u);

  const auto superseded = ledger.Consume(superseded_key);
  ASSERT_TRUE(superseded.first);
  EXPECT_FALSE(superseded.second.committed);
  EXPECT_TRUE(superseded.second.superseded);
  EXPECT_FALSE(superseded.second.indeterminate);
  EXPECT_EQ(ledger.size(), 1u);

  const auto indeterminate = ledger.Consume(indeterminate_key);
  ASSERT_TRUE(indeterminate.first);
  EXPECT_FALSE(indeterminate.second.committed);
  EXPECT_FALSE(indeterminate.second.superseded);
  EXPECT_TRUE(indeterminate.second.indeterminate);
  EXPECT_EQ(ledger.size(), 0u);
}

TEST(RaftDurabilityAccountingTest,
     RaftViewsTranslateGlobalSitesAtPartitionLocalBoundary) {
  constexpr siteid_t invalid = static_cast<siteid_t>(INVALID_SITEID);

  // Catalogue partition 1 has global sites 3/4/5 but local routing IDs 0/1/2.
  // Self site 4 must publish locale 1, and remote site 5 must publish the
  // validated lookup result 2 -- never either global site ID.
  EXPECT_EQ(janus::raft_server_view_leader_locale(
                /*leader_site=*/4, /*self_site=*/4, /*self_locale=*/1,
                /*mapped_locale=*/-1, invalid),
            1);
  EXPECT_EQ(janus::raft_server_view_leader_locale(
                /*leader_site=*/5, /*self_site=*/4, /*self_locale=*/1,
                /*mapped_locale=*/2, invalid),
            2);
  EXPECT_EQ(janus::raft_server_view_leader_locale(
                /*leader_site=*/5, /*self_site=*/4, /*self_locale=*/1,
                /*mapped_locale=*/-1, invalid),
            -1);
  EXPECT_EQ(janus::raft_server_view_leader_locale(
                invalid, /*self_site=*/4, /*self_locale=*/1,
                /*mapped_locale=*/2, invalid),
            -1);

  EXPECT_EQ(janus::raft_server_recovery_leader_site(
                /*leader_locale=*/1, /*self_locale=*/1, /*self_site=*/4,
                /*mapped_site=*/invalid, invalid),
            4);
  EXPECT_EQ(janus::raft_server_recovery_leader_site(
                /*leader_locale=*/2, /*self_locale=*/1, /*self_site=*/4,
                /*mapped_site=*/5, invalid),
            5);
  EXPECT_EQ(janus::raft_server_recovery_leader_site(
                /*leader_locale=*/2, /*self_locale=*/1, /*self_site=*/4,
                /*mapped_site=*/invalid, invalid),
            invalid);
  EXPECT_EQ(janus::raft_server_recovery_leader_site(
                /*leader_locale=*/-1, /*self_locale=*/1, /*self_site=*/4,
                /*mapped_site=*/5, invalid),
            invalid);

  EXPECT_TRUE(janus::raft_server_recovery_view_matches_term(
      /*incoming_view_id=*/9, /*local_view_id=*/9));
  EXPECT_FALSE(janus::raft_server_recovery_view_matches_term(
      /*incoming_view_id=*/10, /*local_view_id=*/9));

  EXPECT_TRUE(janus::raft_server_recovery_view_shape_is_valid(
      /*incoming_partition=*/1, /*expected_partition=*/1,
      /*incoming_replicas=*/3, /*expected_replicas=*/3,
      /*leader_count=*/1, /*allow_empty=*/false));
  EXPECT_FALSE(janus::raft_server_recovery_view_shape_is_valid(
      /*incoming_partition=*/0, /*expected_partition=*/1,
      /*incoming_replicas=*/3, /*expected_replicas=*/3,
      /*leader_count=*/1, /*allow_empty=*/false));
  EXPECT_FALSE(janus::raft_server_recovery_view_shape_is_valid(
      /*incoming_partition=*/1, /*expected_partition=*/1,
      /*incoming_replicas=*/2, /*expected_replicas=*/3,
      /*leader_count=*/1, /*allow_empty=*/false));
  EXPECT_FALSE(janus::raft_server_recovery_view_shape_is_valid(
      /*incoming_partition=*/1, /*expected_partition=*/1,
      /*incoming_replicas=*/3, /*expected_replicas=*/3,
      /*leader_count=*/2, /*allow_empty=*/false));
  EXPECT_TRUE(janus::raft_server_recovery_view_shape_is_valid(
      /*incoming_partition=*/1, /*expected_partition=*/1,
      /*incoming_replicas=*/0, /*expected_replicas=*/3,
      /*leader_count=*/0, /*allow_empty=*/true));
  EXPECT_TRUE(janus::raft_server_recovery_view_shape_is_valid(
      /*incoming_partition=*/1, /*expected_partition=*/1,
      /*incoming_replicas=*/2, /*expected_replicas=*/3,
      /*leader_count=*/1, /*allow_empty=*/true));
  EXPECT_FALSE(janus::raft_server_recovery_view_shape_is_valid(
      /*incoming_partition=*/1, /*expected_partition=*/1,
      /*incoming_replicas=*/0, /*expected_replicas=*/3,
      /*leader_count=*/0, /*allow_empty=*/false));

  // A same-term recovery view may reaffirm a local leader or identify a
  // remote leader to a follower/candidate. It may not invent two leaders in
  // one term, route a follower to itself, or replace an already-known remote
  // leader with a different site in that same term.
  EXPECT_TRUE(janus::raft_server_recovery_view_matches_role(
      /*term_matches=*/true, /*local_is_leader=*/true,
      /*view_leader_is_self=*/true, /*has_known_leader=*/false,
      /*known_leader_matches_view=*/false));
  EXPECT_FALSE(janus::raft_server_recovery_view_matches_role(
      /*term_matches=*/true, /*local_is_leader=*/true,
      /*view_leader_is_self=*/false, /*has_known_leader=*/false,
      /*known_leader_matches_view=*/false));
  EXPECT_TRUE(janus::raft_server_recovery_view_matches_role(
      /*term_matches=*/true, /*local_is_leader=*/false,
      /*view_leader_is_self=*/false, /*has_known_leader=*/false,
      /*known_leader_matches_view=*/false));
  EXPECT_TRUE(janus::raft_server_recovery_view_matches_role(
      /*term_matches=*/true, /*local_is_leader=*/false,
      /*view_leader_is_self=*/false, /*has_known_leader=*/true,
      /*known_leader_matches_view=*/true));
  EXPECT_FALSE(janus::raft_server_recovery_view_matches_role(
      /*term_matches=*/true, /*local_is_leader=*/false,
      /*view_leader_is_self=*/false, /*has_known_leader=*/true,
      /*known_leader_matches_view=*/false));
  EXPECT_FALSE(janus::raft_server_recovery_view_matches_role(
      /*term_matches=*/true, /*local_is_leader=*/false,
      /*view_leader_is_self=*/true, /*has_known_leader=*/false,
      /*known_leader_matches_view=*/false));
  EXPECT_FALSE(janus::raft_server_recovery_view_matches_role(
      /*term_matches=*/false, /*local_is_leader=*/false,
      /*view_leader_is_self=*/false, /*has_known_leader=*/false,
      /*known_leader_matches_view=*/false));
}

TEST(RaftDurabilityAccountingTest,
     LeaderRpcAuthorityAndTermDurabilityAreFailClosed) {
  // A higher-term remote voter can establish a new leader identity.
  EXPECT_TRUE(janus::raft_server_leader_rpc_sender_is_authoritative(
      /*leader_has_higher_term=*/true, /*local_is_leader=*/true,
      /*sender_is_self=*/false, /*has_known_leader=*/true,
      /*known_leader_matches_sender=*/false));

  // In one term, a follower may learn a previously unknown remote leader or
  // reaffirm the known one, but may not switch from known A to competing B.
  EXPECT_TRUE(janus::raft_server_leader_rpc_sender_is_authoritative(
      /*leader_has_higher_term=*/false, /*local_is_leader=*/false,
      /*sender_is_self=*/false, /*has_known_leader=*/false,
      /*known_leader_matches_sender=*/false));
  EXPECT_TRUE(janus::raft_server_leader_rpc_sender_is_authoritative(
      /*leader_has_higher_term=*/false, /*local_is_leader=*/false,
      /*sender_is_self=*/false, /*has_known_leader=*/true,
      /*known_leader_matches_sender=*/true));
  EXPECT_FALSE(janus::raft_server_leader_rpc_sender_is_authoritative(
      /*leader_has_higher_term=*/false, /*local_is_leader=*/false,
      /*sender_is_self=*/false, /*has_known_leader=*/true,
      /*known_leader_matches_sender=*/false));
  EXPECT_FALSE(janus::raft_server_leader_rpc_sender_is_authoritative(
      /*leader_has_higher_term=*/false, /*local_is_leader=*/true,
      /*sender_is_self=*/false, /*has_known_leader=*/false,
      /*known_leader_matches_sender=*/false));

  // TimeoutNow alone permits the local leader's idempotent self case; inbound
  // AppendEntries/InstallSnapshot additionally reject self before this helper.
  EXPECT_TRUE(janus::raft_server_leader_rpc_sender_is_authoritative(
      /*leader_has_higher_term=*/false, /*local_is_leader=*/true,
      /*sender_is_self=*/true, /*has_known_leader=*/true,
      /*known_leader_matches_sender=*/true));
  EXPECT_FALSE(janus::raft_server_leader_rpc_sender_is_authoritative(
      /*leader_has_higher_term=*/true, /*local_is_leader=*/false,
      /*sender_is_self=*/true, /*has_known_leader=*/false,
      /*known_leader_matches_sender=*/false));

  EXPECT_TRUE(janus::raft_server_term_advance_is_durable(
      /*has_configured_storage=*/false,
      /*persistence_succeeded=*/false));
  EXPECT_TRUE(janus::raft_server_term_advance_is_durable(
      /*has_configured_storage=*/true,
      /*persistence_succeeded=*/true));
  EXPECT_FALSE(janus::raft_server_term_advance_is_durable(
      /*has_configured_storage=*/true,
      /*persistence_succeeded=*/false));

  EXPECT_FALSE(janus::raft_server_signed_term_is_newer(-1, 0));
  EXPECT_FALSE(janus::raft_server_signed_term_is_newer(9, 9));
  EXPECT_TRUE(janus::raft_server_signed_term_is_newer(10, 9));
}

TEST(RaftDurabilityAccountingTest, LeaderNoopIsRaftInternal) {
  EXPECT_TRUE(janus::raft_server_command_is_internal_noop(
      janus::TpcNoopCommand::static_kind(),
      janus::TpcNoopCommand::static_kind()));
  EXPECT_FALSE(janus::raft_server_command_is_internal_noop(
      janus::TpcCommitCommand::static_kind(),
      janus::TpcNoopCommand::static_kind()));
}

TEST(RaftDurabilityAccountingTest,
     LeaderHintRequiresEvidenceFromTheCurrentRoleTransition) {
  constexpr siteid_t self = 1;
  constexpr siteid_t previous_leader = 2;
  constexpr siteid_t observed_leader = 3;
  constexpr siteid_t invalid = static_cast<siteid_t>(INVALID_SITEID);

  // Winning an election establishes self as leader regardless of a stale
  // hint retained from the previous term.
  EXPECT_EQ(janus::raft_server_leader_hint_after_transition(
                /*is_leader=*/true, /*has_known_leader=*/false,
                self, previous_leader, invalid),
            self);

  // Starting an election or learning only that a higher term exists provides
  // no evidence about that term's elected leader.
  EXPECT_EQ(janus::raft_server_leader_hint_after_transition(
                /*is_leader=*/false, /*has_known_leader=*/false,
                self, previous_leader, invalid),
            invalid);

  // AppendEntries and InstallSnapshot identify their sender as the leader, so
  // follower transitions caused by those RPCs retain the observed identity.
  EXPECT_EQ(janus::raft_server_leader_hint_after_transition(
                /*is_leader=*/false, /*has_known_leader=*/true,
                self, observed_leader, invalid),
            observed_leader);
}

class FailureInjectingLogStorage final
    : public janus::raft::InMemoryLogStorage {
 public:
  bool fail_put = false;
  bool fail_sync = false;
  int fail_sync_call = 0;
  int fail_metadata_call = 0;
  int put_calls = 0;
  int sync_calls = 0;
  int metadata_calls = 0;
  int metadata_batch_calls = 0;

  bool put(const janus::raft::LogEntry& entry) override {
    ++put_calls;
    return !fail_put && InMemoryLogStorage::put(entry);
  }

  bool set_metadata(const std::string& key,
                    const std::string& value) override {
    ++metadata_calls;
    if (metadata_calls == fail_metadata_call) {
      return false;
    }
    return InMemoryLogStorage::set_metadata(key, value);
  }

  bool set_metadata_batch(
      const std::vector<std::pair<std::string, std::string>>& entries)
      override {
    ++metadata_batch_calls;
    if (metadata_calls + 1 == fail_metadata_call) {
      ++metadata_calls;
      return false;
    }
    metadata_calls += static_cast<int>(entries.size());
    return InMemoryLogStorage::set_metadata_batch(entries);
  }

  bool sync() override {
    ++sync_calls;
    return !fail_sync && sync_calls != fail_sync_call &&
        InMemoryLogStorage::sync();
  }
};

TEST(RaftDurabilityAccountingTest, AppendAckStrengthMatchesPersistenceMode) {
  EXPECT_EQ(janus::raft_server_follower_append_ack_type(
                /*has_durable_storage=*/false,
                /*async_persistence=*/false,
                /*persistence_succeeded=*/false),
            0u);
  EXPECT_EQ(janus::raft_server_follower_append_ack_type(
                /*has_durable_storage=*/false,
                /*async_persistence=*/true,
                /*persistence_succeeded=*/true),
            0u);
  EXPECT_EQ(janus::raft_server_follower_append_ack_type(
                /*has_durable_storage=*/true,
                /*async_persistence=*/false,
                /*persistence_succeeded=*/true),
            1u);
  EXPECT_EQ(janus::raft_server_follower_append_ack_type(
                /*has_durable_storage=*/true,
                /*async_persistence=*/false,
                /*persistence_succeeded=*/false),
            0u);
  EXPECT_EQ(janus::raft_server_follower_append_ack_type(
                /*has_durable_storage=*/true,
                /*async_persistence=*/true,
                /*persistence_succeeded=*/true),
            0u);
}

TEST(RaftDurabilityAccountingTest,
     AppendProofIsBoundedByExactWirePayload) {
  EXPECT_TRUE(janus::raft_server_append_entry_count_fits(UINT64_MAX, 0));
  EXPECT_FALSE(janus::raft_server_append_entry_count_fits(UINT64_MAX, 1));
  EXPECT_TRUE(janus::raft_server_append_batch_count_is_valid(
      UINT64_MAX - 3, 3));
  EXPECT_FALSE(janus::raft_server_append_batch_count_is_valid(7, 0));
  EXPECT_FALSE(janus::raft_server_append_batch_count_is_valid(
      UINT64_MAX - 3, 4));

  // Empty AppendEntries proves only its previous-index prefix; raw and batch
  // payloads extend that proof by exactly their encoded entry count.
  EXPECT_EQ(janus::raft_server_append_sent_end(7, 0), 7u);
  EXPECT_EQ(janus::raft_server_append_sent_end(7, 1), 8u);
  EXPECT_EQ(janus::raft_server_append_sent_end(7, 4), 11u);

  // A follower's longer private suffix cannot pre-credit entries that this RPC
  // did not send, even if the leader appended more work while it was in flight.
  EXPECT_EQ(janus::raft_server_append_acknowledged_through(
                /*reported_index=*/20,
                /*sent_end_index=*/10,
                /*leader_last_index=*/15),
            10u);

  // Defensive bounds also retain a shorter report and a leader-side rollback.
  EXPECT_EQ(janus::raft_server_append_acknowledged_through(8, 10, 15), 8u);
  EXPECT_EQ(janus::raft_server_append_acknowledged_through(20, 15, 9), 9u);

  // A delayed, fully matching RPC preserves a newer local suffix. Only a
  // missing or term-conflicting payload slot starts suffix replacement.
  EXPECT_FALSE(janus::raft_server_append_entry_conflicts(true, 7, 7));
  EXPECT_TRUE(janus::raft_server_append_entry_conflicts(true, 6, 7));
  EXPECT_TRUE(janus::raft_server_append_entry_conflicts(false, 0, 7));
  EXPECT_EQ(janus::raft_server_append_result_last_index(10, 8, false), 10u);
  EXPECT_EQ(janus::raft_server_append_result_last_index(10, 8, true), 8u);
  EXPECT_EQ(janus::raft_server_append_result_last_index(8, 10, false), 10u);
}

TEST(RaftDurabilityAccountingTest, DurableBoundaryRequiresPutAndSyncSuccess) {
  janus::raft::LogEntry entry(7, 3);

  FailureInjectingLogStorage put_failure;
  put_failure.fail_put = true;
  EXPECT_FALSE(janus::raft_server_write_and_sync(
      put_failure, [&entry](janus::raft::LogStorage& storage) {
        return storage.put(entry);
      }));
  EXPECT_EQ(put_failure.put_calls, 1);
  EXPECT_EQ(put_failure.sync_calls, 0);

  FailureInjectingLogStorage sync_failure;
  sync_failure.fail_sync = true;
  EXPECT_FALSE(janus::raft_server_write_and_sync(
      sync_failure, [&entry](janus::raft::LogStorage& storage) {
        return storage.put(entry);
      }));
  EXPECT_EQ(sync_failure.put_calls, 1);
  EXPECT_EQ(sync_failure.sync_calls, 1);

  FailureInjectingLogStorage success;
  EXPECT_TRUE(janus::raft_server_write_and_sync(
      success, [&entry](janus::raft::LogStorage& storage) {
        return storage.put(entry);
      }));
  EXPECT_EQ(success.put_calls, 1);
  EXPECT_EQ(success.sync_calls, 1);
}

TEST(RaftDurabilityAccountingTest,
     FailedAppendIsRetryableOnlyAfterDurableCompensatingRemoval) {
  janus::raft::LogEntry entry(7, 3);

  FailureInjectingLogStorage transient_sync_failure;
  transient_sync_failure.fail_sync_call = 1;
  EXPECT_FALSE(janus::raft_server_write_and_sync(
      transient_sync_failure,
      [&entry](janus::raft::LogStorage& storage) {
        return storage.put(entry);
      }));
  ASSERT_TRUE(transient_sync_failure.get(entry.slot_id).is_some());
  EXPECT_TRUE(janus::raft_server_compensating_remove_and_sync(
      transient_sync_failure, entry.slot_id));
  EXPECT_TRUE(transient_sync_failure.get(entry.slot_id).is_none());

  FailureInjectingLogStorage permanent_sync_failure;
  ASSERT_TRUE(permanent_sync_failure.put(entry));
  permanent_sync_failure.fail_sync = true;
  EXPECT_FALSE(janus::raft_server_compensating_remove_and_sync(
      permanent_sync_failure, entry.slot_id));
}

TEST(RaftDurabilityAccountingTest,
     TermAndVoteBoundaryRequiresOneAtomicMetadataWrite) {
  FailureInjectingLogStorage storage;
  storage.fail_metadata_call = 1;
  EXPECT_FALSE(janus::raft_server_write_and_sync(
      storage, [](janus::raft::LogStorage& target) {
        return target.set_metadata_batch({{"term", "9"}, {"vote", "2"}});
      }));
  EXPECT_EQ(storage.metadata_batch_calls, 1);
  EXPECT_EQ(storage.metadata_calls, 1);
  EXPECT_EQ(storage.sync_calls, 0);
}

TEST(RaftDurabilityAccountingTest, AsyncPersistenceTicketsRequireExactFifoTurn) {
  EXPECT_TRUE(janus::raft_server_async_persistence_should_queue(
      /*async_persistence=*/true,
      /*storage_configured=*/true,
      /*has_entries=*/true));
  EXPECT_FALSE(janus::raft_server_async_persistence_should_queue(
      false, true, true));
  EXPECT_FALSE(janus::raft_server_async_persistence_should_queue(
      true, false, true));
  EXPECT_FALSE(janus::raft_server_async_persistence_should_queue(
      true, true, false));

  EXPECT_TRUE(janus::raft_server_persistence_ticket_is_ready(
      /*serving_ticket=*/4, /*worker_ticket=*/4));
  EXPECT_FALSE(janus::raft_server_persistence_ticket_is_ready(
      /*serving_ticket=*/3, /*worker_ticket=*/4));
  EXPECT_FALSE(janus::raft_server_persistence_ticket_is_ready(
      /*serving_ticket=*/5, /*worker_ticket=*/4));
}

TEST(RaftDurabilityAccountingTest,
     LinearizableReadRejectsFollowersAndDisconnectedLeaders) {
  EXPECT_TRUE(janus::raft_server_read_index_local_state_allows(
      /*is_leader=*/true, /*disconnected=*/false));
  EXPECT_FALSE(janus::raft_server_read_index_local_state_allows(
      /*is_leader=*/false, /*disconnected=*/false));
  EXPECT_FALSE(janus::raft_server_read_index_local_state_allows(
      /*is_leader=*/true, /*disconnected=*/true));
}

TEST(RaftDurabilityAccountingTest, ElectionVotesRespectOffSyncAndAsyncModes) {
  const std::set<siteid_t> speculative{1, 2, 3};
  const std::set<siteid_t> early_durable{2};

  // Persistence-off cannot manufacture a durable self vote or count a
  // purported follower durability notification.
  EXPECT_TRUE(janus::raft_server_initial_durable_voters(
                  /*has_durable_storage=*/false,
                  /*async_persistence=*/false,
                  /*local_vote_persisted=*/false,
                  /*self=*/1, speculative, early_durable)
                  .empty());

  // Every successful synchronous reply returns after persistence.
  EXPECT_EQ(janus::raft_server_initial_durable_voters(
                /*has_durable_storage=*/true,
                /*async_persistence=*/false,
                /*local_vote_persisted=*/true,
                /*self=*/1, speculative, early_durable),
            speculative);

  // In async mode only the synchronously-persisted self vote and explicit
  // VoteDurable notifications count; an ordinary vote from site 3 does not.
  EXPECT_EQ(janus::raft_server_initial_durable_voters(
                /*has_durable_storage=*/true,
                /*async_persistence=*/true,
                /*local_vote_persisted=*/true,
                /*self=*/1, speculative, early_durable),
            (std::set<siteid_t>{1, 2}));

  // A failed candidate-local write never manufactures a durable self vote.
  EXPECT_EQ(janus::raft_server_initial_durable_voters(
                /*has_durable_storage=*/true,
                /*async_persistence=*/false,
                /*local_vote_persisted=*/false,
                /*self=*/1, speculative, early_durable),
            (std::set<siteid_t>{2, 3}));
  EXPECT_EQ(janus::raft_server_initial_durable_voters(
                /*has_durable_storage=*/true,
                /*async_persistence=*/true,
                /*local_vote_persisted=*/false,
                /*self=*/1, speculative, early_durable),
            (std::set<siteid_t>{2}));
}

TEST(RaftDurabilityAccountingTest, EarlyDurableVoteRequiresExactAsyncElection) {
  EXPECT_TRUE(janus::raft_server_can_buffer_early_durable_vote(
      /*has_durable_storage=*/true,
      /*async_persistence=*/true,
      /*is_leader=*/false,
      /*election_in_progress=*/true,
      /*vote_term=*/9,
      /*election_term=*/9));
  EXPECT_FALSE(janus::raft_server_can_buffer_early_durable_vote(
      false, true, false, true, 9, 9));
  EXPECT_FALSE(janus::raft_server_can_buffer_early_durable_vote(
      true, false, false, true, 9, 9));
  EXPECT_FALSE(janus::raft_server_can_buffer_early_durable_vote(
      true, true, true, true, 9, 9));
  EXPECT_FALSE(janus::raft_server_can_buffer_early_durable_vote(
      true, true, false, false, 9, 9));
  EXPECT_FALSE(janus::raft_server_can_buffer_early_durable_vote(
      true, true, false, true, 8, 9));
}

TEST(RaftDurabilityAccountingTest, ExactDurableQuorumIncludesLeaderSelf) {
  std::map<uint64_t, std::set<siteid_t>> acknowledgements;
  acknowledgements[11] = {2, 3};

  // A five-node quorum needs three. Two durable followers alone are short.
  EXPECT_EQ(janus::raft_server_highest_contiguous_secured_index(
                /*secured_index=*/10,
                /*speculative_index=*/11,
                /*last_log_index=*/11,
                /*quorum=*/3,
                acknowledgements),
            10u);

  // SetLocalAppend records the durably-persisted leader entry explicitly, so
  // the same two followers plus self reach the exact quorum boundary.
  acknowledgements[11].insert(1);
  EXPECT_EQ(janus::raft_server_highest_contiguous_secured_index(
                10, 11, 11, 3, acknowledgements),
            11u);
}

TEST(RaftDurabilityAccountingTest,
     DurableBeforeSpeculativeAdvancesWithoutAnotherDurableMessage) {
  const std::map<uint64_t, std::set<siteid_t>> acknowledgements{
      {6, {1, 2, 3}},
      {7, {1, 2, 3}},
  };

  // Durable ACKs may arrive first, but securedLogIndex cannot pass the current
  // speculative boundary.
  EXPECT_EQ(janus::raft_server_highest_contiguous_secured_index(
                5, 5, 7, 3, acknowledgements),
            5u);

  // Once memory acknowledgements move specCommitIndex, re-evaluating the same
  // recorded durability is sufficient; no new durable RPC is needed.
  EXPECT_EQ(janus::raft_server_highest_contiguous_secured_index(
                5, 7, 7, 3, acknowledgements),
            7u);
}

TEST(RaftDurabilityAccountingTest, SecuredIndexStopsAtFirstGapAndLastLog) {
  std::map<uint64_t, std::set<siteid_t>> acknowledgements{
      {6, {1, 2, 3}},
      {8, {1, 2, 3}},
  };
  EXPECT_EQ(janus::raft_server_highest_contiguous_secured_index(
                5, 8, 8, 3, acknowledgements),
            6u);

  acknowledgements[7] = {1, 2, 3};
  EXPECT_EQ(janus::raft_server_highest_contiguous_secured_index(
                /*secured_index=*/5,
                /*speculative_index=*/8,
                /*last_log_index=*/7,
                /*quorum=*/3,
                acknowledgements),
            7u);
}

}  // namespace
