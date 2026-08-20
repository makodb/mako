/* srpc_net.c — pure-syscall network helpers as plain C (Goal-0 C
 * demotion). Contract mirrors srpc_connect.c: no C++ types cross this
 * boundary; logging stays in the C++ shims.
 */

#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>

/* Scan 1024..64999 for a locally bindable TCP port. Returns the port,
 * or -1 on socket/resolve failure, or 0 if no port was free. The scan
 * formerly lived in rpc/utils.cpp; canonical src/rrr/src/utils.rs now calls
 * this kernel. AddrInfo RAII is unnecessary here because getaddrinfo and
 * freeaddrinfo pair locally. */
int srpc_find_open_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct addrinfo* local = NULL;
    if (getaddrinfo("0.0.0.0", NULL, NULL, &local) != 0 || local == NULL) {
        close(fd);
        return -1;
    }
    int port = 0;
    for (int i = 1024; i < 65000; ++i) {
        ((struct sockaddr_in*)local->ai_addr)->sin_port = (unsigned short)i;
        if (bind(fd, local->ai_addr, local->ai_addrlen) != 0) {
            continue;
        }
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        memset(&addr, 0, sizeof(addr));
        if (getsockname(fd, (struct sockaddr*)&addr, &addrlen) != 0) {
            port = -1;
        } else {
            port = i;
        }
        break;
    }
    freeaddrinfo(local);
    close(fd);
    return port;
}
