// Serde-isolated microbenchmark.
//
// Measures the hot paths of the archive serde surface
// (BufferSink/BufferSource + Binary{Write,Read}Archive) in isolation
// (no network, no RPC scaffolding). Originally benchmarked the
// chunk-linked-list rrr::Marshal; the scenarios keep their historical
// names so runs stay comparable apples-to-apples.
//
// Each scenario runs for a fixed number of iterations; sinks are
// either freshly constructed or cleared per iteration, mirroring what
// the original fresh/reused Marshal did. Reports ns/op and ops/sec.
//
// Build:  cmake --build build_clang21 --target bench_marshal -j32
// Run:    ./build_clang21/bench_marshal
//
// Output columns:
//   scenario | iters | total_ns | ns/op | ops/sec

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include <rusty/box.hpp>
#include <rusty/option.hpp>

#include "../rrr.hpp"

import std;
import rusty;

using rrr::i32;
using rrr::i64;

namespace {

// Returns nanoseconds elapsed between start and end via steady_clock.
using clk = std::chrono::steady_clock;

struct Scenario {
  const char* name;
  std::size_t iters;
  std::function<void(std::size_t)> body;  // body runs the inner loop
};

void run(const Scenario& s) {
  // Warmup — touch the code path once so caches/branch predictors settle.
  s.body(std::min<std::size_t>(s.iters / 100, 1024));

  const auto t0 = clk::now();
  s.body(s.iters);
  const auto t1 = clk::now();

  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  const double ns_per_op = static_cast<double>(ns) / static_cast<double>(s.iters);
  const double ops_per_sec = (s.iters * 1e9) / static_cast<double>(ns);
  std::printf("%-48s  %10zu  %14lld  %10.2f  %12.0f\n",
              s.name, s.iters,
              static_cast<long long>(ns), ns_per_op, ops_per_sec);
}

// Build a 1 KB blob of pseudo-random bytes once and reuse.
std::vector<std::uint8_t> kBlob1k = [] {
  std::vector<std::uint8_t> v(1024);
  std::uint32_t x = 0x9E3779B9u;
  for (auto& b : v) {
    x = x * 1664525u + 1013904223u;
    b = static_cast<std::uint8_t>(x >> 24);
  }
  return v;
}();

// 100-char string used for string-scenario.
std::string kStr100 = std::string(100, 'x');

}  // namespace

int main() {
  std::printf("%-48s  %10s  %14s  %10s  %12s\n",
              "scenario", "iters", "total_ns", "ns/op", "ops/sec");

  // ------ Single-primitive: operator<< / operator>> for i64 -----------------
  run({"write+read i64 (fresh Marshal each pair)",
       2'000'000,
       [](std::size_t n) {
         for (std::size_t i = 0; i < n; ++i) {
           rrr::BufferSink sink;
           rrr::BinaryWriteArchive war(rrr::make_sink_proxy_buffer(&sink));
           i64 v = static_cast<i64>(i);
           rrr::Serialize_::serialize(v, war);
           rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
           rrr::BinaryReadArchive rar(rrr::make_source_proxy_buffer(&src));
           i64 out;
           rrr::Deserialize_::deserialize(out, rar);
           if (out != v) std::abort();
         }
       }});

  // ------ Same shape but reuse a single Marshal across the loop -----------
  // This isolates the per-op cost of operator<< / operator>> from the
  // construction/teardown cost of the chunk allocator.
  run({"write+read i64 (single Marshal, drains immediately)",
       5'000'000,
       [](std::size_t n) {
         rrr::BufferSink sink;
         rrr::BinaryWriteArchive war(rrr::make_sink_proxy_buffer(&sink));
         for (std::size_t i = 0; i < n; ++i) {
           sink.bytes.clear();
           i64 v = static_cast<i64>(i);
           rrr::Serialize_::serialize(v, war);
           rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
           rrr::BinaryReadArchive rar(rrr::make_source_proxy_buffer(&src));
           i64 out;
           rrr::Deserialize_::deserialize(out, rar);
           if (out != v) std::abort();
         }
       }});

  // ------ Many writes, one big drain ---------------------------------------
  // Forces chunk growth: writes accumulate until reading drains.
  run({"write 1024 i64 then read 1024 i64",
       50'000,
       [](std::size_t n) {
         constexpr std::size_t kCount = 1024;
         for (std::size_t k = 0; k < n; ++k) {
           rrr::BufferSink sink;
           rrr::BinaryWriteArchive war(rrr::make_sink_proxy_buffer(&sink));
           for (std::size_t i = 0; i < kCount; ++i) {
             i64 v = static_cast<i64>(i);
             rrr::Serialize_::serialize(v, war);
           }
           rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
           rrr::BinaryReadArchive rar(rrr::make_source_proxy_buffer(&src));
           for (std::size_t i = 0; i < kCount; ++i) {
             i64 out;
             rrr::Deserialize_::deserialize(out, rar);
             if (out != static_cast<i64>(i)) std::abort();
           }
         }
       }});

  // ------ Raw write/read of an 8-byte blob (lower-level than operator<<) ---
  run({"raw write(8) + read(8) (single Marshal)",
       5'000'000,
       [](std::size_t n) {
         rrr::BufferSink sink;
         std::uint64_t v = 0xDEADBEEFCAFEBABEull;
         std::uint64_t out = 0;
         for (std::size_t i = 0; i < n; ++i) {
           sink.bytes.clear();
           sink.write_bytes(reinterpret_cast<const std::uint8_t*>(&v), sizeof(v));
           rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
           src.read_bytes(reinterpret_cast<std::uint8_t*>(&out), sizeof(out));
           if (out != v) std::abort();
         }
       }});

  // ------ 1 KB blob round-trip ---------------------------------------------
  // Stresses memcpy across chunk boundaries (default chunk is < 4 KB).
  run({"write 1KB blob + read 1KB blob",
       200'000,
       [](std::size_t n) {
         std::vector<std::uint8_t> dst(kBlob1k.size());
         for (std::size_t i = 0; i < n; ++i) {
           rrr::BufferSink sink;
           sink.write_bytes(reinterpret_cast<const std::uint8_t*>(kBlob1k.data()), kBlob1k.size());
           rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
           src.read_bytes(reinterpret_cast<std::uint8_t*>(dst.data()), dst.size());
         }
       }});

  // ------ std::string of 100 chars (varint len + bytes) --------------------
  run({"write+read std::string(100)",
       1'000'000,
       [](std::size_t n) {
         std::string in = kStr100;
         std::string out;
         for (std::size_t i = 0; i < n; ++i) {
           rrr::BufferSink sink;
           rrr::BinaryWriteArchive war(rrr::make_sink_proxy_buffer(&sink));
           rrr::Serialize_::serialize(in, war);
           rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
           rrr::BinaryReadArchive rar(rrr::make_source_proxy_buffer(&src));
           rrr::Deserialize_::deserialize(out, rar);
           if (out != in) std::abort();
         }
       }});

  // ------ Mixed: 4 i32s + 1 string (typical RPC payload shape) -------------
  run({"4*i32 + string(100) round-trip",
       500'000,
       [](std::size_t n) {
         std::string in = kStr100;
         for (std::size_t i = 0; i < n; ++i) {
           rrr::BufferSink sink;
           rrr::BinaryWriteArchive war(rrr::make_sink_proxy_buffer(&sink));
           i32 a = 1, b = 2, c = 3, d = 4;
           rrr::Serialize_::serialize(a, war);
           rrr::Serialize_::serialize(b, war);
           rrr::Serialize_::serialize(c, war);
           rrr::Serialize_::serialize(d, war);
           rrr::Serialize_::serialize(in, war);
           i32 ao, bo, co, dxo;
           std::string so;
           rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
           rrr::BinaryReadArchive rar(rrr::make_source_proxy_buffer(&src));
           rrr::Deserialize_::deserialize(ao, rar);
           rrr::Deserialize_::deserialize(bo, rar);
           rrr::Deserialize_::deserialize(co, rar);
           rrr::Deserialize_::deserialize(dxo, rar);
           rrr::Deserialize_::deserialize(so, rar);
           if (so.size() != in.size()) std::abort();
         }
       }});

  // ------ Chunk-boundary crossing on a single write ------------------------
  // Default chunk size is set by the implementation; a 4 KB write almost
  // certainly straddles at least one boundary.
  run({"write 4KB blob (single write) + read 4KB",
       100'000,
       [](std::size_t n) {
         std::vector<std::uint8_t> blob(4096, 0xAB);
         std::vector<std::uint8_t> dst(4096);
         for (std::size_t i = 0; i < n; ++i) {
           rrr::BufferSink sink;
           sink.write_bytes(reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size());
           rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
           src.read_bytes(reinterpret_cast<std::uint8_t*>(dst.data()), dst.size());
         }
       }});

  // ------ Producer-faster-than-consumer pattern ----------------------------
  // 10 KB written before any read — forces multiple chunks alive at once.
  run({"write 10x 1KB then drain 10x 1KB",
       50'000,
       [](std::size_t n) {
         std::vector<std::uint8_t> dst(kBlob1k.size());
         for (std::size_t k = 0; k < n; ++k) {
           rrr::BufferSink sink;
           for (int i = 0; i < 10; ++i) {
             sink.write_bytes(reinterpret_cast<const std::uint8_t*>(kBlob1k.data()), kBlob1k.size());
           }
           rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
           for (int i = 0; i < 10; ++i) {
             src.read_bytes(reinterpret_cast<std::uint8_t*>(dst.data()), dst.size());
           }
         }
       }});

  return 0;
}
