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

namespace rrr {

size_t blocking_write(int fd, const void* p, size_t len);

int set_nonblocking(int fd, bool nonblocking);

int find_open_port();

std::string get_host_name();

} // namespace rrr
