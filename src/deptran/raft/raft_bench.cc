/**
 * raft_bench — a standalone performance driver for Mako's Raft replication.
 *
 * WHAT THIS MEASURES. One process of a real three-process Raft cluster on the
 * production build. The process that wins the election offers synthetic log
 * entries at a controlled rate with a controlled payload size and batch hint,
 * and measures the time from `add_log_to_nc()` to the leader's own apply
 * callback ("enqueue-to-apply"). It reports applied entries per second over a
 * measured window plus the latency distribution, and writes one structured
 * JSON record per run.
 *
 * WHAT IT DOES NOT MEASURE. Nothing above Raft: no Masstree, no STO
 * concurrency control, no transaction execution. That is the point — dbtest's
 * `agg_persist_throughput` counts TRANSACTIONS, so a change confined to Raft
 * moves it by an amount swamped by the layers above. See
 * docs/performance/raft-harness.md.
 *
 * WHY THREE PROCESSES AND NOT AN IN-PROCESS CLUSTER. The in-process harness
 * (`raft_lab_standalone`, `build_raftlab/deptran_server`) is compiled with
 * RAFT_TEST_CORO, where the leader no-op is compiled out and the heartbeat
 * interval is 100 ms against production's 5 ms. Its numbers describe a
 * different system. This binary is built by the ordinary `build/` tree and
 * launched three times, differing only in `-P` (localhost / p1 / p2), exactly
 * like bash/shard.sh does for dbtest.
 *
 * CAVEAT THE OUTPUT CANNOT CARRY. All three replicas resolve to 127.0.0.1, so
 * three processes buy address-space isolation, not network delay. Every number
 * here understates replication latency for a geo-distributed deployment.
 *
 * PAYLOAD LAYOUT. Latency is measured by embedding the enqueue time in the
 * payload and reading it back in the apply callback — the technique already
 * used by src/mako/benchmarks/paxos_async_commit_test.cc. Every offered entry
 * is laid out as:
 *
 *     [ 0,  8)  magic "RBENCH01"     — lets the callback ignore Raft no-ops
 *                                      and any foreign traffic
 *     [ 8, 24)  16-digit sequence number, zero-padded
 *     [24, 40)  16-digit enqueue time, microseconds since this process's
 *               steady_clock epoch, zero-padded
 *     [40,  N)  filler 'x'
 *
 * so --payload-bytes must be at least 40.
 *
 * SAFETY. This is a benchmark driver: a new .cc file that adds no inline-Rust
 * DSL items (see docs/plans/raft-perf-harness.txt trap T2). It is a bridge to
 * the not-yet-converted replication helper, so the calls into that API and the
 * C-library I/O it needs are annotated @unsafe at the boundary; the arithmetic
 * and the sample bookkeeping are @safe.
 */

#include <getopt.h>
#include <unistd.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "deptran/replication_helper.h"
#include "srpc_log.h"

import std;

namespace {

// ---------------------------------------------------------------------------
// Payload layout
// ---------------------------------------------------------------------------

constexpr const char kMagic[] = "RBENCH01";
constexpr int kMagicBytes = 8;
constexpr int kSeqBytes = 16;
constexpr int kStampBytes = 16;
constexpr int kHeaderBytes = kMagicBytes + kSeqBytes + kStampBytes;  // 40
constexpr char kFiller = 'x';

// @safe - pure arithmetic on a caller-owned buffer of at least `width` bytes
void write_fixed_decimal(char* dst, int width, uint64_t value) {
  for (int i = width - 1; i >= 0; --i) {
    dst[i] = static_cast<char>('0' + (value % 10));
    value /= 10;
  }
}

// @safe - pure arithmetic; reads exactly `width` digits
uint64_t read_fixed_decimal(const char* src, int width) {
  uint64_t value = 0;
  for (int i = 0; i < width; ++i) {
    const char c = src[i];
    if (c < '0' || c > '9') {
      return 0;
    }
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  return value;
}

// ---------------------------------------------------------------------------
// Clock. One steady_clock epoch per process; the enqueue stamp and the apply
// stamp are both offsets from it, so their difference is a true elapsed time
// and needs no cross-clock correction. Only the leader ever compares the two,
// and it does so inside a single process.
// ---------------------------------------------------------------------------

std::chrono::steady_clock::time_point g_epoch{};

// @safe - monotonic read against the process epoch
uint64_t now_us() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - g_epoch)
          .count());
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
  int partitions = 1;
  int replicas = 3;
  int payload_bytes = 1024;
  int batch = 1;
  // Offered entries per second across ALL partitions. 0 means unthrottled,
  // in which case --max-outstanding is the only thing shaping the offer.
  long long rate = 0;
  double duration_sec = 10.0;
  double warmup_sec = 2.0;
  // End-to-end in-flight bound the driver owns: offered minus applied, per
  // partition. Without it an unthrottled run measures queue growth rather
  // than the system, because RaftWorker::submit_queue_ is unbounded.
  long long max_outstanding = 4096;
  double leader_wait_sec = 30.0;
  // Cap on retained latency samples per partition. Throughput is counted with
  // an uncapped atomic, so hitting this cap costs percentile resolution only,
  // and the record says so via samples_dropped.
  long long max_samples = 4000000;
  int log_level = srpc::Log::WARN;
  std::string out_path{};
  std::string proc_name = "localhost";
  std::vector<std::string> configs{};
  std::string group_mode = "single";
  std::string label = "unlabelled";
  // Seconds a follower keeps serving after the leader's end markers, or after
  // the wall-clock budget expires if no end marker ever arrives.
  double follower_linger_sec = 5.0;
};

// @safe - pure text
void usage(const char* argv0) {
  std::printf(
      "raft_bench — standalone Raft performance driver\n"
      "\n"
      "Usage: %s --proc <localhost|p1|p2> --config <topology.yml> "
      "--config <config/raft.yml> [options]\n"
      "\n"
      "Cluster:\n"
      "  --proc NAME            this process's role: localhost, p1 or p2\n"
      "  --config FILE          replication config; repeat for each -f file.\n"
      "                         Pass the raftN_shardidxS.yml topology and\n"
      "                         config/raft.yml (which sets 'ab: raft').\n"
      "  --partitions N         Raft groups to drive (must match the config;\n"
      "                         in the generated configs N is also the worker\n"
      "                         thread count)\n"
      "  --replicas N           recorded for provenance only (default 3)\n"
      "  --group-mode MODE      single|multi; forwarded as --raft-groups\n"
      "\n"
      "Load:\n"
      "  --payload-bytes B      total entry size, >= %d (default 1024)\n"
      "  --batch K              batch hint passed to add_log_to_nc\n"
      "  --rate R               offered entries/sec across all partitions;\n"
      "                         0 = unthrottled (default 0)\n"
      "  --duration-sec S       measured window length (default 10)\n"
      "  --warmup-sec S         discarded prefix (default 2)\n"
      "  --max-outstanding N    per-partition in-flight bound, 0 = unbounded\n"
      "  --leader-wait-sec S    how long to wait for leadership (default 30)\n"
      "  --max-samples N        retained latency samples per partition\n"
      "\n"
      "Output:\n"
      "  --out PATH             JSON record path; written by the LEADER only\n"
      "  --label TEXT           free-form label copied into the record\n"
      "  --log-level N          0=FATAL 1=ERROR 2=WARN 3=INFO 4=DEBUG.\n"
      "                         Defaults to 2: the apply path logs one INFO\n"
      "                         line per applied entry, which would dominate\n"
      "                         any measurement taken at INFO.\n"
      "  --follower-linger-sec S how long a follower serves past the end\n"
      "  --help\n",
      argv0, kHeaderBytes);
}

enum LongOpt {
  kOptPartitions = 1000,
  kOptReplicas,
  kOptPayloadBytes,
  kOptBatch,
  kOptRate,
  kOptDuration,
  kOptWarmup,
  kOptMaxOutstanding,
  kOptLeaderWait,
  kOptMaxSamples,
  kOptLogLevel,
  kOptOut,
  kOptProc,
  kOptConfig,
  kOptGroupMode,
  kOptLabel,
  kOptFollowerLinger,
  kOptHelp,
};

// @unsafe { getopt_long and strtod/strtoll are not borrow-checked }
bool parse_options(int argc, char** argv, Options* opt) {
  static struct option long_options[] = {
      {"partitions", required_argument, nullptr, kOptPartitions},
      {"replicas", required_argument, nullptr, kOptReplicas},
      {"payload-bytes", required_argument, nullptr, kOptPayloadBytes},
      {"batch", required_argument, nullptr, kOptBatch},
      {"rate", required_argument, nullptr, kOptRate},
      {"duration-sec", required_argument, nullptr, kOptDuration},
      {"warmup-sec", required_argument, nullptr, kOptWarmup},
      {"max-outstanding", required_argument, nullptr, kOptMaxOutstanding},
      {"leader-wait-sec", required_argument, nullptr, kOptLeaderWait},
      {"max-samples", required_argument, nullptr, kOptMaxSamples},
      {"log-level", required_argument, nullptr, kOptLogLevel},
      {"out", required_argument, nullptr, kOptOut},
      {"proc", required_argument, nullptr, kOptProc},
      {"config", required_argument, nullptr, kOptConfig},
      {"group-mode", required_argument, nullptr, kOptGroupMode},
      {"label", required_argument, nullptr, kOptLabel},
      {"follower-linger-sec", required_argument, nullptr, kOptFollowerLinger},
      {"help", no_argument, nullptr, kOptHelp},
      {nullptr, 0, nullptr, 0},
  };

  optind = 1;
  while (true) {
    const int c = getopt_long(argc, argv, "", long_options, nullptr);
    if (c == -1) {
      break;
    }
    switch (c) {
      case kOptPartitions: opt->partitions = std::atoi(optarg); break;
      case kOptReplicas: opt->replicas = std::atoi(optarg); break;
      case kOptPayloadBytes: opt->payload_bytes = std::atoi(optarg); break;
      case kOptBatch: opt->batch = std::atoi(optarg); break;
      case kOptRate: opt->rate = std::atoll(optarg); break;
      case kOptDuration: opt->duration_sec = std::atof(optarg); break;
      case kOptWarmup: opt->warmup_sec = std::atof(optarg); break;
      case kOptMaxOutstanding: opt->max_outstanding = std::atoll(optarg); break;
      case kOptLeaderWait: opt->leader_wait_sec = std::atof(optarg); break;
      case kOptMaxSamples: opt->max_samples = std::atoll(optarg); break;
      case kOptLogLevel: opt->log_level = std::atoi(optarg); break;
      case kOptOut: opt->out_path = optarg; break;
      case kOptProc: opt->proc_name = optarg; break;
      case kOptConfig: opt->configs.emplace_back(optarg); break;
      case kOptGroupMode: opt->group_mode = optarg; break;
      case kOptLabel: opt->label = optarg; break;
      case kOptFollowerLinger: opt->follower_linger_sec = std::atof(optarg); break;
      case kOptHelp: usage(argv[0]); return false;
      default: usage(argv[0]); return false;
    }
  }

  if (opt->configs.empty()) {
    std::fprintf(stderr, "raft_bench: --config is required (pass the topology yml and config/raft.yml)\n");
    return false;
  }
  if (opt->partitions <= 0) {
    std::fprintf(stderr, "raft_bench: --partitions must be positive\n");
    return false;
  }
  if (opt->payload_bytes < kHeaderBytes) {
    std::fprintf(stderr, "raft_bench: --payload-bytes must be >= %d (the stamped header)\n", kHeaderBytes);
    return false;
  }
  if (opt->batch < 1) {
    std::fprintf(stderr, "raft_bench: --batch must be >= 1\n");
    return false;
  }
  if (opt->duration_sec <= 0.0) {
    std::fprintf(stderr, "raft_bench: --duration-sec must be positive\n");
    return false;
  }
  if (opt->warmup_sec < 0.0) {
    std::fprintf(stderr, "raft_bench: --warmup-sec must not be negative\n");
    return false;
  }
  if (opt->group_mode != "single" && opt->group_mode != "multi") {
    std::fprintf(stderr, "raft_bench: --group-mode must be single or multi\n");
    return false;
  }
  if (opt->rate < 0) {
    std::fprintf(stderr, "raft_bench: --rate must be >= 0 (0 means unthrottled)\n");
    return false;
  }
  if (opt->rate > 100000000LL) {
    // The pacer works in nanoseconds, so it can express up to 1e9/s per
    // partition; beyond that the only honest answer is --rate 0.
    std::fprintf(stderr, "raft_bench: --rate above 100000000 is not pace-able; use --rate 0\n");
    return false;
  }
  if (opt->max_outstanding < 0) {
    std::fprintf(stderr, "raft_bench: --max-outstanding must be >= 0 (0 means unbounded)\n");
    return false;
  }
  if (opt->max_outstanding == 0) {
    std::fprintf(stderr,
                 "raft_bench: WARNING: --max-outstanding 0 removes the in-flight bound. "
                 "RaftWorker::submit_queue_ is unbounded and holds a copy of every "
                 "payload, so an unthrottled run measures queue growth and may exhaust "
                 "memory.\n");
  }
  if (opt->log_level < 0 || opt->log_level > 4) {
    std::fprintf(stderr, "raft_bench: --log-level must be 0..4\n");
    return false;
  }
  if (opt->leader_wait_sec < 0.0) {
    std::fprintf(stderr, "raft_bench: --leader-wait-sec must not be negative\n");
    return false;
  }
  if (opt->max_samples < 1) {
    opt->max_samples = 1;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Per-partition measurement state
//
// The apply callback runs on Raft's apply thread, not the poll thread, so it
// must stay cheap and must not take a lock the replication path holds (trap
// T6). It records one sample into a preallocated slot claimed with a single
// fetch_add and returns; percentiles are computed after the run.
// ---------------------------------------------------------------------------

struct PartitionState {
  // Latency samples, microseconds, preallocated. A slot is claimed with one
  // relaxed fetch_add on samples_claimed; the value is written; then
  // samples_committed is bumped with release. main() reads samples_committed
  // with acquire, which is the happens-before edge that makes the written
  // slots visible. Each PartitionState has at most ONE concurrent writer —
  // in single-group mode one apply thread serves every partition serially, in
  // multi-group mode there is one apply thread per partition — so committed
  // slots are always the contiguous prefix [0, samples_committed).
  std::vector<uint32_t> samples{};
  std::atomic<size_t> samples_claimed{0};
  std::atomic<size_t> samples_committed{0};
  std::atomic<long long> samples_dropped{0};

  std::atomic<long long> applied_total{0};
  std::atomic<long long> applied_in_window{0};
  std::atomic<long long> offered_total{0};
  std::atomic<long long> offered_in_window{0};
  std::atomic<long long> offer_rejected{0};
  // Nanoseconds of offering the driver's own in-flight bound prevented. See
  // offer_loop for what is charged: real sleep time when unthrottled, and the
  // full value of the skipped slot when throttled.
  std::atomic<long long> stalled_ns{0};
  std::atomic<long long> foreign_applied{0};

  // Log integrity. Every offered entry carries a per-partition sequence
  // number, numbered from 1, in the header field the apply callback used to
  // skip over. Reading it back and comparing against the last one seen gives
  // ordering, no-loss and no-duplicate for this partition's applied stream in
  // a single pass — on the leader and on every follower, since both register
  // the same callback.
  //
  // Single writer, exactly like the latency samples above: at most one apply
  // thread touches a given PartitionState, so relaxed is enough and main()
  // reads them only after that thread has stopped.
  //
  // A true loss shows up as gaps > 0 with duplicates == 0. A pure reordering,
  // which Raft's in-order apply should make impossible, shows up as one gap
  // and one duplicate; out_of_order counts the breaks either way, so any
  // non-zero value is a failure regardless of how the three split.
  std::atomic<uint64_t> last_seq{0};
  std::atomic<long long> out_of_order{0};
  std::atomic<long long> gaps{0};
  std::atomic<long long> duplicates{0};
  // Leadership probes carry sequence 0 and are outside the ordered stream.
  std::atomic<long long> probes_applied{0};

  // Driver-owned in-flight depth: offered minus applied.
  std::atomic<long long> in_flight{0};
  std::atomic<long long> peak_raft_outstanding{0};

  std::atomic<int> end_markers{0};
};

// Fold one applied entry's sequence number into this partition's integrity
// counters. Called only from the apply callback, which has a single writer
// per PartitionState.
//
// Sequence 0 is a leadership probe: offer_loop numbers real entries from 1
// (`++seq` precedes the write at every offer site), while the probe loop
// writes a literal 0. A probe carries no ordering information, so it is
// counted and otherwise ignored.
//
// Counting only what arrived means a stream that is cut short — the drain
// window closing on entries still in flight, or an offer rejected when
// leadership moves — is not reported as loss. That case is what
// offered_total versus applied_total is for.
// @safe - relaxed single-writer bookkeeping over plain counters
void check_sequence(PartitionState* st, uint64_t seq) {
  if (seq == 0) {
    st->probes_applied.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint64_t last = st->last_seq.load(std::memory_order_relaxed);
  const uint64_t expected = last + 1;
  if (seq != expected) {
    st->out_of_order.fetch_add(1, std::memory_order_relaxed);
    if (seq > expected) {
      st->gaps.fetch_add(static_cast<long long>(seq - expected),
                         std::memory_order_relaxed);
    } else {
      st->duplicates.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (seq > last) {
    st->last_seq.store(seq, std::memory_order_relaxed);
  }
}

// Measured window, in microseconds since the process epoch. Published by the
// offer loop before it starts and read by the apply callback. Until it is
// published, `window_valid` is false and every sample counts as out-of-window.
std::atomic<bool> g_window_valid{false};
std::atomic<uint64_t> g_window_begin_us{0};
std::atomic<uint64_t> g_window_end_us{0};

// @safe - pure predicate over published window bounds
bool in_window(uint64_t apply_us) {
  if (!g_window_valid.load(std::memory_order_acquire)) {
    return false;
  }
  return apply_us >= g_window_begin_us.load(std::memory_order_relaxed) &&
         apply_us <= g_window_end_us.load(std::memory_order_relaxed);
}

// Close the measured window early, at `when`. Called when an offer loop loses
// leadership: without it the denominator shrinks to the moment the offering
// stopped while the numerator keeps accruing from the backlog still
// committing, which inflates applied_per_sec by whatever the in-flight depth
// happened to be.
// @safe - monotone narrowing of a published bound
void close_window_at(uint64_t when) {
  uint64_t current = g_window_end_us.load(std::memory_order_relaxed);
  while (when < current &&
         !g_window_end_us.compare_exchange_weak(current, when,
                                                std::memory_order_relaxed)) {
  }
}

// ---------------------------------------------------------------------------
// Percentiles. Computed once at the end over the retained in-window samples,
// nearest-rank on the sorted vector. Copied in shape from bench.cc's latency
// arithmetic (definitions reused, code not — Mako's numbers are counters on
// the transaction hot path and arithmetic in the tail of bench.cc, not a
// library that can be called).
// ---------------------------------------------------------------------------

// @safe - reads a sorted vector
double percentile_us(const std::vector<uint32_t>& sorted, double p) {
  if (sorted.empty()) {
    return 0.0;
  }
  double rank = p * static_cast<double>(sorted.size());
  size_t idx = static_cast<size_t>(rank);
  if (rank > static_cast<double>(idx)) {
    idx += 1;
  }
  if (idx == 0) {
    idx = 1;
  }
  if (idx > sorted.size()) {
    idx = sorted.size();
  }
  return static_cast<double>(sorted[idx - 1]);
}

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------

#ifndef RAFT_BENCH_BUILD_FLAVOUR
#define RAFT_BENCH_BUILD_FLAVOUR "unknown"
#endif
#ifndef RAFT_BENCH_CMAKE_BUILD_TYPE
#define RAFT_BENCH_CMAKE_BUILD_TYPE "unknown"
#endif

// @unsafe { std::getenv is not borrow-checked }
std::string env_or(const char* name, const char* fallback) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') {
    return std::string(fallback);
  }
  return std::string(v);
}

// @unsafe { gethostname is a libc call }
std::string hostname() {
  char buf[256];
  buf[0] = '\0';
  if (::gethostname(buf, sizeof(buf) - 1) != 0) {
    return std::string("unknown");
  }
  buf[sizeof(buf) - 1] = '\0';
  return std::string(buf);
}

// @unsafe { gmtime_r and strftime are libc calls }
std::string utc_now_iso8601() {
  const std::time_t t = std::time(nullptr);
  std::tm tm_utc{};
  if (::gmtime_r(&t, &tm_utc) == nullptr) {
    return std::string("unknown");
  }
  char buf[32];
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
    return std::string("unknown");
  }
  return std::string(buf);
}

// @safe - escapes the small subset of JSON string escapes these values need
std::string json_escape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char esc[8];
          std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(c) & 0xff);
          out += esc;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// The record. Flat, no nesting, so scripts/raft_perf/processing.py can read a
// directory of these without a schema walk. Every input parameter and every
// metric is present, plus enough provenance to identify the run without the
// surrounding directory (plan step 4, decision D3).
// ---------------------------------------------------------------------------

struct Record {
  std::string commit;
  std::string date;
  std::string host;
  std::string build_flavour;
  std::string cmake_build_type;
  bool raft_test_coro = false;
  bool raft_default_single_group = false;
  std::string config;
  int partitions = 0;
  int replicas = 0;
  std::string group_mode = "single";
  int payload_bytes = 0;
  int batch = 0;
  long long offered_rate = 0;
  double duration_sec = 0.0;
  double warmup_sec = 0.0;
  long long max_outstanding = 0;
  double leader_wait_sec = 0.0;
  long long max_samples = 0;
  double follower_linger_sec = 0.0;
  int log_level = 0;
  std::string proc = "localhost";
  std::string role = "leader";
  std::string label{};

  double measured_window_sec = 0.0;
  double applied_per_sec = 0.0;
  double offered_per_sec = 0.0;
  long long offered_total = 0;
  long long applied_total = 0;
  long long offered_in_window = 0;
  long long applied_in_window = 0;
  long long offer_rejected = 0;
  // Seconds the offer threads spent blocked on the driver's in-flight bound,
  // summed across partitions. Large relative to the window means the run did
  // not reach its requested rate.
  double offer_stalled_sec = 0.0;
  long long foreign_applied = 0;
  long long out_of_order = 0;
  long long gaps = 0;
  long long duplicates = 0;
  long long probes_applied = 0;
  long long samples_used = 0;
  long long samples_dropped = 0;
  double latency_mean_us = 0.0;
  double latency_p50_us = 0.0;
  double latency_p90_us = 0.0;
  double latency_p99_us = 0.0;
  double latency_p999_us = 0.0;
  double latency_max_us = 0.0;
  long long peak_outstanding = 0;
  // Leadership transitions this process was notified of. Non-zero beyond the
  // initial election means the cluster flapped during the run.
  long long leadership_changes = 0;
  // Gap 9: the aggregate hides a straggler. Six partitions averaged into one
  // number dilute one slow partition to a sixth of its weight. These three
  // make it visible without nesting.
  double applied_per_sec_per_partition = 0.0;
  long long min_partition_applied_in_window = 0;
  long long max_partition_applied_in_window = 0;
  // Latency CDF as percentile -> microseconds, 1..99 plus the tail. The CDF
  // plot consumes this directly; the raw samples are not retained on disk.
  std::vector<std::pair<std::string, double>> latency_cdf_us{};
};

// @unsafe { C stdio is not borrow-checked }
bool write_record(const std::string& path, const Record& r) {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (f == nullptr) {
    std::fprintf(stderr, "raft_bench: cannot open --out '%s' for writing\n", path.c_str());
    return false;
  }
  std::fprintf(f, "{\n");
  std::fprintf(f, "  \"commit\": \"%s\",\n", json_escape(r.commit).c_str());
  std::fprintf(f, "  \"date\": \"%s\",\n", json_escape(r.date).c_str());
  std::fprintf(f, "  \"host\": \"%s\",\n", json_escape(r.host).c_str());
  std::fprintf(f, "  \"build_flavour\": \"%s\",\n", json_escape(r.build_flavour).c_str());
  std::fprintf(f, "  \"cmake_build_type\": \"%s\",\n", json_escape(r.cmake_build_type).c_str());
  std::fprintf(f, "  \"raft_test_coro\": %s,\n", r.raft_test_coro ? "true" : "false");
  std::fprintf(f, "  \"raft_default_single_group\": %s,\n",
               r.raft_default_single_group ? "true" : "false");
  std::fprintf(f, "  \"config\": \"%s\",\n", json_escape(r.config).c_str());
  std::fprintf(f, "  \"partitions\": %d,\n", r.partitions);
  std::fprintf(f, "  \"replicas\": %d,\n", r.replicas);
  std::fprintf(f, "  \"group_mode\": \"%s\",\n", json_escape(r.group_mode).c_str());
  std::fprintf(f, "  \"payload_bytes\": %d,\n", r.payload_bytes);
  std::fprintf(f, "  \"batch\": %d,\n", r.batch);
  std::fprintf(f, "  \"offered_rate\": %lld,\n", r.offered_rate);
  std::fprintf(f, "  \"duration_sec\": %.6f,\n", r.duration_sec);
  std::fprintf(f, "  \"warmup_sec\": %.6f,\n", r.warmup_sec);
  std::fprintf(f, "  \"max_outstanding\": %lld,\n", r.max_outstanding);
  std::fprintf(f, "  \"leader_wait_sec\": %.6f,\n", r.leader_wait_sec);
  std::fprintf(f, "  \"max_samples\": %lld,\n", r.max_samples);
  std::fprintf(f, "  \"follower_linger_sec\": %.6f,\n", r.follower_linger_sec);
  std::fprintf(f, "  \"log_level\": %d,\n", r.log_level);
  std::fprintf(f, "  \"proc\": \"%s\",\n", json_escape(r.proc).c_str());
  std::fprintf(f, "  \"role\": \"%s\",\n", json_escape(r.role).c_str());
  std::fprintf(f, "  \"label\": \"%s\",\n", json_escape(r.label).c_str());
  std::fprintf(f, "  \"measured_window_sec\": %.6f,\n", r.measured_window_sec);
  std::fprintf(f, "  \"applied_per_sec\": %.3f,\n", r.applied_per_sec);
  std::fprintf(f, "  \"offered_per_sec\": %.3f,\n", r.offered_per_sec);
  std::fprintf(f, "  \"offered_total\": %lld,\n", r.offered_total);
  std::fprintf(f, "  \"applied_total\": %lld,\n", r.applied_total);
  std::fprintf(f, "  \"offered_in_window\": %lld,\n", r.offered_in_window);
  std::fprintf(f, "  \"applied_in_window\": %lld,\n", r.applied_in_window);
  std::fprintf(f, "  \"offer_rejected\": %lld,\n", r.offer_rejected);
  std::fprintf(f, "  \"offer_stalled_sec\": %.6f,\n", r.offer_stalled_sec);
  std::fprintf(f, "  \"foreign_applied\": %lld,\n", r.foreign_applied);
  std::fprintf(f, "  \"out_of_order\": %lld,\n", r.out_of_order);
  std::fprintf(f, "  \"gaps\": %lld,\n", r.gaps);
  std::fprintf(f, "  \"duplicates\": %lld,\n", r.duplicates);
  std::fprintf(f, "  \"probes_applied\": %lld,\n", r.probes_applied);
  std::fprintf(f, "  \"samples_used\": %lld,\n", r.samples_used);
  std::fprintf(f, "  \"samples_dropped\": %lld,\n", r.samples_dropped);
  std::fprintf(f, "  \"latency_mean_us\": %.3f,\n", r.latency_mean_us);
  std::fprintf(f, "  \"latency_p50_us\": %.3f,\n", r.latency_p50_us);
  std::fprintf(f, "  \"latency_p90_us\": %.3f,\n", r.latency_p90_us);
  std::fprintf(f, "  \"latency_p99_us\": %.3f,\n", r.latency_p99_us);
  std::fprintf(f, "  \"latency_p999_us\": %.3f,\n", r.latency_p999_us);
  std::fprintf(f, "  \"latency_max_us\": %.3f,\n", r.latency_max_us);
  std::fprintf(f, "  \"peak_outstanding\": %lld,\n", r.peak_outstanding);
  std::fprintf(f, "  \"leadership_changes\": %lld,\n", r.leadership_changes);
  std::fprintf(f, "  \"applied_per_sec_per_partition\": %.3f,\n",
               r.applied_per_sec_per_partition);
  std::fprintf(f, "  \"min_partition_applied_in_window\": %lld,\n",
               r.min_partition_applied_in_window);
  std::fprintf(f, "  \"max_partition_applied_in_window\": %lld,\n",
               r.max_partition_applied_in_window);
  std::fprintf(f, "  \"latency_cdf_us\": {");
  for (size_t i = 0; i < r.latency_cdf_us.size(); ++i) {
    std::fprintf(f, "%s\n    \"%s\": %.3f", i == 0 ? "" : ",",
                 json_escape(r.latency_cdf_us[i].first).c_str(),
                 r.latency_cdf_us[i].second);
  }
  std::fprintf(f, "\n  }\n");
  std::fprintf(f, "}\n");
  const bool ok = (std::fclose(f) == 0);
  return ok;
}

// ---------------------------------------------------------------------------
// Offer loop
// ---------------------------------------------------------------------------

// @safe - monotonic read against the process epoch, nanosecond resolution
uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - g_epoch)
          .count());
}

// @safe - deadline arithmetic only
//
// Nanoseconds, not microseconds: a microsecond interval computed as
// 1000000/rate truncates to ZERO for any per-partition rate above 1e6, and a
// zero interval would read as "unthrottled" — turning a requested 2,000,000/s
// into a saturation run that the record still labels 2,000,000/s. It also
// removes the systematic overshoot integer microseconds cause at high rates
// (700,000/s would round to a 1 us interval, i.e. 1,000,000/s offered).
void pace_until(uint64_t target_ns) {
  const uint64_t now = now_ns();
  if (target_ns <= now) {
    return;
  }
  const uint64_t remaining = target_ns - now;
  // sleep_for below ~200 us is dominated by wake-up jitter, so spin the tail.
  if (remaining > 200000) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(remaining - 100000));
  }
  while (now_ns() < target_ns) {
    std::this_thread::yield();
  }
}

// @unsafe { calls the replication helper, which is not borrow-checked }
//
// `unthrottled` is passed explicitly rather than inferred from a zero rate.
// Inferring it is wrong: --rate 1 across 6 partitions divides to 0 for five of
// them, and a thread that reads its own zero rate as "unthrottled" would
// saturate instead of standing down, silently turning a 1 entry/sec point into
// a saturation point.
void offer_loop(const Options& opt,
                uint32_t par_id,
                bool unthrottled,
                long long rate_for_this_partition,
                uint64_t offer_begin_us,
                uint64_t offer_end_us,
                PartitionState* st) {
  if (!unthrottled && rate_for_this_partition <= 0) {
    // This partition's share of the requested aggregate rounded to nothing.
    // The low-numbered partitions carry the whole rate; this one offers
    // nothing, which is what the caller was warned about.
    return;
  }

  std::vector<char> payload(static_cast<size_t>(opt.payload_bytes), kFiller);
  std::memcpy(payload.data(), kMagic, static_cast<size_t>(kMagicBytes));

  // Sub-200-microsecond intervals are spun rather than slept; see pace_until.
  // At the top of the rate array that means one busy thread per partition,
  // which is ordinary for a load generator but does consume a core each.
  const uint64_t interval_ns =
      unthrottled
          ? 0
          : static_cast<uint64_t>(1000000000.0 /
                                  static_cast<double>(rate_for_this_partition));

  const uint64_t offer_end_ns = offer_end_us * 1000ULL;
  uint64_t seq = 0;
  uint64_t next_send_ns = offer_begin_us * 1000ULL;

  while (true) {
    const uint64_t now_n = now_ns();
    if (now_n >= offer_end_ns) {
      break;
    }

    if (interval_ns > 0) {
      if (next_send_ns > offer_end_ns) {
        break;
      }
      pace_until(next_send_ns);
      // Do not accumulate unbounded debt: if the pacer has fallen more than
      // one second behind, resynchronise rather than sprinting to catch up,
      // which would report an offered rate the harness never actually paced.
      const uint64_t after = now_ns();
      if (after > next_send_ns + 1000000000ULL) {
        next_send_ns = after;
      }
      next_send_ns += interval_ns;
    }

    if (opt.max_outstanding > 0 &&
        st->in_flight.load(std::memory_order_relaxed) >= opt.max_outstanding) {
      if (interval_ns == 0) {
        // Unthrottled: the in-flight bound IS the pacer, so wait for room and
        // charge the wall time that wait actually cost.
        const uint64_t stall_begin = now_ns();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        st->stalled_ns.fetch_add(static_cast<long long>(now_ns() - stall_begin),
                                 std::memory_order_relaxed);
      } else {
        // Throttled: skip this slot rather than waiting. A run that cannot
        // keep up is a real result; offered-versus-applied shows the
        // shortfall. Charge the SLOT, not the branch — the branch does no
        // work, so timing it would read as zero and the stall diagnostic
        // would be dead for every throttled point, which is 20 of the 21
        // points in every rate array. The time this thread actually lost was
        // spent in pace_until above, and the slot's interval is its value.
        st->stalled_ns.fetch_add(static_cast<long long>(interval_ns),
                                 std::memory_order_relaxed);
      }
      continue;
    }

    ++seq;
    write_fixed_decimal(payload.data() + kMagicBytes, kSeqBytes, seq);
    const uint64_t stamp = now_us();
    write_fixed_decimal(payload.data() + kMagicBytes + kSeqBytes, kStampBytes, stamp);

    st->in_flight.fetch_add(1, std::memory_order_relaxed);
    // @unsafe
    const bool accepted =
        add_log_to_nc(payload.data(), opt.payload_bytes, par_id, opt.batch);
    if (!accepted) {
      st->in_flight.fetch_sub(1, std::memory_order_relaxed);
      st->offer_rejected.fetch_add(1, std::memory_order_relaxed);
      // Leadership is gone; nothing this loop offers will be applied. Close
      // the measured window here, so the backlog that keeps committing
      // afterwards is not counted against a denominator that stopped.
      close_window_at(now_us());
      break;
    }

    st->offered_total.fetch_add(1, std::memory_order_relaxed);
    if (in_window(stamp)) {
      st->offered_in_window.fetch_add(1, std::memory_order_relaxed);
    }

    // Sample rather than poll. get_outstanding_logs() takes the replication
    // helper's global callback mutex, which the apply path also takes, so
    // calling it once per offered entry would put the measurement on the hot
    // path it is measuring. Every 64th entry is plenty to catch a peak.
    if ((seq & 0x3F) == 0) {
      // @unsafe
      const long long outstanding = static_cast<long long>(get_outstanding_logs(par_id));
      if (outstanding > st->peak_raft_outstanding.load(std::memory_order_relaxed)) {
        st->peak_raft_outstanding.store(outstanding, std::memory_order_relaxed);
      }
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

// @unsafe { drives the replication helper and C stdio; the helper is a
//           not-yet-converted boundary and is annotated as such at each call }
int main(int argc, char** argv) {
  g_epoch = std::chrono::steady_clock::now();

  Options opt;
  if (!parse_options(argc, argv, &opt)) {
    return 2;
  }

  // The Raft apply path emits one Log_info line per applied entry
  // ("[APPLY-LOGS] site=... applying index=..."), and janus's static
  // initialiser leaves the level at INFO. Measuring at INFO measures the
  // logger, so lower it before setup() brings any worker up.
  srpc::Log::set_level(opt.log_level);

  // Make the group mode explicit and authoritative rather than inheriting the
  // compile-time default (trap T3: SINGLE_RAFT_INSTANCE is ON, so the default
  // is one Raft instance per process with the other partitions' ports served
  // by stub servers behind one shared recursive mutex).
  const std::string raft_groups_arg = "--raft-groups=" + opt.group_mode;

  std::printf("[raft_bench:%s] partitions=%d payload=%dB batch=%d rate=%lld "
              "warmup=%.1fs duration=%.1fs group=%s\n",
              opt.proc_name.c_str(), opt.partitions, opt.payload_bytes, opt.batch,
              opt.rate, opt.warmup_sec, opt.duration_sec, opt.group_mode.c_str());
  std::fflush(stdout);

  // Route the unqualified replication_helper entry points to raft_impl. This
  // must happen before setup(); without it the default is PAXOS and setup()
  // goes through paxos_main_helper, which does not disable Jetpack recovery
  // and yields RPC handler mismatches at runtime.
  janus::set_replication_type(janus::ReplicationType::RAFT);

  // Build the replication argv by hand, mirroring
  // src/mako/benchmarks/paxos_async_commit_test.cc and
  // examples/mako-raft-tests/simpleRaft.cc.
  //
  // -T is deliberately 0: RaftWorker::WaitForSubmit() spins while
  // n_submit < Config::get_tot_req(), so a non-zero -T would make
  // wait_for_submit() block until that many entries had been submitted
  // regardless of what this run actually offered.
  const std::string duration_arg =
      std::to_string(static_cast<long long>(opt.warmup_sec + opt.duration_sec +
                                            opt.leader_wait_sec + 60.0));
  std::vector<std::string> argv_storage;
  argv_storage.emplace_back("");
  argv_storage.emplace_back("-b");
  argv_storage.emplace_back("-d");
  argv_storage.emplace_back(duration_arg);
  for (const auto& cfg : opt.configs) {
    argv_storage.emplace_back("-f");
    argv_storage.emplace_back(cfg);
  }
  argv_storage.emplace_back("-t");
  argv_storage.emplace_back("30");
  argv_storage.emplace_back("-T");
  argv_storage.emplace_back("0");
  argv_storage.emplace_back("-n");
  argv_storage.emplace_back("32");
  argv_storage.emplace_back("-P");
  argv_storage.emplace_back(opt.proc_name);
  argv_storage.emplace_back("-A");
  argv_storage.emplace_back("10000");
  argv_storage.emplace_back(raft_groups_arg);

  std::vector<char*> replication_argv;
  replication_argv.reserve(argv_storage.size());
  for (auto& s : argv_storage) {
    replication_argv.push_back(const_cast<char*>(s.c_str()));
  }

  // @unsafe
  const std::vector<std::string> sites =
      setup(static_cast<int>(replication_argv.size()), replication_argv.data());
  if (sites.empty()) {
    std::fprintf(stderr, "[raft_bench:%s] setup() failed\n", opt.proc_name.c_str());
    return 1;
  }
  if (static_cast<int>(sites.size()) != opt.partitions) {
    std::fprintf(stderr,
                 "[raft_bench:%s] WARNING: --partitions %d but the config gives this "
                 "process %zu local sites; the config is authoritative\n",
                 opt.proc_name.c_str(), opt.partitions, sites.size());
  }

  const int partitions = opt.partitions;
  std::vector<PartitionState> state(static_cast<size_t>(partitions));
  for (auto& st : state) {
    st.samples.resize(static_cast<size_t>(opt.max_samples));
  }

  std::atomic<int> leadership_notifications{0};
  // @unsafe
  register_leader_election_callback([&opt, &leadership_notifications](int control) {
    leadership_notifications.fetch_add(1, std::memory_order_relaxed);
    std::printf("[raft_bench:%s] leadership notification: %s\n", opt.proc_name.c_str(),
                control == 1 ? "became leader" : "lost leadership");
    std::fflush(stdout);
  });

  // One apply callback per partition, used for both roles. On the leader it is
  // the measurement point; on a follower it only counts end markers.
  //
  // The return value follows Mako's replay watermark protocol,
  // `timestamp * 10 + status`. The status is always STATUS_NORMAL (0) here:
  // Raft filters its own TpcNoopCommand entries before invoking the
  // application callback (src/deptran/raft/server.cc:1400-1402), so the
  // short-entry "no-op" case other drivers special-case cannot reach this
  // code, and RaftWorker::Next only acts on the status when it equals
  // STATUS_SAFETY_FAIL.
  for (int i = 0; i < partitions; ++i) {
    const uint32_t par_id = static_cast<uint32_t>(i);
    PartitionState* st = &state[static_cast<size_t>(i)];

    auto apply_cb = [st, &opt](const char*& log, int len, int /*par_id*/, int /*slot_id*/,
                               std::queue<std::tuple<int, int, int, int, const char*>>&) -> int {
      const uint64_t apply_us = now_us();
      constexpr int kStatusNormal = 0;
      if (len == 0) {
        st->end_markers.fetch_add(1, std::memory_order_relaxed);
      } else if (len >= kHeaderBytes && log != nullptr &&
                 std::memcmp(log, kMagic, static_cast<size_t>(kMagicBytes)) == 0) {
        check_sequence(st, read_fixed_decimal(log + kMagicBytes, kSeqBytes));
        const uint64_t stamp =
            read_fixed_decimal(log + kMagicBytes + kSeqBytes, kStampBytes);
        const uint64_t latency_us = apply_us > stamp ? apply_us - stamp : 0;

        st->in_flight.fetch_sub(1, std::memory_order_relaxed);
        st->applied_total.fetch_add(1, std::memory_order_relaxed);
        if (in_window(apply_us)) {
          st->applied_in_window.fetch_add(1, std::memory_order_relaxed);
          const size_t slot = st->samples_claimed.fetch_add(1, std::memory_order_relaxed);
          if (slot < static_cast<size_t>(opt.max_samples)) {
            st->samples[slot] =
                static_cast<uint32_t>(std::min<uint64_t>(latency_us, UINT32_MAX));
            st->samples_committed.fetch_add(1, std::memory_order_release);
          } else {
            st->samples_dropped.fetch_add(1, std::memory_order_relaxed);
          }
        }
      } else {
        st->foreign_applied.fetch_add(1, std::memory_order_relaxed);
      }
      return static_cast<int>(apply_us % 100000000ULL) * 10 + kStatusNormal;
    };

    // @unsafe
    register_for_leader_par_id_return(apply_cb, par_id);
    // @unsafe
    register_for_follower_par_id_return(apply_cb, par_id);
  }

  // @unsafe
  setup2(0, 0);
  std::printf("[raft_bench:%s] setup2() complete; waiting for leadership\n",
              opt.proc_name.c_str());
  std::fflush(stdout);

  // Decide the role by probing the offer path itself rather than by watching
  // the election callback: the callback carries no partition id, whereas
  // add_log_to_nc()'s bool return IS the authoritative per-partition
  // leadership answer (RaftServer::Start() checks under the server lock). In
  // multi-group mode each partition elects independently, so a process only
  // offers load if it leads EVERY partition it was asked to drive.
  //
  // Probes are stamped like real entries, but they land before the measured
  // window opens, so the window filter discards them.
  //
  // The race this loop must not lose: a follower that keeps probing for the
  // whole --leader-wait-sec will win an election the moment the real leader
  // finishes and shuts down, and will then run a second, spurious
  // measurement. So the moment this process applies an entry it did not
  // propose, a leader exists elsewhere and this process settles as a follower
  // for good. Its own accepted probes are excluded from that evidence.
  std::vector<char> probe(static_cast<size_t>(opt.payload_bytes), kFiller);
  std::memcpy(probe.data(), kMagic, static_cast<size_t>(kMagicBytes));

  bool is_leader = false;
  bool partial_leadership = false;
  long long probes_accepted = 0;
  const uint64_t leader_deadline_us =
      now_us() + static_cast<uint64_t>(opt.leader_wait_sec * 1e6);
  while (now_us() < leader_deadline_us) {
    if (probes_accepted == 0) {
      long long applied_elsewhere = 0;
      for (auto& st : state) {
        applied_elsewhere += st.applied_total.load(std::memory_order_relaxed);
      }
      if (applied_elsewhere > 0) {
        std::printf("[raft_bench:%s] another replica is leading (saw %lld applied "
                    "entries); settling as follower\n",
                    opt.proc_name.c_str(), applied_elsewhere);
        std::fflush(stdout);
        break;
      }
    }

    int accepted_this_round = 0;
    for (int i = 0; i < partitions; ++i) {
      write_fixed_decimal(probe.data() + kMagicBytes, kSeqBytes, 0);
      write_fixed_decimal(probe.data() + kMagicBytes + kSeqBytes, kStampBytes, now_us());
      // The probe carries the same magic as a real entry, so the apply
      // callback will decrement in_flight for it. Increment here to match, or
      // the driver's in-flight bound drifts by one per accepted probe.
      state[static_cast<size_t>(i)].in_flight.fetch_add(1, std::memory_order_relaxed);
      // @unsafe
      if (add_log_to_nc(probe.data(), opt.payload_bytes, static_cast<uint32_t>(i), 1)) {
        ++accepted_this_round;
        ++probes_accepted;
        // A probe is a real, stamped entry that the apply callback will count,
        // so it must be counted as offered too. Without this applied_total
        // exceeds offered_total by one per partition, which reads as an
        // instrumentation bug in exactly the pair the plan asks for so that a
        // shortfall is legible. It lands before the window opens, so it does
        // not touch offered_in_window.
        state[static_cast<size_t>(i)].offered_total.fetch_add(1, std::memory_order_relaxed);
      } else {
        state[static_cast<size_t>(i)].in_flight.fetch_sub(1, std::memory_order_relaxed);
      }
    }
    if (accepted_this_round == partitions) {
      is_leader = true;
      break;
    }
    if (accepted_this_round > 0) {
      partial_leadership = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  int exit_code = 0;
  if (!is_leader && partial_leadership) {
    // Leading a subset would produce a number for a cluster shape nobody
    // asked for. Say so, and exit non-zero, instead of quietly serving on as
    // if this were an ordinary follower.
    std::fprintf(stderr,
                 "[raft_bench:%s] leads some but not all %d partitions after %.0fs; "
                 "refusing to measure a partial cluster\n",
                 opt.proc_name.c_str(), partitions, opt.leader_wait_sec);
    exit_code = 4;
  }

  if (!is_leader) {
    // Did the cluster form at all? A genuine follower has applied the
    // leader's leadership probes by now. A process that leads nothing AND has
    // seen nothing from anyone else is in a cluster with no leader — the
    // per-partition-group-mode failure, for one — and waiting out the full
    // follower budget for end markers nobody will send turns a 30-second
    // failure into a two-minute one, 210 times over in a full sweep.
    long long applied_from_elsewhere = 0;
    for (auto& st : state) {
      applied_from_elsewhere += st.applied_total.load(std::memory_order_relaxed);
    }
    if (applied_from_elsewhere == 0) {
      std::fprintf(stderr,
                   "[raft_bench:%s] no leader anywhere after %.0fs: this process leads "
                   "nothing and has applied nothing. Giving up rather than waiting out "
                   "the follower budget.\n",
                   opt.proc_name.c_str(), opt.leader_wait_sec);
      if (exit_code == 0) {
        exit_code = 7;
      }
      // @unsafe
      pre_shutdown_step();
      // @unsafe
      shutdown_paxos();
      std::printf("[raft_bench:%s] done (exit %d)\n", opt.proc_name.c_str(), exit_code);
      std::fflush(stdout);
      return exit_code;
    }

    // Follower: serve, then leave. Wait for the leader's end markers, with a
    // wall-clock budget so a lost leader cannot wedge the launch script.
    const double budget_sec =
        opt.leader_wait_sec + opt.warmup_sec + opt.duration_sec + opt.follower_linger_sec + 30.0;
    const uint64_t deadline_us = now_us() + static_cast<uint64_t>(budget_sec * 1e6);
    std::printf("[raft_bench:%s] serving as follower for up to %.0fs\n",
                opt.proc_name.c_str(), budget_sec);
    std::fflush(stdout);
    while (now_us() < deadline_us) {
      int seen = 0;
      for (auto& st : state) {
        if (st.end_markers.load(std::memory_order_relaxed) > 0) {
          ++seen;
        }
      }
      if (seen >= partitions) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    // Let any straggling apply work land before tearing the transport down.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<long long>(opt.follower_linger_sec * 1000.0)));

    long long applied = 0;
    for (auto& st : state) {
      applied += st.applied_total.load(std::memory_order_relaxed);
    }
    std::printf("[raft_bench:%s] follower applied %lld stamped entries\n",
                opt.proc_name.c_str(), applied);
    std::fflush(stdout);
  } else {
    std::printf("[raft_bench:%s] leader; offering load\n", opt.proc_name.c_str());
    std::fflush(stdout);

    const uint64_t offer_begin_us = now_us();
    const uint64_t window_begin_us =
        offer_begin_us + static_cast<uint64_t>(opt.warmup_sec * 1e6);
    const uint64_t window_end_us =
        window_begin_us + static_cast<uint64_t>(opt.duration_sec * 1e6);
    g_window_begin_us.store(window_begin_us, std::memory_order_relaxed);
    g_window_end_us.store(window_end_us, std::memory_order_relaxed);
    g_window_valid.store(true, std::memory_order_release);

    // Split the offered rate across partitions, giving the remainder to the
    // low-numbered ones so the requested aggregate is exact.
    std::vector<long long> per_partition_rate(static_cast<size_t>(partitions), 0);
    if (opt.rate > 0) {
      const long long base = opt.rate / partitions;
      const long long extra = opt.rate % partitions;
      for (int i = 0; i < partitions; ++i) {
        per_partition_rate[static_cast<size_t>(i)] = base + (i < extra ? 1 : 0);
      }
      for (int i = 0; i < partitions; ++i) {
        if (per_partition_rate[static_cast<size_t>(i)] <= 0) {
          std::fprintf(stderr,
                       "[raft_bench:%s] WARNING: --rate %lld across %d partitions "
                       "gives partition %d a rate of 0; it will offer nothing\n",
                       opt.proc_name.c_str(), opt.rate, partitions, i);
        }
      }
    }

    std::vector<std::thread> offer_threads;
    offer_threads.reserve(static_cast<size_t>(partitions));
    for (int i = 0; i < partitions; ++i) {
      offer_threads.emplace_back(offer_loop, std::cref(opt), static_cast<uint32_t>(i),
                                 opt.rate <= 0,
                                 per_partition_rate[static_cast<size_t>(i)],
                                 offer_begin_us, window_end_us,
                                 &state[static_cast<size_t>(i)]);
    }
    for (auto& th : offer_threads) {
      th.join();
    }
    const uint64_t offer_stop_us = now_us();

    // Drain: let the submit queue empty, then give the apply thread a moment
    // to catch up before we stop counting.
    for (int i = 0; i < partitions; ++i) {
      // @unsafe
      wait_for_submit(static_cast<uint32_t>(i));
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // End markers, so followers can leave without waiting out their budget.
    for (int i = 0; i < partitions; ++i) {
      // @unsafe
      add_log_to_nc("", 0, static_cast<uint32_t>(i), 1);
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // ----- aggregate -----
    Record rec;
    rec.commit = env_or("MAKO_BENCH_COMMIT", "unknown");
    rec.date = utc_now_iso8601();
    rec.host = hostname();
    rec.build_flavour = RAFT_BENCH_BUILD_FLAVOUR;
    rec.cmake_build_type = RAFT_BENCH_CMAKE_BUILD_TYPE;
#ifdef RAFT_TEST_CORO
    rec.raft_test_coro = true;
#endif
#ifdef RAFT_DEFAULT_SINGLE_GROUP
    rec.raft_default_single_group = true;
#endif
    {
      std::string joined;
      for (size_t i = 0; i < opt.configs.size(); ++i) {
        if (i != 0) {
          joined += ",";
        }
        joined += opt.configs[i];
      }
      rec.config = joined;
    }
    rec.partitions = partitions;
    rec.replicas = opt.replicas;
    rec.group_mode = opt.group_mode;
    rec.payload_bytes = opt.payload_bytes;
    rec.batch = opt.batch;
    rec.offered_rate = opt.rate;
    rec.duration_sec = opt.duration_sec;
    rec.warmup_sec = opt.warmup_sec;
    rec.max_outstanding = opt.max_outstanding;
    rec.leader_wait_sec = opt.leader_wait_sec;
    rec.max_samples = opt.max_samples;
    rec.follower_linger_sec = opt.follower_linger_sec;
    rec.log_level = opt.log_level;
    rec.proc = opt.proc_name;
    rec.role = "leader";
    rec.label = opt.label;

    // The window the run actually achieved: it closes early if the offer loop
    // stopped early (leadership loss).
    const uint64_t achieved_end_us = std::min<uint64_t>(offer_stop_us, window_end_us);
    rec.measured_window_sec =
        achieved_end_us > window_begin_us
            ? static_cast<double>(achieved_end_us - window_begin_us) / 1e6
            : 0.0;

    std::vector<uint32_t> latencies;
    long long samples_used = 0;
    bool first_partition = true;
    for (auto& st : state) {
      const long long par_applied = st.applied_in_window.load(std::memory_order_relaxed);
      if (first_partition) {
        rec.min_partition_applied_in_window = par_applied;
        rec.max_partition_applied_in_window = par_applied;
        first_partition = false;
      } else {
        rec.min_partition_applied_in_window =
            std::min(rec.min_partition_applied_in_window, par_applied);
        rec.max_partition_applied_in_window =
            std::max(rec.max_partition_applied_in_window, par_applied);
      }
      rec.offered_total += st.offered_total.load(std::memory_order_relaxed);
      rec.applied_total += st.applied_total.load(std::memory_order_relaxed);
      rec.offered_in_window += st.offered_in_window.load(std::memory_order_relaxed);
      rec.applied_in_window += st.applied_in_window.load(std::memory_order_relaxed);
      rec.offer_rejected += st.offer_rejected.load(std::memory_order_relaxed);
      rec.offer_stalled_sec +=
          static_cast<double>(st.stalled_ns.load(std::memory_order_relaxed)) / 1e9;
      rec.foreign_applied += st.foreign_applied.load(std::memory_order_relaxed);
      rec.out_of_order += st.out_of_order.load(std::memory_order_relaxed);
      rec.gaps += st.gaps.load(std::memory_order_relaxed);
      rec.duplicates += st.duplicates.load(std::memory_order_relaxed);
      rec.probes_applied += st.probes_applied.load(std::memory_order_relaxed);
      rec.samples_dropped += st.samples_dropped.load(std::memory_order_relaxed);
      const long long peak = st.peak_raft_outstanding.load(std::memory_order_relaxed);
      if (peak > rec.peak_outstanding) {
        rec.peak_outstanding = peak;
      }
      const size_t written = std::min<size_t>(st.samples_committed.load(std::memory_order_acquire),
                                              static_cast<size_t>(opt.max_samples));
      samples_used += static_cast<long long>(written);
      latencies.reserve(latencies.size() + written);
      for (size_t i = 0; i < written; ++i) {
        latencies.push_back(st.samples[i]);
      }
    }
    rec.samples_used = samples_used;

    rec.leadership_changes =
        static_cast<long long>(leadership_notifications.load(std::memory_order_relaxed));

    if (rec.measured_window_sec > 0.0) {
      rec.applied_per_sec = static_cast<double>(rec.applied_in_window) / rec.measured_window_sec;
      rec.offered_per_sec = static_cast<double>(rec.offered_in_window) / rec.measured_window_sec;
      // bench.cc divides its aggregate by the worker count for a per-core
      // figure (avg_per_core_persist_throughput); the analogue here is per
      // Raft group. D2 says to copy the definitions.
      rec.applied_per_sec_per_partition =
          rec.applied_per_sec / static_cast<double>(partitions);
    }
    // A straggling partition is invisible in the aggregate. Say so.
    if (partitions > 1 && rec.max_partition_applied_in_window > 0 &&
        rec.min_partition_applied_in_window <
            rec.max_partition_applied_in_window / 2) {
      std::printf("[raft_bench:%s] NOTE: partitions are uneven — slowest applied %lld, "
                  "fastest %lld. The aggregate hides this.\n",
                  opt.proc_name.c_str(), rec.min_partition_applied_in_window,
                  rec.max_partition_applied_in_window);
    }

    if (!latencies.empty()) {
      double sum = 0.0;
      for (uint32_t v : latencies) {
        sum += static_cast<double>(v);
      }
      rec.latency_mean_us = sum / static_cast<double>(latencies.size());
      std::sort(latencies.begin(), latencies.end());
      rec.latency_p50_us = percentile_us(latencies, 0.50);
      rec.latency_p90_us = percentile_us(latencies, 0.90);
      rec.latency_p99_us = percentile_us(latencies, 0.99);
      rec.latency_p999_us = percentile_us(latencies, 0.999);
      rec.latency_max_us = static_cast<double>(latencies.back());
      for (int p = 1; p <= 99; ++p) {
        rec.latency_cdf_us.emplace_back(std::to_string(p),
                                        percentile_us(latencies, p / 100.0));
      }
      rec.latency_cdf_us.emplace_back("99.9", rec.latency_p999_us);
      rec.latency_cdf_us.emplace_back("100", rec.latency_max_us);
    }

    std::printf("[raft_bench:%s] applied_per_sec=%.1f offered=%lld applied=%lld "
                "p50=%.0fus p99=%.0fus peak_outstanding=%lld window=%.3fs\n",
                opt.proc_name.c_str(), rec.applied_per_sec, rec.offered_in_window,
                rec.applied_in_window, rec.latency_p50_us, rec.latency_p99_us,
                rec.peak_outstanding, rec.measured_window_sec);
    if (rec.offer_rejected > 0) {
      std::printf("[raft_bench:%s] WARNING: %lld offers rejected — leadership was lost "
                  "during the run; this point is not a valid measurement\n",
                  opt.proc_name.c_str(), rec.offer_rejected);
      exit_code = 3;
    }
    if (rec.applied_in_window == 0 || rec.measured_window_sec <= 0.0) {
      // Without this the record still writes, with a full set of zeroed
      // latency percentiles and a zeroed CDF, and a plot anchors a curve on a
      // false origin at (0 entries/s, 0 us).
      std::printf("[raft_bench:%s] WARNING: nothing was applied inside the measured "
                  "window (applied_in_window=%lld window=%.3fs); this point is not a "
                  "measurement\n",
                  opt.proc_name.c_str(), rec.applied_in_window, rec.measured_window_sec);
      exit_code = 6;
    }
    // Stalling for most of the window means the run never offered what it was
    // asked to. That is a real result at saturation, but it is also what a
    // leaked in-flight bound looks like, so say so rather than let the two be
    // indistinguishable in the record.
    if (rec.measured_window_sec > 0.0 &&
        rec.offer_stalled_sec >
            rec.measured_window_sec * static_cast<double>(partitions) * 0.5) {
      std::printf("[raft_bench:%s] NOTE: offer threads were blocked on the in-flight "
                  "bound for %.1fs of a %.1fs window across %d partitions. Either this "
                  "point is past saturation, or Raft accepted entries it then dropped "
                  "(a leadership flap) and the bound has leaked.\n",
                  opt.proc_name.c_str(), rec.offer_stalled_sec, rec.measured_window_sec,
                  partitions);
    }
    std::fflush(stdout);

    if (!opt.out_path.empty()) {
      if (!write_record(opt.out_path, rec)) {
        exit_code = 1;
      } else {
        std::printf("[raft_bench:%s] record written to %s\n", opt.proc_name.c_str(),
                    opt.out_path.c_str());
        std::fflush(stdout);
      }
    }
  }

  // Log integrity, checked for both roles and last, so that no performance
  // verdict above can overwrite it. A follower verifies the same stream the
  // leader does — it registers the same callback — which is the only
  // correctness evidence this driver gathers about replication rather than
  // about the leader's own local apply.
  //
  // This outranks every verdict above it: a number measured over a log that
  // lost, duplicated or reordered an entry is not a slower number, it is a
  // wrong one. It is therefore assigned unconditionally, not under
  // `if (exit_code == 0)`.
  {
    long long out_of_order = 0;
    long long gaps = 0;
    long long duplicates = 0;
    long long foreign = 0;
    for (auto& st : state) {
      out_of_order += st.out_of_order.load(std::memory_order_relaxed);
      gaps += st.gaps.load(std::memory_order_relaxed);
      duplicates += st.duplicates.load(std::memory_order_relaxed);
      foreign += st.foreign_applied.load(std::memory_order_relaxed);
    }
    if (out_of_order > 0 || gaps > 0 || duplicates > 0 || foreign > 0) {
      std::fprintf(stderr,
                   "[raft_bench:%s] LOG INTEGRITY VIOLATION: out_of_order=%lld "
                   "gaps=%lld duplicates=%lld foreign_applied=%lld\n",
                   opt.proc_name.c_str(), out_of_order, gaps, duplicates, foreign);
      std::fflush(stderr);
      exit_code = 8;
    } else {
      // Print the clean case too. A follower writes no record, so this line is
      // the only place its verdict is legible, and the flap test reads it.
      long long checked = 0;
      long long probes = 0;
      for (auto& st : state) {
        checked += st.applied_total.load(std::memory_order_relaxed);
        probes += st.probes_applied.load(std::memory_order_relaxed);
      }
      std::printf("[raft_bench:%s] log integrity: OK (%lld entries checked, "
                  "%lld leadership probes)\n",
                  opt.proc_name.c_str(), checked - probes, probes);
      std::fflush(stdout);
    }
  }

  // @unsafe
  pre_shutdown_step();
  // @unsafe - shutdown_paxos() IS the Raft teardown; the name is historical
  // and the unqualified replication_helper entry points dispatch on
  // replication type (trap T7).
  shutdown_paxos();

  std::printf("[raft_bench:%s] done (exit %d)\n", opt.proc_name.c_str(), exit_code);
  std::fflush(stdout);
  return exit_code;
}
