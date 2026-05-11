/**
 * testPreferredLeaderAudit.cc
 *
 * Dedicated preferred-leader audit test.
 *
 * The test is intentionally stricter than the older observation-oriented
 * preferred-replica examples:
 * - all replicas run the same binary and same timeout logic;
 * - only the configured preferred replica should be final leader;
 * - only the final leader submits records;
 * - all replicas must apply the expected committed records;
 * - the process exits nonzero on failure.
 *
 * Build once in Single-Raft and once in Multi-Raft to exercise both topology
 * modes. The companion shell script launches the three processes and checks
 * the aggregate result.
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

constexpr int kStartupWaitSec = 7;       // Exceeds the 5s startup bias window.
constexpr int kPostSubmitWaitSec = 8;
constexpr int kRecordsPerPartition = 6;
constexpr int kBatchSize = 1;

atomic<bool> g_is_leader{false};
atomic<int> g_became_leader_count{0};
atomic<int> g_lost_leader_count{0};
atomic<int> g_records_submitted{0};
atomic<int> g_records_applied{0};

mutex g_mu;
set<string> g_seen_records;
string g_proc_name;

int env_int(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (!value || !*value) return fallback;
  return std::max(1, std::atoi(value));
}

string env_string(const char* name, const string& fallback) {
  const char* value = std::getenv(name);
  if (!value || !*value) return fallback;
  return value;
}

void safe_print(const string& msg) {
  std::lock_guard<std::mutex> lock(g_mu);
  cout << msg << endl;
}

string make_record(int partition, int seq) {
  ostringstream oss;
  oss << "audit:p" << partition << ":r" << seq;
  return oss.str();
}

vector<string> make_config_args(const string& proc_name) {
  vector<string> config{
      get_current_absolute_path() + "../config/none_raft.yml",
      get_current_absolute_path() + "../config/" +
          env_string("PREFERRED_AUDIT_CONFIG", "1c1s3r3p_cluster_test.yml")};

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

void submit_records_if_leader(int num_partitions) {
  if (!g_is_leader.load()) {
    safe_print("[" + g_proc_name + "] follower: no local submission");
    return;
  }

  safe_print("[" + g_proc_name + "] final leader: submitting audit records");
  for (int partition = 0; partition < num_partitions; ++partition) {
    for (int seq = 1; seq <= kRecordsPerPartition; ++seq) {
      string record = make_record(partition, seq);
      add_log_to_nc(record.c_str(), static_cast<int>(record.size()),
                    static_cast<uint32_t>(partition), kBatchSize);
      ++g_records_submitted;
      this_thread::sleep_for(milliseconds(20));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    cerr << "Usage: " << argv[0] << " <localhost|p1|p2>" << endl;
    return 2;
  }

  g_proc_name = argv[1];
  bool is_preferred_process = (g_proc_name == "localhost");
  int num_partitions = env_int("PREFERRED_AUDIT_PARTITIONS", 3);

  safe_print("=================================================================");
  safe_print("Preferred Leader Audit Test");
  safe_print("Process: " + g_proc_name +
             (is_preferred_process ? " (preferred)" : " (backup)"));
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
      if (record.rfind("audit:p", 0) == 0) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_seen_records.insert(to_string(par_id) + ":" + record);
        g_records_applied.store(static_cast<int>(g_seen_records.size()));
      }
    }
    uint32_t timestamp = static_cast<uint32_t>(getCurrentTimeMillis());
    return static_cast<int>(timestamp * 10 + 1);
  };

  for (int partition = 0; partition < num_partitions; ++partition) {
    register_for_leader_par_id_return(callback, partition);
    register_for_follower_par_id_return(callback, partition);
  }

  setup2(0, 0);

  safe_print("[" + g_proc_name + "] waiting for startup election and bias window");
  this_thread::sleep_for(seconds(kStartupWaitSec));

  bool final_leader_before_submit = g_is_leader.load();
  safe_print("[" + g_proc_name + "] FINAL_ROLE_BEFORE_SUBMIT=" +
             string(final_leader_before_submit ? "leader" : "follower"));

  submit_records_if_leader(num_partitions);

  int expected_records = num_partitions * kRecordsPerPartition;
  for (int i = 0; i < kPostSubmitWaitSec * 10; ++i) {
    this_thread::sleep_for(milliseconds(100));
  }

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
  safe_print("[" + g_proc_name + "] applied=" + to_string(applied) +
             "/" + to_string(expected_records));

  bool role_ok = is_preferred_process ? final_leader : !final_leader;
  bool submit_ok = is_preferred_process ? (submitted == expected_records)
                                        : (submitted == 0);
  bool apply_ok = (applied == expected_records);
  bool overall_ok = role_ok && submit_ok && apply_ok;

  safe_print("[" + g_proc_name + "] role_ok=" + string(role_ok ? "true" : "false"));
  safe_print("[" + g_proc_name + "] submit_ok=" + string(submit_ok ? "true" : "false"));
  safe_print("[" + g_proc_name + "] apply_ok=" + string(apply_ok ? "true" : "false"));
  safe_print("[" + g_proc_name + "] OVERALL=" + string(overall_ok ? "PASS" : "FAIL"));
  safe_print("=================================================================");

  _exit(overall_ok ? 0 : 1);
}
