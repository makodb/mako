// std_compat.hpp — `import std;`, ordered after textual STL.
//
// Clang 19 + libc++ 19 reject the order
//
//     import std;
//     #include <vector>
//
// because the importer's ODR check fires before the deserializer
// sees the textually-included copy and rejects libc++ internal
// helpers like `__synth_three_way` as redeclared with a different
// type. The reverse order works: `#include <vector>` first lets the
// deserializer merge the symbols when `import std;` lands. (See
// llvm-project issue #61465; libc++ documents this in
// `<https://libcxx.llvm.org/Modules.html>`.)
//
// This aggregate therefore textually pulls in the full surface area
// the codebase uses BEFORE importing the std module. Everything
// reachable through `<std_compat.hpp>` sees both:
//   * the textual declarations (so transitive `#include <vector>`
//     from rusty-cpp/yaml-cpp/etc. is a no-op via the header guard);
//   * the imported module names (so `std::*` resolves through the
//     module path and `import std;` is available for hand-written
//     code that wants to spell it that way).
#pragma once

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <bitset>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cfloat>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <codecvt>
#include <compare>
#include <complex>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <exception>
#include <execution>
#include <filesystem>
#include <forward_list>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <iomanip>
#include <ios>
#include <iosfwd>
#include <iostream>
#include <istream>
#include <iterator>
#include <latch>
#include <limits>
#include <list>
#include <locale>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <numbers>
#include <numeric>
#include <optional>
#include <ostream>
#include <queue>
#include <random>
#include <ranges>
#include <ratio>
#include <regex>
#include <scoped_allocator>
#include <semaphore>
#include <set>
#include <shared_mutex>
#include <source_location>
#include <span>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <stop_token>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <valarray>
#include <variant>
#include <vector>
#include <version>

import std;
