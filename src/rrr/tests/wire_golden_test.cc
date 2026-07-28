// Bidirectional wire-format golden corpus: C++ <-> Rust.
//
// The corpus file (crates/srpc/tests/golden/wire_corpus.txt) is the
// contract between the C++ wire implementation (src/rrr) and the Rust
// crate (crates/srpc). Each line is `<name> <hex>`; both sides build
// the same named cases with their own encoders and assert the bytes.
//
//   * This test (default mode): encodes every case with the REAL rrr
//     serializers and asserts equality against the checked-in corpus.
//   * SRPC_GOLDEN_WRITE=1: regenerates the corpus file instead (run
//     once when the case list changes, commit the file).
//   * The Rust side (crates/srpc/tests/golden.rs) asserts the same
//     corpus with the crate's encoders via include_str!.
//
// Keep the case list in lockstep with golden.rs — both sides fail on
// unknown/missing names.

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include <rusty/arc.hpp>
#include <rusty/option.hpp>
#include <rusty/box.hpp>
#include <gtest/gtest.h>
#include "../rrr.hpp"

import std;
import rusty;

using namespace rrr;

namespace {

struct GoldenCase {
    std::string name;
    std::string hex;
};

std::string to_hex(const uint8_t* p, size_t n) {
    if (n == 0) return "-";
    std::string out;
    char b[3];
    for (size_t i = 0; i < n; i++) {
        snprintf(b, sizeof(b), "%02x", p[i]);
        out += b;
    }
    return out;
}

// Encode one value with a fresh archive; return lowercase hex ("-" for
// empty).
template <typename F>
std::string encode_case(F&& fill) {
    rrr::BufferSink sink;
    rrr::BinaryWriteArchive ar{rrr::make_sink_proxy(&sink)};
    fill(ar);
    return to_hex(sink.bytes.data(), sink.bytes.len());
}

std::vector<GoldenCase> build_corpus() {
    std::vector<GoldenCase> cs;
    auto add = [&](const char* name, std::string hex) {
        cs.push_back(GoldenCase{name, std::move(hex)});
    };

    // --- v32 boundaries ---------------------------------------------------
    const int32_t v32_vals[] = {0, 1, -1, 63, 64, -64, -65, 8191, 8192,
                                -8192, -8193, 1048575, 1048576, -1048576,
                                -1048577, 134217727, 134217728, -134217728,
                                -134217729, 2147483647, -2147483648};
    for (size_t i = 0; i < sizeof(v32_vals) / sizeof(v32_vals[0]); i++) {
        char name[64];
        snprintf(name, sizeof(name), "v32_%zu", i);
        int32_t v = v32_vals[i];
        add(name, encode_case([&](BinaryWriteArchive& ar) {
                rrr::Serialize_::serialize(rrr::v32{v}, ar);
            }));
    }

    // --- v64 boundaries (incl. the 8-length quirk case) -------------------
    const int64_t v64_vals[] = {0LL, 1LL, -1LL, 63LL, -64LL, 8191LL, -8192LL,
                                1048575LL, -1048576LL, 134217727LL,
                                -134217728LL, 17179869183LL, -17179869184LL,
                                2199023255551LL, -2199023255552LL,
                                281474976710655LL, -281474976710656LL,
                                36028797018963967LL,  // val_size 8: quirk
                                -36028797018963968LL, // val_size 8: quirk
                                9223372036854775807LL,
                                (-9223372036854775807LL - 1)};
    for (size_t i = 0; i < sizeof(v64_vals) / sizeof(v64_vals[0]); i++) {
        char name[64];
        snprintf(name, sizeof(name), "v64_%zu", i);
        int64_t v = v64_vals[i];
        add(name, encode_case([&](BinaryWriteArchive& ar) {
                rrr::Serialize_::serialize(rrr::v64{v}, ar);
            }));
    }

    // --- fixed-width scalars ----------------------------------------------
    add("i8_min", encode_case([](BinaryWriteArchive& ar) {
            rrr::Serialize_::serialize(static_cast<int8_t>(-128), ar);
        }));
    add("i16_min", encode_case([](BinaryWriteArchive& ar) {
            rrr::Serialize_::serialize(static_cast<int16_t>(-32768), ar);
        }));
    add("i32_pattern", encode_case([](BinaryWriteArchive& ar) {
            rrr::Serialize_::serialize(static_cast<int32_t>(0x01020304), ar);
        }));
    add("i64_pattern", encode_case([](BinaryWriteArchive& ar) {
            rrr::Serialize_::serialize(
                static_cast<int64_t>(0x0102030405060708LL), ar);
        }));
    add("u8_max", encode_case([](BinaryWriteArchive& ar) {
            rrr::Serialize_::serialize(static_cast<uint8_t>(255), ar);
        }));
    add("u16_max", encode_case([](BinaryWriteArchive& ar) {
            rrr::Serialize_::serialize(static_cast<uint16_t>(65535), ar);
        }));
    add("u32_max", encode_case([](BinaryWriteArchive& ar) {
            rrr::Serialize_::serialize(static_cast<uint32_t>(4294967295u), ar);
        }));
    add("u64_max", encode_case([](BinaryWriteArchive& ar) {
            rrr::Serialize_::serialize(
                static_cast<uint64_t>(18446744073709551615ull), ar);
        }));
    add("f64_pi", encode_case([](BinaryWriteArchive& ar) {
            rrr::Serialize_::serialize(3.14159, ar);
        }));

    // --- strings ----------------------------------------------------------
    add("str_empty", encode_case([](BinaryWriteArchive& ar) {
            rrr::Serialize_::serialize(std::string(""), ar);
        }));
    add("str_a", encode_case([](BinaryWriteArchive& ar) {
            rrr::Serialize_::serialize(std::string("a"), ar);
        }));
    add("str_hello", encode_case([](BinaryWriteArchive& ar) {
            rrr::Serialize_::serialize(std::string("hello rrr wire"), ar);
        }));
    add("str_200", encode_case([](BinaryWriteArchive& ar) {
            rrr::Serialize_::serialize(std::string(200, 'x'), ar);
        }));

    // --- containers -------------------------------------------------------
    add("vec_i32_empty", encode_case([](BinaryWriteArchive& ar) {
            std::vector<int32_t> v;
            rrr::Serialize_::serialize(v, ar);
        }));
    add("vec_i32_123", encode_case([](BinaryWriteArchive& ar) {
            std::vector<int32_t> v{1, -2, 3};
            rrr::Serialize_::serialize(v, ar);
        }));
    add("vec_str", encode_case([](BinaryWriteArchive& ar) {
            std::vector<std::string> v{"x", "yz"};
            rrr::Serialize_::serialize(v, ar);
        }));
    add("pair_v64_str", encode_case([](BinaryWriteArchive& ar) {
            std::pair<rrr::v64, std::string> p{rrr::v64{7}, std::string("kv")};
            rrr::Serialize_::serialize(p, ar);
        }));

    // --- request-body composite: [v64 xid][i32 rpc_id][string arg] --------
    add("request_body", encode_case([](BinaryWriteArchive& ar) {
            rrr::Serialize_::serialize(rrr::v64{1}, ar);
            rrr::Serialize_::serialize(static_cast<int32_t>(0x1234), ar);
            rrr::Serialize_::serialize(std::string("payload"), ar);
        }));

    // --- ordered map: [v64 len][k,v...] in key order -----------------------
    add("map_i32_str", encode_case([](BinaryWriteArchive& ar) {
            std::map<int32_t, std::string> m;
            m[7] = "seven";
            m[-1] = "neg";
            rrr::Serialize_::serialize(m, ar);
        }));

    // --- frame codec: [i32 LE (size | ext<<31)][payload] -------------------
    {
        auto frame_hex = [](const char* payload, bool ext) {
            std::vector<std::uint8_t> out;
            const auto* p = reinterpret_cast<const std::uint8_t*>(payload);
            bool ok = rrr::frame_codec_encode_into(
                out, p, static_cast<std::int32_t>(strlen(payload)), ext);
            EXPECT_TRUE(ok);
            return to_hex(out.data(), out.size());
        };
        add("frame_empty", frame_hex("", false));
        add("frame_hello", frame_hex("hello", false));
        add("frame_ext_x", frame_hex("x", true));
    }

    return cs;
}

// Corpus path: SRPC_GOLDEN_CORPUS env override, else derived from this
// source file's location (source tree is present in CI).
std::string corpus_path() {
    const char* env = getenv("SRPC_GOLDEN_CORPUS");
    if (env != nullptr && env[0] != '\0') return std::string(env);
    std::string self(__FILE__);
    auto pos = self.rfind("src/rrr/tests/");
    if (pos == std::string::npos) return std::string();
    return self.substr(0, pos) + "crates/srpc/tests/golden/wire_corpus.txt";
}

}  // namespace

TEST(WireGolden, CorpusMatchesCpp) {
    auto cases = build_corpus();
    auto path = corpus_path();
    ASSERT_FALSE(path.empty()) << "cannot derive corpus path";

    const char* wr = getenv("SRPC_GOLDEN_WRITE");
    if (wr != nullptr && wr[0] == '1') {
        std::ofstream out(path);
        ASSERT_TRUE(out.good()) << "cannot open " << path << " for write";
        out << "# rrr wire golden corpus — generated by wire_golden_test.cc"
            << " (SRPC_GOLDEN_WRITE=1); asserted byte-exact by BOTH the C++\n"
            << "# implementation (this test) and the Rust crate"
            << " (crates/srpc/tests/golden.rs). Format: <name> <hex|->\n";
        for (auto& c : cases) out << c.name << " " << c.hex << "\n";
        GTEST_SKIP() << "corpus regenerated at " << path;
    }

    std::ifstream in(path);
    if (!in.good()) {
        GTEST_SKIP() << "corpus not found at " << path
                     << " (generate with SRPC_GOLDEN_WRITE=1)";
    }
    std::map<std::string, std::string> want;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto sp = line.find(' ');
        ASSERT_NE(sp, std::string::npos) << "bad corpus line: " << line;
        want[line.substr(0, sp)] = line.substr(sp + 1);
    }
    ASSERT_EQ(want.size(), cases.size())
        << "corpus/case-list drift — regenerate and sync golden.rs";
    for (auto& c : cases) {
        auto it = want.find(c.name);
        ASSERT_NE(it, want.end()) << "missing corpus entry: " << c.name;
        EXPECT_EQ(it->second, c.hex) << "byte mismatch for " << c.name;
    }
}
