module;

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

export module rrr:utils.hostname;



export inline char* gethostip(const char* hostname) {
    struct hostent* h = gethostbyname(hostname);
    if (h == nullptr) {
        return nullptr;
    }
    char* buf = static_cast<char*>(std::malloc(100));
    inet_ntop(h->h_addrtype, *h->h_addr_list, buf, 100);
    return buf;
}
