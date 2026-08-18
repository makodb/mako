// Crash-durability tests for the masstree-rocks cache.
//
// These verify the three properties the design actually promises:
//
//   1. COVERED => DURABLE. A write whose version is <= the watermark W
//      is recoverable after the process is killed, with exactly the
//      bytes that were written.
//   2. UNCOVERED => ATOMIC, NEVER CORRUPT. A write above W may be lost
//      or present, but only ever with a value that was really written
//      -- never a partial or torn one -- and the store must reopen
//      normally afterwards.
//   3. CLEAN EXIT => COMPLETE. After mrx_store_close(), every
//      acknowledged write is recoverable.
//
// WHY THIS IS A SEPARATE BINARY THAT SELF-EXECS. A real crash needs a
// real SIGKILL, so the writer must be a child process. But the RCU
// ticker daemon runs for the life of any process that has touched the
// store, so a plain fork() would leave the child with malloc/RocksDB
// locks possibly held by a thread that does not exist in the child --
// textbook fork-in-a-threaded-process UB. Self-exec via /proc/self/exe
// gives the child a clean single-threaded start.
//
// The child journals what it wrote into a MAP_SHARED file mapping,
// which survives SIGKILL (it is page cache, not process memory), so the
// parent can reconstruct the exact history and check it against what
// recovery produced.

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "mako/storage/masstree_rocks_index.hh"

#include <algorithm>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// The journal: what the child tells the parent about what it did.
// ---------------------------------------------------------------------------

static const uint64_t kMaxRecords = 1u << 20;

struct CrashRec {
  uint64_t version;  // version stamped on this write
  uint32_t key_id;
  uint32_t val_id;   // which value variant; UINT32_MAX for a remove
};

struct Journal {
  std::atomic<uint64_t> n_records;
  // Max watermark the child ever observed. W only rises, so a value
  // seen at any moment is a valid LOWER BOUND on W at the instant of
  // the kill -- which is exactly what property 1 needs.
  std::atomic<uint64_t> w_observed;
  std::atomic<uint64_t> child_started;
  std::atomic<uint64_t> clean_exit;
  CrashRec recs[kMaxRecords];
};

static Journal *MapJournal(const std::string &path, bool create) {
  const int fd = ::open(path.c_str(), create ? (O_RDWR | O_CREAT) : O_RDWR,
                        0644);
  if (fd < 0) return nullptr;
  if (create && ::ftruncate(fd, sizeof(Journal)) != 0) {
    ::close(fd);
    return nullptr;
  }
  void *p = ::mmap(nullptr, sizeof(Journal), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) return nullptr;
  return reinterpret_cast<Journal *>(p);
}

// Distinctive, self-describing values: a torn or fabricated value
// cannot accidentally equal one of these.
static std::string KeyOf(uint32_t id) {
  char buf[32];
  snprintf(buf, sizeof(buf), "key%08u", id);
  return std::string(buf);
}

static std::string ValOf(uint32_t key_id, uint32_t val_id) {
  char buf[64];
  snprintf(buf, sizeof(buf), "K%08u-V%08u-", key_id, val_id);
  std::string v(buf);
  v.append(200, static_cast<char>('a' + (val_id % 26)));
  v.append("-END");
  return v;
}

// ---------------------------------------------------------------------------
// Child roles
// ---------------------------------------------------------------------------

// Writes until killed (or until `limit` ops), journaling every write.
// mode: "distinct" gives each write its own key; "overwrite" cycles a
// small key set so per-key history matters; "mixed" adds removes.
static int ChildWriter(const std::string &db_path, const std::string &jpath,
                       const std::string &mode, uint64_t limit,
                       bool clean_close, int threads) {
  // CHILD HYGIENE - both of these are load-bearing, learned the hard
  // way from a child that outlived its parent by five hours:
  //
  // 1. Die with the parent. The kill-mode child waits to be SIGKILLed;
  //    if the parent ever exits first (a failed ASSERT before the kill,
  //    a timeout, a crash), the child would otherwise sleep forever.
  // 2. Drop inherited stdio. The child inherits the parent's stdout,
  //    which in CI is a PIPE. A reader on that pipe blocks until every
  //    writer closes it, so an immortal child hangs the whole pipeline
  //    even after the parent is gone.
  ::prctl(PR_SET_PDEATHSIG, SIGKILL);
  if (::getppid() == 1) ::_exit(9);  // parent died during the race window
  {
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) ::close(devnull);
    }
  }
  // Belt and braces: hard cap the child's lifetime.
  ::alarm(300);

  Journal *j = MapJournal(jpath, false);
  if (j == nullptr) return 2;

  concurrent_btree tree;
  mrx_store *store = mrx_store_open(&tree, db_path, 0);
  if (store == nullptr) return 3;
  masstree_rocks_index idx("crash", 1, &tree, store);

  j->child_started.store(1, std::memory_order_release);

  // Each thread owns a DISJOINT key range. Sharing keys across threads
  // would make the version read-back below ambiguous (thread A could
  // observe thread B's version and journal it against A's value), which
  // would make the parent's exactness check fail spuriously. Threads
  // still contend on the ring, the flusher, and the watermark -- the
  // parts under test -- just not on individual keys.
  const uint32_t kPerThread = 512;

  auto run = [&](int tid) {
    for (uint64_t i = 0; i < limit; i++) {
      const uint32_t key_id =
          (mode == "distinct")
              ? static_cast<uint32_t>(tid * 10000000u + i)
              : static_cast<uint32_t>(tid * kPerThread + (i % kPerThread));
      const uint32_t val_id = static_cast<uint32_t>(i * threads + tid);
      const std::string key = KeyOf(key_id);
      const bool do_remove = (mode == "mixed") && (i % 5 == 4);

      if (do_remove) {
        idx.remove(lcdf::Str(key.data(), static_cast<int>(key.size())));
      } else {
        idx.put(lcdf::Str(key.data(), static_cast<int>(key.size())),
                ValOf(key_id, val_id));
      }

      // Read back the version this op stamped. Single writer per key in
      // the single-threaded modes; in the concurrent mode a racing
      // writer may have superseded it, which only makes our recorded
      // version an under-estimate -- safe for the parent's checks.
      uint64_t ver = 0;
      {
        const auto region = mrx_rcu_region();
        mrx_entry *e =
            mrx_lookup(&tree, lcdf::Str(key.data(),
                                        static_cast<int>(key.size())));
        if (e != nullptr) {
          ver = e->val.load(std::memory_order_acquire)->version;
        }
      }

      // Reserve a slot, fill it, and let the counter stand as the
      // publish point. The parent reads only after the child is dead,
      // so the only hazard is a kill between reserving and filling --
      // which leaves a zeroed record below n_records. Zeroed records
      // are version 0 / key 0, and the parent tolerates them because a
      // version-0 op is never "covered" by a non-zero W.
      const uint64_t my = j->n_records.fetch_add(1, std::memory_order_acq_rel);
      if (my >= kMaxRecords) return;
      j->recs[my].version = ver;
      j->recs[my].key_id = key_id;
      j->recs[my].val_id = do_remove ? UINT32_MAX : val_id;

      // Periodically let the flusher breathe. Writing flat out, the
      // producer outruns RocksDB ingest, the dirty map grows without
      // bound, and W -- a LOW-water mark over undischarged versions --
      // never advances, so nothing is ever covered and property 1 has
      // nothing to test. A brief pause every so often lets writeback
      // discharge a prefix and the watermark move, which is also what
      // any real workload with think-time looks like. The kill still
      // lands during the full-speed phase that follows.
      if ((i % 8000) == 7999) ::usleep(150000);

      // Sample W after the write is journaled.
      const uint64_t w = store->persisted_version.load(std::memory_order_acquire);
      uint64_t prev = j->w_observed.load(std::memory_order_relaxed);
      while (w > prev &&
             !j->w_observed.compare_exchange_weak(prev, w,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed)) {
      }
    }
  };

  std::vector<std::thread> ts;
  for (int t = 1; t < threads; t++) ts.emplace_back([&, t]() { run(t); });
  run(0);
  for (auto &t : ts) t.join();

  if (clean_close) {
    mrx_store_close(store);
    // Publish AFTER close returns: property 3 says everything acked
    // before a clean exit must be recoverable.
    const uint64_t w = UINT64_MAX;
    j->w_observed.store(w, std::memory_order_release);
    j->clean_exit.store(1, std::memory_order_release);
    ::_exit(0);
  }

  // Wait to be killed.
  for (;;) ::sleep(1);
  return 0;
}

// ---------------------------------------------------------------------------
// Parent side
// ---------------------------------------------------------------------------

struct KeyOp {
  uint64_t version;
  uint32_t val_id;  // UINT32_MAX = remove
};

class CrashTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/mrx_crash_XXXXXX";
    char *d = mkdtemp(tmpl);
    ASSERT_NE(d, nullptr);
    dir_ = std::string(d);
    db_path_ = dir_ + "/db";
    jpath_ = dir_ + "/journal.bin";
    j_ = MapJournal(jpath_, true);
    ASSERT_NE(j_, nullptr);
  }

  void TearDown() override {
    ReapChild(live_child_);
    live_child_ = -1;
    if (j_ != nullptr) ::munmap(j_, sizeof(Journal));
    (void)system(("rm -rf " + dir_).c_str());
  }

  // Launch the writer child by re-executing this binary.
  pid_t LaunchChild(const std::string &mode, uint64_t limit, bool clean,
                    int threads) {
    const pid_t pid = ::fork();
    if (pid == 0) {
      char exe[4096];
      const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
      if (n <= 0) ::_exit(4);
      exe[n] = '\0';
      const std::string lim = std::to_string(limit);
      const std::string thr = std::to_string(threads);
      const char *cl = clean ? "clean" : "kill";
      char *const argv[] = {exe,
                            const_cast<char *>("--child"),
                            const_cast<char *>(db_path_.c_str()),
                            const_cast<char *>(jpath_.c_str()),
                            const_cast<char *>(mode.c_str()),
                            const_cast<char *>(lim.c_str()),
                            const_cast<char *>(cl),
                            const_cast<char *>(thr.c_str()),
                            nullptr};
      ::execv(exe, argv);
      ::_exit(5);
    }
    live_child_ = pid;
    return pid;
  }

  // Block until the child has journaled at least `n` records.
  bool WaitForRecords(uint64_t n, int timeout_ms = 30000) {
    for (int waited = 0; waited < timeout_ms; waited += 5) {
      if (j_->n_records.load(std::memory_order_acquire) >= n) return true;
      ::usleep(5000);
    }
    return false;
  }

  // Block until at least `min_covered` journaled ops are provably
  // durable (version <= observed W). Property 1 has nothing to say
  // about an uncovered write, so killing before coverage exists makes
  // the test vacuous -- which the first run of this suite caught. This
  // waits on the quantity that actually matters instead of on a proxy.
  size_t CoveredNow() {
    const uint64_t w = j_->w_observed.load(std::memory_order_acquire);
    if (w == 0) return 0;
    const uint64_t n =
        std::min<uint64_t>(j_->n_records.load(std::memory_order_acquire),
                           kMaxRecords);
    size_t c = 0;
    for (uint64_t i = 0; i < n; i++) {
      const uint64_t v = j_->recs[i].version;
      if (v != 0 && v <= w) c++;
    }
    return c;
  }

  bool WaitForCoverage(size_t min_covered, int timeout_ms = 60000) {
    for (int waited = 0; waited < timeout_ms; waited += 5) {
      if (CoveredNow() >= min_covered) return true;
      ::usleep(5000);
    }
    return false;
  }

  // How many journaled ops the watermark actually covers. Reported by
  // every kill test so a vacuous pass is visible rather than silent.
  static size_t CoveredOps(
      const std::map<uint32_t, std::vector<KeyOp>> &hist, uint64_t w) {
    size_t n = 0;
    for (const auto &kv : hist) {
      for (const KeyOp &o : kv.second) {
        if (o.version <= w) n++;
      }
    }
    return n;
  }

  // Rebuild per-key history from the journal, in record order.
  std::map<uint32_t, std::vector<KeyOp>> History() {
    std::map<uint32_t, std::vector<KeyOp>> h;
    const uint64_t n =
        std::min<uint64_t>(j_->n_records.load(std::memory_order_acquire),
                           kMaxRecords);
    for (uint64_t i = 0; i < n; i++) {
      // A kill between reserving a slot and filling it leaves the slot
      // zeroed. Version 0 is never stamped by a real write (the counter
      // starts at 1), so this cleanly identifies a torn record.
      if (j_->recs[i].version == 0) continue;
      h[j_->recs[i].key_id].push_back(
          KeyOp{j_->recs[i].version, j_->recs[i].val_id});
    }
    for (auto &kv : h) {
      std::sort(kv.second.begin(), kv.second.end(),
                [](const KeyOp &a, const KeyOp &b) {
                  return a.version < b.version;
                });
    }
    return h;
  }

  // Reap a child unconditionally, so a failed ASSERT between launch and
  // kill cannot strand one. Safe to call twice.
  void ReapChild(pid_t pid) {
    if (pid <= 0) return;
    ::kill(pid, SIGKILL);
    int st = 0;
    ::waitpid(pid, &st, 0);
  }

  std::string dir_, db_path_, jpath_;
  Journal *j_{nullptr};
  pid_t live_child_{-1};
};

// Reopen the store and check every key against its journaled history.
// Returns a failure description, or "" if all invariants hold.
static std::string Verify(const std::string &db_path,
                          const std::map<uint32_t, std::vector<KeyOp>> &hist,
                          uint64_t w, bool require_complete) {
  concurrent_btree tree;
  mrx_store *store = mrx_store_open(&tree, db_path, 0);
  if (store == nullptr) return "PROPERTY 2 VIOLATED: store failed to reopen";
  masstree_rocks_index idx("verify", 1, &tree, store);

  std::string fail;
  for (const auto &kv : hist) {
    const uint32_t key_id = kv.first;
    const std::vector<KeyOp> &ops = kv.second;
    if (ops.empty()) continue;

    const std::string key = KeyOf(key_id);
    std::string got;
    const bool present =
        idx.get(lcdf::Str(key.data(), static_cast<int>(key.size())), got,
                std::string::npos);

    // What the durable tier is allowed to reflect: the effect of some
    // op at or after the newest COVERED op (version <= w). Anything
    // older than that would mean a covered write was lost.
    size_t covered_idx = ops.size();  // none covered
    for (size_t i = 0; i < ops.size(); i++) {
      if (ops[i].version <= w) covered_idx = i;
    }
    const size_t lo = (covered_idx == ops.size()) ? 0 : covered_idx;

    // Build the set of acceptable outcomes.
    bool ok = false;
    std::string expected_desc;
    if (covered_idx == ops.size()) {
      // Nothing covered: absent is acceptable (nothing was ever forced
      // to disk for this key).
      if (!present) ok = true;
      expected_desc = "absent or any journaled value";
    } else if (require_complete || covered_idx + 1 == ops.size()) {
      // The newest op is covered (or a clean exit covers everything):
      // the store must reflect EXACTLY it.
      const KeyOp &want = ops.back();
      if (want.val_id == UINT32_MAX) {
        ok = !present;
        expected_desc = "absent (removed)";
      } else {
        ok = present && got == ValOf(key_id, want.val_id);
        expected_desc = "exactly val_id " + std::to_string(want.val_id);
      }
    } else {
      expected_desc = "some op at or after index " + std::to_string(lo);
    }

    if (!ok) {
      // Accept the effect of any op in [lo, end] -- that is the window
      // the design permits when the tail is uncovered.
      for (size_t i = lo; i < ops.size() && !ok; i++) {
        if (ops[i].val_id == UINT32_MAX) {
          if (!present) ok = true;
        } else if (present && got == ValOf(key_id, ops[i].val_id)) {
          ok = true;
        }
      }
    }

    if (!ok) {
      char buf[512];
      snprintf(buf, sizeof(buf),
               "key %u: expected %s, got %s (w=%llu, ops=%zu, "
               "newest_version=%llu)",
               key_id, expected_desc.c_str(),
               present ? "a value" : "absent",
               static_cast<unsigned long long>(w), ops.size(),
               static_cast<unsigned long long>(ops.back().version));
      fail = buf;
      break;
    }

    // PROPERTY 2, the corruption half: whatever is there must be a
    // value we actually wrote, byte for byte.
    if (present) {
      bool known = false;
      for (const KeyOp &o : ops) {
        if (o.val_id != UINT32_MAX && got == ValOf(key_id, o.val_id)) {
          known = true;
          break;
        }
      }
      if (!known) {
        fail = "PROPERTY 2 VIOLATED: key " + std::to_string(key_id) +
               " holds a value that was never written (torn or corrupt), "
               "length " + std::to_string(got.size());
        break;
      }
    }
  }

  mrx_store_close(store);
  return fail;
}

// ---------------------------------------------------------------------------
// PROPERTY 1 + 2: kill -9 mid-write
// ---------------------------------------------------------------------------

TEST_F(CrashTest, KillDuringDistinctKeyWrites) {
  const pid_t pid = LaunchChild("distinct", 500000, false, 1);
  ASSERT_GT(pid, 0);
  ASSERT_TRUE(WaitForRecords(20000)) << "child never made progress";
  // A big backlog of distinct keys holds the low-water mark down, so
  // wait for it to advance before killing -- otherwise nothing is
  // covered and property 1 is untested.
  ASSERT_TRUE(WaitForCoverage(500))
      << "the flusher never covered enough writes to test property 1";
  ASSERT_EQ(::kill(pid, SIGKILL), 0);
  int st = 0;
  ::waitpid(pid, &st, 0);

  const uint64_t w = j_->w_observed.load(std::memory_order_acquire);
  const auto hist = History();
  ASSERT_GT(hist.size(), 1000u) << "too little history to be meaningful";
  const size_t covered = CoveredOps(hist, w);
  ASSERT_GE(covered, 500u)
      << "only " << covered << " ops covered; property 1 barely exercised";

  const std::string fail = Verify(db_path_, hist, w, false);
  EXPECT_TRUE(fail.empty()) << fail;
  RecordProperty("covered_ops", static_cast<int>(covered));
  RecordProperty("total_ops", static_cast<int>(hist.size()));
}

TEST_F(CrashTest, KillDuringOverwrites) {
  const pid_t pid = LaunchChild("overwrite", 200000, false, 1);
  ASSERT_GT(pid, 0);
  ASSERT_TRUE(WaitForRecords(20000));
  ASSERT_TRUE(WaitForCoverage(500))
      << "the flusher never covered enough writes to test property 1";
  ASSERT_EQ(::kill(pid, SIGKILL), 0);
  int st = 0;
  ::waitpid(pid, &st, 0);

  const uint64_t w = j_->w_observed.load(std::memory_order_acquire);
  const auto hist = History();
  ASSERT_FALSE(hist.empty());
  const size_t covered = CoveredOps(hist, w);
  ASSERT_GE(covered, 500u)
      << "only " << covered << " ops covered; property 1 barely exercised";
  const std::string fail = Verify(db_path_, hist, w, false);
  EXPECT_TRUE(fail.empty()) << fail;
  RecordProperty("covered_ops", static_cast<int>(covered));
}

TEST_F(CrashTest, KillDuringMixedPutsAndRemoves) {
  const pid_t pid = LaunchChild("mixed", 200000, false, 1);
  ASSERT_GT(pid, 0);
  ASSERT_TRUE(WaitForRecords(20000));
  ASSERT_TRUE(WaitForCoverage(500))
      << "the flusher never covered enough writes to test property 1";
  ASSERT_EQ(::kill(pid, SIGKILL), 0);
  int st = 0;
  ::waitpid(pid, &st, 0);

  const uint64_t w = j_->w_observed.load(std::memory_order_acquire);
  const auto hist = History();
  ASSERT_FALSE(hist.empty());
  const size_t covered = CoveredOps(hist, w);
  ASSERT_GE(covered, 500u)
      << "only " << covered << " ops covered; property 1 barely exercised";
  const std::string fail = Verify(db_path_, hist, w, false);
  EXPECT_TRUE(fail.empty()) << fail;
  RecordProperty("covered_ops", static_cast<int>(covered));
}

TEST_F(CrashTest, KillDuringConcurrentWrites) {
  const pid_t pid = LaunchChild("overwrite", 100000, false, 8);
  ASSERT_GT(pid, 0);
  ASSERT_TRUE(WaitForRecords(40000));
  ASSERT_TRUE(WaitForCoverage(500))
      << "the flusher never covered enough writes to test property 1";
  ASSERT_EQ(::kill(pid, SIGKILL), 0);
  int st = 0;
  ::waitpid(pid, &st, 0);

  const uint64_t w = j_->w_observed.load(std::memory_order_acquire);
  const auto hist = History();
  ASSERT_FALSE(hist.empty());
  const size_t covered = CoveredOps(hist, w);
  ASSERT_GE(covered, 500u)
      << "only " << covered << " ops covered; property 1 barely exercised";
  const std::string fail = Verify(db_path_, hist, w, false);
  EXPECT_TRUE(fail.empty()) << fail;
  RecordProperty("covered_ops", static_cast<int>(covered));
}

// Killing repeatedly at different depths lands the kill in different
// internal states (mid-batch, mid-writeback, mid-watermark pass).
TEST_F(CrashTest, RepeatedKillsAtVaryingDepths) {
  const std::string base_db = db_path_;
  for (int round = 0; round < 4; round++) {
    j_->n_records.store(0, std::memory_order_release);
    j_->w_observed.store(0, std::memory_order_release);
    j_->child_started.store(0, std::memory_order_release);
    // Each round needs its OWN database: the journal is reset per
    // round, so rows left by an earlier round would be values the
    // current round's history has never heard of.
    db_path_ = base_db + "_r" + std::to_string(round);

    const uint64_t depth = 5000 + round * 9000;
    const pid_t pid = LaunchChild("overwrite", 200000, false, 4);
    ASSERT_GT(pid, 0);
    ASSERT_TRUE(WaitForRecords(depth)) << "round " << round;
    ASSERT_TRUE(WaitForCoverage(200)) << "round " << round;
    ASSERT_EQ(::kill(pid, SIGKILL), 0);
    int st = 0;
    ::waitpid(pid, &st, 0);

    const uint64_t w = j_->w_observed.load(std::memory_order_acquire);
    const auto hist = History();
    const size_t covered = CoveredOps(hist, w);
    EXPECT_GE(covered, 200u)
        << "round " << round << " covered only " << covered;
    const std::string fail = Verify(db_path_, hist, w, false);
    EXPECT_TRUE(fail.empty()) << "round " << round << ": " << fail;
  }
}

// ---------------------------------------------------------------------------
// PROPERTY 3: clean shutdown loses nothing
// ---------------------------------------------------------------------------

TEST_F(CrashTest, CleanCloseMakesEverythingRecoverable) {
  const pid_t pid = LaunchChild("overwrite", 30000, true, 4);
  ASSERT_GT(pid, 0);
  int st = 0;
  ASSERT_EQ(::waitpid(pid, &st, 0), pid);
  ASSERT_TRUE(WIFEXITED(st)) << "child did not exit cleanly";
  ASSERT_EQ(WEXITSTATUS(st), 0);
  ASSERT_EQ(j_->clean_exit.load(std::memory_order_acquire), 1u);

  const auto hist = History();
  ASSERT_FALSE(hist.empty());
  // require_complete: after a clean close EVERY key must hold exactly
  // its final journaled value.
  const std::string fail = Verify(db_path_, hist, UINT64_MAX, true);
  EXPECT_TRUE(fail.empty()) << fail;
}

TEST_F(CrashTest, CleanCloseWithDistinctKeysRecoversAll) {
  const pid_t pid = LaunchChild("distinct", 20000, true, 2);
  ASSERT_GT(pid, 0);
  int st = 0;
  ASSERT_EQ(::waitpid(pid, &st, 0), pid);
  ASSERT_TRUE(WIFEXITED(st));
  ASSERT_EQ(WEXITSTATUS(st), 0);

  const auto hist = History();
  ASSERT_GT(hist.size(), 10000u);
  const std::string fail = Verify(db_path_, hist, UINT64_MAX, true);
  EXPECT_TRUE(fail.empty()) << fail;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc >= 8 && ::strcmp(argv[1], "--child") == 0) {
    return ChildWriter(argv[2], argv[3], argv[4], ::strtoull(argv[5], 0, 10),
                       ::strcmp(argv[6], "clean") == 0, ::atoi(argv[7]));
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
