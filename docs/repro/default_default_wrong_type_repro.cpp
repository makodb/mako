// Repro: `Default::default()` emits `rusty::default_like<int32_t>()` -- the
// WRONG TYPE -- instead of the turbofish'd type, in a `#[cpp_ctor]` whose
// parameter is stored into a field, when there is more than one
// `Default::default()` field.
//
// Expected for both fields:
//     rusty::Mutex<std::vector<uint8_t>>::new_(rusty::default_like<std::vector<uint8_t>>())
//     rusty::Mutex<Cb>::new_(rusty::default_like<Cb>())
// Actual: `rusty::default_like<int32_t>()` for BOTH -- i32 being the type of
// the `fd` parameter.
//
// Ruled out individually (each of these alone emits correctly):
//   * a `#[cpp_ctor]` with an i32 parameter that is NOT stored;
//   * a `#[cpp_ctor]` with an i32 FIELD but no parameter;
//   * a plain (non-cpp_ctor) fn with the parameter stored;
//   * `#[cpp_ctor]` + parameter stored + exactly ONE Default field;
//   * two Default fields (incl. `std::vector<u8>`) with NO parameter.
// It needs `#[cpp_ctor]` + a stored parameter + at least TWO Default fields,
// which reads like a positional mismatch rather than a type-lookup failure.
//
// Caught in src/srpc/rpc/tcp_channel.cpp (TcpConnection::new takes `fd: i32`);
// TcpListener in the same file takes no parameters and lowers correctly.

#include <rusty/function.hpp>
#include <rusty/mutex.hpp>
#include <vector>
using Cb = rusty::Function<void(int)>;

#if RUSTYCPP_RUST
struct DP2 {
    n: i32,
    v: rusty::Mutex<std::vector<u8>>,
    m: rusty::Mutex<Cb>,
}
impl DP2 {
    #[cpp_ctor] fn new(fd: i32) -> DP2 {
        DP2 {
            n: fd,
            v: rusty::Mutex::<std::vector<u8>>::new(Default::default()),
            m: rusty::Mutex::<Cb>::new(Default::default()),
        }
    }
}
#endif
