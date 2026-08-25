#ifndef _LIB_COMMON_H_
#define _LIB_COMMON_H_

#include "lib/timestamp.h"

#include <iostream>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <sys/file.h>
#ifdef MAKO_ENABLE_ERPC
#include "rpc.h"
#endif
#include <mutex>
#include <condition_variable>
#include <stdlib.h>
#include <chrono>
#include <ctime>
#include <string>
#include <sstream>
#include <iomanip>
#include <random>
#include <functional>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

// promise.timeout is abandoned
#define GET_TIMEOUT 250
#define ABORT_TIMEOUT 250
#define BASIC_TIMEOUT 250

// COCO epoch advancing time: xms
#define COCO_ADVANCING_DURATION 10

static void _wan_wait_time() {
  int num = 50;
  std::this_thread::sleep_for(std::chrono::milliseconds(num));
}

#define WAN_WAIT_TIME _wan_wait_time();

namespace mako
{
    // Paxos status codes used for encoding with timestamps
    enum PaxosStatus {
        STATUS_NORMAL = 0,          // Normal/default status
        STATUS_INIT = 1,            // Init/initialization
        STATUS_ENDING = 2,          // Ending of Paxos group
        STATUS_SAFETY_FAIL = 3,     // Can't pass safety check
        STATUS_REPLAY_DONE = 4,     // Complete replay/replay done
        STATUS_NOOPS = 5            // No-ops
    };

    const int ADVANCER_MARKER_NUM = 2;
    const int NUM_TABLES_PER_SHARD = 200; // for pre-allocated

  #if defined(MEGA_BENCHMARK)
    const int mega_batch_size = 100; // no more than max_batch_size?
  #elif defined(MEGA_BENCHMARK_MICRO) 
    const int mega_batch_size = 300; // no more than max_batch_size?
  #else
    const int mega_batch_size = 100; // no more than max_batch_size?
  #endif

    const int size_per_stock_value = 36; // value in stock table

    // ----------
    const std::string LOCALHOST_CENTER = "localhost";
    const std::string LEARNER_CENTER = "learner";
    const std::string P1_CENTER = "p1";
    const std::string P2_CENTER = "p2";
    const int LOCALHOST_CENTER_INT = 0;
    const int LEARNER_CENTER_INT = 1;
    const int P1_CENTER_INT = 2;
    const int P2_CENTER_INT = 3;

    static int convertCluster(std::string cluster) {
        if (cluster == LOCALHOST_CENTER) return LOCALHOST_CENTER_INT;
        else if (cluster == LEARNER_CENTER) return LEARNER_CENTER_INT;
        else if (cluster == P1_CENTER) return P1_CENTER_INT;
        else if (cluster == P2_CENTER) return P2_CENTER_INT;
        else {
            Panic("cluster name is not matched in configuration, got: %s!", cluster.c_str());
            return -1;
        }
    }

    static std::string convertClusterRole(int clusterRole) {
        if (clusterRole==LOCALHOST_CENTER_INT) return LOCALHOST_CENTER;
        else if (clusterRole==LEARNER_CENTER_INT) return LEARNER_CENTER;
        else if (clusterRole==P1_CENTER_INT) return P1_CENTER;
        else if (clusterRole==P2_CENTER_INT) return P2_CENTER;
        else {
            Panic("cluster role is not matched in configuration, got: %d!", clusterRole);
            return "";
        }
    }

    // ----------- several utils
    static std::string printStringAsBit(const char *str, size_t len,std::string prefix="") {
        std::cout << "[" << prefix << "] printStringAsBit:" << std::endl;
        for (size_t i = 0; i < len; ++i) {
            unsigned char c = str[i];
            // snprintf, not `std::hex << setw(2) << setfill('0')`: under
            // clang 22 with `import std` the iomanip manipulators lose
            // their operator<< overloads ("invalid operands ... and
            // '__iom_t6'").
            char hexbuf[8];
            snprintf(hexbuf, sizeof(hexbuf), "%02x ", static_cast<int>(c));
            std::cout << hexbuf;
            
            // Print newline every 8 bytes
            if ((i + 1) % 8 == 0) {
                std::cout << std::endl;
            }
        }
        
        // Print final newline if last line wasn't complete
        if (len % 8 != 0) {
            std::cout << std::endl;
        }

        return std::string(str, len);
    }

    static std::string printStringAsBit(std::string str,std::string prefix="") {
        return printStringAsBit(str.c_str(), str.size(), prefix);
    }

    struct Node {
        uint32_t timestamp;  // Single timestamp instead of vector
        int16_t data_size;
        char *data;
    };

    // TODO: you have to do anyway; data stored here;
    // extra bytes for stored value: actual value + (timestamp, term) + sizeof(node)
    const int EXTRA_BITS_FOR_VALUE = sizeof(uint32_t) + sizeof(struct Node);
    const int BITS_OF_NODE = sizeof(struct Node);
    const int BITS_OF_TT = sizeof(uint32_t);

    // Stored metadata follows arbitrary-length user bytes and is therefore not
    // naturally aligned. Keep all accesses in byte/memcpy form rather than
    // forming misaligned uint32_t* or Node* pointers.
    inline void ResetEncodedNodeState(char* encoded_value, size_t encoded_size) {
        uint32_t timestamp = 0;
        int16_t data_size = 0;
        char* node_bytes = encoded_value + encoded_size - BITS_OF_NODE;
        const char* timestamp_bytes = reinterpret_cast<const char*>(&timestamp);
        for (size_t i = 0; i != sizeof(timestamp); ++i)
            std::atomic_ref<char>(node_bytes[offsetof(Node, timestamp) + i])
                .store(timestamp_bytes[i], std::memory_order_relaxed);
        const char* size_bytes = reinterpret_cast<const char*>(&data_size);
        for (size_t i = 0; i != sizeof(data_size); ++i)
            std::atomic_ref<char>(node_bytes[offsetof(Node, data_size) + i])
                .store(size_bytes[i], std::memory_order_relaxed);
    }

    inline void ResetEncodedMetadata(char* encoded_value, size_t encoded_size) {
        uint32_t time_term = 0;
        char* data = nullptr;
        std::memcpy(encoded_value + encoded_size - EXTRA_BITS_FOR_VALUE,
                    &time_term, sizeof(time_term));
        ResetEncodedNodeState(encoded_value, encoded_size);
        char* node_bytes = encoded_value + encoded_size - BITS_OF_NODE;
        std::memcpy(node_bytes + offsetof(Node, data), &data, sizeof(data));
    }

    // Helper function to encode values with required metadata padding
    inline std::string Encode(const std::string& value) {
        // Create string with exact size needed - single allocation
        std::string encoded_value;
        encoded_value.resize(value.size() + EXTRA_BITS_FOR_VALUE, '\0');

        // Copy the value to the beginning - single memory copy
        std::memcpy(encoded_value.data(), value.data(), value.size());

        ResetEncodedMetadata(encoded_value.data(), encoded_value.size());

        return encoded_value;
    }

    // Generate a random integer
    inline int generateRandomInt() {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<int> dist(0, INT_MAX);
        return dist(gen);
    }

    // --------------------------- for erpc APIs
    const uint8_t getReqType = 1;
    const uint8_t lockReqType = 2;
    const uint8_t validateReqType = 3;
    const uint8_t installReqType = 4;
    const uint8_t unLockReqType = 5;
    const uint8_t abortReqType = 6;
    const uint8_t scanReqType = 7;
    const uint8_t getTimestampReqType = 8;
    const uint8_t serializeUtilReqType = 9;
    const uint8_t batchLockReqType = 10;
    const uint8_t warmupReqType = 11;

    // reserved for the leader data center
    const uint8_t controlReqType = 12;
    // reserved for watermark exchange between follower data center
    const uint8_t watermarkReqType = 13;

    // Self-contained non-transactional ops (Masstree-shape API,
    // docs/storage-interface.md). Server-side handlers run the op
    // as a local one-op OCC transaction on the owning shard (writes
    // replicate through the normal commit path). Wire format:
    // nontxn_write_request_t / client_kv_response_t. Writes return the
    // op's boolean result in value[0] with vlen=1; get returns the
    // stored value bytes (status ABORT = key not found). Unlike
    // getReqType, nontxnGetReqType stages NOTHING in the serving
    // worker's participant transaction — no follow-up 2PC abort/commit
    // is ever expected from the caller.
    const uint8_t nontxnPutReqType = 14;
    const uint8_t nontxnInsertReqType = 15;
    const uint8_t nontxnRemoveReqType = 16;
    const uint8_t nontxnGetReqType = 17;

    // --------------------------- Remote client API (for decoupled clients)
    // These message types enable clients to run on different servers
    const uint8_t clientBeginTxnReqType = 20;
    const uint8_t clientCommitReqType = 21;
    const uint8_t clientRollbackReqType = 22;
    const uint8_t clientPutReqType = 23;
    const uint8_t clientGetReqType = 24;
    const uint8_t clientDeleteReqType = 25;
    const uint8_t clientServerBusyType = 26;  // Server busy rejection response

    // Maximum message length for server busy response
    const size_t max_busy_message_length = 64;

    // Server busy response (sent when all workers are occupied)
    struct client_server_busy_response_t {
        uint8_t status;     // Always ErrorCode::SERVER_BUSY
        char message[max_busy_message_length];  // Human-readable message
    };

    const size_t max_key_length = 64;
#if defined(MEGA_BENCHMARK)
    const size_t max_value_length = 7000; // mega in new order 
#elif defined(MEGA_BENCHMARK_MICRO)
    const size_t max_value_length = 8000; // mega in MICRO 
#else
    const size_t max_value_length = 700; // in the get api
#endif
    const size_t max_vector_int_length = 80; // uint64 * nshards

    struct TargetServerIDReader {
        uint16_t targert_server_id; // (0-255) <= warehouses * shards
    };

    /* used in batch_lock_request_t */
    /* in one batch theres a sequence of (table_id, key, value) */
    /* max_batch_length indicates the maximum length of this sequence */
  #if defined(MEGA_BENCHMARK)
    const size_t max_batch_size = 100; 
  #elif defined(MEGA_BENCHMARK_MICRO) 
    const size_t max_batch_size = 400; 
  #else
    const size_t max_batch_size = 100; 
  #endif

    struct vector_int_request_t
    {
        uint16_t targert_server_id; // (0-255) <= warehouses * shards
        uint32_t req_nr;
        uint16_t len;
        char value[max_vector_int_length];
    };

    struct get_request_t
    {
        uint16_t targert_server_id; // (0-255) <= warehouses * shards
        uint32_t req_nr;
        uint16_t table_id;
        uint16_t len;
        char key[max_key_length];
    };

    struct scan_request_t
    {
        uint16_t targert_server_id; // (0-255) <= warehouses * shards
        uint32_t req_nr;
        uint16_t table_id;
        uint16_t slen;
        uint16_t elen;
        char start_end_key[128]; // start_key => 64, end_key => 64
    };

    struct get_response_t
    {
        uint32_t req_nr;
        uint16_t len;
        int status;
        char value[max_value_length];
    };

    struct scan_response_t
    {
        uint32_t req_nr;
        uint16_t len;
        int status;
        char value[max_value_length];
    };

    struct control_request_t 
    {
        uint16_t targert_server_id; // (0-255) <= warehouses * shards
        uint32_t req_nr;
        int control;
        uint64_t value;
    };

    struct warmup_request_t 
    {
        uint16_t targert_server_id; // (0-255) <= warehouses * shards
        uint32_t req_nr;
        uint32_t req_val;
    };

    struct lock_request_t
    {
        uint16_t targert_server_id; // (0-65,535) <= warehouses * shards
        uint32_t req_nr;
        uint16_t table_id;
        uint16_t klen;
        uint16_t vlen;
        char key_and_value[max_key_length + max_value_length];
    };

    struct batch_lock_request_t {
        uint16_t targert_server_id; // (0-255) <= warehouses * shards
        uint32_t req_nr;
        uint16_t batch_size;
        char data[
            sizeof(uint16_t) * max_batch_size // the table_id sequence
            + (sizeof(uint16_t) + max_key_length) * max_batch_size // the (klen, kdata) sequence
            + (sizeof(uint16_t) + max_value_length) * max_batch_size // the (vlen, vdata) sequence
        ];
    };

    class BatchLockRequestWrapper {
    public:
        BatchLockRequestWrapper() {
            msg_len = 0;
            request = new batch_lock_request_t;
            need_to_delete = true;
            request->batch_size = 0;
            num_request_handled = 0;
            data_ptr = request->data;
        }

        BatchLockRequestWrapper(char *raw_data) {
            request = (batch_lock_request_t *)raw_data;
            need_to_delete = false;
            num_request_handled = 0;
            data_ptr = request->data;
        }

        ~BatchLockRequestWrapper() {
            if (need_to_delete)
                delete request;
        }

        // Note the string copy in this step could be avoided
        void add_request(std::string &key, std::string& value, uint16_t table_id, uint16_t server_id) {
            request->batch_size ++;
            request->targert_server_id = server_id;
            uint16_t klen = key.size(), vlen = value.size();
            char *ptr = request->data + msg_len;
            auto bytes_shift = sizeof(uint16_t);

            memcpy(ptr, &table_id, bytes_shift);
            ptr += bytes_shift, msg_len += bytes_shift;
            memcpy(ptr, &klen, bytes_shift);
            ptr += bytes_shift, msg_len += bytes_shift;
            memcpy(ptr, key.c_str(), klen);
            ptr += klen, msg_len += klen;
            memcpy(ptr, &vlen, bytes_shift);
            ptr += bytes_shift, msg_len += bytes_shift;
            memcpy(ptr, value.c_str(), vlen);
            ptr += vlen, msg_len += vlen;
        }

        void set_req_nr(uint64_t req_nr) {
            request->req_nr = req_nr;
        }

        size_t get_msg_len() {
            return msg_len + offsetof(batch_lock_request_t, data);
        }

        batch_lock_request_t *get_request_ptr() {
            return request;
        }

        bool all_request_handled() {
            return num_request_handled == request->batch_size;
        }

        void read_one_request(char **key, uint16_t *klen, char **value, uint16_t *vlen, uint16_t *table_id) {
            auto bytes_shift = sizeof(uint16_t);
            memcpy((char*)table_id, data_ptr, bytes_shift);
            data_ptr += bytes_shift;
            memcpy((char*)klen, data_ptr, bytes_shift);
            data_ptr += bytes_shift;
            *key = data_ptr;
            data_ptr += *klen;
            memcpy((char*)vlen, data_ptr, bytes_shift);
            data_ptr += bytes_shift;
            *value = data_ptr;
            data_ptr += *vlen;

            num_request_handled += 1;
        }

    private:
        batch_lock_request_t *request;
        bool need_to_delete;
        size_t msg_len;

        uint16_t num_request_handled;
        char *data_ptr;
    };

    struct basic_response_t
    {
        uint32_t req_nr;
        int status;
    };

    struct get_int_response_t
    {
        uint32_t req_nr;
        uint32_t result;
        int shard_index;
        int status;
    };

    struct basic_request_t
    {
        uint16_t targert_server_id; // (0-255) <= warehouses * shards
        uint32_t req_nr;
    };

    // --------------------------- Remote client API structures
    // Used for decoupled client-server communication

    // Request to begin a new transaction on the server
    struct client_begin_txn_request_t
    {
        uint32_t req_nr;
        uint64_t client_id;         // Unique client identifier
    };

    // Response to begin transaction - contains server-assigned txn_id
    struct client_begin_txn_response_t
    {
        uint32_t req_nr;
        uint64_t txn_id;            // Server-assigned transaction ID
        int status;
    };

    // Request for Put/Get/Delete operations
    struct client_kv_request_t
    {
        uint32_t req_nr;
        uint64_t txn_id;            // Transaction ID from begin_txn
        uint16_t table_id;          // Target table
        uint16_t klen;              // Key length
        uint16_t vlen;              // Value length (0 for Get/Delete)
        char key_and_value[max_key_length + max_value_length];
    };

    // Request for the self-contained non-txn ops (types 14-17;
    // vlen==0 for remove and get).
    // MUST start with targert_server_id: the transport backends peek
    // the first uint16_t of every shard request to pick the helper
    // queue (see TargetServerIDReader in rrr_rpc_backend.cc).
    struct nontxn_write_request_t
    {
        uint16_t targert_server_id; // requesting client's global warehouse id
        uint32_t req_nr;
        uint16_t table_id;          // target table
        uint16_t klen;              // key length
        uint16_t vlen;              // value length (0 for remove)
        char key_and_value[max_key_length + max_value_length];
    };

    // Response for Put/Get/Delete operations
    struct client_kv_response_t
    {
        uint32_t req_nr;
        uint16_t vlen;              // Value length (for Get response)
        int status;
        char value[max_value_length];
    };

    // Request to commit a transaction
    struct client_commit_request_t
    {
        uint32_t req_nr;
        uint64_t txn_id;            // Transaction ID to commit
    };

    // Response to commit/rollback
    struct client_commit_response_t
    {
        uint32_t req_nr;
        int status;
    };

    class ErrorCode
    {
    public:
        static const int SUCCESS = 0;
        static const int TIMEOUT = 1;
        static const int ERROR = 2;
        static const int ABORT = 3;
        static const int SERVER_BUSY = 4;  // All workers occupied
    };

    using resp_continuation_t =
        std::function<void(char *respBuf)>;
    using basic_continuation_t =
        std::function<void(char *respBuf)>;
    using continuation_t =
        std::function<void(const std::string &request, const std::string &reply)>;
    using error_continuation_t =
        std::function<void(const std::string &request, ErrorCode err)>;

    // Single timestamp encoding
    static char* encode_single_timestamp(uint32_t timestamp) {
        char *cc=(char*)malloc(sizeof(uint32_t));
        memcpy(cc, &timestamp, sizeof(uint32_t));
        return cc;
    }
    
    // Single timestamp decoding
    static uint32_t decode_single_timestamp(const char* cc) {
        uint32_t timestamp;
        memcpy(&timestamp, cc, sizeof(uint32_t));
        return timestamp;
    }


    /// Simple time that uses std::chrono
    class ChronoTimer {
        public:
            ChronoTimer() { reset(); }
            void reset() { start_time_ = std::chrono::high_resolution_clock::now(); }

            /// Return seconds elapsed since this timer was created or last reset
            double get_sec() const { return get_ns() / 1e9; }

            /// Return milliseconds elapsed since this timer was created or last reset
            double get_ms() const { return get_ns() / 1e6; }

            /// Return microseconds elapsed since this timer was created or last reset
            double get_us() const { return get_ns() / 1e3; }

            /// Return nanoseconds elapsed since this timer was created or last reset
            size_t get_ns() const {
                return static_cast<size_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::high_resolution_clock::now() - start_time_)
                        .count());
            }

        private:
            std::chrono::time_point<std::chrono::high_resolution_clock> start_time_;
    };

    /// Return the TSC
    static inline size_t rdtsc() {
#if defined(__APPLE__)
        return static_cast<size_t>(mach_absolute_time());
#elif defined(__i386__) || defined(__x86_64__)
        uint32_t lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return static_cast<size_t>((static_cast<uint64_t>(hi) << 32) | lo);
#elif defined(__aarch64__) || defined(__arm__)
        uint64_t tsc;
        asm volatile("mrs %0, cntvct_el0" : "=r"(tsc));
        return static_cast<size_t>(tsc);
#elif defined(__clang__) && __has_builtin(__builtin_readcyclecounter)
        return static_cast<size_t>(__builtin_readcyclecounter());
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (static_cast<size_t>(ts.tv_sec) << 32) ^
               static_cast<size_t>(ts.tv_nsec);
#endif
    }

    static double measure_rdtsc_freq() {
        ChronoTimer chrono_timer;
        const uint64_t rdtsc_start = rdtsc();

        // Do not change this loop! The hardcoded value below depends on this loop
        // and prevents it from being optimized out.
        uint64_t sum = 5;
        for (uint64_t i = 0; i < 1000000; i++) {
            sum += i + (sum + i) * (i % sum);
        }
        assert(sum == 13580802877818827968ull); // "Error in RDTSC freq measurement"

        const uint64_t rdtsc_cycles = rdtsc() - rdtsc_start;
        const double freq_ghz = rdtsc_cycles * 1.0 / chrono_timer.get_ns();
        assert(freq_ghz >= 0.5 && freq_ghz <= 5.0); // "Invalid RDTSC frequency");

        return freq_ghz;
    }

    static size_t ms_to_cycles(double ms, double freq_ghz) {
        return static_cast<size_t>(ms * 1000 * 1000 * freq_ghz);
    }

    // @unsafe: uses std::chrono::duration::count
    static uint64_t getCurrentTimeMillis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static std::string intToString(long long num) {
        // snprintf rather than setw/setfill: see the note above about
        // iomanip under clang 22 + `import std`.
        char buf[32];
        snprintf(buf, sizeof(buf), "%016lld", num);
        return std::string(buf);
    }

    static size_t parse_memory_spec(const std::string &s)
    {
        std::string x(s);
        size_t mult = 1;
        if (x.back() == 'G') {
            mult = static_cast<size_t>(1) << 30;
            x.pop_back();
        } else if (x.back() == 'M') {
            mult = static_cast<size_t>(1) << 20;
            x.pop_back();
        } else if (x.back() == 'K') {
            mult = static_cast<size_t>(1) << 10;
            x.pop_back();
        }
        return strtoul(x.c_str(), nullptr, 10) * mult;
    }
}

#endif
