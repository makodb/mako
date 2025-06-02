//
// Created by weihshen on 3/29/21.
//

#ifndef SILO_STO_COMMON_H
#define SILO_STO_COMMON_H
#include <iostream>
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <thread>
#include "lib/memcached_client.h"

class HashWrapper {
    public:
    
    std::map<std::string, int> data;
    
    void set_tprops(std::string k, int v) {
        data[k] = v;
    }

    int get_tprops(std::string k) {
        if (data.find(k) != data.end()) {
            return data[k];
        } else {
            return -1;
        }
    }
} ;

// the simplest memcached implementation with socket
// running on the shard-0 on the leader datacenter
namespace srolis {
    class Memcached
    {
    public:
        static int set_key(std::string kk, const char *value, const char*host, int port) {
            MemCachedClient *mc2 = new MemCachedClient(host, port);
            int r = mc2->Insert(kk.c_str(), value);
            delete mc2;
            if (r!=0) { std::cout << "memClient can't insert a key:" << host << ", port:" << port << std::endl; }
            return r;
        }

        static void wait_for_key(std::string kk, const char*host, int port) {
            MemCachedClient *mc2 = new MemCachedClient(host, port);
            Warning("wait a key:%s from host:%s, port:%d", kk.c_str(), host, port);
            while (1) {
                if (mc2->Get(kk.c_str()).compare("") == 0) {
                    usleep(0);
                } else {
                    break;
                }
            }
            delete mc2;
        }

        static string get_key(std::string kk, const char*host, int port) {
            MemCachedClient *mc2 = new MemCachedClient(host, port);
            std::string v = mc2->Get(kk.c_str());
            delete mc2;
            return v;
        }
    };
}

#endif //SILO_STO_COMMON_H