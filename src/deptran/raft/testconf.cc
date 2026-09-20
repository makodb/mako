#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "testconf.h"
#include "frame.h"
#include "commo.h"
#include "application_log.h"
#include "../replication_log_entry.h"

#include "rrr/rrr.hpp"

#include <rusty/slice.hpp>

import std;

namespace janus {

#ifdef RAFT_TEST_CORO

// Test-harness-only index decisions. Keep signed i32 arithmetic so the
// generated C++ retains the incumbent overflow and division preconditions.
#if RUSTYCPP_RUST
pub const fn raft_test_server_index_is_valid(index: i32,
                                              server_count: i32) -> bool {
    index >= 0 && index < server_count
}

pub const fn raft_test_wrapped_server_index(index: i32,
                                             offset: i32,
                                             server_count: i32) -> i32 {
    let mut wrapped = (index + offset) % server_count;
    if wrapped < 0 {
        wrapped += server_count;
    }
    wrapped
}

pub const fn raft_test_connected_term_moved_on(disconnected: bool,
                                               current_term: u64,
                                               observed_term: u64) -> bool {
    !disconnected && current_term > observed_term
}

pub const fn raft_test_wait_leader_is_invalid(disconnected: bool,
                                               is_leader: bool,
                                               current_term: u64,
                                               expected_term: u64) -> bool {
    disconnected || !is_leader || current_term != expected_term
}

pub const fn raft_test_should_record_agreement_command(command_kind: i32,
                                                        agreement_kind: i32) -> bool {
    command_kind == agreement_kind
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_testconf.index_math version=1 rust_sha256=922d60e1a09b2c2f685aae7bbdf244ab746ea6ac855b9e45ca5f8aa802aeed1e*/
constexpr bool raft_test_server_index_is_valid(int32_t index, int32_t server_count);
constexpr int32_t raft_test_wrapped_server_index(int32_t index, int32_t offset, int32_t server_count);
constexpr bool raft_test_connected_term_moved_on(bool disconnected, uint64_t current_term, uint64_t observed_term);
constexpr bool raft_test_wait_leader_is_invalid(bool disconnected, bool is_leader, uint64_t current_term, uint64_t expected_term);
constexpr bool raft_test_should_record_agreement_command(int32_t command_kind, int32_t agreement_kind);
constexpr bool raft_test_server_index_is_valid(int32_t index, int32_t server_count) {
    return (rusty::detail::deref_if_pointer_like(index) >= 0) && (rusty::detail::deref_if_pointer_like(index) < rusty::detail::deref_if_pointer_like(server_count));
}
constexpr int32_t raft_test_wrapped_server_index(int32_t index, int32_t offset, int32_t server_count) {
    auto wrapped = ((rusty::detail::deref_if_pointer_like(index) + rusty::detail::deref_if_pointer_like(offset))) % rusty::detail::deref_if_pointer_like(server_count);
    if (rusty::detail::deref_if_pointer_like(wrapped) < 0) {
        rusty::detail::deref_if_pointer_like(wrapped) += server_count;
    }
    return std::move(wrapped);
}
constexpr bool raft_test_connected_term_moved_on(bool disconnected, uint64_t current_term, uint64_t observed_term) {
    return !disconnected && (rusty::detail::deref_if_pointer_like(current_term) > rusty::detail::deref_if_pointer_like(observed_term));
}
constexpr bool raft_test_wait_leader_is_invalid(bool disconnected, bool is_leader, uint64_t current_term, uint64_t expected_term) {
    return (rusty::detail::deref_if_pointer_like(disconnected) || !is_leader) || (rusty::detail::deref_if_pointer_like(current_term) != rusty::detail::deref_if_pointer_like(expected_term));
}
constexpr bool raft_test_should_record_agreement_command(int32_t command_kind, int32_t agreement_kind) {
    return rusty::detail::deref_if_pointer_like(command_kind) == rusty::detail::deref_if_pointer_like(agreement_kind);
}
/*RUSTYCPP:GEN-END id=raft_testconf.index_math*/

static_assert(!raft_test_server_index_is_valid(-1, 5));
static_assert(raft_test_server_index_is_valid(0, 5));
static_assert(raft_test_server_index_is_valid(4, 5));
static_assert(!raft_test_server_index_is_valid(5, 5));
static_assert(raft_test_wrapped_server_index(4, 1, 5) == 0);
static_assert(raft_test_wrapped_server_index(0, -1, 5) == 4);
static_assert(raft_test_wrapped_server_index(0, -6, 5) == 4);
static_assert(raft_test_connected_term_moved_on(false, 51, 50));
static_assert(!raft_test_connected_term_moved_on(true, 51, 50));
static_assert(raft_test_wait_leader_is_invalid(true, true, 50, 50));
static_assert(raft_test_wait_leader_is_invalid(false, false, 50, 50));
static_assert(raft_test_wait_leader_is_invalid(false, true, 51, 50));
static_assert(!raft_test_wait_leader_is_invalid(false, true, 50, 50));
static_assert(raft_test_should_record_agreement_command(7, 7));
static_assert(!raft_test_should_record_agreement_command(8, 7));

namespace {

// Command values in the RaftLab are non-negative. Keep -1 as an explicit
// missing-slot marker so a snapshot-restored server is not credited for log
// history that its new apply callback did not replay.
constexpr int kMissingCommittedCommand = -1;

}  // namespace

int _test_id_g = 0;

std::map<siteid_t, RaftFrame*> RaftTestConfig::replicas;
std::map<siteid_t, std::function<int(slotid_t, janus::Command)>>
    RaftTestConfig::commit_callbacks;
rusty::Mutex<std::map<siteid_t, std::vector<int>>>
    RaftTestConfig::committed_cmds{
        std::map<siteid_t, std::vector<int>>{}};
std::map<siteid_t, uint64_t> RaftTestConfig::rpc_count_last;

RaftTestConfig::RaftTestConfig(std::map<siteid_t, RaftFrame*>& replicas) {
  verify(RaftTestConfig::replicas.empty());
  RaftTestConfig::replicas = replicas;
  for (auto& pair : replicas) {
    auto svr = pair.first;
    auto frame = pair.second;
    {
      auto committed = committed_cmds.lock().unwrap();
      (*committed)[svr] = {kMissingCommittedCommand};
    }
    RaftTestConfig::rpc_count_last[svr] = 0;
    disconnected_[svr] = false;
  }
  th_ = std::thread([this](){ netctlLoop(); });
}

void RaftTestConfig::SetLearnerAction(void) {
  for (auto& pair : replicas) {
    auto svr = pair.first;
    auto frame = pair.second;
    RaftTestConfig::commit_callbacks[svr] =
        [svr](slotid_t slot, janus::Command md) -> int {
          verify(raft_test_should_record_agreement_command(
              md.kind_, TpcCommitCommand::static_kind()));
          const auto commit_cmd = marshallable_cast<TpcCommitCommand>(md);
          verify(commit_cmd.is_some());
          Log_debug("server {} committed value {} at slot {}",
                    svr, commit_cmd.unwrap()->tx_id_, slot);
          RaftTestConfig::RecordCommittedCommand(
              svr, slot, commit_cmd.unwrap()->tx_id_);
          return 0;
        };
    // Runtime apply takes the same gate before copying/invoking app_next_.
    // Replace the fail-closed startup placeholder without a data race.
    std::lock_guard<std::mutex> apply_lock(
        frame->svr_->LabApplyMutex());
    frame->svr_->reg_learner_action(RaftTestConfig::commit_callbacks[svr]);
  }
}

int RaftTestConfig::OneLeader(int expected) {
  return waitOneLeader(true, expected);
}

bool RaftTestConfig::NoLeader(void) {
  int r = waitOneLeader(false, -1);
  return r == -1;
}

int RaftTestConfig::waitOneLeader(bool want_leader, int expected) {
  uint64_t mostRecentTerm = 0, term;
  int leader = -1;  // Use int instead of siteid_t to avoid unsigned conversion
  bool isleader;
  
  for (int retry = 0; retry < 10; retry++) {
    Fiber::sleep(ELECTIONTIMEOUT/10);
    leader = -1;
    mostRecentTerm = 0;
    for (auto& pair : replicas) {
      auto svr = pair.first;
      auto frame = pair.second;
      // ignore disconnected servers
      if (frame->svr_->IsDisconnected()) {
        continue;
      }
      frame->svr_->GetState(&isleader, &term);
      if (isleader) {
        if (term == mostRecentTerm) {
          Failed("multiple leaders elected in term %ld", term);
          return -2;
        } else if (term > mostRecentTerm) {
          leader = svr;
          mostRecentTerm = term;
          Log_debug("found leader {} with term {}", leader, term);
        }
      }
    }
    if (leader != -1) {
      if (!want_leader) {
        Failed("leader elected despite lack of quorum");
      } else if (expected >= 0 && leader != expected) {
        Failed("unexpected leader change, expecting %d, got %d", expected, leader);
        return -3;
      }
      return leader;
    }
  }
  if (want_leader) {
    Log_debug("failing, timeout?");
    Failed("waited too long for leader election");
  }
  return -1;
}

bool RaftTestConfig::TermMovedOn(uint64_t term) {
  for (auto& pair : replicas) {
    auto frame = pair.second;
    uint64_t curTerm;
    bool isLeader;
    frame->svr_->GetState(&isLeader, &curTerm);
    const bool disconnected = frame->svr_->IsDisconnected();
    if (raft_test_connected_term_moved_on(disconnected, curTerm, term)) {
      return true;
    }
  }
  return false;
}

uint64_t RaftTestConfig::OneTerm(void) {
  if (replicas.empty()) return -1;
  
  uint64_t term, curTerm;
  bool isLeader;
  auto first_frame = replicas.begin()->second;
  first_frame->svr_->GetState(&isLeader, &term);
  
  for (auto it = ++replicas.begin(); it != replicas.end(); ++it) {
    auto frame = it->second;
    frame->svr_->GetState(&isLeader, &curTerm);
    if (curTerm != term) {
      return -1;
    }
  }
  return term;
}

// @safe - The guarded map makes resize/assignment atomic with oracle readers.
void RaftTestConfig::RecordCommittedCommand(
    siteid_t svr, slotid_t slot, int cmd) {
  verify(cmd != kMissingCommittedCommand);
  verify(slot <= static_cast<slotid_t>(std::numeric_limits<size_t>::max() - 1));
  auto committed = committed_cmds.lock().unwrap();
  auto& commands = (*committed)[svr];
  const size_t slot_index = static_cast<size_t>(slot);
  if (commands.size() <= slot_index) {
    commands.resize(slot_index + 1, kMissingCommittedCommand);
  }
  verify(commands[slot_index] == kMissingCommittedCommand ||
         commands[slot_index] == cmd);
  commands[slot_index] = cmd;
}

int RaftTestConfig::NCommitted(uint64_t index) {
  auto committed = committed_cmds.lock().unwrap();
  int cmd,n = 0;
  for (auto& pair : replicas) {
    auto svr = pair.first;
    auto commands = committed->find(svr);
    if (commands != committed->end() &&
        commands->second.size() > index &&
        commands->second[index] != kMissingCommittedCommand) {
      auto curcmd = commands->second[index];
      if (n == 0) {
        cmd = curcmd;
      } else {
        if (curcmd != cmd) {
          return -1;
        }
      }
      n++;
    }
  }
  return n;
}

bool RaftTestConfig::Start(siteid_t svr, int cmd, uint64_t *index, uint64_t *term) {
  auto it = replicas.find(svr);
  if (it == replicas.end())
  {
    Log_error("Server {} not found in replicas map", svr);
    return false;
  }

  // Construct a TpcCommitCommand containing cmd as its tx_id_. Use the same
  // replication-native inner payload as the production Raft worker.
  auto cmdptr = rusty::Arc<TpcCommitCommand>::make();
  LogEntry raw_log;
  verify(raft::EncodeApplicationLog(nullptr, 0, 0, &raw_log.log_entry));
  raw_log.length = static_cast<int>(raw_log.log_entry.size());
  {
    auto& mut_cmd = cmdptr.get_mut().unwrap();
    mut_cmd.tx_id_ = cmd;
    mut_cmd.cmd_ = rusty::Arc<LogEntry>::make(std::move(raw_log));
  }
  // call Start()
  // Log_info("Start: Calling Start() on server {} for command {}", svr, cmd);
  const RaftStartResult result =
      it->second->svr_->Start(std::move(cmdptr), index, term);
  // Log_info("Start: Server {} Start() for command {} returned {}, index={}, term={}",
  //          svr, cmd, result ? "SUCCESS" : "FAILED", *index, *term);
  return raft_server_start_was_appended(result);
}

int RaftTestConfig::Wait(uint64_t index, int n, uint64_t term) {
  int nc = 0, i;
  auto to = 10000; // 10 milliseconds
  for (i = 0; i < 30; i++) {
    nc = NCommitted(index);
    if (nc < 0) {
      return -3; // values differ
    } else if (nc >= n) {
      break;
    }
    create_sp_timeout_event(to)->wait();
    if (to < 1000000) {
      to *= 2;
    }
    if (TermMovedOn(term)) {
      return -2; // term changed
    }
  }
  if (i == 30) {
    return -1; // timeout
  }
  auto committed = committed_cmds.lock().unwrap();
  for (auto& pair : replicas) {
    auto svr = pair.first;
    auto commands = committed->find(svr);
    if (commands != committed->end() &&
        commands->second.size() > index &&
        commands->second[index] != kMissingCommittedCommand) {
      return commands->second[index];
    }
  }
  verify(0);
}

uint64_t RaftTestConfig::DoAgreement(int cmd, int n, bool retry) {
  Log_info("DoAgreement: Starting agreement for command {}, expecting {} servers, retry={}", cmd, n, retry ? "true" : "false");
  auto start = chrono::steady_clock::now();
  while ((chrono::steady_clock::now() - start) < chrono::seconds{10}) {
    // Fiber::sleep(50000);
    usleep(50000);
    // Call Start() to all servers until leader is found
    siteid_t ldr = -1;
    uint64_t index, term;
    // Log_info("DoAgreement: Trying to find leader for command {}", cmd);
    for (auto& pair : replicas) {
      auto svr = pair.first;
      auto frame = pair.second;
      // skip disconnected servers
      if (frame->svr_->IsDisconnected()) {
        // Log_info("DoAgreement: Skipping disconnected server {} for command {}", svr, cmd);
        continue;
      }
      Log_info("DoAgreement: Attempting Start() on server {} for command {}", svr, cmd);
      if (Start(svr, cmd, &index, &term)) {
        Log_info("DoAgreement: SUCCESS - found leader {} for command {}, index={}, term={}", svr, cmd, index, term);
        ldr = svr;
        break;
      } else {
        // Log_info("DoAgreement: FAILED - server {} rejected Start() for command {}", svr, cmd);
      }
    }
    if (ldr != -1) {
      // If Start() successfully called, wait for agreement
      // Log_info("DoAgreement: Waiting for agreement on command {} at index {}", cmd, index);
      auto start2 = chrono::steady_clock::now();
      int nc;
      int iteration = 0;
      while ((chrono::steady_clock::now() - start2) < chrono::seconds{10}) {
        if (retry) {
          // If leadership/term moved on, this index may be stale. Retry Start() quickly.
          if (TermMovedOn(term)) {
            Log_info("DoAgreement: Term moved on from {} while waiting for command {} at index {}, retrying Start()", term, cmd, index);
            break;
          }

          bool isLeader = false;
          uint64_t curTerm = 0;
          auto ldr_it = replicas.find(ldr);
          if (ldr_it == replicas.end() || ldr_it->second == nullptr || ldr_it->second->svr_ == nullptr) {
            Log_info("DoAgreement: Leader {} disappeared while waiting for command {} at index {}, retrying Start()", ldr, cmd, index);
            break;
          }
          ldr_it->second->svr_->GetState(&isLeader, &curTerm);
          const bool disconnected =
              ldr_it->second->svr_->IsDisconnected();
          if (raft_test_wait_leader_is_invalid(
                  disconnected, isLeader, curTerm, term)) {
            Log_info("DoAgreement: Leader changed (server={} isLeader={} term={} expected_term={}) while waiting for command {} at index {}, retrying Start()",
                     ldr, isLeader ? 1 : 0, curTerm, term, cmd, index);
            break;
          }
        }

        nc = NCommitted(index);
        Log_info("DoAgreement: Iteration {} - NCommitted({}) returned {} for command {}", iteration++, index, nc, cmd);
        if (nc < 0) {
          // Log_info("DoAgreement: ERROR - NCommitted returned {} (values differ) for command {} at index {}", nc, cmd, index);
          break;
        } else if (nc >= n) {
          // Log_info("DoAgreement: SUCCESS - {} servers committed index {} for command {}", nc, index, cmd);
          auto committed = committed_cmds.lock().unwrap();
          for (auto& pair : replicas) {
            auto svr = pair.first;
            auto commands = committed->find(svr);
            if (commands != committed->end() &&
                commands->second.size() > index &&
                commands->second[index] != kMissingCommittedCommand) {
              // Log_info("DoAgreement: Found commit log on server {} at index {}", svr, index);
              auto cmd2 = commands->second[index];
              // Log_info("DoAgreement: Server {} committed command {} at index {} (expected {})", svr, cmd2, index, cmd);
              if (cmd == cmd2) {
                // Log_info("DoAgreement: AGREEMENT REACHED - command {} successfully committed at index {}", cmd, index);
                return index;
              } else {
                // Log_info("DoAgreement: COMMAND MISMATCH - expected {}, got {} at index {}", cmd, cmd2, index);
                break;
              }
            }
          }
          break;
        }
        // Log_info("DoAgreement: Waiting... only {}/{} servers committed index {} for command {}", nc, n, index, cmd);
        // Fiber::sleep(50000);
        usleep(20000);
      }
      // Log_info("DoAgreement: Agreement wait loop ended - {} committed server at index {} for command {}", nc, index, cmd);
      if (!retry) {
          // Log_info("DoAgreement: FAILED - no retry allowed for command {}", cmd);
          return 0;
        }
    } else {
      // If no leader found, sleep and retry.
      // Log_info("DoAgreement: No leader found for command {}, sleeping and retrying", cmd);
      // Fiber::sleep(50000)
      usleep(50000);
    }
  }
  // Log_info("DoAgreement: FAILED - timeout reached for command {}", cmd);
  return 0;
}

// removed
//   `shared_ptr<CommitIndex> RaftTestConfig::StartAgreement(siteid_t,
//    int)`
// — body started with `verify(0); // this function has been replaced
// by Start()`.  The function had been intentionally disabled and
// replaced by `RaftTestConfig::Start` long ago; `grep StartAgreement`
// returned only the declaration in `testconf.h:99` and the
// definition.  Header declaration also went away in the same commit.

void RaftTestConfig::Disconnect(siteid_t svr) {
  std::lock_guard<std::mutex> lk(disconnect_mtx_);
  verify(!disconnected_[svr]);
  disconnect(svr);
  disconnected_[svr] = true;
}

void RaftTestConfig::Reconnect(siteid_t svr) {
  std::lock_guard<std::mutex> lk(disconnect_mtx_);
  verify(disconnected_[svr]);
  reconnect(svr);
  disconnected_[svr] = false;
}

int RaftTestConfig::NDisconnected(void) {
  int count = 0;
  for (auto& pair : disconnected_) {
    if (pair.second)
      count++;
  }
  return count;
}

void RaftTestConfig::SetUnreliable(bool unreliable) {
  std::unique_lock<std::mutex> lk(cv_m_);
  verify(!finished_);
  if (unreliable) {
    verify(!unreliable_);
    // lk acquired cv_m_ in state 1 or 0
    unreliable_ = true;
    // if cv_m_ was in state 1, must signal cv_ to wake up netctlLoop
    lk.unlock();
    cv_.notify_one();
  } else {
    verify(unreliable_);
    // lk acquired cv_m_ in state 2 or 0
    unreliable_ = false;
    // wait until netctlLoop moves cv_m_ from state 2 (or 0) to state 1,
    // restoring the network to reliable state in the process.
    lk.unlock();
    lk.lock();
  }
}

bool RaftTestConfig::IsUnreliable(void) {
  return unreliable_;
}

// @unsafe - Coordinates native netctl shutdown and reactor-fiber Raft barriers.
void RaftTestConfig::Shutdown(void) {
  // trigger netctlLoop shutdown
  {
    std::unique_lock<std::mutex> lk(cv_m_);
    verify(!finished_);
    // lk acquired cv_m_ in state 0, 1, or 2
    finished_ = true;
    // if cv_m_ was in state 1, must signal cv_ to wake up netctlLoop
    lk.unlock();
    cv_.notify_one();
  }
  // wait for netctlLoop thread to exit
  th_.join();
  // Reconnect() all Deconnect()ed servers
  for (auto& pair : disconnected_) {
    if (pair.second) {
      Reconnect(pair.first);
    }
  }

  // This method runs inside the RaftLab reactor fiber.  Quiesce every current
  // server through its owner-thread completion barrier before the harness
  // stops poll threads.
  for (auto& pair : replicas) {
    if (pair.second != nullptr && pair.second->svr_) {
      pair.second->svr_->PrepareForShutdown();
    }
  }
}

uint64_t RaftTestConfig::RpcCount(siteid_t svr, bool reset) {
  std::lock_guard<std::recursive_mutex> lk(
    RaftTestConfig::replicas[svr]->commo_->rpc_mtx_);
  uint64_t count = RaftTestConfig::replicas[svr]->commo_->rpc_count_;
  uint64_t count_last = RaftTestConfig::rpc_count_last[svr];
  if (reset) {
    RaftTestConfig::rpc_count_last[svr] = count;
  }
  verify(count >= count_last);
  return count - count_last;
}

uint64_t RaftTestConfig::RpcTotal(void) {
  uint64_t total = 0;
  for (auto& pair : replicas) {
    total += RaftTestConfig::replicas[pair.first]->commo_->rpc_count_;
  }
  return total;
}

bool RaftTestConfig::ServerCommitted(siteid_t svr, uint64_t index, int cmd) {
  auto committed = committed_cmds.lock().unwrap();
  auto commands = committed->find(svr);
  if (commands == committed->end() || commands->second.size() <= index)
    return false;
  return commands->second[index] == cmd;
}

void RaftTestConfig::netctlLoop(void) {
  bool isdown;
  // cv_m_ unlocked state 0 (finished_ == false)
  std::unique_lock<std::mutex> lk(cv_m_);
  while (!finished_) {
    if (!unreliable_) {
      {
        std::lock_guard<std::mutex> prlk(disconnect_mtx_);
        // unset all unreliable-related disconnects and slows
        for (const auto& pair : replicas) {
          siteid_t svr = pair.first;
          if (!disconnected_[svr]) {
            reconnect(svr, true);
            slow(svr, 0);
          }
        }
      }
      // sleep until unreliable_ or finished_ is set
      // cv_m_ unlocked state 1 (unreliable_ == false && finished_ == false)
      cv_.wait(lk, [this](){ return unreliable_ || finished_; });
      continue;
    }
    {
      std::lock_guard<std::mutex> prlk(disconnect_mtx_);
      for (const auto& pair : replicas) {
        siteid_t svr = pair.first;
        // skip server if it was disconnected using Disconnect()
        if (disconnected_[svr]) {
          continue;
        }
        // server has DOWNRATE_N / DOWNRATE_D chance of being down
        if ((rand() % DOWNRATE_D) < DOWNRATE_N) {
          // disconnect server if not already disconnected in the previous period
          disconnect(svr, true);
        } else {
          // Server not down: random slow timeout
          // Reconnect server if it was disconnected in the previous period
          reconnect(svr, true);
          // server's slow timeout should be btwn 0-(MAXSLOW-1) ms
          slow(svr, rand() % MAXSLOW);
        }
      }
    }
    // change unreliable state every 0.1s
    usleep(100000);
    lk.unlock();
    usleep(10000);

    // cv_m_ unlocked state 2 (unreliable_ == true && finished_ == false)
    lk.lock();
  }
  // If network is still unreliable, unset it
  if (unreliable_) {
    unreliable_ = false;
    {
      std::lock_guard<std::mutex> prlk(disconnect_mtx_);
      // unset all unreliable-related disconnects and slows
      for (const auto& pair : replicas) {
        siteid_t svr = pair.first;
        if (!disconnected_[svr]) {
          reconnect(svr, true);
          slow(svr, 0);
        }
      }
    }
  }
  // cv_m_ unlocked state 3 (unreliable_ == false && finished_ == true)
}

bool RaftTestConfig::isDisconnected(siteid_t svr) {
  std::lock_guard<std::recursive_mutex> lk(connection_m_);
  auto it = RaftTestConfig::replicas.find(svr);
  if (it == RaftTestConfig::replicas.end() || it->second == nullptr || !it->second->svr_) {
    // Missing replica is effectively disconnected for test-control purposes.
    return true;
  }
  return it->second->svr_->IsDisconnected();
}

void RaftTestConfig::disconnect(siteid_t svr, bool ignore) {
  std::lock_guard<std::recursive_mutex> lk(connection_m_);
  auto it = RaftTestConfig::replicas.find(svr);
  if (it == RaftTestConfig::replicas.end() || it->second == nullptr || !it->second->svr_) {
    if (!ignore) {
      Log_warn("[RAFT-TEST] disconnect({}): replica not present", svr);
    }
    return;
  }
  if (!it->second->svr_->IsDisconnected()) {
    // simulate disconnected server
    // `true` explicitly: Disconnect is a DSL method now and the DSL has no
    // default arguments.
    it->second->svr_->Disconnect(true);
  } else if (!ignore) {
    verify(0);
  }
}

void RaftTestConfig::reconnect(siteid_t svr, bool ignore) {
  std::lock_guard<std::recursive_mutex> lk(connection_m_);
  auto it = RaftTestConfig::replicas.find(svr);
  if (it == RaftTestConfig::replicas.end() || it->second == nullptr || !it->second->svr_) {
    if (!ignore) {
      Log_warn("[RAFT-TEST] reconnect({}): replica not present", svr);
    }
    return;
  }
  if (it->second->svr_->IsDisconnected()) {
    // simulate reconnected server
    it->second->svr_->Reconnect();
  } else if (!ignore) {
    verify(0);
  }
}

void RaftTestConfig::slow(siteid_t svr, uint32_t msec) {
  // Instead of using reactor's slow mode, use Fiber::Sleep
  // This will introduce the same delay but without needing reactor changes
  usleep(msec * 1000);  // Convert msec to microseconds
}

// @unsafe - Locks the legacy test mutex and returns a borrowed server pointer.
RaftServer *RaftTestConfig::GetServer(siteid_t svr) {
  std::lock_guard<std::recursive_mutex> lk(connection_m_);
  auto it = RaftTestConfig::replicas.find(svr);
  if (it == RaftTestConfig::replicas.end() || it->second == nullptr ||
      !it->second->svr_) {
    return nullptr;
  }
  return it->second->svr_;
}

siteid_t RaftTestConfig::mapServerId(siteid_t server_id) const {
  // Find the server_id in the replicas map and return its position (0-4)
  int index = 0;
  for (const auto& pair : replicas) {
    if (pair.first == server_id) {
      return index;
    }
    index++;
  }
  // If not found, return the original ID (this should not happen in normal operation)
  return server_id;
}

siteid_t RaftTestConfig::getServerIdByIndex(int index) const {
  // Get server ID by its position in the replicas map (0-4)
  if (!raft_test_server_index_is_valid(index, NSERVERS)) {
    // Index out of range, return -1
    return -1;
  }
  
  int i = 0;
  for (const auto& pair : replicas) {
    if (i == index) {
      return pair.first;
    }
    i++;
  }
  // If we get here, something is wrong with the replicas map
  // This should not happen in normal operation
  return -1;
}

siteid_t RaftTestConfig::getNextServerId(siteid_t current_server_id, int offset) const {
  // Find current server's index and add offset, wrapping around
  int current_index = -1;
  int i = 0;
  for (const auto& pair : replicas) {
    if (pair.first == current_server_id) {
      current_index = i;
      break;
    }
    i++;
  }
  
  if (current_index == -1) {
    return current_server_id; // Return original if not found
  }
  
  // Calculate new index with wrapping
  int new_index = raft_test_wrapped_server_index(
      current_index, offset, NSERVERS);
  
  siteid_t result = getServerIdByIndex(new_index);
  if (result == -1) {
    // If getServerIdByIndex returns -1, return the original server ID
    // This should not happen in normal operation, but provides safety
    return current_server_id;
  }

  return result;
}

#endif

}
