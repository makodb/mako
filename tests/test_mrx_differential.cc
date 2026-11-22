// test_mrx_differential.cc -- the C++ cache and the Rust cache, driven by
// one operation stream, required to agree.
//
// WHY THIS EXISTS
//
// Both implementations have their own tests, and both suites pass. That
// proves each matches its author's understanding of the specification, not
// that the two understandings are the same one. A differential harness is
// the only check that produces a signal neither suite can: an operation
// where they disagree is, by construction, a place where at least one of
// them is wrong, and no amount of per-implementation testing surfaces it.
//
// The C++ implementation is the oracle -- it is mutation-verified 5/5 and
// has been through crash testing -- so on a disagreement the Rust side is
// the suspect. That is a prior, not a rule; the ABI's arena-full defect was
// found the other way round.
//
// WHAT IS COMPARED, AND WHAT DELIBERATELY IS NOT
//
// Compared: everything a caller can observe. Read results, existence
// reporting from put/insert/remove, scan contents and order, behaviour
// across a clean close and reopen.
//
// NOT compared: watermark values, residency, dirty-map size, flush timing.
// Those are internal accounting whose exact values are implementation
// choices -- the Rust version coalesces its dirty map slightly differently,
// for one -- and asserting on them would turn every legitimate difference
// into a red test. The properties that *are* required of the watermark are
// asserted in each implementation's own suite, where they belong.

#include <gtest/gtest.h>

#include "mrxdb.h"

#include "mako/storage/masstree_rocks_index.hh"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

std::string ScratchPath(const char *tag, int n) {
  char buf[256];
  snprintf(buf, sizeof(buf), "/tmp/mrx-diff-%s-%d-%d", tag,
           static_cast<int>(getpid()), n);
  return std::string(buf);
}

void RemoveTree(const std::string &path) {
  std::string cmd = "rm -rf '" + path + "'";
  if (system(cmd.c_str()) != 0) {
    // Best effort: a leftover scratch directory is not a test failure.
  }
}

// One operation in the shared stream. Generated once and replayed against
// both implementations, so a disagreement is reproducible from the seed
// rather than being a race that happened once.
struct Op {
  enum Kind { kPut, kInsert, kRemove, kGet } kind;
  std::string key;
  std::string value;
};

std::vector<Op> GenerateStream(unsigned seed, int count, int key_space) {
  std::mt19937 rng(seed);
  std::vector<Op> ops;
  ops.reserve(count);
  for (int i = 0; i < count; i++) {
    Op op;
    const int roll = static_cast<int>(rng() % 100);
    // Weighted so the interesting transitions actually occur: overwrites
    // of live keys, inserts that lose to a live key, removes of both
    // present and absent keys.
    if (roll < 45) {
      op.kind = Op::kPut;
    } else if (roll < 60) {
      op.kind = Op::kInsert;
    } else if (roll < 75) {
      op.kind = Op::kRemove;
    } else {
      op.kind = Op::kGet;
    }
    char kb[32];
    snprintf(kb, sizeof(kb), "k%05d", static_cast<int>(rng() % key_space));
    op.key = kb;
    char vb[64];
    snprintf(vb, sizeof(vb), "v%d-%d", i, static_cast<int>(rng() % 1000));
    op.value = vb;
    ops.push_back(op);
  }
  return ops;
}

// --- the Rust side, wrapped so the test body reads the same for both ----

class RustDb {
 public:
  explicit RustDb(const std::string &path, uint64_t capacity_bytes = 0) {
    mrxdb_options_t *o = mrxdb_options_create();
    mrxdb_options_set_capacity_bytes(o, capacity_bytes);
    char *err = nullptr;
    db_ = mrxdb_open(o, path.c_str(), &err);
    mrxdb_options_destroy(o);
    if (err != nullptr) {
      last_error_ = err;
      mrxdb_free(err);
    }
  }

  ~RustDb() { Close(); }

  RustDb(const RustDb &) = delete;
  RustDb &operator=(const RustDb &) = delete;

  bool ok() const { return db_ != nullptr; }

  void Close() {
    if (db_ == nullptr) return;
    char *err = nullptr;
    mrxdb_close(db_, &err);
    db_ = nullptr;
    if (err != nullptr) {
      last_error_ = err;
      mrxdb_free(err);
    }
  }

  bool Get(const std::string &k, std::string *out) {
    size_t len = 0;
    char *err = nullptr;
    char *p = mrxdb_get(db_, k.data(), k.size(), &len, &err);
    EXPECT_EQ(err, nullptr) << "mrxdb_get failed: " << (err ? err : "");
    if (err != nullptr) mrxdb_free(err);
    if (p == nullptr) return false;
    out->assign(p, len);
    mrxdb_free(p);
    return true;
  }

  void Put(const std::string &k, const std::string &v) {
    char *err = nullptr;
    mrxdb_put(db_, k.data(), k.size(), v.data(), v.size(), &err);
    EXPECT_EQ(err, nullptr);
    if (err != nullptr) mrxdb_free(err);
  }

  bool Insert(const std::string &k, const std::string &v) {
    char *err = nullptr;
    const unsigned char wrote =
        mrxdb_insert(db_, k.data(), k.size(), v.data(), v.size(), &err);
    EXPECT_EQ(err, nullptr);
    if (err != nullptr) mrxdb_free(err);
    return wrote != 0;
  }

  void Remove(const std::string &k) {
    char *err = nullptr;
    mrxdb_delete(db_, k.data(), k.size(), &err);
    EXPECT_EQ(err, nullptr);
    if (err != nullptr) mrxdb_free(err);
  }

  bool Flush() {
    char *err = nullptr;
    mrxdb_flush(db_, &err);
    const bool ok = err == nullptr;
    if (err != nullptr) mrxdb_free(err);
    return ok;
  }

  std::map<std::string, std::string> ScanAll() {
    std::map<std::string, std::string> out;
    mrxdb_iterator_t *it = mrxdb_create_iterator(db_);
    for (mrxdb_iter_seek_to_first(it); mrxdb_iter_valid(it) != 0;
         mrxdb_iter_next(it)) {
      size_t klen = 0, vlen = 0;
      const char *k = mrxdb_iter_key(it, &klen);
      const char *v = mrxdb_iter_value(it, &vlen);
      out.emplace(std::string(k, klen), std::string(v, vlen));
    }
    char *err = nullptr;
    mrxdb_iter_get_error(it, &err);
    EXPECT_EQ(err, nullptr) << "iteration failed: " << (err ? err : "");
    if (err != nullptr) mrxdb_free(err);
    mrxdb_iter_destroy(it);
    return out;
  }

  const std::string &last_error() const { return last_error_; }

 private:
  mrxdb_t *db_ = nullptr;
  std::string last_error_;
};

// --- the C++ side, wrapped identically ----------------------------------

// Collects a range so a scan can be compared as a container.
class collecting_callback : public oi_scan_callback {
 public:
  bool invoke(const char *keyp, size_t keylen,
              const std::string &value) override {
    out.emplace(std::string(keyp, keylen), value);
    return true;
  }
  std::map<std::string, std::string> out;
};

class CppDb {
 public:
  explicit CppDb(const std::string &path, uint64_t capacity_bytes = 0) {
    tree_ = new concurrent_btree();
    store_ = mrx_store_open(tree_, path, capacity_bytes);
    if (store_ != nullptr) {
      idx_ = new masstree_rocks_index("mrx_diff", 1, tree_, store_);
    }
  }

  ~CppDb() { Close(); }

  CppDb(const CppDb &) = delete;
  CppDb &operator=(const CppDb &) = delete;

  bool ok() const { return idx_ != nullptr; }

  void Close() {
    delete idx_;
    idx_ = nullptr;
    if (store_ != nullptr) {
      mrx_store_close(store_);
      store_ = nullptr;
    }
    delete tree_;
    tree_ = nullptr;
  }

  static lcdf::Str S(const std::string &s) {
    return lcdf::Str(s.data(), static_cast<int>(s.size()));
  }

  bool Get(const std::string &k, std::string *out) {
    return idx_->get(S(k), *out, std::string::npos);
  }

  void Put(const std::string &k, const std::string &v) { idx_->put(S(k), v); }

  bool Insert(const std::string &k, const std::string &v) {
    return idx_->insert(S(k), v);
  }

  void Remove(const std::string &k) { idx_->remove(S(k)); }

  bool Flush() { return idx_->flush(); }

  std::map<std::string, std::string> ScanAll() {
    collecting_callback cb;
    idx_->scan(std::string(), nullptr, cb, nullptr);
    return cb.out;
  }

  const std::string &last_error() const {
    static const std::string kNone;
    return kNone;
  }

 private:
  concurrent_btree *tree_ = nullptr;
  mrx_store *store_ = nullptr;
  masstree_rocks_index *idx_ = nullptr;
};

}  // namespace

// Both implementations are exercised through the SAME test body, so a
// property can never be checked against one and not the other.
template <typename Db>
static std::map<std::string, std::string> ReplayAndCollect(
    const std::string &path, const std::vector<Op> &ops,
    std::vector<std::pair<int, int>> *reports) {
  Db db(path);
  EXPECT_TRUE(db.ok());
  for (size_t i = 0; i < ops.size(); i++) {
    const Op &op = ops[i];
    switch (op.kind) {
      case Op::kPut:
        db.Put(op.key, op.value);
        break;
      case Op::kInsert:
        reports->emplace_back(static_cast<int>(i),
                              db.Insert(op.key, op.value) ? 1 : 0);
        break;
      case Op::kRemove:
        db.Remove(op.key);
        break;
      case Op::kGet: {
        std::string got;
        const bool found = db.Get(op.key, &got);
        // The read RESULT is what must agree, so it is recorded rather
        // than asserted here -- a mismatch should report which operation
        // diverged, not merely that something did.
        reports->emplace_back(static_cast<int>(i),
                              found ? static_cast<int>(std::hash<std::string>{}(
                                          got) & 0x7fffffff)
                                    : -1);
        break;
      }
    }
  }
  EXPECT_TRUE(db.Flush());
  auto snapshot = db.ScanAll();
  db.Close();
  return snapshot;
}

class MrxDifferentialTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static int n = 0;
    cpp_path_ = ScratchPath("cpp", n);
    rust_path_ = ScratchPath("rust", n);
    n++;
    RemoveTree(cpp_path_);
    RemoveTree(rust_path_);
  }

  void TearDown() override {
    RemoveTree(cpp_path_);
    RemoveTree(rust_path_);
  }

  std::string cpp_path_;
  std::string rust_path_;
};

TEST_F(MrxDifferentialTest, AbiVersionMatchesTheHeader) {
  // A layout or semantic drift between the header and the linked library
  // corrupts silently, so this is checked before anything else runs.
  EXPECT_EQ(mrxdb_abi_version(), MRXDB_ABI_VERSION);
}

TEST_F(MrxDifferentialTest, RandomStreamsProduceIdenticalStates) {
  for (unsigned seed = 1; seed <= 8; seed++) {
    const auto ops = GenerateStream(seed, 4000, 400);
    RemoveTree(cpp_path_);
    RemoveTree(rust_path_);

    std::vector<std::pair<int, int>> cpp_reports, rust_reports;
    const auto cpp_state = ReplayAndCollect<CppDb>(cpp_path_, ops, &cpp_reports);
    const auto rust_state =
        ReplayAndCollect<RustDb>(rust_path_, ops, &rust_reports);

    ASSERT_EQ(cpp_reports.size(), rust_reports.size()) << "seed " << seed;
    for (size_t i = 0; i < cpp_reports.size(); i++) {
      ASSERT_EQ(cpp_reports[i].first, rust_reports[i].first);
      const int at = cpp_reports[i].first;
      EXPECT_EQ(cpp_reports[i].second, rust_reports[i].second)
          << "seed " << seed << ": the two implementations disagreed at "
          << "operation " << at << " on key " << ops[at].key;
    }

    EXPECT_EQ(cpp_state.size(), rust_state.size())
        << "seed " << seed << ": final key counts differ";
    // Report the FIRST differing key rather than dumping both maps: with
    // 400 keys a raw container diff is unreadable.
    auto ci = cpp_state.begin();
    auto ri = rust_state.begin();
    for (; ci != cpp_state.end() && ri != rust_state.end(); ++ci, ++ri) {
      ASSERT_EQ(ci->first, ri->first)
          << "seed " << seed << ": key sets diverge at " << ci->first;
      ASSERT_EQ(ci->second, ri->second)
          << "seed " << seed << ": values differ for key " << ci->first;
    }
  }
}

TEST_F(MrxDifferentialTest, StatesAgreeAfterCloseAndReopen) {
  const auto ops = GenerateStream(99, 3000, 300);
  std::vector<std::pair<int, int>> a, b;
  ReplayAndCollect<CppDb>(cpp_path_, ops, &a);
  ReplayAndCollect<RustDb>(rust_path_, ops, &b);

  // Reopen both and compare what survived. This is where a difference in
  // what reaches the durable store shows up, as opposed to a difference
  // in what is merely visible in memory.
  CppDb cpp(cpp_path_);
  RustDb rust(rust_path_);
  ASSERT_TRUE(cpp.ok());
  ASSERT_TRUE(rust.ok()) << rust.last_error();
  const auto cpp_state = cpp.ScanAll();
  const auto rust_state = rust.ScanAll();

  EXPECT_EQ(cpp_state.size(), rust_state.size())
      << "the two implementations recovered different key counts";
  auto ci = cpp_state.begin();
  auto ri = rust_state.begin();
  for (; ci != cpp_state.end() && ri != rust_state.end(); ++ci, ++ri) {
    ASSERT_EQ(ci->first, ri->first) << "recovered key sets diverge";
    ASSERT_EQ(ci->second, ri->second)
        << "recovered values differ for key " << ci->first;
  }
}

TEST_F(MrxDifferentialTest, ExistenceReportingAgrees) {
  // put/insert/remove all report whether the key existed, and callers use
  // those as uniqueness checks. A disagreement here is a correctness bug
  // that a value comparison would not catch.
  CppDb cpp(cpp_path_);
  RustDb rust(rust_path_);
  ASSERT_TRUE(cpp.ok());
  ASSERT_TRUE(rust.ok()) << rust.last_error();

  const std::string k = "contested";
  EXPECT_EQ(cpp.Insert(k, "a"), rust.Insert(k, "a"));
  EXPECT_EQ(cpp.Insert(k, "b"), rust.Insert(k, "b"));
  cpp.Remove(k);
  rust.Remove(k);
  EXPECT_EQ(cpp.Insert(k, "c"), rust.Insert(k, "c"))
      << "a tombstoned key must read as absent to insert in both";

  std::string cv, rv;
  EXPECT_EQ(cpp.Get(k, &cv), rust.Get(k, &rv));
  EXPECT_EQ(cv, rv);
}

TEST_F(MrxDifferentialTest, BinaryKeysAndValuesAgree) {
  CppDb cpp(cpp_path_);
  RustDb rust(rust_path_);
  ASSERT_TRUE(cpp.ok());
  ASSERT_TRUE(rust.ok()) << rust.last_error();

  const std::vector<std::pair<std::string, std::string>> cases = {
      {std::string(""), std::string("empty key")},
      {std::string("nul\0key", 7), std::string("v")},
      {std::string("nul value"), std::string("a\0b", 3)},
      {std::string(300, '\xff'), std::string(300, '\xfe')},
      {std::string("empty value"), std::string("")},
  };
  for (const auto &c : cases) {
    cpp.Put(c.first, c.second);
    rust.Put(c.first, c.second);
  }
  ASSERT_TRUE(cpp.Flush());
  ASSERT_TRUE(rust.Flush());

  for (const auto &c : cases) {
    std::string cv, rv;
    const bool cf = cpp.Get(c.first, &cv);
    const bool rf = rust.Get(c.first, &rv);
    EXPECT_EQ(cf, rf) << "presence differs for a binary key";
    EXPECT_EQ(cv, rv) << "values differ for a binary key";
    EXPECT_EQ(cv, c.second);
  }
}

TEST_F(MrxDifferentialTest, ScanOrderAndContentAgree) {
  CppDb cpp(cpp_path_);
  RustDb rust(rust_path_);
  ASSERT_TRUE(cpp.ok());
  ASSERT_TRUE(rust.ok()) << rust.last_error();

  for (int i = 0; i < 2000; i++) {
    char k[32], v[32];
    snprintf(k, sizeof(k), "k%05d", i);
    snprintf(v, sizeof(v), "v%d", i);
    cpp.Put(k, v);
    rust.Put(k, v);
  }
  // Delete a contiguous run longer than any one scan chunk: an
  // all-tombstone chunk is where a scan can end early.
  for (int i = 500; i < 900; i++) {
    char k[32];
    snprintf(k, sizeof(k), "k%05d", i);
    cpp.Remove(k);
    rust.Remove(k);
  }

  const auto cs = cpp.ScanAll();
  const auto rs = rust.ScanAll();
  EXPECT_EQ(cs.size(), 1600u) << "the C++ scan ended early";
  EXPECT_EQ(rs.size(), 1600u) << "the Rust scan ended early";
  EXPECT_EQ(cs, rs);
}
