/*
 * raft_kill_node -- one Raft replica for the process-kill tests
 * (docs/verus/disk-persistence-plan.md P6; driven by scripts/raft_kill/run.py).
 *
 * No Raft logic of its own: it starts one Raft server through the replication
 * helper (as raft_bench does), proposes paced commands while it leads, and
 * writes what it observes to an events file, one line per event, each with a
 * single write(2) on an O_APPEND descriptor, so a SIGKILL cannot lose or tear
 * a line that was written:
 *
 *   start <proc> <incarnation> <pid>
 *   ready                       setup2() returned
 *   role leader|follower        the leader-change callback
 *   propose <id>                add_log_to_nc accepted <id> while leading
 *   apply <role> <id>           an applied command, in apply order
 *   ack <id>                    this leader applied its own <id>: committed
 *   install <id>,<id>,...       a snapshot image became the state machine
 *                               (--snapshots): the applied ids it holds
 *   stopping / stopped          a clean shutdown (SIGTERM)
 *
 * A command id is "<proc>.<incarnation>.<n>". The checker
 * (scripts/raft_kill/check.py) reads these files: every incarnation's apply
 * sequence must be a prefix of one history, and every acknowledged id must
 * be in every survivor's.
 */
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <vector>

#include <mutex>

#include "deptran/replication_helper.h"
#include "deptran/raft/snapshot_callbacks.h"

namespace {

int g_events = -1;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_leader{false};
std::string g_proc;
int g_incarnation = 0;
// The state machine: the applied ids, in order. A snapshot image is this
// list, comma-separated.
std::mutex g_sm_mu;
std::vector<std::string> g_applied;

// @unsafe - one formatted line, one write(2).
void ev(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > static_cast<int>(sizeof(buf)) - 2) n = static_cast<int>(sizeof(buf)) - 2;
  buf[n++] = '\n';
  ssize_t w = write(g_events, buf, static_cast<size_t>(n));
  (void)w;
}

void on_term(int) { g_stop.store(true); }

// A whole line of any length, one write(2).
void ev_line(const std::string& line) {
  std::string l = line + "\n";
  ssize_t w = write(g_events, l.data(), l.size());
  (void)w;
}

class StagedInstall : public janus::PreparedStateMachineSnapshotInstall {
 public:
  explicit StagedInstall(std::vector<std::string> ids) : ids_(std::move(ids)) {}
  bool Commit() override {
    std::string line = "install ";
    for (size_t i = 0; i < ids_.size(); ++i) line += (i ? "," : "") + ids_[i];
    {
      std::lock_guard<std::mutex> g(g_sm_mu);
      g_applied = ids_;
    }
    ev_line(line);
    return true;
  }

 private:
  std::vector<std::string> ids_;
};

void usage() {
  fprintf(stderr,
          "usage: raft_kill_node --proc NAME --topology YML --mode YML --events FILE\n"
          "                      [--incarnation N] [--rate HZ] [--duration SEC]\n"
          "                      [--pause-file PATH]  (no proposals while PATH exists)\n");
}

// The apply callback, for one role. Mako's watermark protocol wants
// timestamp * 10 + status; status 0 is normal.
int apply_as(bool leader_role, const char*& log, int len) {
  if (len > 0 && log != nullptr) {
    std::string id(log, static_cast<size_t>(len));
    {
      std::lock_guard<std::mutex> g(g_sm_mu);
      g_applied.push_back(id);
    }
    ev("apply %s %s", leader_role ? "leader" : "follower", id.c_str());
    const std::string mine = g_proc + "." + std::to_string(g_incarnation) + ".";
    if (leader_role && id.compare(0, mine.size(), mine) == 0) {
      ev("ack %s", id.c_str());
    }
  }
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int>((ts.tv_sec % 10000000) * 10);
}

}  // namespace

// @unsafe - process entry; drives the C replication helper.
int main(int argc, char** argv) {
  std::string topology, mode, events, pause_file;
  bool snapshots = false;
  double rate = 50.0;
  double duration = 3600.0;
  static const struct option kOpts[] = {
      {"proc", required_argument, nullptr, 'p'},
      {"topology", required_argument, nullptr, 't'},
      {"mode", required_argument, nullptr, 'm'},
      {"events", required_argument, nullptr, 'e'},
      {"incarnation", required_argument, nullptr, 'i'},
      {"rate", required_argument, nullptr, 'r'},
      {"duration", required_argument, nullptr, 'd'},
      {"pause-file", required_argument, nullptr, 'P'},
      {"snapshots", no_argument, nullptr, 'S'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };
  int c;
  while ((c = getopt_long(argc, argv, "", kOpts, nullptr)) != -1) {
    switch (c) {
      case 'p': g_proc = optarg; break;
      case 't': topology = optarg; break;
      case 'm': mode = optarg; break;
      case 'e': events = optarg; break;
      case 'i': g_incarnation = atoi(optarg); break;
      case 'r': rate = atof(optarg); break;
      case 'd': duration = atof(optarg); break;
      case 'P': pause_file = optarg; break;
      case 'S': snapshots = true; break;
      default: usage(); return 2;
    }
  }
  if (g_proc.empty() || topology.empty() || mode.empty() || events.empty() || rate <= 0) {
    usage();
    return 2;
  }
  g_events = open(events.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (g_events < 0) {
    perror(events.c_str());
    return 2;
  }
  ev("start %s %d %d", g_proc.c_str(), g_incarnation, static_cast<int>(getpid()));
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);

  janus::set_replication_type(janus::ReplicationType::RAFT);
  const std::string duration_arg = std::to_string(static_cast<long long>(duration + 600));
  std::vector<std::string> args = {"",   "-b", "-d", duration_arg, "-f", topology, "-f", mode,
                                   "-t", "30", "-T", "0",          "-n", "32",     "-P", g_proc,
                                   "-A", "10000", "--raft-groups=single"};
  std::vector<char*> cargv;
  for (auto& s : args) cargv.push_back(const_cast<char*>(s.c_str()));
  const std::vector<std::string> sites = setup(static_cast<int>(cargv.size()), cargv.data());
  if (sites.empty()) {
    ev("setup_failed");
    return 3;
  }

  register_leader_election_callback([](int control) {
    g_leader.store(control == 1);
    ev("role %s", control == 1 ? "leader" : "follower");
  });
  register_for_leader_par_id_return(
      [](const char*& log, int len, int, int, std::queue<std::tuple<int, int, int, int, const char*>>&) {
        return apply_as(true, log, len);
      },
      0);
  register_for_follower_par_id_return(
      [](const char*& log, int len, int, int, std::queue<std::tuple<int, int, int, int, const char*>>&) {
        return apply_as(false, log, len);
      },
      0);

  if (snapshots) {
    register_snapshot_callbacks_for_partition(
        0,
        [](uint64_t) -> std::string {
          std::lock_guard<std::mutex> g(g_sm_mu);
          std::string image = "ids:";
          for (size_t i = 0; i < g_applied.size(); ++i) image += (i ? "," : "") + g_applied[i];
          return image;
        },
        [](const std::string& image, uint64_t) -> std::unique_ptr<janus::PreparedStateMachineSnapshotInstall> {
          if (image.compare(0, 4, "ids:") != 0) return nullptr;
          std::vector<std::string> ids;
          size_t p = 4;
          while (p < image.size()) {
            size_t q = image.find(',', p);
            if (q == std::string::npos) q = image.size();
            ids.emplace_back(image.substr(p, q - p));
            p = q + 1;
          }
          return std::make_unique<StagedInstall>(std::move(ids));
        });
  }

  if (setup2(0, 0) != 0) {
    ev("setup2_failed");
    return 4;
  }
  ev("ready");

  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  const useconds_t gap = static_cast<useconds_t>(1e6 / rate);
  long n = 0;
  while (!g_stop.load()) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (static_cast<double>(now.tv_sec - t0.tv_sec) >= duration) break;
    usleep(gap);
    if (!g_leader.load()) continue;
    if (!pause_file.empty() && access(pause_file.c_str(), F_OK) == 0) continue;
    char id[128];
    int len = snprintf(id, sizeof(id), "%s.%d.%ld", g_proc.c_str(), g_incarnation, n);
    if (add_log_to_nc(id, len, 0, 1)) {
      ev("propose %s", id);
      ++n;
    }
  }
  ev("stopping");
  pre_shutdown_step();
  shutdown_paxos();
  ev("stopped");
  return 0;
}
