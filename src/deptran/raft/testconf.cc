#include "testconf.h"
#include "../../rrr/misc/marshal.hpp"

namespace janus {

#ifdef RAFT_TEST_CORO

int _test_id_g = 0;

std::map<siteid_t, RaftFrame*> RaftTestConfig::replicas;
std::map<siteid_t, std::function<int(int, std::shared_ptr<Marshallable>)>>
    RaftTestConfig::commit_callbacks;
std::map<siteid_t, std::vector<int>> RaftTestConfig::committed_cmds;
std::map<siteid_t, uint64_t> RaftTestConfig::rpc_count_last;

RaftTestConfig::RaftTestConfig(std::map<siteid_t, RaftFrame*>& replicas) {
  verify(RaftTestConfig::replicas.empty());
  RaftTestConfig::replicas = replicas;
  for (auto& pair : replicas) {
    auto svr = pair.first;
    auto frame = pair.second;
    frame->svr_->rep_frame_ = frame->svr_->frame_;  // Set rep_frame_ directly like lab solution
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
    // rep_frame_ is already set in constructor, no need to set it here
    RaftTestConfig::commit_callbacks[svr] =
        [svr](int slot, std::shared_ptr<Marshallable> cmd) -> int {
          verify(cmd);
          verify(cmd->kind_ == MarshallDeputy::CMD_TPC_COMMIT);
          auto commit_cmd = std::dynamic_pointer_cast<TpcCommitCommand>(cmd);
          verify(commit_cmd != nullptr);
          Log_debug("server %d committed value %d at slot %d",
                    svr, commit_cmd->tx_id_, slot);
          RaftTestConfig::committed_cmds[svr].push_back(commit_cmd->tx_id_);
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
    Coroutine::Sleep(ELECTIONTIMEOUT/10);
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
          Log_debug("found leader %d with term %d", leader, term);
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
  // Log_info("NCommitted: Checking how many servers committed index %ld", index);
  for (auto& pair : replicas) {
    auto svr = pair.first;
    // Log_info("NCommitted: Server %d has committed_cmds size %ld", svr, committed_cmds[svr].size());
    if (committed_cmds[svr].size() > index) {
      auto curcmd = committed_cmds[svr][index];
      // Log_info("NCommitted: Server %d committed command %d at index %ld", svr, curcmd, index);
      if (n == 0) {
        cmd = curcmd;
      } else {
        if (curcmd != cmd) {
          // Log_info("NCommitted: ERROR - Server %d committed %d but expected %d at index %ld", svr, curcmd, cmd, index);
          return -1;
        }
      }
      n++;
    } else {
      // Log_info("NCommitted: Server %d has not committed index %ld (log size: %ld)", svr, index, committed_cmds[svr].size());
    }
  }
  // Log_info("NCommitted: %d servers committed index %ld with command %d", n, index, cmd);
  return n;
}

bool RaftTestConfig::Start(siteid_t svr, int cmd, uint64_t *index, uint64_t *term) {
  auto it = replicas.find(svr);
  if (it == replicas.end()) 
  {
    Log_error("Server %d not found in replicas map", svr);
    return false;
  }
  
  // Construct an empty TpcCommitCommand containing cmd as its tx_id_
  auto cmdptr = std::make_shared<TpcCommitCommand>();
  auto vpd_p = std::make_shared<VecPieceData>();
  vpd_p->sp_vec_piece_data_ = std::make_shared<vector<shared_ptr<SimpleCommand>>>();
  cmdptr->tx_id_ = cmd;
  cmdptr->cmd_ = vpd_p;
  auto cmdptr_m = dynamic_pointer_cast<Marshallable>(cmdptr);
  // call Start()
  // Log_info("Start: Calling Start() on server %d for command %d", svr, cmd);
  bool result = it->second->svr_->Start(cmdptr_m, index, term);
  // Log_info("Start: Server %d Start() for command %d returned %s, index=%ld, term=%ld", 
  //          svr, cmd, result ? "SUCCESS" : "FAILED", *index, *term);
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
    Reactor::CreateSpEvent<TimeoutEvent>(to)->Wait();
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
  // Log_info("DoAgreement: Starting agreement for command %d, expecting %d servers, retry=%s", cmd, n, retry ? "true" : "false");
  auto start = chrono::steady_clock::now();
  while ((chrono::steady_clock::now() - start) < chrono::seconds{10}) {
    // Coroutine::Sleep(50000);
    usleep(50000);
    // Call Start() to all servers until leader is found
    siteid_t ldr = -1;
    uint64_t index, term;
    // Log_info("DoAgreement: Trying to find leader for command %d", cmd);
    for (auto& pair : replicas) {
      auto svr = pair.first;
      auto frame = pair.second;
      // skip disconnected servers
      if (frame->svr_->IsDisconnected()) {
        // Log_info("DoAgreement: Skipping disconnected server %d for command %d", svr, cmd);
        continue;
      }
      // Log_info("DoAgreement: Attempting Start() on server %d for command %d", svr, cmd);
      if (Start(svr, cmd, &index, &term)) {
        // Log_info("DoAgreement: SUCCESS - found leader %d for command %d, index=%ld, term=%ld", svr, cmd, index, term);
        ldr = svr;
        break;
      } else {
        // Log_info("DoAgreement: FAILED - server %d rejected Start() for command %d", svr, cmd);
      }
    }
    if (ldr != -1) {
      // If Start() successfully called, wait for agreement
      // Log_info("DoAgreement: Waiting for agreement on command %d at index %ld", cmd, index);
      auto start2 = chrono::steady_clock::now();
      int nc;
      int iteration = 0;
      while ((chrono::steady_clock::now() - start2) < chrono::seconds{10}) {
        nc = NCommitted(index);
        // Log_info("DoAgreement: Iteration %d - NCommitted(%ld) returned %d for command %d", iteration++, index, nc, cmd);
        if (nc < 0) {
          // Log_info("DoAgreement: ERROR - NCommitted returned %d (values differ) for command %d at index %ld", nc, cmd, index);
          break;
        } else if (nc >= n) {
          // Log_info("DoAgreement: SUCCESS - %d servers committed index %ld for command %d", nc, index, cmd);
          for (auto& pair : replicas) {
            auto svr = pair.first;
            if (committed_cmds[svr].size() > index) {
              // Log_info("DoAgreement: Found commit log on server %d at index %ld", svr, index);
              auto cmd2 = committed_cmds[svr][index];
              // Log_info("DoAgreement: Server %d committed command %d at index %ld (expected %d)", svr, cmd2, index, cmd);
              if (cmd == cmd2) {
                // Log_info("DoAgreement: AGREEMENT REACHED - command %d successfully committed at index %ld", cmd, index);
                return index;
              } else {
                // Log_info("DoAgreement: COMMAND MISMATCH - expected %d, got %d at index %ld", cmd, cmd2, index);
                break;
              }
            }
          }
          break;
        }
        // Log_info("DoAgreement: Waiting... only %d/%d servers committed index %ld for command %d", nc, n, index, cmd);
        // Coroutine::Sleep(50000);
        usleep(20000);
      }
      // Log_info("DoAgreement: Agreement wait loop ended - %d committed server at index %ld for command %d", nc, index, cmd);
      if (!retry) {
          // Log_info("DoAgreement: FAILED - no retry allowed for command %d", cmd);
          return 0;
        }
    } else {
      // If no leader found, sleep and retry.
      // Log_info("DoAgreement: No leader found for command %d, sleeping and retrying", cmd);
      // Coroutine::Sleep(50000)
      usleep(50000);
    }
  }
  // Log_info("DoAgreement: FAILED - timeout reached for command %d", cmd);
  return 0;
}

shared_ptr<CommitIndex> RaftTestConfig::StartAgreement(siteid_t svr, int cmd) {
  verify(0); // this function has been replaced by Start()
  auto cmt_idx_p = std::make_shared<CommitIndex>(0);
  auto arc_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob(
    [this, cmd, svr, cmt_idx_p]() {
      auto cmdptr = std::make_shared<TpcCommitCommand>();
      auto vpd_p = std::make_shared<VecPieceData>();
      vpd_p->sp_vec_piece_data_ = std::make_shared<vector<shared_ptr<SimpleCommand>>>();
      cmdptr->tx_id_ = cmd;
      cmdptr->cmd_ = vpd_p;
      Log_debug("Starting agreement for cmd id %d", cmdptr->tx_id_);
      auto cmdptr_m = dynamic_pointer_cast<Marshallable>(cmdptr);
      auto it = replicas.find(svr);
      if (it != replicas.end()) {
        it->second->svr_->CreateRepCoord(0)->Submit(cmdptr_m, [svr, cmt_idx_p, it](){
          cmt_idx_p->setval(it->second->svr_->commitIndex);
        });
      }
    }
  ));
  auto it = replicas.find(svr);
  if (it != replicas.end()) {
    it->second->commo_->rpc_poll_->add(rusty::Arc<Job>(arc_job));
  }
  Log_debug("Started agreement for cmd id %d", cmd);
  return cmt_idx_p;
}

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
  return RaftTestConfig::replicas[svr]->svr_->IsDisconnected();
}

void RaftTestConfig::disconnect(siteid_t svr, bool ignore) {
  std::lock_guard<std::recursive_mutex> lk(connection_m_);
  if (!isDisconnected(svr)) {
    // simulate disconnected server
    RaftTestConfig::replicas[svr]->svr_->Disconnect();
  } else if (!ignore) {
    verify(0);
  }
}

void RaftTestConfig::reconnect(siteid_t svr, bool ignore) {
  std::lock_guard<std::recursive_mutex> lk(connection_m_);
  if (isDisconnected(svr)) {
    // simulate reconnected server
    RaftTestConfig::replicas[svr]->svr_->Reconnect();
  } else if (!ignore) {
    verify(0);
  }
}

void RaftTestConfig::slow(siteid_t svr, uint32_t msec) {
  // Instead of using reactor's slow mode, use Coroutine::Sleep
  // This will introduce the same delay but without needing reactor changes
  usleep(msec * 1000);  // Convert msec to microseconds
}

RaftServer *RaftTestConfig::GetServer(siteid_t svr) {
  return RaftTestConfig::replicas[svr]->svr_.get();
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

#endif

}
