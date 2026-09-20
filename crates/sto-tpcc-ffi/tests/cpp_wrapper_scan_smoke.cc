#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "benchmarks/benchmark_config.h"
#include "lib/common.h"
#include "storage/rust_sto_tpcc_wrapper.hh"

namespace {

void require(bool condition) {
  if (!condition)
    std::abort();
}

using row = std::pair<std::string, std::string>;

class legacy_collect final : public oi_scan_callback {
public:
  bool invoke(const char *keyp, size_t keylen,
              const std::string &value) override {
    rows.emplace_back(std::string(keyp, keylen), value);
    return true;
  }

  std::vector<row> rows;
};

class raw_collect final : public oi_scan_callback {
public:
  explicit raw_collect(size_t limit, bool stop_after_first = false)
      : limit_(limit), stop_after_first_(stop_after_first) {}

  bool invoke(const char *, size_t, const std::string &) override {
    legacy_called = true;
    return false;
  }

  bool invoke_bytes(const char *keyp, size_t keylen, const char *valuep,
                    size_t valuelen) override {
    raw_called = true;
    rows.emplace_back(std::string(keyp, keylen),
                      valuelen == 0 ? std::string()
                                    : std::string(valuep, valuelen));
    return !stop_after_first_;
  }

  size_t max_records_hint() const override { return limit_; }

  bool legacy_called = false;
  bool raw_called = false;
  std::vector<row> rows;

private:
  size_t limit_;
  bool stop_after_first_;
};

class throwing_raw final : public oi_scan_callback {
public:
  bool invoke(const char *, size_t, const std::string &) override {
    std::abort();
  }

  bool invoke_bytes(const char *, size_t, const char *, size_t) override {
    throw std::runtime_error("expected scan callback failure");
  }
};

void require_rows(const std::vector<row> &actual,
                  const std::vector<row> &expected) {
  require(actual == expected);
}

} // namespace

// Focused C++/Rust smoke test. Its CMake target uses the matched sto_tpcc_bench
// object/link flags, and CTest runs it with a one-shard configuration. Keeping
// it next to native_ffi.rs makes the borrowed-row bridge easy to audit.
int main(int argc, char **argv) {
  require(argc == 2);

  transport::Configuration config(argv[1]);
  auto &benchmark = BenchmarkConfig::getInstance();
  benchmark.setConfig(&config);
  benchmark.setNthreads(1);
  benchmark.setNshards(1);
  benchmark.setShardIndex(0);
  benchmark.setIsReplicated(false);
  benchmark.setPinCpus(false);

  rust_sto_tpcc_wrapper db;
  db.init();
  abstract_ordered_index *table = db.open_index("order_line_1", -1);
  require(table != nullptr);
  db.thread_init(true, 0);

  // Nontransactional insertion performs the normal Mako value encoding. The
  // raw callback must observe only the logical prefix, including a true empty
  // value rather than the metadata suffix.
  require(table->insert(lcdf::Str("a"), "alpha"));
  require(table->insert(lcdf::Str("b"), ""));
  require(table->insert(lcdf::Str("c"), "charlie"));
  const std::vector<row> all_rows{{"a", "alpha"}, {"b", ""},
                                  {"c", "charlie"}};

  str_arena arena;
  void *txn = db.new_txn(0, arena, nullptr);
  legacy_collect legacy;
  tx_scan(table, txn, std::string(), nullptr, legacy, &arena);
  require_rows(legacy.rows, all_rows);
  require(db.commit_txn(txn));

  // The override is selected directly, while the historical invoke method is
  // never entered. The callback-provided bound remains an exact backend walk
  // boundary even when invoke_bytes asks to continue.
  txn = db.new_txn(0, arena, nullptr);
  raw_collect bounded(2);
  tx_scan(table, txn, std::string(), nullptr, bounded, &arena);
  require(bounded.raw_called);
  require(!bounded.legacy_called);
  require_rows(bounded.rows, {{"a", "alpha"}, {"b", ""}});
  require(db.commit_txn(txn));

  // Returning false still stops immediately when the backend limit is wider.
  txn = db.new_txn(0, arena, nullptr);
  raw_collect stopping(std::numeric_limits<size_t>::max(), true);
  tx_scan(table, txn, std::string(), nullptr, stopping, &arena);
  require_rows(stopping.rows, {{"a", "alpha"}});
  require(db.commit_txn(txn));

  // C++ exceptions are caught inside the C callback, the Rust frame returns,
  // and only then is the original exception rethrown. Scan failure retains the
  // historical active-transaction lifecycle, so the caller aborts it.
  txn = db.new_txn(0, arena, nullptr);
  throwing_raw throwing;
  bool caught = false;
  try {
    tx_scan(table, txn, std::string(), nullptr, throwing, &arena);
  } catch (const std::runtime_error &error) {
    caught = std::string(error.what()) == "expected scan callback failure";
  }
  require(caught);
  db.abort_txn(txn);

  // The self-contained scan surface owns its transaction. An arbitrary
  // callback exception must close that attempt before it is rethrown, so the
  // next self-contained operation cannot fail as a nested transaction.
  caught = false;
  try {
    table->scan(std::string(), nullptr, throwing, &arena);
  } catch (const std::runtime_error &error) {
    caught = std::string(error.what()) == "expected scan callback failure";
  }
  require(caught);

  legacy_collect after_owned_failure;
  table->scan(std::string(), nullptr, after_owned_failure, &arena);
  require_rows(after_owned_failure.rows, all_rows);

  txn = db.new_txn(0, arena, nullptr);
  legacy_collect after_failure;
  tx_scan(table, txn, std::string(), nullptr, after_failure, &arena);
  require_rows(after_failure.rows, all_rows);
  require(db.commit_txn(txn));

  db.thread_end();
  return 0;
}
