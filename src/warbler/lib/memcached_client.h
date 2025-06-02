#ifndef _LIB_MEM_CLIENT_H_
#define _LIB_MEM_CLIENT_H_
#include <iostream>
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <libmemcached/memcached.h>
#include <iostream>
#include <time.h>
#include <vector>

class MemCachedClient {
public:
    ~MemCachedClient() {
        memcached_free(memc);
    };

    MemCachedClient(const char *host, int port) {
        memcached_return rc;
        memcached_server_st *server = NULL;

        memc = memcached_create(NULL);
        server = memcached_server_list_append(server, host, port, &rc);
        rc = memcached_server_push(memc, server);
        if (MEMCACHED_SUCCESS != rc) {
            std::cout << "memcached_server_push failed! rc: " << rc << std::endl;
        }
        memcached_server_list_free(server);
    };

    // expiration: seconds
    int Insert(const char *key, const char *value, time_t expiration = 30) {
        if (NULL == key || NULL == value) {
            return -1;
        }

        uint32_t flags = 0;
        memcached_return rc;
        rc = memcached_set(memc, key, strlen(key), value, strlen(value), expiration, flags);

        // insert ok
        if (MEMCACHED_SUCCESS == rc) {
            return 0;
        } else {
            return 1;
        }
    };

    std::string Get(const char *key) {
        if (NULL == key) {
            return "";
        }

        uint32_t flags = 0;
        memcached_return rc;
        size_t value_length;
        char *value = memcached_get(memc, key, strlen(key), &value_length, &flags, &rc);

        // get ok
        if (rc == MEMCACHED_SUCCESS) {
            return value;
        }

        return "";
    };

private:
    memcached_st *memc;
};

#endif