#pragma once
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <rusty/box.hpp>
#include <rusty/thread.hpp>
#include <rusty/mutex.hpp>
#include <rusty/once.hpp>
#include <rusty/option.hpp>

// Reserve without constructing elements, preserving each tree test's workload.
template<class T> std::vector<T> reserved_vector(std::size_t capacity) {
    std::vector<T> result;
    result.reserve(capacity);
    return result;
}
