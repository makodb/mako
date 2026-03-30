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
#include <iostream>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include "benchmarks/benchmark_config.h"
//#include "lib/memcached_client.h"

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

// in this implementation, we rely on nfs to sync file
// running on the shard-0 on the leader datacenter
namespace mako {
    class NFSSync {
public:
        static int mark_shard_up_and_wait() {
            mako::NFSSync::mark_current_shard_up() ;
            mako::NFSSync::wait_for_all_up() ;
            return 0;
        }

        static int mark_current_shard_up() {
            auto& cfg = BenchmarkConfig::getInstance();
            std::string filename = std::string("nfs_sync_") + std::to_string(cfg.getShardIndex()) ;
            std::ofstream outfile(filename);
            if (!outfile) {
                std::cerr << "Failed to open file for writing: " << filename << std::endl;
                return 1;
            }
            outfile << "DONE";
            return 0;
        }

        static void wait_for_all_up() {
            auto& cfg = BenchmarkConfig::getInstance();
            for (int i=0; i<cfg.getConfig()->nshards; i++) {
                std::string filename = std::string("nfs_sync_") + std::to_string(i) ;
                while (1) {
                    std::ifstream infile(filename);
                    if (infile) {
                        std::cout << "shard:" << i << " up..." << std::endl;
                        break;
                    }
                    usleep(0);
                }
            }
            std::cout << "wait_for_all_up setup finish!" << std::endl;
        }

        static int set_key(std::string kk, const char *value, const char*host, int port) {
            std::string filename = std::string("nfs_sync_") + host + "_" + std::to_string(port) + "_" + kk;
            std::ofstream outfile(filename);
            if (!outfile) {
                std::cerr << "Failed to open file for writing: " << filename << std::endl;
                return 1;
            }
            outfile << value;
            return 0;
        }

        static void wait_for_key(std::string kk, const char*host, int port) {
            std::string filename = std::string("nfs_sync_") + host + "_" + std::to_string(port) + "_" + kk;
            while (1) {
                std::ifstream infile(filename);
                if (infile) {
                    break;
                }
                usleep(0);
            }
        }

        static string get_key(std::string kk, const char*host, int port) {
            std::string filename = std::string("nfs_sync_") + host + "_" + std::to_string(port) + "_" + kk;
            std::ifstream infile(filename);
            return std::string((std::istreambuf_iterator<char>(infile)),
                            std::istreambuf_iterator<char>());
        }
    };
}


/*
namespace mako {
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
*/

#endif //SILO_STO_COMMON_H
