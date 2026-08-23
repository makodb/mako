#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "testconf.h"
#include "../config.h"
#include "frame.h"
#include "service.h"
#include "commo.h"
#include "recovery_manager.hpp"
#include "application_log.h"
#include "../replication_log_entry.h"

#include "rrr/rrr.hpp"

import std;

namespace janus {

#ifdef RAFT_TEST_CORO

int _test_id_g = 0;

std::map<siteid_t, RaftFrame*> RaftTestConfig::replicas;
std::map<siteid_t, std::function<int(int, janus::Command)>>
    RaftTestConfig::commit_callbacks;
std::map<siteid_t, std::vector<int>> RaftTestConfig::committed_cmds;
std::map<siteid_t, uint64_t> RaftTestConfig::rpc_count_last;

RaftTestConfig::RaftTestConfig(std::map<siteid_t, RaftFrame*>& replicas) {
  verify(RaftTestConfig::replicas.empty());
  RaftTestConfig::replicas = replicas;
  for (auto& pair : replicas) {
    auto svr = pair.first;
    auto frame = pair.second;
    RaftTestConfig::committed_cmds[svr].push_back(-1);
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
        [svr](int slot, janus::Command md) -> int {
          verify(md.kind_ == TpcCommitCommand::static_kind());
          const auto commit_cmd = marshallable_cast<TpcCommitCommand>(md);
          verify(commit_cmd.is_some());
          Log_debug("server {} committed value {} at slot {}",
                    svr, commit_cmd.unwrap()->tx_id_, slot);
          RaftTestConfig::committed_cmds[svr].push_back(commit_cmd.unwrap()->tx_id_);
          return 0;
        };
    frame->svr_->RegLearnerAction(RaftTestConfig::commit_callbacks[svr]);
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
    if (curTerm > term) {
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

int RaftTestConfig::NCommitted(uint64_t index) {
  int cmd,n = 0;
  for (auto& pair : replicas) {
    auto svr = pair.first;
    if (committed_cmds[svr].size() > index) {
      auto curcmd = committed_cmds[svr][index];
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
  bool result = it->second->svr_->Start(std::move(cmdptr), index, term);
  // Log_info("Start: Server {} Start() for command {} returned {}, index={}, term={}",
  //          svr, cmd, result ? "SUCCESS" : "FAILED", *index, *term);
  return result;
}

bool RaftTestConfig::StartWithCallback(siteid_t svr, int cmd, uint64_t *index, uint64_t *term,
                                       std::function<void(CommitStatus)> callback) {
  // First, call Start to submit the command
  bool result = Start(svr, cmd, index, term);
  if (!result) {
    return false;
  }

  // If successful, register the callback for commit notifications
  auto it = replicas.find(svr);
  if (it != replicas.end()) {
    it->second->svr_->RegisterCommitCallback(*index, std::move(callback));
  }

  return result;
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
  for (auto& pair : replicas) {
    auto svr = pair.first;
    if (committed_cmds[svr].size() > index) {
      return committed_cmds[svr][index];
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
          if (!isLeader || curTerm != term) {
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
          for (auto& pair : replicas) {
            auto svr = pair.first;
            if (committed_cmds[svr].size() > index) {
              // Log_info("DoAgreement: Found commit log on server {} at index {}", svr, index);
              auto cmd2 = committed_cmds[svr][index];
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
  if (committed_cmds[svr].size() <= index)
    return false;
  return committed_cmds[svr][index] == cmd;
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
    it->second->svr_->Disconnect();
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

RaftServer *RaftTestConfig::GetServer(siteid_t svr) {
  return RaftTestConfig::replicas[svr]->svr_.get();
}

void RaftTestConfig::Kill(siteid_t svr) {
  std::lock_guard<std::recursive_mutex> lk(connection_m_);
  std::lock_guard<std::mutex> lk2(disconnect_mtx_);

  Log_info("[RAFT-TEST] Killing server {}", svr);

  auto it = replicas.find(svr);
  if (it == replicas.end()) {
    Log_error("[RAFT-TEST] Server {} not found in replicas", svr);
    return;
  }

  // Mark as disconnected
  disconnected_[svr] = true;

  // Clear atomic pointer in RaftServiceImpl BEFORE deleting frame
  // This ensures in-flight RPCs get nullptr and return failure gracefully
  RaftServiceImpl::UpdateServer(svr, nullptr);

  // Disconnect to save RPC proxies before deletion
  RaftFrame* frame = it->second;
  if (frame && frame->svr_) {
    frame->svr_->Disconnect(true);
  }

  // Sleep to allow pending coroutines to complete
  // The election timer coroutine sleeps for HEARTBEAT_INTERVAL * 2-4 (200-400ms)
  // We must wait longer than this to ensure stale coroutines exit before we delete
  usleep(450000); // 450ms > max election timer sleep (400ms)

  // Delete the frame (this will cascade delete svr_ and commo_)
  delete frame;

  // Remove from replicas map
  replicas.erase(it);

  // Clear committed commands for this server
  committed_cmds[svr].clear();
  committed_cmds[svr].push_back(-1); // Re-initialize with sentinel

  // Reset RPC count
  rpc_count_last[svr] = 0;

  Log_info("[RAFT-TEST] Server {} killed successfully", svr);
}

void RaftTestConfig::Restart(siteid_t svr) {
  std::lock_guard<std::recursive_mutex> lk(connection_m_);
  std::lock_guard<std::mutex> lk2(disconnect_mtx_);

  Log_info("[RAFT-TEST] Restarting server {}", svr);

  // Check if server is already running
  if (replicas.find(svr) != replicas.end()) {
    Log_error("[RAFT-TEST] Server {} is already running, cannot restart", svr);
    return;
  }

  // Get the config to find site info
  auto config = Config::GetConfig();
  verify(config != nullptr);

  // Find the site info for this server ID
  Config::SiteInfo* site_info = nullptr;
  for (auto& site : config->sites_) {
    if (site.id == svr) {
      site_info = &site;
      break;
    }
  }

  if (!site_info) {
    Log_error("[RAFT-TEST] Could not find site info for server {}", svr);
    return;
  }

  // Create new RaftFrame
  RaftFrame* frame = new RaftFrame();
  frame->site_info_ = site_info;

  // Create new RaftServer (persistence will be loaded when EnsureSetup is called)
  frame->svr_ = std::make_unique<RaftServer>();
  frame->svr_->site_id_ = svr;
  frame->svr_->partition_id_ = site_info->partition_id_;
  frame->svr_->loc_id_ = site_info->locale_id;

  // Fix 2: Get the ORIGINAL poll thread from RaftServiceImpl (survives Kill)
  // This ensures inbound RPCs (via RPC server) and outbound RPCs (via Commo)
  // use the SAME poll thread, eliminating race conditions on RaftServer state
  auto poll_thread = RaftServiceImpl::GetPollThread(svr);
  if (poll_thread.is_some()) {
    frame->commo_ = std::make_unique<RaftCommo>(std::move(poll_thread));
  } else {
    Log_warn("[RAFT-RESTART] site {}: poll thread not found, creating new one", svr);
    frame->commo_ = std::make_unique<RaftCommo>(rusty::None);
  }
  frame->commo_->loc_id_ = site_info->locale_id;

  // Set commo_ in server before initializing
  frame->svr_->commo_ = frame->commo_.get();

  // Manually initialize persistence and load state (without starting coroutines)
  const char* persistence_flag = std::getenv("MAKO_RAFT_PERSISTENCE");
  bool should_enable = (persistence_flag &&
                       (strcmp(persistence_flag, "1") == 0 ||
                        strcmp(persistence_flag, "true") == 0));

  if (should_enable) {
    // Set async persistence flag on the server (default: sync)
    const char* async_flag = std::getenv("MAKO_RAFT_ASYNC_PERSISTENCE");
    frame->svr_->async_persistence_ = (async_flag &&
                                       (strcmp(async_flag, "1") == 0 ||
                                        strcmp(async_flag, "true") == 0));

    Log_info("[RAFT-TEST-RESTART] Loading persistence for site {} (mode={})",
             svr, frame->svr_->async_persistence_ ? "async" : "sync");

    // Create RecoveryConfig
    raft::RecoveryConfig config;
    std::string base_path = "/tmp";
    config.storage_path = base_path + "/raft_" + std::to_string(svr) +
                         "_partition_" + std::to_string(site_info->partition_id_);

    // Create RecoveryManager and storage
    raft::RecoveryManager manager(config);
    auto storage = manager.create_storage();

    if (storage) {
      // Use RecoveryManager to orchestrate recovery
      auto result = manager.recover(
        [frame](std::shared_ptr<janus::raft::LogStorage> s) { frame->svr_->SetLogStorage(s); },
        [frame]() { return frame->svr_->RecoverFromStorage(); },
        [frame](raft::RecoveryResult& r) {
          r.recovered_term = frame->svr_->currentTerm;
          r.recovered_entries = frame->svr_->raft_logs_.size();
        }
      );

      if (result.success) {
        Log_info("[RAFT-TEST-RESTART] Loaded: term={} vote={} lastLogIndex={} (mode={})",
                 frame->svr_->currentTerm, frame->svr_->vote_for_,
                 frame->svr_->lastLogIndex, static_cast<int>(result.mode));
      } else {
        Log_error("[RAFT-TEST-RESTART] Recovery failed: {}", result.error_message.c_str());
      }
    }
  }

  // Record startup timestamp for grace period logic (same as Setup())
  frame->svr_->startup_timestamp_ = Time::now(false);

  // CRITICAL: Mark Setup() as already done to prevent EnsureSetup() from calling it again
  // This prevents double-initialization of persistence which would reset the loaded state
  frame->svr_->heartbeat_setup_ = true;

  // Start the heartbeat loop and election timer manually since we're skipping Setup()
  // CRITICAL (Fix 2 part 2): Must add coroutines to the CORRECT poll thread!
  // Using Fiber::create_run would schedule on the current reactor (site 0's test thread),
  // not on this server's poll thread. We must use poll_thread->add() instead.
#ifdef RAFT_TEST_CORO
  if (frame->svr_->heartbeat_ && frame->commo_->rpc_poll_.is_some()) {
    auto& poll_thread = frame->commo_->rpc_poll_.as_ref().unwrap();

    // Add HeartbeatLoop as a job to the correct poll thread
    auto hb_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([frame]() {
      Fiber::create_run([frame]() {
        frame->svr_->HeartbeatLoop();
      });
    }));
    poll_thread->add(rusty::Arc<Job>(hb_job));

    // Add election timer as a job to the correct poll thread
    if (frame->svr_->failover_) {
      auto election_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([frame]() {
        Fiber::create_run([frame]() {
          frame->svr_->StartElectionTimer();
        });
      }));
      poll_thread->add(rusty::Arc<Job>(election_job));
    }

  }
#endif

  // Re-register learner action BEFORE adding to replicas map
  commit_callbacks[svr] =
      [svr](int slot, janus::Command md) -> int {
        verify(md.kind_ == TpcCommitCommand::static_kind());
        const auto commit_cmd = marshallable_cast<TpcCommitCommand>(md);
        verify(commit_cmd.is_some());
        Log_debug("server {} committed value {} at slot {}",
                  svr, commit_cmd.unwrap()->tx_id_, slot);
        RaftTestConfig::committed_cmds[svr].push_back(commit_cmd.unwrap()->tx_id_);
        return 0;
      };
  frame->svr_->RegLearnerAction(commit_callbacks[svr]);

  // Update atomic pointer in RaftServiceImpl to point to the new server
  // This allows the existing RPC service to forward requests to the new server
  RaftServiceImpl::UpdateServer(svr, frame->svr_.get());

  // Add back to replicas map - EnsureSetup() will be called lazily on first RPC to start coroutines
  replicas[svr] = frame;

  // Mark as connected in test config (don't call Reconnect, proxies will be restored on demand)
  disconnected_[svr] = false;

  // Reset RPC count
  rpc_count_last[svr] = 0;

  // Notify all other servers to reconnect their client connections to this server
  // This is needed because after Kill/Restart, other servers' TCP connections to us are stale
  if (frame->commo_ != nullptr) {
    Log_info("[RAFT-TEST] Sending NotifyRestart from site {} to all peers", svr);
    auto commo = dynamic_cast<RaftCommo*>(frame->commo_.get());
    if (commo != nullptr) {
      commo->SendNotifyRestart(svr, frame->svr_->partition_id_);
    }
  }

  Log_info("[RAFT-TEST] Server {} restarted successfully (term={}, lastLogIndex={})",
           svr, frame->svr_->currentTerm, frame->svr_->lastLogIndex);
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
  if (index < 0 || index >= NSERVERS) {
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
  int new_index = (current_index + offset) % NSERVERS;
  if (new_index < 0) {
    new_index += NSERVERS;
  }
  
  siteid_t result = getServerIdByIndex(new_index);
  if (result == -1) {
    // If getServerIdByIndex returns -1, return the original server ID
    // This should not happen in normal operation, but provides safety
    return current_server_id;
  }

  return result;
}

// ============================================================================
// SPECULATIVE RAFT STATE QUERIES
// ============================================================================

bool RaftTestConfig::IsSecuredLeader(siteid_t svr) {
  auto server = GetServer(svr);
  if (!server || !server->IsLeader()) {
    return false;
  }
  return server->IsSecuredLeader();
}

uint64_t RaftTestConfig::GetSpecCommitIndex(siteid_t svr) {
  auto server = GetServer(svr);
  if (!server) {
    return 0;
  }
  return server->GetSpecCommitIndex();
}

uint64_t RaftTestConfig::GetSecuredLogIndex(siteid_t svr) {
  auto server = GetServer(svr);
  if (!server) {
    return 0;
  }
  return server->GetSecuredLogIndex();
}

size_t RaftTestConfig::GetSpecVotersCount(siteid_t svr) {
  auto server = GetServer(svr);
  if (!server) {
    return 0;
  }
  return server->GetSpecVotersCount();
}

size_t RaftTestConfig::GetDurableVotersCount(siteid_t svr) {
  auto server = GetServer(svr);
  if (!server) {
    return 0;
  }
  return server->GetDurableVotersCount();
}

bool RaftTestConfig::VerifySpecInvariants(siteid_t svr) {
  auto server = GetServer(svr);
  if (!server) {
    return true;  // Non-existent server trivially satisfies invariants
  }

  uint64_t securedLogIndex = server->GetSecuredLogIndex();
  uint64_t specCommitIndex = server->GetSpecCommitIndex();
  uint64_t lastLogIndex = server->GetLastLogIndex();

  // Invariant: securedLogIndex <= specCommitIndex <= lastLogIndex
  if (securedLogIndex > specCommitIndex) {
    Log_error("[SPEC-TEST] Invariant violation: securedLogIndex ({}) > specCommitIndex ({})",
              securedLogIndex, specCommitIndex);
    return false;
  }
  if (specCommitIndex > lastLogIndex) {
    Log_error("[SPEC-TEST] Invariant violation: specCommitIndex ({}) > lastLogIndex ({})",
              specCommitIndex, lastLogIndex);
    return false;
  }
  return true;
}

size_t RaftTestConfig::GetMemoryAckCount(siteid_t svr, uint64_t index) {
  auto server = GetServer(svr);
  if (!server) {
    return 0;
  }
  return server->GetMemoryAckCount(index);
}

size_t RaftTestConfig::GetDurableAckCount(siteid_t svr, uint64_t index) {
  auto server = GetServer(svr);
  if (!server) {
    return 0;
  }
  return server->GetDurableAckCount(index);
}

#endif

}
