/**
 * testPreferredLeaderFailbackAudit.cc
 *
 * Preferred-leader failover/failback audit.
 *
 * The companion shell script starts only the two backup replicas first. One
 * backup must be able to become leader and commit records while the preferred
 * replica is absent. The preferred replica is then started later; after it
 * catches up, leadership should return to the preferred replica and all
 * replicas should observe both the failover and failback records.
 */

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <examples/common.h>
#include <mako.hh>

using namespace std;
using namespace mako;
using namespace chrono;

namespace {

constexpr int kRecordsPerPhase = 3;
constexpr int kBatchSize = 1;
constexpr int kInitialPreferredAliveSec = 60;
constexpr int kBackupElectionWaitSec = 14;
constexpr int kPreferredFailbackWaitSec = 14;
constexpr int kBackupFinalWaitSec = 17;
constexpr int kPreferredFinalApplyWaitSec = 20;

atomic<bool> g_is_leader{false};
atomic<int> g_became_leader_count{0};
atomic<int> g_lost_leader_count{0};
atomic<int> g_records_submitted{0};
atomic<int> g_records_applied{0};

mutex g_mu;
set<string> g_seen_records;
string g_proc_name;

string env_string(const char* name, const string& fallback) {
  const char* value = std::getenv(name);
  if (!value || !*value) return fallback;
  return value;
}

void safe_print(const string& msg) {
  std::lock_guard<std::mutex> lock(g_mu);
  cout << msg << endl;
}

string make_record(const string& phase, int seq) {
  ostringstream oss;
  oss << "failback:" << phase << ":" << g_proc_name << ":r" << seq;
  return oss.str();
}

vector<string> make_config_args(const string& proc_name) {
  vector<string> config{
      get_current_absolute_path() + "../config/none_raft.yml",
      get_current_absolute_path() + "../config/" +
          env_string("PREFERRED_FAILBACK_CONFIG", "1c1s3r1p_cluster_test.yml")};

  static vector<string> owned;
  owned = {
      "", "-b", "-d", "60",
      "-f", config[0],
      "-f", config[1],
      "-t", "30",
      "-T", "100000",
      "-n", "32",
      "-P", proc_name,
      "-A", "10000"};
  return owned;
}

void submit_phase_records(const string& phase) {
  if (!g_is_leader.load()) {
    safe_print("[" + g_proc_name + "] not leader; no " + phase + " submission");
    return;
  }

  safe_print("[" + g_proc_name + "] leader: submitting " + phase + " records");
  for (int seq = 1; seq <= kRecordsPerPhase; ++seq) {
    string record = make_record(phase, seq);
    add_log_to_nc(record.c_str(), static_cast<int>(record.size()), 0, kBatchSize);
    ++g_records_submitted;
    this_thread::sleep_for(milliseconds(30));
  }
}

bool wait_until_leader(seconds timeout, const string& reason) {
  auto deadline = steady_clock::now() + timeout;
  while (steady_clock::now() < deadline) {
    if (g_is_leader.load()) {
      safe_print("[" + g_proc_name + "] became leader while waiting for " + reason);
      return true;
    }
    this_thread::sleep_for(milliseconds(100));
  }
  safe_print("[" + g_proc_name + "] stayed follower while waiting for " + reason);
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    cerr << "Usage: " << argv[0] << " <localhost|p1|p2>" << endl;
    return 2;
  }

  g_proc_name = argv[1];
  bool is_preferred_process = (g_proc_name == "localhost");
  string phase = env_string("PREFERRED_FAILBACK_PHASE",
                            is_preferred_process ? "rejoin" : "backup");

  safe_print("=================================================================");
  safe_print("Preferred Leader Failback Audit Test");
  safe_print("Process: " + g_proc_name +
             (is_preferred_process ? " (preferred)" : " (backup)"));
  safe_print("Phase: " + phase);
  safe_print("=================================================================");

  janus::set_replication_type(janus::ReplicationType::RAFT);

  auto args = make_config_args(g_proc_name);
  vector<char*> argv_raft;
  argv_raft.reserve(args.size());
  for (auto& arg : args) {
    argv_raft.push_back(const_cast<char*>(arg.c_str()));
  }

  vector<string> setup_result = setup(static_cast<int>(argv_raft.size()),
                                      argv_raft.data());
  if (setup_result.empty()) {
    cerr << "[" << g_proc_name << "] setup failed" << endl;
    return 1;
  }

  register_leader_election_callback([&](int control) {
    if (control == 1) {
      g_is_leader.store(true);
      ++g_became_leader_count;
      safe_print("[" + g_proc_name + "] BECAME_LEADER");
    } else {
      g_is_leader.store(false);
      ++g_lost_leader_count;
      safe_print("[" + g_proc_name + "] LOST_LEADER");
    }
  });

  auto callback = [&](const char*& log, int len, int par_id, int slot_id,
                      queue<tuple<int, int, int, int, const char*>>& unused) {
    (void)slot_id;
    (void)unused;
    if (len > 0 && log != nullptr) {
      string record(log, len);
      if (record.rfind("failback:", 0) == 0) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_seen_records.insert(to_string(par_id) + ":" + record);
        g_records_applied.store(static_cast<int>(g_seen_records.size()));
      }
    }
    uint32_t timestamp = static_cast<uint32_t>(getCurrentTimeMillis());
    return static_cast<int>(timestamp * 10 + 1);
  };

  register_for_leader_par_id_return(callback, 0);
  register_for_follower_par_id_return(callback, 0);

  setup2(0, 0);

  if (is_preferred_process && phase == "initial") {
    safe_print("[" + g_proc_name + "] initial preferred replica running until test harness stops it");
    this_thread::sleep_for(seconds(kInitialPreferredAliveSec));
  } else if (is_preferred_process) {
    safe_print("[" + g_proc_name + "] waiting for failback to preferred replica");
    wait_until_leader(seconds(kPreferredFailbackWaitSec), "preferred failback");
    safe_print("[" + g_proc_name + "] ROLE_AFTER_FAILBACK_WAIT=" +
               string(g_is_leader.load() ? "leader" : "follower"));
    submit_phase_records("preferred-return");
    this_thread::sleep_for(seconds(kPreferredFinalApplyWaitSec));
  } else {
    safe_print("[" + g_proc_name + "] waiting for backup failover election");
    wait_until_leader(seconds(kBackupElectionWaitSec), "backup failover");
    safe_print("[" + g_proc_name + "] ROLE_DURING_PREFERRED_ABSENCE=" +
               string(g_is_leader.load() ? "leader" : "follower"));
    submit_phase_records("backup-failover");
    this_thread::sleep_for(seconds(kBackupFinalWaitSec));
  }

  int minimum_expected_records = kRecordsPerPhase * 2;
  bool final_leader = g_is_leader.load();
  int applied = g_records_applied.load();
  int submitted = g_records_submitted.load();
  int became = g_became_leader_count.load();
  int lost = g_lost_leader_count.load();

  safe_print("=================================================================");
  safe_print("[" + g_proc_name + "] FINAL RESULTS");
  safe_print("[" + g_proc_name + "] final_leader=" +
             string(final_leader ? "true" : "false"));
  safe_print("[" + g_proc_name + "] became_leader_count=" + to_string(became));
  safe_print("[" + g_proc_name + "] lost_leader_count=" + to_string(lost));
  safe_print("[" + g_proc_name + "] submitted=" + to_string(submitted));
  safe_print("[" + g_proc_name + "] applied=" + to_string(applied));
  safe_print("[" + g_proc_name + "] minimum_expected_applied=" +
             to_string(minimum_expected_records));

  bool role_ok = is_preferred_process ? final_leader : !final_leader;
  bool backup_failover_seen = is_preferred_process ? true : (became > 0 || submitted > 0);
  bool preferred_submit_ok = is_preferred_process ? (submitted == kRecordsPerPhase)
                                                  : true;
  bool apply_ok = (applied >= minimum_expected_records);
  bool participant_ok = is_preferred_process
      ? (role_ok && preferred_submit_ok && apply_ok)
      : (role_ok && apply_ok);
  bool overall_ok = (phase == "initial") ? true : participant_ok;

  safe_print("[" + g_proc_name + "] role_ok=" + string(role_ok ? "true" : "false"));
  safe_print("[" + g_proc_name + "] backup_failover_seen=" +
             string(backup_failover_seen ? "true" : "false"));
  safe_print("[" + g_proc_name + "] preferred_submit_ok=" +
             string(preferred_submit_ok ? "true" : "false"));
  safe_print("[" + g_proc_name + "] apply_ok=" + string(apply_ok ? "true" : "false"));
  safe_print("[" + g_proc_name + "] OVERALL=" + string(overall_ok ? "PASS" : "FAIL"));
  safe_print("=================================================================");

  _exit(overall_ok ? 0 : 1);
}
