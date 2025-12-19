#include <utility>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


#include "base/all.hpp"
#include "utils.hpp"

// External safety annotations for atomic operations
// @external: {
//   std::__atomic_base::load: [unsafe]
//   std::__atomic_base::store: [unsafe]
//   std::__atomic_base::fetch_add: [unsafe]
//   std::__atomic_base::fetch_sub: [unsafe]
// }


using namespace std;

namespace rrr {

// @unsafe - Calls fcntl (external unsafe)
// SAFETY: fcntl is POSIX-compliant, fd must be valid
int set_nonblocking(int fd, bool nonblocking) {
    int ret = fcntl(fd, F_GETFL, 0);
    if (ret != -1) {
        if (nonblocking) {
            ret = fcntl(fd, F_SETFL, ret | O_NONBLOCK);
        } else {
            ret = fcntl(fd, F_SETFL, ret & ~O_NONBLOCK);
        }
    }
    return ret;
}

// @unsafe - Uses address-of operations for socket functions
// SAFETY: All pointers remain valid throughout function scope
int find_open_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    addrinfo *local_addr;

    addrinfo hints;
    bzero(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo("0.0.0.0", nullptr, nullptr, &local_addr) != 0) {
        Log_error("Failed to getaddrinfo");
        return -1;
    }

    int port = -1;

    for (int i = 1024; i < 65000; ++i) {
        ((sockaddr_in*)local_addr->ai_addr)->sin_port = i;
        if (::bind(fd, local_addr->ai_addr, local_addr->ai_addrlen) != 0) {
            continue;
        }

        sockaddr_in addr;
        socklen_t addrlen;
        memset(&addr, 0, sizeof(addr));
        if (getsockname(fd, (sockaddr*)&addr, &addrlen) != 0) {
            Log_error("Failed to get socket address");
            return -1;
        }

        port = i;
        break;
    }

    freeaddrinfo(local_addr);
    ::close(fd);

    if (port != -1) {
        Log_info("Found open port: %d", port);
        return port;
    }

    Log_error("Failed to find open port.");
    return -1;
}

// @unsafe - Calls unsafe Log::error on failure
// SAFETY: Buffer is properly sized, error logging is thread-safe
std::string get_host_name() {
    char buffer[1024];
    if (gethostname(buffer, 1024) != 0) {
        Log_error("Failed to get hostname.");
        return "";
    }
    return std::string(buffer);
}

} // namespace rrr
