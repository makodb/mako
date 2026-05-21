// MarshalV2 microbenchmark — identical scenarios to bench_marshal,
// but exercises rrr::MarshalV2 (Vec<u8> + read_pos) instead of the
// chunk-linked-list `rrr::Marshal`. Lets us compare ns/op directly
// against the baseline in docs/dev/marshal_perf_baseline.md.

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include <rusty/box.hpp>
#include <rusty/option.hpp>

#include "../rrr.hpp"

import std;
import rrr.marshal_v2;

using rrr::MarshalV2;
using rrr::i32;
using rrr::i64;

namespace {

using clk = std::chrono::steady_clock;

struct Scenario {
  const char* name;
  std::size_t iters;
  std::function<void(std::size_t)> body;
};

void run(const Scenario& s) {
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

std::vector<std::uint8_t> kBlob1k = [] {
  std::vector<std::uint8_t> v(1024);
  std::uint32_t x = 0x9E3779B9u;
  for (auto& b : v) {
    x = x * 1664525u + 1013904223u;
    b = static_cast<std::uint8_t>(x >> 24);
  }
  return v;
}();

std::string kStr100 = std::string(100, 'x');

}  // namespace

int main() {
  std::printf("%-48s  %10s  %14s  %10s  %12s\n",
              "scenario", "iters", "total_ns", "ns/op", "ops/sec");

  run({"write+read i64 (fresh Marshal each pair)",
       2'000'000,
       [](std::size_t n) {
         for (std::size_t i = 0; i < n; ++i) {
           MarshalV2 m;
           i64 v = static_cast<i64>(i);
           m << v;
           i64 out;
           m >> out;
           if (out != v) std::abort();
         }
       }});

  run({"write+read i64 (single Marshal, drains immediately)",
       5'000'000,
       [](std::size_t n) {
         MarshalV2 m;
         for (std::size_t i = 0; i < n; ++i) {
           i64 v = static_cast<i64>(i);
           m << v;
           i64 out;
           m >> out;
           if (out != v) std::abort();
         }
       }});

  run({"write 1024 i64 then read 1024 i64",
       50'000,
       [](std::size_t n) {
         constexpr std::size_t kCount = 1024;
         for (std::size_t k = 0; k < n; ++k) {
           MarshalV2 m;
           for (std::size_t i = 0; i < kCount; ++i) {
             i64 v = static_cast<i64>(i);
             m << v;
           }
           for (std::size_t i = 0; i < kCount; ++i) {
             i64 out;
             m >> out;
             if (out != static_cast<i64>(i)) std::abort();
           }
         }
       }});

  run({"raw write(8) + read(8) (single Marshal)",
       5'000'000,
       [](std::size_t n) {
         MarshalV2 m;
         std::uint64_t v = 0xDEADBEEFCAFEBABEull;
         std::uint64_t out = 0;
         for (std::size_t i = 0; i < n; ++i) {
           m.write(&v, sizeof(v));
           m.read(&out, sizeof(out));
           if (out != v) std::abort();
         }
       }});

  run({"write 1KB blob + read 1KB blob",
       200'000,
       [](std::size_t n) {
         std::vector<std::uint8_t> sink(kBlob1k.size());
         for (std::size_t i = 0; i < n; ++i) {
           MarshalV2 m;
           m.write(kBlob1k.data(), kBlob1k.size());
           m.read(sink.data(), sink.size());
         }
       }});

  run({"write+read std::string(100)",
       1'000'000,
       [](std::size_t n) {
         std::string in = kStr100;
         std::string out;
         for (std::size_t i = 0; i < n; ++i) {
           MarshalV2 m;
           m << in;
           m >> out;
           if (out != in) std::abort();
         }
       }});

  run({"4*i32 + string(100) round-trip",
       500'000,
       [](std::size_t n) {
         std::string in = kStr100;
         for (std::size_t i = 0; i < n; ++i) {
           MarshalV2 m;
           i32 a = 1, b = 2, c = 3, d = 4;
           m << a << b << c << d << in;
           i32 ao, bo, co, dxo;
           std::string so;
           m >> ao >> bo >> co >> dxo >> so;
           if (so.size() != in.size()) std::abort();
         }
       }});

  run({"write 4KB blob (single write) + read 4KB",
       100'000,
       [](std::size_t n) {
         std::vector<std::uint8_t> blob(4096, 0xAB);
         std::vector<std::uint8_t> sink(4096);
         for (std::size_t i = 0; i < n; ++i) {
           MarshalV2 m;
           m.write(blob.data(), blob.size());
           m.read(sink.data(), sink.size());
         }
       }});

  run({"write 10x 1KB then drain 10x 1KB",
       50'000,
       [](std::size_t n) {
         std::vector<std::uint8_t> sink(kBlob1k.size());
         for (std::size_t k = 0; k < n; ++k) {
           MarshalV2 m;
           for (int i = 0; i < 10; ++i) {
             m.write(kBlob1k.data(), kBlob1k.size());
           }
           for (int i = 0; i < 10; ++i) {
             m.read(sink.data(), sink.size());
           }
         }
       }});

  return 0;
}
