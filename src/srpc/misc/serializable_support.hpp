#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

extern "C" {
void srpc_fd_write_all(int fd, const void* pointer, std::size_t length);
std::size_t srpc_fd_read_upto(int fd, void* pointer, std::size_t length);
}

// C++-only support for canonical `srpc.serializable`.  These helpers model
// operations whose Rust spelling is intentionally an inert rustc facade:
// open-set ADL dispatch and polymorphic Arc construction.  They are templates,
// so no extra provider ABI or C shim is introduced.
namespace rusty {
template <typename T>
class Arc;
template <typename Signature>
class Function;

namespace srpc_adl_detail {

// Poison ordinary lookup.  Only an overload in a payload/archive associated
// namespace can satisfy the dependent call below.
void serialize();
void deserialize();

template <typename T, typename Archive>
decltype(auto) call_serialize(const T& value, Archive& archive) {
    return serialize(value, archive);
}

template <typename T, typename Archive>
decltype(auto) call_deserialize(T& value, Archive& archive) {
    return deserialize(value, archive);
}

}  // namespace srpc_adl_detail

template <typename T, typename Archive>
void srpc_adl_serialize(const T& value, Archive& archive) {
    srpc_adl_detail::call_serialize(value, archive);
}

template <typename T, typename Archive>
void srpc_adl_deserialize(T& value, Archive& archive) {
    srpc_adl_detail::call_deserialize(value, archive);
}

template <typename Sink>
void srpc_sink_write(Sink& sink, const unsigned char* pointer, std::size_t length) {
    sink.write_bytes(pointer, length);
}

template <typename Source>
std::size_t srpc_source_read(Source& source, unsigned char* pointer, std::size_t length) {
    return source.read_bytes(pointer, length);
}

template <typename T>
Arc<T> srpc_arc_default() {
    return Arc<T>::make();
}

template <typename T>
Arc<T> srpc_arc_copy(const T& value) {
    return Arc<T>::make(value);
}

template <typename Holder, typename T>
Arc<Holder> srpc_holder_proxy(Arc<T> value) {
    return Arc<Holder>::make(std::move(value));
}

template <typename Callable>
auto srpc_factory_from_callable(Callable&& callable) {
    using C = std::remove_cvref_t<Callable>;
    using R = std::invoke_result_t<C&>;
    return Function<R()>(std::forward<Callable>(callable));
}

}  // namespace rusty
