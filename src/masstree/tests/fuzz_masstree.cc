// Tier 6 of docs/masstree-test-plan.md — libFuzzer differential harness.
//
// Decodes the libFuzzer-supplied byte stream into a sequence of
// {insert, insert_if_absent, remove, search} ops on a small key
// keyspace and applies each op to both Masstree (single_threaded_btree)
// and a std::map<std::string, uint64_t> oracle. Any divergence in
// the per-op return value or value payload aborts, which libFuzzer
// treats as a finding and minimizes.
//
// Build via the MAKO_FUZZER=1 cmake toggle (see CMakeLists.txt):
//
//   mkdir build_fuzz && cd build_fuzz
//   MAKO_FUZZER=1 cmake .. -GNinja
//   ninja fuzz_masstree
//   ./fuzz_masstree -max_total_time=30 -max_len=4096 corpus/

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include <rusty/box.hpp>
#include <rusty/vec.hpp>

#include "mako/masstree_btree.h"
#include "mako/varkey.h"

import std;
import rusty;

using TestTree = single_threaded_btree;

namespace {

inline varkey vk(const std::string& s) {
  return varkey(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// Cursor into the libFuzzer byte buffer. Methods return zero-equivalent
// values when the buffer is exhausted; callers should also check has().
class ByteStream {
 public:
  ByteStream(const uint8_t* d, size_t n) : data_(d), size_(n) {}
  bool has(size_t n) const { return pos_ + n <= size_; }
  uint8_t byte() { return pos_ < size_ ? data_[pos_++] : 0; }
  uint64_t u64() {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | byte();
    return v;
  }
  std::string take(size_t n) {
    n = std::min(n, size_ - pos_);
    std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return s;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

[[noreturn]] void Diverge(const char* what) {
  // libFuzzer treats abort() as a crash and minimizes the failing
  // input; the message goes to stderr in the crash report.
  std::fprintf(stderr, "DIVERGENCE: %s\n", what);
  std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  TestTree tree;
  auto oracle = rusty::BTreeMap<std::string, uint64_t>::new_();
  // Heap-backed value storage so the pointers Masstree retains stay
  // alive until tree destruction. libFuzzer creates a fresh
  // (tree, oracle) per invocation, so growth is bounded by the input
  // length.
  rusty::Vec<rusty::Box<uint64_t>> storage;
  auto MakeValue = [&](uint64_t v) -> TestTree::value_type {
    storage.push(rusty::Box<uint64_t>::make(v));
    return reinterpret_cast<TestTree::value_type>(storage.back().get());
  };
  auto Decode = [](TestTree::value_type p) -> uint64_t {
    return *reinterpret_cast<const uint64_t*>(p);
  };

  ByteStream s(data, size);

  // Each op consumes 1 control byte (op kind in low 2 bits, key length in
  // bits 2..6) plus `klen` key bytes plus, for insert/insert_if_absent,
  // 8 value bytes. When the buffer is exhausted mid-op, stop.
  while (s.has(1)) {
    const uint8_t ctrl = s.byte();
    const uint8_t op = ctrl & 0x3;
    const uint8_t klen = (ctrl >> 2) & 0x1F;  // 0..31

    if (!s.has(klen)) break;
    const std::string key = s.take(klen);

    switch (op) {
      case 0: {  // insert (always overwrites; returns true iff new key)
        if (!s.has(8)) return 0;
        const uint64_t v = s.u64();
        const bool oracle_was_new = !oracle.contains_key(key);
        oracle.insert(key, v);
        const bool tree_was_new = tree.insert(vk(key), MakeValue(v));
        if (tree_was_new != oracle_was_new) Diverge("insert.return");
        break;
      }
      case 1: {  // insert_if_absent
        if (!s.has(8)) return 0;
        const uint64_t v = s.u64();
        const bool oracle_existed = oracle.contains_key(key);
        const bool tree_inserted = tree.insert_if_absent(vk(key), MakeValue(v));
        if (tree_inserted == oracle_existed) Diverge("insert_if_absent.return");
        if (tree_inserted) oracle.insert(key, v);
        break;
      }
      case 2: {  // remove
        const bool oracle_existed = oracle.remove(key).is_some();
        const bool tree_removed = tree.remove(vk(key));
        if (tree_removed != oracle_existed) Diverge("remove.return");
        break;
      }
      case 3: {  // search
        auto oracle_val = oracle.get(key);
        TestTree::value_type out = nullptr;
        const bool tree_found = tree.search(vk(key), out);
        if (tree_found != oracle_val.is_some()) Diverge("search.return");
        if (tree_found && Decode(out) != oracle_val.unwrap()) Diverge("search.value");
        break;
      }
    }
  }

  // Final cross-check: a full forward scan must enumerate the oracle in
  // sorted order with matching values.
  class Cb : public TestTree::search_range_callback {
   public:
    ~Cb() noexcept {}  // rusty::Vec member -> force noexcept dtor
    rusty::Vec<std::pair<std::string, uint64_t>> seen;
    bool invoke(const TestTree::string_type& k, TestTree::value_type v) override {
      seen.push(std::pair<std::string, uint64_t>(
          std::string(k.data(), k.length()),
          *reinterpret_cast<const uint64_t*>(v)));
      return true;
    }
  };
  Cb cb;
  const std::string empty_key;
  varkey lo = vk(empty_key);
  tree.search_range_call(lo, nullptr, cb);

  if (cb.seen.len() != oracle.len()) Diverge("scan.size");
  size_t i = 0;
  auto scan_it = oracle.iter();
  while (true) {
    auto scan_e = scan_it.next();
    if (scan_e.is_none()) break;
    auto [k, v] = scan_e.unwrap();
    if (cb.seen[i].first != k) Diverge("scan.key");
    if (cb.seen[i].second != v) Diverge("scan.value");
    ++i;
  }

  return 0;
}
