module;

#include <rusty/rusty.hpp>
#include <rusty/sync/atomic.hpp>
#include <inttypes.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>

export module rrr.basetypes;

import std;
import rusty;

// @safe - POD/value-type helpers + small classes (SparseInt, v32/v64,
// NoCopy, Counter, Time, Timer, Rand, Enumerator, MergedEnumerator).
// Time / Timer time syscalls (clock_gettime, gettimeofday, nanosleep)
// now flow through `rusty::sys::time::*` helpers (each itself @safe
// with an inner @unsafe block). The remaining per-method `// @unsafe`
// overrides cover raw `char*` byte slicing via `reinterpret_cast<char*>`
// and `pthread_self`-based hashing in `Rand`.
export namespace rrr {

// Bring `Ordering` and `AtomicI64` into the `rrr` namespace so DSL
// bodies can write `Ordering::Relaxed` / `AtomicI64::new(...)` (Rust
// idiom) and the emitted C++ resolves via these using-decls. The
// `i8`..`i64` aliases are the historical rrr integer spellings that
// src/bench/tpcc, src/memdb and rpc/server.cpp still name as
// `rrr::i32` / `base::i64`, so they are live, not dead scaffolding.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. Probe-verified nuance: the
// transpiler does NOT rewrite an alias NAME that collides with a Rust
// primitive -- `pub type i32 = int32_t;` emits `using i32 = int32_t;`,
// which is exactly what the call sites need. The emitted order is
// aliases first, usings second (no dependency either way).
#if RUSTYCPP_RUST
use rusty::sync::atomic::Ordering;
use rusty::sync::atomic::AtomicI64;

pub type i8 = int8_t;
pub type i16 = int16_t;
pub type i32 = int32_t;
pub type i64 = int64_t;
#endif
/*RUSTYCPP:GEN-BEGIN id=basetypes.7 version=1 rust_sha256=a6c5879d9739eedcd4342f4cabfdeb6a104e29338254af74ad9e299b9bdc0101*/
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using rusty::sync::atomic::Ordering;

using rusty::sync::atomic::AtomicI64;
/*RUSTYCPP:GEN-END id=basetypes.7*/


// `SparseInt` — the wire varint format's pointer-free queries, as DSL
// statics (the v32/v64 DSL blocks above call SparseInt::val_size).
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block.
#if RUSTYCPP_RUST
struct SparseInt {}

impl SparseInt {
    // Total encoded length implied by the first byte.
    fn buf_size(byte0: u8) -> usize {
        if (byte0 & 0x80) == 0 {
            return 1;
        } else if (byte0 & 0xC0) == 0x80 {
            return 2;
        } else if (byte0 & 0xE0) == 0xC0 {
            return 3;
        } else if (byte0 & 0xF0) == 0xE0 {
            return 4;
        } else if (byte0 & 0xF8) == 0xF0 {
            return 5;
        } else if (byte0 & 0xFC) == 0xF8 {
            return 6;
        } else if (byte0 & 0xFE) == 0xFC {
            return 7;
        } else if (byte0 & 0xFF) == 0xFE {
            return 8;
        }
        9
    }

    // Wire varint encode (shift-based; byte-identical to the legacy
    // little-endian reinterpret_cast copies, including the legacy
    // 8-length/9-byte quirk in the i64 case-8 encoding). All byte
    // values are u8 (see the signedness trap in the porting notes).
    fn dump32(val: i32, buf: *mut u8) -> usize {
        let u = val as u32;
        if -64 <= val && val <= 63 {
            buf[0] = (u & 0xFF) as u8;
            buf[0] &= 0x7F;
            return 1;
        } else if -8192 <= val && val <= 8191 {
            buf[0] = ((u >> 8) & 0xFF) as u8;
            buf[1] = (u & 0xFF) as u8;
            buf[0] &= 0x3F;
            buf[0] |= 0x80;
            return 2;
        } else if -1048576 <= val && val <= 1048575 {
            buf[0] = ((u >> 16) & 0xFF) as u8;
            buf[1] = ((u >> 8) & 0xFF) as u8;
            buf[2] = (u & 0xFF) as u8;
            buf[0] &= 0x1F;
            buf[0] |= 0xC0;
            return 3;
        } else if -134217728 <= val && val <= 134217727 {
            buf[0] = ((u >> 24) & 0xFF) as u8;
            buf[1] = ((u >> 16) & 0xFF) as u8;
            buf[2] = ((u >> 8) & 0xFF) as u8;
            buf[3] = (u & 0xFF) as u8;
            buf[0] &= 0x0F;
            buf[0] |= 0xE0;
            return 4;
        }
        buf[1] = ((u >> 24) & 0xFF) as u8;
        buf[2] = ((u >> 16) & 0xFF) as u8;
        buf[3] = ((u >> 8) & 0xFF) as u8;
        buf[4] = (u & 0xFF) as u8;
        if val < 0 {
            buf[0] = 0xF7;
        } else {
            buf[0] = 0xF0;
        }
        5
    }

    fn dump64(val: i64, buf: *mut u8) -> usize {
        let u = val as u64;
        let n = SparseInt::val_size(val) as i32;
        if n <= 7 {
            // bytes big-endian: buf[j] = byte (n-1-j) of u, then mask/tag.
            let mut j: i32 = 0;
            while j < n {
                buf[j] = ((u >> (8 * ((n - 1) - j))) & 0xFF) as u8;
                j += 1;
            }
            if n == 1 {
                buf[0] &= 0x7F;
            } else if n == 2 {
                buf[0] &= 0x3F;
                buf[0] |= 0x80;
            } else if n == 3 {
                buf[0] &= 0x1F;
                buf[0] |= 0xC0;
            } else if n == 4 {
                buf[0] &= 0x0F;
                buf[0] |= 0xE0;
            } else if n == 5 {
                buf[0] &= 0x07;
                buf[0] |= 0xF0;
            } else if n == 6 {
                buf[0] &= 0x03;
                buf[0] |= 0xF8;
            } else {
                buf[0] &= 0x01;
                buf[0] |= 0xFC;
            }
            return n as usize;
        }
        // n == 8 or 9: marker byte + all eight payload bytes at buf[1..8]
        // (legacy quirk: n == 8 also writes nine bytes but reports 8).
        let mut j: i32 = 0;
        while j < 8 {
            buf[1 + j] = ((u >> (8 * (7 - j))) & 0xFF) as u8;
            j += 1;
        }
        if n == 8 {
            buf[0] = 0xFE;
            return 8;
        }
        buf[0] = 0xFF;
        9
    }

    // Wire varint decode (shift-based; matches the legacy byte copies
    // exactly, including sign extension via the top-byte bit surgery).
    fn load32(buf: *const u8) -> i32 {
        let bsize = SparseInt::buf_size(buf[0]) as i32;
        let mut u: u32 = 0;
        if bsize < 5 {
            let mut i: i32 = 0;
            while i < bsize - 1 {
                u |= (buf[(bsize - 1) - i] as u32) << (8 * i);
                i += 1;
            }
            let mut top = buf[0];
            top &= (0xFF >> bsize) as u8;
            if ((top >> (7 - bsize)) & 0x1) == 1 {
                top |= ((0xFF << (7 - bsize)) & 0xFF) as u8;
                let mut k = bsize;
                while k < 4 {
                    u |= 0xFFu32 << (8 * k);
                    k += 1;
                }
            }
            u |= (top as u32) << (8 * (bsize - 1));
            return u as i32;
        }
        let mut i: i32 = 0;
        while i < 4 {
            u |= (buf[4 - i] as u32) << (8 * i);
            i += 1;
        }
        u as i32
    }

    fn load64(buf: *const u8) -> i64 {
        let bsize = SparseInt::buf_size(buf[0]) as i32;
        let mut u: u64 = 0;
        if bsize < 8 {
            let mut i: i32 = 0;
            while i < bsize - 1 {
                u |= (buf[(bsize - 1) - i] as u64) << (8 * i);
                i += 1;
            }
            let mut top = buf[0];
            top &= (0xFF >> bsize) as u8;
            if ((top >> (7 - bsize)) & 0x1) == 1 {
                top |= ((0xFF << (7 - bsize)) & 0xFF) as u8;
                let mut k = bsize;
                while k < 8 {
                    u |= 0xFFu64 << (8 * k);
                    k += 1;
                }
            }
            u |= (top as u64) << (8 * (bsize - 1));
            return u as i64;
        }
        let mut i: i32 = 0;
        while i < 8 {
            u |= (buf[8 - i] as u64) << (8 * i);
            i += 1;
        }
        u as i64
    }

    // Encoded length required for the value.
    fn val_size(val: i64) -> usize {
        if -64 <= val && val <= 63 {
            return 1;
        } else if -8192 <= val && val <= 8191 {
            return 2;
        } else if -1048576 <= val && val <= 1048575 {
            return 3;
        } else if -134217728 <= val && val <= 134217727 {
            return 4;
        } else if -17179869184 <= val && val <= 17179869183 {
            return 5;
        } else if -2199023255552 <= val && val <= 2199023255551 {
            return 6;
        } else if -281474976710656 <= val && val <= 281474976710655 {
            return 7;
        } else if -36028797018963968 <= val && val <= 36028797018963967 {
            return 8;
        }
        9
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=basetypes.sparseint version=1 rust_sha256=a76c04001fb5c161217cb2aa2dba74d9ded042f93483750462d69ab8beba9867*/
struct SparseInt;

struct SparseInt {

    static size_t buf_size(uint8_t byte0);
    static size_t dump32(int32_t val, uint8_t* buf);
    static size_t dump64(int64_t val, uint8_t* buf);
    static int32_t load32(const uint8_t* buf);
    static int64_t load64(const uint8_t* buf);
    static size_t val_size(int64_t val);
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


size_t SparseInt::buf_size(uint8_t byte0) {
    if (((rusty::detail::deref_if_pointer_like(byte0) & static_cast<int32_t>(128))) == static_cast<uint8_t>(0)) {
        return static_cast<size_t>(1);
    } else if (((rusty::detail::deref_if_pointer_like(byte0) & static_cast<int32_t>(192))) == static_cast<uint8_t>(128)) {
        return static_cast<size_t>(2);
    } else if (((rusty::detail::deref_if_pointer_like(byte0) & static_cast<int32_t>(224))) == static_cast<uint8_t>(192)) {
        return static_cast<size_t>(3);
    } else if (((rusty::detail::deref_if_pointer_like(byte0) & static_cast<int32_t>(240))) == static_cast<uint8_t>(224)) {
        return static_cast<size_t>(4);
    } else if (((rusty::detail::deref_if_pointer_like(byte0) & static_cast<int32_t>(248))) == static_cast<uint8_t>(240)) {
        return static_cast<size_t>(5);
    } else if (((rusty::detail::deref_if_pointer_like(byte0) & static_cast<int32_t>(252))) == static_cast<uint8_t>(248)) {
        return static_cast<size_t>(6);
    } else if (((rusty::detail::deref_if_pointer_like(byte0) & static_cast<int32_t>(254))) == static_cast<uint8_t>(252)) {
        return static_cast<size_t>(7);
    } else if (((rusty::detail::deref_if_pointer_like(byte0) & static_cast<int32_t>(255))) == static_cast<uint8_t>(254)) {
        return static_cast<size_t>(8);
    }
    return static_cast<size_t>(9);
}

size_t SparseInt::dump32(int32_t val, uint8_t* buf) {
    const auto u = static_cast<uint32_t>(val);
    if ((-64 <= rusty::detail::deref_if_pointer_like(val)) && (rusty::detail::deref_if_pointer_like(val) <= 63)) {
        buf[static_cast<size_t>(0)] = static_cast<uint8_t>((rusty::detail::deref_if_pointer_like(u) & 255));
        buf[static_cast<size_t>(0)] &= 127;
        return static_cast<size_t>(1);
    } else if ((-8192 <= rusty::detail::deref_if_pointer_like(val)) && (rusty::detail::deref_if_pointer_like(val) <= 8191)) {
        buf[static_cast<size_t>(0)] = static_cast<uint8_t>((((rusty::detail::deref_if_pointer_like(u) >> 8)) & 255));
        buf[static_cast<size_t>(1)] = static_cast<uint8_t>((rusty::detail::deref_if_pointer_like(u) & 255));
        buf[static_cast<size_t>(0)] &= 63;
        buf[static_cast<size_t>(0)] |= 128;
        return static_cast<size_t>(2);
    } else if ((-1048576 <= rusty::detail::deref_if_pointer_like(val)) && (rusty::detail::deref_if_pointer_like(val) <= 1048575)) {
        buf[static_cast<size_t>(0)] = static_cast<uint8_t>((((rusty::detail::deref_if_pointer_like(u) >> 16)) & 255));
        buf[static_cast<size_t>(1)] = static_cast<uint8_t>((((rusty::detail::deref_if_pointer_like(u) >> 8)) & 255));
        buf[static_cast<size_t>(2)] = static_cast<uint8_t>((rusty::detail::deref_if_pointer_like(u) & 255));
        buf[static_cast<size_t>(0)] &= 31;
        buf[static_cast<size_t>(0)] |= 192;
        return static_cast<size_t>(3);
    } else if ((-134217728 <= rusty::detail::deref_if_pointer_like(val)) && (rusty::detail::deref_if_pointer_like(val) <= 134217727)) {
        buf[static_cast<size_t>(0)] = static_cast<uint8_t>((((rusty::detail::deref_if_pointer_like(u) >> 24)) & 255));
        buf[static_cast<size_t>(1)] = static_cast<uint8_t>((((rusty::detail::deref_if_pointer_like(u) >> 16)) & 255));
        buf[static_cast<size_t>(2)] = static_cast<uint8_t>((((rusty::detail::deref_if_pointer_like(u) >> 8)) & 255));
        buf[static_cast<size_t>(3)] = static_cast<uint8_t>((rusty::detail::deref_if_pointer_like(u) & 255));
        buf[static_cast<size_t>(0)] &= 15;
        buf[static_cast<size_t>(0)] |= 224;
        return static_cast<size_t>(4);
    }
    buf[static_cast<size_t>(1)] = static_cast<uint8_t>((((rusty::detail::deref_if_pointer_like(u) >> 24)) & 255));
    buf[static_cast<size_t>(2)] = static_cast<uint8_t>((((rusty::detail::deref_if_pointer_like(u) >> 16)) & 255));
    buf[static_cast<size_t>(3)] = static_cast<uint8_t>((((rusty::detail::deref_if_pointer_like(u) >> 8)) & 255));
    buf[static_cast<size_t>(4)] = static_cast<uint8_t>((rusty::detail::deref_if_pointer_like(u) & 255));
    if (rusty::detail::deref_if_pointer_like(val) < 0) {
        buf[static_cast<size_t>(0)] = 247;
    } else {
        buf[static_cast<size_t>(0)] = 240;
    }
    return static_cast<size_t>(5);
}

size_t SparseInt::dump64(int64_t val, uint8_t* buf) {
    const auto u = static_cast<uint64_t>(val);
    const auto n = static_cast<int32_t>(SparseInt::val_size(std::move(val)));
    if (rusty::detail::deref_if_pointer_like(n) <= 7) {
        int32_t j = static_cast<int32_t>(0);
        while (rusty::detail::deref_if_pointer_like(j) < rusty::detail::deref_if_pointer_like(n)) {
            buf[j] = static_cast<uint8_t>((((rusty::detail::deref_if_pointer_like(u) >> ((8 * ((((rusty::detail::deref_if_pointer_like(n) - 1)) - rusty::detail::deref_if_pointer_like(j))))))) & 255));
            j += 1;
        }
        if (rusty::detail::deref_if_pointer_like(n) == 1) {
            buf[static_cast<size_t>(0)] &= 127;
        } else if (rusty::detail::deref_if_pointer_like(n) == 2) {
            buf[static_cast<size_t>(0)] &= 63;
            buf[static_cast<size_t>(0)] |= 128;
        } else if (rusty::detail::deref_if_pointer_like(n) == 3) {
            buf[static_cast<size_t>(0)] &= 31;
            buf[static_cast<size_t>(0)] |= 192;
        } else if (rusty::detail::deref_if_pointer_like(n) == 4) {
            buf[static_cast<size_t>(0)] &= 15;
            buf[static_cast<size_t>(0)] |= 224;
        } else if (rusty::detail::deref_if_pointer_like(n) == 5) {
            buf[static_cast<size_t>(0)] &= 7;
            buf[static_cast<size_t>(0)] |= 240;
        } else if (rusty::detail::deref_if_pointer_like(n) == 6) {
            buf[static_cast<size_t>(0)] &= 3;
            buf[static_cast<size_t>(0)] |= 248;
        } else {
            buf[static_cast<size_t>(0)] &= 1;
            buf[static_cast<size_t>(0)] |= 252;
        }
        return static_cast<size_t>(n);
    }
    int32_t j = static_cast<int32_t>(0);
    while (rusty::detail::deref_if_pointer_like(j) < 8) {
        buf[1 + rusty::detail::deref_if_pointer_like(j)] = static_cast<uint8_t>((((rusty::detail::deref_if_pointer_like(u) >> ((8 * ((7 - rusty::detail::deref_if_pointer_like(j))))))) & 255));
        j += 1;
    }
    if (rusty::detail::deref_if_pointer_like(n) == 8) {
        buf[static_cast<size_t>(0)] = 254;
        return static_cast<size_t>(8);
    }
    buf[static_cast<size_t>(0)] = 255;
    return static_cast<size_t>(9);
}

int32_t SparseInt::load32(const uint8_t* buf) {
    auto bsize = static_cast<int32_t>(SparseInt::buf_size(buf[static_cast<size_t>(0)]));
    uint32_t u = static_cast<uint32_t>(0);
    if (rusty::detail::deref_if_pointer_like(bsize) < 5) {
        int32_t i = static_cast<int32_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < (rusty::detail::deref_if_pointer_like(bsize) - 1)) {
            u |= ((static_cast<uint32_t>(buf[((rusty::detail::deref_if_pointer_like(bsize) - 1)) - rusty::detail::deref_if_pointer_like(i)]))) << ((8 * rusty::detail::deref_if_pointer_like(i)));
            i += 1;
        }
        auto top = buf[static_cast<size_t>(0)];
        rusty::detail::deref_if_pointer_like(top) &= static_cast<uint8_t>((255 >> rusty::detail::deref_if_pointer_like(bsize)));
        if (((((rusty::detail::deref_if_pointer_like(top) >> ((7 - rusty::detail::deref_if_pointer_like(bsize))))) & static_cast<int32_t>(1))) == static_cast<int32_t>(1)) {
            rusty::detail::deref_if_pointer_like(top) |= static_cast<uint8_t>((((255 << ((7 - rusty::detail::deref_if_pointer_like(bsize))))) & 255));
            auto k = std::move(bsize);
            while (rusty::detail::deref_if_pointer_like(k) < 4) {
                u |= static_cast<uint32_t>(255) << ((8 * rusty::detail::deref_if_pointer_like(k)));
                rusty::detail::deref_if_pointer_like(k) += 1;
            }
        }
        u |= ((static_cast<uint32_t>(top))) << ((8 * ((rusty::detail::deref_if_pointer_like(bsize) - 1))));
        return static_cast<int32_t>(u);
    }
    int32_t i = static_cast<int32_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < 4) {
        u |= ((static_cast<uint32_t>(buf[4 - rusty::detail::deref_if_pointer_like(i)]))) << ((8 * rusty::detail::deref_if_pointer_like(i)));
        i += 1;
    }
    return static_cast<int32_t>(u);
}

int64_t SparseInt::load64(const uint8_t* buf) {
    auto bsize = static_cast<int32_t>(SparseInt::buf_size(buf[static_cast<size_t>(0)]));
    uint64_t u = static_cast<uint64_t>(0);
    if (rusty::detail::deref_if_pointer_like(bsize) < 8) {
        int32_t i = static_cast<int32_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < (rusty::detail::deref_if_pointer_like(bsize) - 1)) {
            u |= ((static_cast<uint64_t>(buf[((rusty::detail::deref_if_pointer_like(bsize) - 1)) - rusty::detail::deref_if_pointer_like(i)]))) << ((8 * rusty::detail::deref_if_pointer_like(i)));
            i += 1;
        }
        auto top = buf[static_cast<size_t>(0)];
        rusty::detail::deref_if_pointer_like(top) &= static_cast<uint8_t>((255 >> rusty::detail::deref_if_pointer_like(bsize)));
        if (((((rusty::detail::deref_if_pointer_like(top) >> ((7 - rusty::detail::deref_if_pointer_like(bsize))))) & static_cast<int32_t>(1))) == static_cast<int32_t>(1)) {
            rusty::detail::deref_if_pointer_like(top) |= static_cast<uint8_t>((((255 << ((7 - rusty::detail::deref_if_pointer_like(bsize))))) & 255));
            auto k = std::move(bsize);
            while (rusty::detail::deref_if_pointer_like(k) < 8) {
                u |= static_cast<uint64_t>(255) << ((8 * rusty::detail::deref_if_pointer_like(k)));
                rusty::detail::deref_if_pointer_like(k) += 1;
            }
        }
        u |= ((static_cast<uint64_t>(top))) << ((8 * ((rusty::detail::deref_if_pointer_like(bsize) - 1))));
        return static_cast<int64_t>(u);
    }
    int32_t i = static_cast<int32_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < 8) {
        u |= ((static_cast<uint64_t>(buf[8 - rusty::detail::deref_if_pointer_like(i)]))) << ((8 * rusty::detail::deref_if_pointer_like(i)));
        i += 1;
    }
    return static_cast<int64_t>(u);
}

size_t SparseInt::val_size(int64_t val) {
    if ((-64 <= rusty::detail::deref_if_pointer_like(val)) && (rusty::detail::deref_if_pointer_like(val) <= 63)) {
        return static_cast<size_t>(1);
    } else if ((-8192 <= rusty::detail::deref_if_pointer_like(val)) && (rusty::detail::deref_if_pointer_like(val) <= 8191)) {
        return static_cast<size_t>(2);
    } else if ((-1048576 <= rusty::detail::deref_if_pointer_like(val)) && (rusty::detail::deref_if_pointer_like(val) <= 1048575)) {
        return static_cast<size_t>(3);
    } else if ((-134217728 <= rusty::detail::deref_if_pointer_like(val)) && (rusty::detail::deref_if_pointer_like(val) <= 134217727)) {
        return static_cast<size_t>(4);
    } else if ((-17179869184 <= rusty::detail::deref_if_pointer_like(val)) && (rusty::detail::deref_if_pointer_like(val) <= 17179869183)) {
        return static_cast<size_t>(5);
    } else if ((-2199023255552 <= rusty::detail::deref_if_pointer_like(val)) && (rusty::detail::deref_if_pointer_like(val) <= 2199023255551)) {
        return static_cast<size_t>(6);
    } else if ((-281474976710656 <= rusty::detail::deref_if_pointer_like(val)) && (rusty::detail::deref_if_pointer_like(val) <= 281474976710655)) {
        return static_cast<size_t>(7);
    } else if ((-36028797018963968 <= rusty::detail::deref_if_pointer_like(val)) && (rusty::detail::deref_if_pointer_like(val) <= 36028797018963967)) {
        return static_cast<size_t>(8);
    }
    return static_cast<size_t>(9);
}
/*RUSTYCPP:GEN-END id=basetypes.sparseint*/

// `v32` — variable-length 32-bit integer wrapper for Marshal wire ops.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. `fn new(v)` lowers to a static
// `v32::new_(v)` factory.
//
// Behavioral diffs from the original C++ class:
//   * No default constructor — callers that previously default-
//     constructed (`v32 v_err;`) now write `v32 v_err = {}` (value-
//     init zero-fills the aggregate) or `v32::new_(0)` explicitly.
//   * The single-arg ctor (`v32(123)`) keeps compiling via C++20
//     aggregate paren-init (P0960), which binds the arg to
//     `val_field`.
//   * `set()` becomes `&mut self` (Rust-idiomatic) and stays non-const
//     on the C++ side. `get()` and `val_size()` stay `const`.
//   * Field renamed `val_` → `val_field` (cosmetic; private in the
//     legacy class, public in the DSL-emitted aggregate, but no
//     callers reach into them — `get()`/`set()` is the public API).
#if RUSTYCPP_RUST
struct v32 {
    val_field: i32,
}

impl v32 {
    fn new(v: i32) -> v32 { v32 { val_field: v } }
    fn set(&mut self, v: i32) { self.val_field = v; }
    fn get(&self) -> i32 { self.val_field }
    fn val_size(&self) -> usize { SparseInt::val_size(self.val_field as i64) }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=basetypes.4 version=1 rust_sha256=3ef01e6050afc2b59a98ef8361930b1cb4d4cbb1345d73481ad776eeeb410641*/
struct v32;

struct v32 {
    int32_t val_field;

    static v32 new_(int32_t v);
    void set(int32_t v);
    int32_t get() const;
    size_t val_size() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


v32 v32::new_(int32_t v) {
    return v32{.val_field = std::move(v)};
}

void v32::set(int32_t v) {
    this->val_field = std::move(v);
}

int32_t v32::get() const {
    return this->val_field;
}

size_t v32::val_size() const {
    return SparseInt::val_size(static_cast<int64_t>(this->val_field));
}
/*RUSTYCPP:GEN-END id=basetypes.4*/

// `v64` — variable-length 64-bit integer wrapper for Marshal wire ops.
// Same DSL pattern + behavioral diffs as `v32` (see above), just at
// 64-bit width.
#if RUSTYCPP_RUST
struct v64 {
    val_field: i64,
}

impl v64 {
    fn new(v: i64) -> v64 { v64 { val_field: v } }
    fn set(&mut self, v: i64) { self.val_field = v; }
    fn get(&self) -> i64 { self.val_field }
    fn val_size(&self) -> usize { SparseInt::val_size(self.val_field) }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=basetypes.5 version=1 rust_sha256=b79e4b80239015966c8c9d9c9c3cffc923aab52fb8b1c3c28bbba0b2188818bd*/
struct v64;

struct v64 {
    int64_t val_field;

    static v64 new_(int64_t v);
    void set(int64_t v);
    int64_t get() const;
    size_t val_size() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


v64 v64::new_(int64_t v) {
    return v64{.val_field = std::move(v)};
}

void v64::set(int64_t v) {
    this->val_field = std::move(v);
}

int64_t v64::get() const {
    return this->val_field;
}

size_t v64::val_size() const {
    return SparseInt::val_size(this->val_field);
}
/*RUSTYCPP:GEN-END id=basetypes.5*/


// `Counter` — atomic-backed monotonically-increasing counter.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. `fn new(start)` lowers to a
// static `Counter::new_(start)` factory.
//
// Behavioral diffs from the original C++ class:
//   * No default constructor — callers that previously default-
//     constructed (`Counter xid_counter_;`, `static Counter g_nop_counter;`)
//     now write `Counter::new_(0)` explicitly.
//   * `next()` and `reset()` no longer carry default arguments —
//     callers write `next(1)` / `reset(0)` explicitly.
//   * `next` is now `&self` (const-qualified) since the underlying
//     atomic provides interior mutability — same change as SpinLock.
//   * No more inheritance from `NoCopy`; DSL structs are non-copyable
//     by default (move-only) which matches what `NoCopy` provided.
#if RUSTYCPP_RUST
struct Counter {
    next_field: AtomicI64,
}

impl Counter {
    fn new(start: i64) -> Counter {
        Counter { next_field: AtomicI64::new(start) }
    }

    fn peek_next(&self) -> i64 {
        self.next_field.load(Ordering::Relaxed)
    }

    fn next(&self, step: i64) -> i64 {
        self.next_field.fetch_add(step, Ordering::AcqRel)
    }

    fn reset(&self, start: i64) {
        self.next_field.store(start, Ordering::Relaxed);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=basetypes.1 version=1 rust_sha256=f1d8fa5f0444e6e132967de2255c0a516a65acd4aaaaa581d3c1bc9b4dbe5c02*/
struct Counter;

struct Counter {
    rusty::sync::atomic::AtomicI64 next_field;

    static Counter new_(int64_t start);
    int64_t peek_next() const;
    int64_t next(int64_t step) const;
    void reset(int64_t start) const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


Counter Counter::new_(int64_t start) {
    return Counter{.next_field = AtomicI64::new_(std::move(start))};
}

int64_t Counter::peek_next() const {
    return this->next_field.load(Ordering::Relaxed);
}

int64_t Counter::next(int64_t step) const {
    return this->next_field.fetch_add(std::move(step), Ordering::AcqRel);
}

void Counter::reset(int64_t start) const {
    this->next_field.store(std::move(start), Ordering::Relaxed);
}
/*RUSTYCPP:GEN-END id=basetypes.1*/

// `RRR_USEC_PER_SEC` was a class-static const on `Time`. DSL impl
// blocks don't model associated constants today, so the constant
// moves to a free `inline constexpr` at namespace scope. The 2
// callers (fiber.cpp, fiber_test.cc) migrate from
// `Time::RRR_USEC_PER_SEC` to bare `RRR_USEC_PER_SEC` (or
// `rrr::RRR_USEC_PER_SEC` from outside the namespace).
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block with the C++ constexpr.
#if RUSTYCPP_RUST
const RRR_USEC_PER_SEC: u64 = 1000000u64;
#endif
/*RUSTYCPP:GEN-BEGIN id=basetypes.usec_per_sec version=1 rust_sha256=fdcd10a701ceea804c90d4007d1b8fd274b61b7daade45fc6388979c7afb3fb2*/
constexpr uint64_t RRR_USEC_PER_SEC = static_cast<uint64_t>(1000000);
/*RUSTYCPP:GEN-END id=basetypes.usec_per_sec*/

// @safe - precondition check that aborts on failure. Defined outside
// the DSL because `verify()` lives in `rrr.debugging` which imports
// basetypes (would be circular). Same shape, different name.
#if RUSTYCPP_RUST
fn abort_if_false(cond: bool) {
    if !cond {
        unsafe { abort(); }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=basetypes.6 version=1 rust_sha256=222bd5266a4084da249eec86eb678b5e37fddea9991e41adf9831006b4c8ce72*/
void abort_if_false(bool cond);

void abort_if_false(bool cond) {
    if (!cond) {
        // @unsafe
        {
            abort();
        }
    }
}
/*RUSTYCPP:GEN-END id=basetypes.6*/

// @safe - architecture-conditional wall-clock helper. Defined outside
// the DSL because the DSL has no preprocessor / `cfg!` support; the
// `#ifdef __APPLE__` branch maps to a single dispatch on Linux.
// @unsafe - PLATFORM #ifdef SPLIT (__APPLE__). The DSL has no
// preprocessor conditionals, so this stays a C++ kernel.
inline uint64_t time_now_us(bool accurate) {
#ifdef __APPLE__
    (void)accurate;
    return rusty::sys::time::clock_realtime_us();
#else
    return accurate ? rusty::sys::time::clock_monotonic_us()
                    : rusty::sys::time::clock_realtime_coarse_us();
#endif
}

// `Time` — wall-clock + sleep facade. The historical class was static-
// only; the DSL form is an empty struct with associated functions.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// Behavioral diff vs the pre-DSL form:
//   * `now()` no longer carries a default arg; callers explicitly
//     write `now(false)` for coarse wall-clock and `now(true)` for
//     monotonic. 15 no-arg sites updated.
//   * `RRR_USEC_PER_SEC` is now a namespace-level free constant
//     (see above), not a class-static member.
#if RUSTYCPP_RUST
struct Time {}

impl Time {
    fn now(accurate: bool) -> u64 {
        time_now_us(accurate)
    }

    fn sleep(t: u64) {
        rusty::sys::time::sleep_us(t);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=basetypes.2 version=1 rust_sha256=083eafe4dc21a130d6016d56995496d81632d3839d29622e2d0ec49e43ea890d*/
struct Time;

struct Time {

    static uint64_t now(bool accurate);
    static void sleep(uint64_t t);
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


uint64_t Time::now(bool accurate) {
    return time_now_us(std::move(accurate));
}

void Time::sleep(uint64_t t) {
    rusty::sys::time::sleep_us(std::move(t));
}
/*RUSTYCPP:GEN-END id=basetypes.2*/

// `Timer` — wall-clock stopwatch. Pre-DSL stored two `struct timeval`
// fields and converted to/from `uint64_t` microseconds on every read
// and write. The DSL form stores `u64` microseconds directly (0 = not
// set), which is what the C++ impl was doing under the hood anyway.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// Behavioral diff vs the pre-DSL form:
//   * No default constructor — callers move from `Timer t;` to
//     `auto t = Timer::new();`.
//   * `elapsed()` aborts (via `verify(false)`) if start() was never
//     called, matching the pre-DSL `std::abort()` semantics.
#if RUSTYCPP_RUST
struct Timer {
    begin_us: u64,
    end_us: u64,
}

impl Timer {
    fn new() -> Timer {
        Timer { begin_us: 0u64, end_us: 0u64 }
    }

    fn start(&mut self) {
        self.begin_us = rusty::sys::time::gettimeofday_us();
        self.end_us = 0u64;
    }

    fn stop(&mut self) {
        self.end_us = rusty::sys::time::gettimeofday_us();
    }

    fn reset(&mut self) {
        self.begin_us = 0u64;
        self.end_us = 0u64;
    }

    fn elapsed(&self) -> f64 {
        abort_if_false(self.begin_us != 0u64);
        let end: u64 = if self.end_us == 0u64 {
            rusty::sys::time::gettimeofday_us()
        } else {
            self.end_us
        };
        ((end - self.begin_us) as f64) / 1000000.0f64
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=basetypes.3 version=1 rust_sha256=09ec6064726af792d141ac2b60837dc2b58a3b91b6a6d23f41084d6ed843e5cd*/
struct Timer;

struct Timer {
    uint64_t begin_us;
    uint64_t end_us;

    static Timer new_();
    void start();
    void stop();
    void reset();
    double elapsed() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


Timer Timer::new_() {
    return Timer{.begin_us = static_cast<uint64_t>(0), .end_us = static_cast<uint64_t>(0)};
}

void Timer::start() {
    this->begin_us = rusty::sys::time::gettimeofday_us();
    this->end_us = static_cast<uint64_t>(0);
}

void Timer::stop() {
    this->end_us = rusty::sys::time::gettimeofday_us();
}

void Timer::reset() {
    this->begin_us = static_cast<uint64_t>(0);
    this->end_us = static_cast<uint64_t>(0);
}

double Timer::elapsed() const {
    abort_if_false(rusty::detail::deref_if_pointer_like(this->begin_us) != static_cast<uint64_t>(0));
    const uint64_t end = (rusty::detail::deref_if_pointer_like(this->end_us) == static_cast<uint64_t>(0) ? rusty::sys::time::gettimeofday_us() : this->end_us);
    return ((static_cast<double>((rusty::detail::deref_if_pointer_like(end) - rusty::detail::deref_if_pointer_like(this->begin_us))))) / 1000000.0;
}
/*RUSTYCPP:GEN-END id=basetypes.3*/

} // export namespace rrr

