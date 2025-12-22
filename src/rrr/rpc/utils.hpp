#pragma once

#include <list>
#include <map>
#include <unordered_map>
#include <functional>

#include <sys/types.h>
#include <sys/time.h>
#include <stdarg.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <inttypes.h>

// External annotations for system functions that cannot have in-place annotations
// Note: Log functions now have in-place annotations in logging.hpp
// @external: {
//   fcntl: [unsafe, (int, int, ...) -> int]
//   socket: [unsafe, (int, int, int) -> int]
//   bind: [unsafe, (int, const sockaddr*, socklen_t) -> int]
//   getsockname: [unsafe, (int, sockaddr*, socklen_t*) -> int]
//   gethostname: [unsafe, (char*, size_t) -> int]
//   bzero: [unsafe, (void*, size_t) -> void]
//   memset: [unsafe, (void*, int, size_t) -> void*]
//   getaddrinfo: [unsafe, (const char*, const char*, const addrinfo*, addrinfo**) -> int]
//   freeaddrinfo: [unsafe, (addrinfo*) -> void]
//   close: [unsafe, (int) -> int]
// }

namespace rrr {

// @unsafe - Calls fcntl (external unsafe)
int set_nonblocking(int fd, bool nonblocking);

// @unsafe - Uses address-of operations for socket functions
int find_open_port();

// @unsafe - Calls unsafe Log::error on failure
std::string get_host_name();

} // namespace rrr
