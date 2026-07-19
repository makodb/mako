#pragma once
#include <rusty/arc.hpp>

#include "__dep__.h"
#include "coordinator.h"
#include "benchmark_control_rpc.h"
#include "server_status.h"
#include "frame.h"
#include "scheduler.h"
#include "communicator.h"
#include "config.h"
#include "./paxos/coordinator.h"
#include "concurrentqueue.h"
#include "mako_commands.h"

namespace janus {

	typedef std::chrono::time_point<std::chrono::high_resolution_clock> timepoint;

  class BulkPaxosCmd;

	inline void read_log(const char* log, int length, const char* custom){
		uint32_t cid = 0;
		memcpy(&cid, log, sizeof(uint32_t));
		Log_info("commit id %ld and length %d from %s", cid, length, custom);
	}

	// removed `class SubmitPool` (~120 LOC) —
	// the only user was the `SubmitPool* submit_pool = nullptr;` field
	// on `PaxosWorker`, and that field was always nullptr (the only
	// assignment, `submit_pool = new SubmitPool();`, was already
	// commented out in `SetupBase()`).  The class itself was a small
	// pthread-based job-queue thread pool; nothing in the rest of the
	// codebase referenced it.

// TypeList-derived kind.
class BulkPrepareLog : public rrr::Serializable<BulkPrepareLog,
                                                MakoCommands> {
  public:
  vector<pair<uint32_t,slotid_t>> min_prepared_slots;
  uint32_t leader_id;
  int epoch;

  BulkPrepareLog() = default;

  void save(BinaryWriteArchive& ar) const {
    rrr::Serialize_::serialize(static_cast<int32_t>(min_prepared_slots.size()), ar);
    for (auto i : min_prepared_slots) rrr::Serialize_::serialize(i, ar);
    rrr::Serialize_::serialize(leader_id, ar);
    rrr::Serialize_::serialize(epoch, ar);
  }

  void load(BinaryReadArchive& ar) {
    int32_t sz;
    rrr::Deserialize_::deserialize(sz, ar);
    for (int i = 0; i < sz; i++) {
      pair<uint32_t, slotid_t> pr;
      rrr::Deserialize_::deserialize(pr, ar);
      min_prepared_slots.push_back(pr);
    }
    rrr::Deserialize_::deserialize(leader_id, ar);
    rrr::Deserialize_::deserialize(epoch, ar);
  }
};

// TypeList-derived kind.
class PaxosPrepCmd : public rrr::Serializable<PaxosPrepCmd, MakoCommands> {
  public:
  vector<slotid_t> slots{};
  vector<ballot_t> ballots{};
  int leader_id;

  PaxosPrepCmd() = default;

  // NOTE: preserves the legacy bug-or-feature where the second size
  // prefix is `slots.size()` instead of `ballots.size()` — wire
  // format byte-for-byte identical.
  void save(BinaryWriteArchive& ar) const {
    rrr::Serialize_::serialize(static_cast<int32_t>(slots.size()), ar);
    for (auto i : slots) rrr::Serialize_::serialize(i, ar);
    rrr::Serialize_::serialize(static_cast<int32_t>(slots.size()), ar);
    for (auto i : ballots) rrr::Serialize_::serialize(i, ar);
    rrr::Serialize_::serialize(leader_id, ar);
  }

  void load(BinaryReadArchive& ar) {
    int32_t sz;
    rrr::Deserialize_::deserialize(sz, ar);
    for (int i = 0; i < sz; i++) {
      slotid_t x;
      rrr::Deserialize_::deserialize(x, ar);
      slots.push_back(x);
    }
    rrr::Deserialize_::deserialize(sz, ar);
    for (int i = 0; i < sz; i++) {
      ballot_t x;
      rrr::Deserialize_::deserialize(x, ar);
      ballots.push_back(x);
    }
    rrr::Deserialize_::deserialize(leader_id, ar);
  }
};

// TypeList-derived kind.
class HeartBeatLog : public rrr::Serializable<HeartBeatLog, MakoCommands> {
  public:
  uint32_t leader_id;
  int epoch;

  HeartBeatLog() = default;

  void save(BinaryWriteArchive& ar) const {
    rrr::Serialize_::serialize(leader_id, ar);
    rrr::Serialize_::serialize(epoch, ar);
  }

  void load(BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(leader_id, ar);
    rrr::Deserialize_::deserialize(epoch, ar);
  }
};

// TypeList-derived kind.
class SyncLogRequest : public rrr::Serializable<SyncLogRequest,
                                                MakoCommands> {
  public:
    int leader_id;
    ballot_t epoch;
    vector<slotid_t> sync_commit_slot;
    SyncLogRequest() = default;

    void save(BinaryWriteArchive& ar) const {
      rrr::Serialize_::serialize(leader_id, ar);
      rrr::Serialize_::serialize(epoch, ar);
      rrr::Serialize_::serialize(static_cast<int32_t>(sync_commit_slot.size()), ar);
      for (size_t i = 0; i < sync_commit_slot.size(); i++) {
        rrr::Serialize_::serialize(sync_commit_slot[i], ar);
      }
    }

    void load(BinaryReadArchive& ar) {
      rrr::Deserialize_::deserialize(leader_id, ar);
      rrr::Deserialize_::deserialize(epoch, ar);
      int32_t sz;
      rrr::Deserialize_::deserialize(sz, ar);
      for (int i = 0; i < sz; i++) {
        slotid_t x;
        rrr::Deserialize_::deserialize(x, ar);
        sync_commit_slot.push_back(x);
      }
    }
};

// migrated from Marshallable to Serializable.
// Wire payload preserved byte-for-byte:
//   int32_t sync_data.size() | N x MarshallDeputy bytes
//   int32_t missing_slots.size()
//   per missing_slots row: int32_t inner.size() | M x slotid_t
// The nested `vector<shared_ptr<janus::Command>>` field uses the
// prep `operator<<` / `operator>>` overloads for
// MarshallDeputy on BinaryWriteArchive / BinaryReadArchive — same byte
// layout as the legacy `m << *sync_data[i]` / `m >> *x`.
class SyncLogResponse : public rrr::Serializable<SyncLogResponse,
                                                 MakoCommands> {
  public:
    vector<shared_ptr<janus::Command>> sync_data;
    vector<vector<slotid_t>> missing_slots;
    SyncLogResponse() = default;

    void save(BinaryWriteArchive& ar) const {
      rrr::Serialize_::serialize(static_cast<int32_t>(sync_data.size()), ar);
      for (size_t i = 0; i < sync_data.size(); i++) {
        rrr::Serialize_::serialize(*sync_data[i], ar);
      }
      rrr::Serialize_::serialize(static_cast<int32_t>(missing_slots.size()), ar);
      for (size_t i = 0; i < missing_slots.size(); i++) {
        rrr::Serialize_::serialize(static_cast<int32_t>(missing_slots[i].size()), ar);
        for (size_t j = 0; j < missing_slots[i].size(); j++) {
          rrr::Serialize_::serialize(missing_slots[i][j], ar);
        }
      }
    }

    void load(BinaryReadArchive& ar) {
      int32_t sz;
      rrr::Deserialize_::deserialize(sz, ar);
      for (int i = 0; i < sz; i++) {
        auto x = std::make_shared<janus::Command>();
        rrr::Deserialize_::deserialize(*x, ar);
        sync_data.push_back(std::move(x));
      }
      rrr::Deserialize_::deserialize(sz, ar);
      for (int i = 0; i < sz; i++) {
        int32_t sz1;
        rrr::Deserialize_::deserialize(sz1, ar);
        vector<slotid_t> cur;
        for (int j = 0; j < sz1; j++) {
          slotid_t x;
          rrr::Deserialize_::deserialize(x, ar);
          cur.push_back(x);
        }
        missing_slots.push_back(std::move(cur));
      }
    }
};

// TypeList-derived kind.
class SyncNoOpRequest : public rrr::Serializable<SyncNoOpRequest,
                                                 MakoCommands> {
  public:
  int leader_id;
  ballot_t epoch;
  vector<slotid_t> sync_slots;
  SyncNoOpRequest() = default;

  void save(BinaryWriteArchive& ar) const {
    rrr::Serialize_::serialize(leader_id, ar);
    rrr::Serialize_::serialize(epoch, ar);
    rrr::Serialize_::serialize(static_cast<int32_t>(sync_slots.size()), ar);
    for (size_t i = 0; i < sync_slots.size(); i++) {
      rrr::Serialize_::serialize(sync_slots[i], ar);
    }
  }

  void load(BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(leader_id, ar);
    rrr::Deserialize_::deserialize(epoch, ar);
    int32_t sz;
    rrr::Deserialize_::deserialize(sz, ar);
    for (int i = 0; i < sz; i++) {
      slotid_t x;
      rrr::Deserialize_::deserialize(x, ar);
      sync_slots.push_back(x);
    }
  }
};


// migrated from TypedPaxosLogEnvelopeAdapter
// to Serializable. Wire format byte-for-byte preserved:
//   int length
//   std::string log_entry-equivalent bytes
// (the shared_ptr_apprch=1 fast path that copies operation_test bytes
// into a temporary std::string is reproduced inside save()).
//
// 1 cleanup: deleted the unused `bypass_to_socket_` /
// `entity_size` / `write_to_fd` / `length_as_v64` / `operation_` /
// `len_v64` members. They were a zero-copy fast path that no caller
// ever enabled; only `length`, `log_entry`, and `operation_test` are
// actually used by save/load.
class LogEntry : public rrr::Serializable<LogEntry, MakoCommands> {
public:
  int length = 0;
  std::string log_entry;  // for the serialization over the network, syncLog using shared_ptr as well
  shared_ptr<char> operation_test;

  LogEntry() = default;

  // Serializable interface. Implementations live in
  // paxos_worker.cc — they reference the file-static `shared_ptr_apprch`
  // flag that gates the operation_test-vs-log_entry encoding choice.
  void save(BinaryWriteArchive& ar) const;
  void load(BinaryReadArchive& ar);
};
// migrated from TypedPaxosLogEnvelopeAdapter
// to Serializable. Wire format byte-for-byte preserved:
//   int32_t leader_id
//   int32_t slots.size() | N x slotid_t
//   int32_t ballots.size() | N x ballot_t
//   int32_t cmds.size() | N x MarshallDeputy bytes
// The nested MarshallDeputies use the Phase 3f-prep `operator<<` /
// `operator>>` overloads on BinaryWriteArchive / BinaryReadArchive —
// same byte layout as legacy `m << *cmds[i]` / `m >> *cmds[i]`.
//
// 1 cleanup: deleted the unused `bypass_to_socket_` /
// `entity_size` / `serialize_slots_ballots` / `write_to_fd` /
// `serialized_slots` members. They were a zero-copy fast path that
// no caller ever enabled.
class BulkPaxosCmd : public rrr::Serializable<BulkPaxosCmd, MakoCommands> {
public:
  int32_t leader_id;
  vector<slotid_t> slots{};
  vector<ballot_t> ballots{};
  vector<shared_ptr<janus::Command>> cmds{};

  BulkPaxosCmd() = default;
  ~BulkPaxosCmd() {
      slots.clear();
      ballots.clear();
      cmds.clear();
  }

  void save(BinaryWriteArchive& ar) const {
      rrr::Serialize_::serialize(static_cast<int32_t>(leader_id), ar);
      rrr::Serialize_::serialize(static_cast<int32_t>(slots.size()), ar);
      for (auto i : slots) {
          rrr::Serialize_::serialize(i, ar);
      }
      rrr::Serialize_::serialize(static_cast<int32_t>(ballots.size()), ar);
      for (auto i : ballots) {
          rrr::Serialize_::serialize(i, ar);
      }
      rrr::Serialize_::serialize(static_cast<int32_t>(cmds.size()), ar);
      for (const auto& sp : cmds) {
          rrr::Serialize_::serialize(*sp, ar);
      }
  }

  void load(BinaryReadArchive& ar) {
      int32_t szs, szb, szc;
      rrr::Deserialize_::deserialize(leader_id, ar);
      rrr::Deserialize_::deserialize(szs, ar);
      for (int i = 0; i < szs; i++) {
          slotid_t x;
          rrr::Deserialize_::deserialize(x, ar);
          slots.push_back(x);
      }
      rrr::Deserialize_::deserialize(szb, ar);
      // Read exactly the number of ballots that were serialized.
      for (int i = 0; i < szb; i++) {
          ballot_t x;
          rrr::Deserialize_::deserialize(x, ar);
          ballots.push_back(x);
      }
      rrr::Deserialize_::deserialize(szc, ar);
      for (int i = 0; i < szc; i++) {
          auto sp_md = std::make_shared<janus::Command>();
          rrr::Deserialize_::deserialize(*sp_md, ar);
          cmds.push_back(std::move(sp_md));
      }
  }
};

class PaxosWorker {
private:
  // take const janus::Command&;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  inline void _Submit(const janus::Command&);
  inline void _BulkSubmit(const janus::Command&, int);

  std::mutex finish_mutex{};
  std::condition_variable finish_cond{};
  bool noops_received=false;
  std::function<void(const char*, int)> callback_ = nullptr;
  std::function<void(const char*&, int, int)> callback_par_id_ = nullptr;
  std::function<int(const char*&, int, int, int, std::queue<std::tuple<int, int, int, int, const char *>> &)> callback_par_id_return_ = nullptr;
  vector<Coordinator*> created_coordinators_{};
  vector<shared_ptr<Coordinator>> created_coordinators_shrd{};
  struct timeval t1;
  struct timeval t2;

public:
  std::atomic<int> n_current{0};  // requests sent out
  std::atomic<int> n_submit{0};
  std::atomic<int> n_tot{0};
  // removed `SubmitPool* submit_pool = nullptr;`
  // — always nullptr; the assignment was commented out and the
  // class itself is now deleted.
  rusty::Option<rusty::Arc<rrr::PollThread>> svr_poll_thread_worker_;
  // Services are now owned by rpc_server_ via reg_service()
  rrr::Server* rpc_server_ = nullptr;
  // removed `std::atomic<int> submit_num{0};`
  // `int submit_tot_sec_ = 0;` / `int submit_tot_usec_ = 0;` — these
  // fed only the now-deleted `microbench_paxos` / `microbench_paxos_queue`
  // drivers in `paxos_main_helper.cc`.  `tot_num` is left in place: it
  // is initialized via `Config::get_tot_req()` at worker construction
  // and a follow-up sweep can chase the Config wiring.
  int tot_num = 0;
  int cur_epoch;
  int is_leader;
  // removed `int bulk_writer = 0;` and
  // `int bulk_reader = 0;` — only used inside the now-deleted
  // `AddAcceptNc` / `StartReadAcceptNc` NC-batching pair.


  rusty::Option<rusty::Arc<rrr::PollThread>> svr_hb_poll_thread_worker_g;
  rusty::Option<rusty::Arc<ServerStatus>> server_status_;
  rrr::Server* hb_rpc_server_ = nullptr;

  Config::SiteInfo* site_info_ = nullptr;
  std::queue<std::tuple<int, int, int, int, const char *>> un_replay_logs_ ;  // timestamp, slot_id, status, len, log
  Frame* rep_frame_ = nullptr;
  TxLogServer* rep_sched_ = nullptr;
  Communicator* rep_commo_ = nullptr;
  std::recursive_mutex mtx_worker_submit{};
  std::mutex condition_mutex;
  static moodycamel::ConcurrentQueue<shared_ptr<Coordinator>> coo_queue;
  // removed `static std::queue<shared_ptr<Coordinator>>
  // coo_queue_nc;` — declared and defined but never read or written
  // anywhere outside commented-out code in the now-deleted
  // `StartReadAcceptNc` / `AddAcceptNc` pair.
  // removed `replay_queue`, `all_coords`,
  // `bulkops_th_`, `replay_th_`, `stop_replay_flag` — all only fed
  // the dead `AddAcceptNc` / `StartReadAcceptNc` / `AddReplayEntry`
  // / `StartReplayRead` paths.
  // removed `std::mutex nc_submit_l_;` —
  // declared but never locked anywhere outside commented-out code.
  std::recursive_mutex election_state_lock;
  const unsigned int cnt = bulkBatchCount;
  bool stop_flag = false;

  void SetupHeartbeat();
  void InitQueueRead();
  void SetupBase();
  int  deq_from_coo(vector<shared_ptr<Coordinator>>&);
  void SetupService();
  void SetupCommo();
  void ShutDown();
  // take MarshallDeputy (matches RegLearnerAction
  // signature in deptran/scheduler.h).
  int Next(int, janus::Command);
  void WaitForSubmit();
  void WaitForNoops();
  void IncSubmit();
  void BulkSubmit(const vector<shared_ptr<Coordinator>>&);
  void AddAccept(shared_ptr<Coordinator>);
  // removed `AddAcceptNc`, `AddReplayEntry`,
  // `StartReplayRead`, `StartReadAcceptNc` declarations — see
  // paxos_worker.cc retirement comments.
  void submitJob(rusty::Arc<Job>);
  // removed `SendBulkPrepare`, `SendHeartBeat`,
  // `SendSyncNoOpLog` declarations — definitions deleted; see
  // paxos_worker.cc retirement comments.
  int SendSyncLog(shared_ptr<SyncLogRequest>);
  static void* StartReadAccept(void*);
  PaxosWorker();
  ~PaxosWorker();

  static const uint32_t CtrlPortDelta = 10000;
  void WaitForShutdown();
  bool IsLeader(uint32_t);
  bool IsPartition(uint32_t);

  void Submit(const char*, int, uint32_t);
  void register_apply_callback(std::function<void(const char*, int)>);
  void register_apply_callback_par_id(std::function<void(const char*&, int, int)>);
  void register_apply_callback_par_id_return(std::function<int(const char*&, int, int, int, std::queue<std::tuple<int, int, int, int, const char *>> &)>);
  rusty::Arc<rrr::PollThread> GetPollThread(){
      verify(svr_poll_thread_worker_.is_some());
      return svr_poll_thread_worker_.as_ref().unwrap().clone();
  }
};

extern vector<shared_ptr<PaxosWorker>> pxs_workers_g;
extern vector<shared_ptr<PaxosWorker>> ler_workers_g;

class ElectionState {
  ElectionState(){}
public: 
  std::recursive_mutex election_mutex{};
  pthread_t election_th_;
  bool running = true;
  int timeout = 1; // in seconds
  int heartbeat_timeout = 300; // in milliseconds
  int send_prep_anyway_timeout = 1;
  int cur_epoch = 0;
  int cur_state = 0; // 0 Follower, 1 Leader
  int machine_id = -1;
  int leader_id = -1;
  std::mutex election_state;
  std::condition_variable election_cond{};
  std::mutex stuff_after_election_mutex_;
  std::condition_variable stuff_after_election_cond_{};
  timepoint lastseen = std::chrono::high_resolution_clock::now();
  timepoint last_prep_sent = std::chrono::high_resolution_clock::now();

  // weihai
  timepoint heartbeat_seen = std::chrono::high_resolution_clock::now();

  //void operator=(const ElectionState &) = delete;

  static shared_ptr<ElectionState> instance(){
    static shared_ptr<ElectionState> instance_ptr(new ElectionState);
    return instance_ptr;
  }

  int get_machine_id(){
    return machine_id;
  }

  // not to be called while state lock acquired.
  int get_consistent_epoch(){
    int x;
    state_lock();
    x = cur_epoch;
    state_unlock();
    return x;
  }

  bool is_leader(){
    int x;
    state_lock();
    x = cur_state;
    state_unlock();
    return x;
  }

  int get_epoch(){
    return cur_epoch;
  }

  void state_lock(){
    election_mutex.lock();
  }

  void state_unlock(bool sleep = false){
    election_mutex.unlock();
    if(sleep)
        sleep_timeout();
  }

  int set_epoch(int val = -1){
    if(val == -1){
      //Log_info("XXXXX current default epoch %d", cur_epoch);
      return ++cur_epoch;
    } else{
      cur_epoch = val;
    }
    assert(val >= cur_epoch);
    return cur_epoch;
  }

  void reset_timeout(){
    timeout = rand()%4;
  }

  void sleep_timeout(){
    std::this_thread::sleep_for(std::chrono::seconds(timeout));
  }

  void sleep_heartbeat(){
    std::this_thread::sleep_for(std::chrono::milliseconds(heartbeat_timeout));
  }

  void set_state(int val){
    cur_state = val;
  }

  void set_leader(int val){
    //if(val != 0)
//	Log_info("Leader being set %d", val);
    leader_id = val;
  }

  void set_heartbeat_seen(){
    heartbeat_seen = std::chrono::high_resolution_clock::now();
  }

  void set_lastseen(){
    lastseen = std::chrono::high_resolution_clock::now();
  }

  void set_bulkprep_time(){
    last_prep_sent = std::chrono::high_resolution_clock::now();
  }

  bool did_not_see_leader(){
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end-lastseen;
    return (double)timeout < diff.count();
  }

  bool did_not_send_prep(){
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end-last_prep_sent;
    return (double)send_prep_anyway_timeout < diff.count();
  }

  void step_down(int epoch){
    state_lock();
    if(cur_epoch > epoch){
      state_unlock();
      return;
    }
    set_state(0);
    leader_id = -1;
    set_epoch(epoch);
    for(int i = 0 ; i < pxs_workers_g.size(); i++){
      pxs_workers_g[i]->election_state_lock.lock();
      pxs_workers_g[i]->cur_epoch = epoch;
      pxs_workers_g[i]->is_leader = 1;
      pxs_workers_g[i]->election_state_lock.unlock();
    }
    state_unlock();
  }
};

} // namespace janus

//)` (the bridge overload in
// marshal_serializable_bridge.hpp routes Serializable T through
// `wrap_serializable`); cast sites use `marshallable_cast<T>` (also
// bridged). Phase 5a-1 cleanup deleted the unused
// TypedPaxosLogEnvelopeAdapter template and the dead
// `bypass_to_socket_` / `entity_size` / `write_to_fd` /
// `length_as_v64` / `serialize_slots_ballots` fast-path machinery on
// LogEntry and BulkPaxosCmd.)
