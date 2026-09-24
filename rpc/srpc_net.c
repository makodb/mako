/* Linux socket layouts and individual operations for rpc/utils.rs. */
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>

int srpc_net_socket_open(void) {
    return socket(AF_INET, SOCK_STREAM, 0);
}

struct addrinfo* srpc_net_resolve_any(void) {
    struct addrinfo* address = NULL;
    if (getaddrinfo("0.0.0.0", NULL, NULL, &address) != 0) {
        return NULL;
    }
    return address;
}

int srpc_net_bind_port(int fd, struct addrinfo* address, unsigned short port_native) {
    /* Preserve the historical scan's raw sin_port representation. */
    ((struct sockaddr_in*)address->ai_addr)->sin_port = port_native;
    return bind(fd, address->ai_addr, address->ai_addrlen);
}

int srpc_net_socket_name_status(int fd) {
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    memset(&address, 0, sizeof(address));
    return getsockname(fd, (struct sockaddr*)&address, &length);
}

int srpc_net_close(int fd) {
    return close(fd);
}

int srpc_net_hostname(unsigned char* buffer, size_t capacity) {
    return gethostname((char*)buffer, capacity);
}
