#include <stdio.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>

#include "misc.hpp"

// External safety annotations for system functions used in this module
// @external: {
//   sysconf: [unsafe, (int) -> long]
//   readlink: [unsafe, (const char*, char*, size_t) -> ssize_t]
//   snprintf: [unsafe, (char*, size_t, const char*, ...) -> int]
//   getpid: [unsafe, () -> pid_t]
//   std::__atomic_base::load: [unsafe]
//   std::__atomic_base::store: [unsafe]
//   std::__atomic_base::fetch_add: [unsafe]
//   std::__atomic_base::fetch_sub: [unsafe]
// }

namespace rrr {

// @unsafe - Direct pointer manipulation for performance
// SAFETY: Caller must ensure str points to buffer with at least 'digits' bytes
static void make_int(char* str, int val, int digits) {
    char* p = str + digits;
    for (int i = 0; i < digits; i++) {
        int d = val % 10;
        val /= 10;
        p--;
        *p = '0' + d;
    }
}

// format time
// inspired by the TPC-C benchmark from Evan Jones
// strftime is slow because it ends up consulting timezone info
// also, snprintf is slow
// @unsafe - Direct buffer manipulation for performance
// SAFETY: Caller must ensure 'now' points to buffer with at least TIME_NOW_STR_SIZE bytes
void time_now_str(char* now) {
    time_t seconds_since_epoch = time(nullptr);
    struct tm local_calendar;
    localtime_r(&seconds_since_epoch, &local_calendar);
    make_int(now, local_calendar.tm_year + 1900, 4);
    now[4] = '-';
    make_int(now + 5, local_calendar.tm_mon + 1, 2);
    now[7] = '-';
    make_int(now + 8, local_calendar.tm_mday, 2);
    now[10] = ' ';
    make_int(now + 11, local_calendar.tm_hour, 2);
    now[13] = ':';
    make_int(now + 14, local_calendar.tm_min, 2);
    now[16] = ':';
    make_int(now + 17, local_calendar.tm_sec, 2);
    now[19] = '.';
    timeval tv;
    gettimeofday(&tv, nullptr);
    make_int(now + 20, tv.tv_usec / 1000, 3);
    now[23] = '\0';
}

// @unsafe - Calls sysconf (external unsafe)
// SAFETY: sysconf is POSIX-compliant, thread-safe
int get_ncpu() {
    return sysconf(_SC_NPROCESSORS_ONLN);
}

// @unsafe - Calls readlink, snprintf, getpid (external unsafe)
// SAFETY: Returns static buffer, thread-safe after first call
const char* get_exec_path() {
    static char path[PATH_MAX];
    static bool ready = false;
    if (!ready) {
        char link[PATH_MAX];
        snprintf(link, sizeof(link), "/proc/%d/exe", getpid());
        int ret = readlink(link, path, sizeof(path));
        if (ret != -1) {
            path[ret] = '\0';
            ready = true;
        } else {
            return nullptr;
        }
    }
    return path;
}

// @unsafe - Uses raw FILE* and manual memory management
// SAFETY: FILE* must be valid; getdelim allocates buffer that we free
std::string getline(FILE* fp, char delim /* =? */) {
    char* buf = nullptr;
    size_t n = 0;
    ssize_t n_read = ::getdelim(&buf, &n, delim, fp);
    if (n_read > 0 && buf[n_read - 1] == delim) {
        n_read--;
    }
    std::string line(buf, n_read);
    free(buf);
    return line;
}

} // namespace base
