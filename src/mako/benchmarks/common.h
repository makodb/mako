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
#include <array>
#include <cerrno>
#include <filesystem>
#include <random>
#include <utility>
#include "benchmarks/benchmark_config.h"
#include "lib/message.h"
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
        // @unsafe - Same shared namespace as the existing cross-host load barrier.
        static std::string drain_prefix(const BenchmarkConfig &config) {
            const auto *topology = config.getConfig();
            return "nfs_sync_" + topology->shard(0, mako::LOCALHOST_CENTER_INT).host +
                "_" + std::to_string(topology->mports.at(mako::LOCALHOST_CENTER_INT)) +
                "_drain_";
        }

        // Filesystem kernel shared by coordinator and replica shutdown.
        // Acknowledgements bind BOTH fresh process nonces. A previous run's
        // completion cannot authorize closing a current process's services.
        class DrainHandshake {
            using Nonce = std::array<uint64_t, 2>;
            Nonce nonce_{};
            std::string prefix_;
            std::string identity_;
            std::string challenge_;

            // @unsafe - Each path has one writer; rename publishes a whole record.
            template<size_t N>
            static void write_record(const std::string &path,
                                     const std::array<uint64_t, N> &record) {
                const std::string temporary = path + ".tmp";
                std::ofstream output(temporary, std::ios::trunc);
                for (uint64_t value : record)
                    output << value << '\n';
                output.close();
                if (!output)
                    Panic("cannot publish drain record: %s", path.c_str());
                std::error_code error;
                std::filesystem::rename(temporary, path, error);
                if (error)
                    Panic("cannot publish drain record %s: %s",
                          path.c_str(), error.message().c_str());
            }

            // @unsafe - Absence means not ready; malformed/I/O failures fail closed.
            template<size_t N>
            static bool read_record(const std::string &path,
                                    std::array<uint64_t, N> &record) {
                errno = 0;
                std::ifstream input(path);
                if (!input.is_open()) {
                    if (errno == ENOENT)
                        return false;
                    Panic("cannot read drain record: %s", path.c_str());
                }
                for (uint64_t &value : record) {
                    if (!(input >> value))
                        Panic("invalid drain record: %s", path.c_str());
                }
                return true;
            }

        public:
            struct Peer {
                Nonce nonce{};
                std::string challenge;
                std::string publish_ack;
                std::string receive_ack;
            };

            // @unsafe - Announce before admitting any work for this incarnation.
            void announce(std::string prefix, std::string identity) {
                prefix_ = std::move(prefix);
                identity_ = std::move(identity);
                challenge_ = prefix_ + identity_;
                std::random_device random;
                do {
                    for (uint64_t &word : nonce_)
                        word = (uint64_t(random()) << 32) | uint64_t(random());
                } while (nonce_ == Nonce{});
                write_record(challenge_, nonce_);
            }

            // @safe - Bind a peer identity; a role change invalidates the cached nonce.
            void bind(Peer &peer, const std::string &identity) const {
                peer.nonce = {};
                peer.challenge = prefix_ + identity;
                peer.publish_ack = peer.challenge + ".ack_" + identity_;
                peer.receive_ack = challenge_ + ".ack_" + identity;
            }

            // @unsafe - Call only AFTER local work is completely drained.
            bool acknowledge(Peer &peer) const {
                Nonce current{};
                if (!read_record(peer.challenge, current))
                    return false;
                if (peer.nonce != current) {
                    peer.nonce = current;
                    write_record(peer.publish_ack, std::array<uint64_t, 4>{
                        current[0], current[1], nonce_[0], nonce_[1]});
                }
                std::array<uint64_t, 4> acknowledgement{};
                return read_record(peer.receive_ack, acknowledgement) &&
                    acknowledgement == std::array<uint64_t, 4>{
                        nonce_[0], nonce_[1], current[0], current[1]};
            }
        };

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
