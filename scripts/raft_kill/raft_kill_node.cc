/*
 * raft_kill_node -- one Raft replica for the process-kill tests
 * (docs/verus/disk-persistence-plan.md P6; driven by run.py beside it).
 *
 *   raft_kill_node PROC TOPOLOGY EVENTS INCARNATION RATE PAUSE_FILE [snapshots]
 *
 * No Raft logic of its own: it starts one Raft server through the
 * replication helper, the production startup path (recovery included),
 * proposes RATE commands a second while it leads (none while PAUSE_FILE
 * exists), and appends what it observes to EVENTS, one line per event, each
 * a single write(2) on an O_APPEND descriptor, so a SIGKILL cannot lose a
 * line that was written:
 *
 *   start <proc> <incarnation> <pid> [create]   (create: MAKO_RAFT_CREATE=1)
 *   ready                       setup2() returned
 *   role leader|follower        the leader-change callback
 *   propose <id>                add_log_to_nc accepted <id> while leading
 *   apply <role> <id>           an applied command, in apply order
 *   ack <id>                    this leader applied its own <id>: committed
 *   install <id>,<id>,...       a snapshot image became the state machine
 *                               (snapshots): the applied ids it holds
 *   stopping / stopped          a clean shutdown (SIGTERM)
 *
 * A command id is "<proc>.<incarnation>.<n>"; check.py reads the files. The
 * node gets SIGTERM if run.py dies, so an interrupted run leaves no replica.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <tuple>
#include <vector>

#include "deptran/replication_helper.h"
#include "deptran/raft/snapshot_callbacks.h"

namespace {

int g_events = -1;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_leader{false};
std::string g_mine;  // "<proc>.<incarnation>.": this incarnation's ids
// The state machine: the applied ids, in order. A snapshot image is this
// list, comma-separated.
std::mutex g_sm_mu;
std::vector<std::string> g_applied;

// @unsafe - one line, one write(2).
void ev(const std::string& line) {
  const std::string l = line + "\n";
  ssize_t w = write(g_events, l.data(), l.size());
  (void)w;
}

void on_term(int) { g_stop.store(true); }

std::string joined(const std::vector<std::string>& ids) {
  std::string out;
  for (size_t i = 0; i < ids.size(); ++i) out += (i ? "," : "") + ids[i];
  return out;
}

class StagedInstall : public janus::PreparedStateMachineSnapshotInstall {
 public:
  explicit StagedInstall(std::vector<std::string> ids) : ids_(std::move(ids)) {}
  bool Commit() override {
    {
      std::lock_guard<std::mutex> g(g_sm_mu);
      g_applied = ids_;
    }
    ev("install " + joined(ids_));
    return true;
  }

 private:
  std::vector<std::string> ids_;
};

// The apply callback for one role. Mako's watermark protocol wants
// timestamp * 10 + status; status 0 is normal.
auto apply_as(bool leader_role) {
  return [leader_role](const char*& log, int len, int, int,
                       std::queue<std::tuple<int, int, int, int, const char*>>&) {
    if (len > 0 && log != nullptr) {
      std::string id(log, static_cast<size_t>(len));
      {
        std::lock_guard<std::mutex> g(g_sm_mu);
        g_applied.push_back(id);
      }
      ev(std::string("apply ") + (leader_role ? "leader " : "follower ") + id);
      if (leader_role && id.compare(0, g_mine.size(), g_mine) == 0) ev("ack " + id);
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int>((ts.tv_sec % 10000000) * 10);
  };
}

}  // namespace

// @unsafe - process entry; drives the C replication helper.
int main(int argc, char** argv) {
  if (argc < 7 || atof(argv[5]) <= 0) {
    fprintf(stderr, "usage: raft_kill_node PROC TOPOLOGY EVENTS INCARNATION RATE PAUSE_FILE [snapshots]\n");
    return 2;
  }
  const std::string proc = argv[1], topology = argv[2], incarnation = argv[4], pause_file = argv[6];
  const double rate = atof(argv[5]);
  const bool snapshots = argc > 7 && std::string(argv[7]) == "snapshots";
  g_mine = proc + "." + incarnation + ".";
  prctl(PR_SET_PDEATHSIG, SIGTERM);
  g_events = open(argv[3], O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (g_events < 0) {
    perror(argv[3]);
    return 2;
  }
  // A leading newline: the last line of an incarnation a SIGKILL cut short
  // cannot swallow this one.
  const char* create = getenv("MAKO_RAFT_CREATE");
  ev("\nstart " + proc + " " + incarnation + " " + std::to_string(getpid()) +
     (create != nullptr && std::string(create) == "1" ? " create" : ""));
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);

  janus::set_replication_type(janus::ReplicationType::RAFT);
  std::vector<std::string> args = {"", "-b", "-d", "7200", "-f", topology, "-f", "config/raft.yml",
                                   "-t", "30", "-T", "0", "-n", "32", "-P", proc,
                                   "-A", "10000", "--raft-groups=single"};
  std::vector<char*> cargv;
  for (auto& s : args) cargv.push_back(const_cast<char*>(s.c_str()));
  if (setup(static_cast<int>(cargv.size()), cargv.data()).empty()) {
    ev("setup_failed");
    return 3;
  }
  register_leader_election_callback([](int control) {
    g_leader.store(control == 1);
    ev(std::string("role ") + (control == 1 ? "leader" : "follower"));
  });
  register_for_leader_par_id_return(apply_as(true), 0);
  register_for_follower_par_id_return(apply_as(false), 0);
  if (snapshots) {
    register_snapshot_callbacks_for_partition(
        0,
        [](uint64_t) -> std::string {
          std::lock_guard<std::mutex> g(g_sm_mu);
          return "ids:" + joined(g_applied);
        },
        [](const std::string& image, uint64_t) -> std::unique_ptr<janus::PreparedStateMachineSnapshotInstall> {
          if (image.compare(0, 4, "ids:") != 0) return nullptr;
          std::vector<std::string> ids;
          for (size_t p = 4, q; p < image.size(); p = q + 1) {
            q = image.find(',', p);
            if (q == std::string::npos) q = image.size();
            ids.emplace_back(image.substr(p, q - p));
          }
          return std::make_unique<StagedInstall>(std::move(ids));
        });
  }
  if (setup2(0, 0) != 0) {
    ev("setup2_failed");
    return 4;
  }
  ev("ready");

  const useconds_t gap = static_cast<useconds_t>(1e6 / rate);
  for (long n = 0; !g_stop.load();) {
    usleep(gap);
    if (!g_leader.load() || access(pause_file.c_str(), F_OK) == 0) continue;
    const std::string id = g_mine + std::to_string(n);
    if (add_log_to_nc(id.c_str(), static_cast<int>(id.size()), 0, 1)) {
      ev("propose " + id);
      ++n;
    }
  }
  ev("stopping");
  pre_shutdown_step();
  shutdown_paxos();
  ev("stopped");
  return 0;
}
