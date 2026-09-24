#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" {
std::int64_t srpc_fd_write_once(int fd, const void* pointer, std::size_t length);
std::int64_t srpc_fd_read_once(int fd, void* pointer, std::size_t length);
std::int32_t srpc_fd_last_errno();
std::int32_t srpc_fd_interrupted_errno();
}

// C++ ADL call adapters. The Rust counterparts forward into canonical traits;
// serialization and payload ownership live in those traits.
namespace rusty {
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

}  // namespace rusty

// These overloads belong to the global module fragment. Every call to a
// canonical helper depends on Archive, so ADL resolves its exported definition
// after srpc.serializable declares it. No container is converted or copied.
namespace rusty {
template<class T> inline constexpr bool srpc_stl_sequence = false;
template<class T, class A> inline constexpr bool srpc_stl_sequence<std::vector<T, A>> = true;
template<class T, class A> inline constexpr bool srpc_stl_sequence<std::list<T, A>> = true;
template<class T, class C, class A> inline constexpr bool srpc_stl_sequence<std::set<T, C, A>> = true;
template<class T, class H, class E, class A> inline constexpr bool srpc_stl_sequence<std::unordered_set<T, H, E, A>> = true;
template<class T> inline constexpr bool srpc_stl_pair = false;
template<class First, class Second> inline constexpr bool srpc_stl_pair<std::pair<First, Second>> = true;
template<class T> inline constexpr bool srpc_stl_map = false;
template<class K, class V, class C, class A> inline constexpr bool srpc_stl_map<std::map<K, V, C, A>> = true;
template<class K, class V, class H, class E, class A> inline constexpr bool srpc_stl_map<std::unordered_map<K, V, H, E, A>> = true;
template<class T> inline constexpr bool srpc_stl_serialize = srpc_stl_sequence<T> || srpc_stl_map<T> || srpc_stl_pair<T>
    || std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>;
template<class T> inline constexpr bool srpc_stl_deserialize = srpc_stl_serialize<T>
    && !std::is_same_v<T, std::string_view>;
}  // namespace rusty

namespace srpc {
template<class Archive>
void serialize(const std::string& value, Archive& archive) {
    serialize_bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size(), archive);
}

template<class Archive>
void serialize(const std::string_view& value, Archive& archive) {
    serialize_bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size(), archive);
}

template<class Archive>
void deserialize(std::string& value, Archive& archive) {
    deserialize_bytes_with(value, archive,
        [](auto& storage, std::size_t count) { storage.resize(count); },
        [](auto& storage) { return reinterpret_cast<std::uint8_t*>(storage.data()); });
}

template<class Pair, class Archive> requires rusty::srpc_stl_pair<Pair>
void serialize(const Pair& value, Archive& archive) {
    serialize_pair_fields(value.first, value.second, archive);
}

template<class Pair, class Archive> requires rusty::srpc_stl_pair<Pair>
void deserialize(Pair& value, Archive& archive) {
    deserialize_pair_fields(value.first, value.second, archive);
}

template<class Container, class Archive> requires rusty::srpc_stl_sequence<Container>
void serialize(const Container& value, Archive& archive) {
    auto iterator = value.begin();
    serialize_counted(value.size(), archive, [&iterator](auto& output) {
        serialize_value(*iterator++, output);
    });
}

template<class Container, class Archive> requires rusty::srpc_stl_map<Container>
void serialize(const Container& value, Archive& archive) {
    auto iterator = value.begin();
    serialize_counted(value.size(), archive, [&iterator](auto& output) {
        serialize_pair_fields(iterator->first, iterator->second, output);
        ++iterator;
    });
}

template<class Container, class Archive> requires rusty::srpc_stl_sequence<Container>
void deserialize(Container& value, Archive& archive) {
    deserialize_counted(value, archive,
        [](auto& storage, std::size_t count) {
            storage.clear();
            if constexpr (requires { storage.reserve(count); }) storage.reserve(count);
        },
        [](auto& storage, auto& input) {
            typename Container::value_type element{};
            deserialize_value(element, input);
            if constexpr (requires { storage.push_back(std::move(element)); })
                storage.push_back(std::move(element));
            else storage.insert(std::move(element));
        });
}

template<class Container, class Archive> requires rusty::srpc_stl_map<Container>
void deserialize(Container& value, Archive& archive) {
    deserialize_map_first<typename Container::key_type, typename Container::mapped_type>(
        value, archive,
        [](auto& storage) { storage.clear(); },
        [](auto& storage, auto key, auto mapped) {
            storage.try_emplace(std::move(key), std::move(mapped));
        });
}
}  // namespace srpc
