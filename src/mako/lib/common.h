#ifndef _LIB_COMMON_H_
#define _LIB_COMMON_H_

#include "lib/timestamp.h"

#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <sys/file.h>
#include "rpc.h"
#include <mutex>
#include <condition_variable>
#include <stdlib.h>
#include <chrono>
#include <ctime>

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

namespace srolis
{
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
    static void printStringAsBit(std::string data,std::string prefix="") {
        Warning("# of data: %lu", data.length());
        std::string tmp=prefix;
        for (int i=0; i<(int)data.length(); i++) {
            if (tmp.length()==0)
                tmp += std::to_string((int)(data.at(i)));
            else
                tmp += ","+std::to_string((int)(data.at(i)));
        }
        std::cout << tmp << std::endl;
    }

    static std::string getStringAsBit(std::string data) {
        std::string tmp="";
        for (int i=0; i<(int)data.length(); i++) {
            if (tmp.length()==0)
                tmp += std::to_string((int)(data.at(i)));
            else
                tmp += ","+std::to_string((int)(data.at(i)));
        }
        return tmp;
    }

    struct Node {
#if MERGE_KEYS_GROUPS != SHARDS
        uint32_t timestamps[MERGE_KEYS_GROUPS];
#else
        uint32_t timestamps[SHARDS];
#endif
        int16_t data_size;
        char *data;
    };

    // TODO: you have to do anyway; data stored here;
    // extra bytes for stored value: actual value + (timestamp, term) + sizeof(node)
    const int EXTRA_BITS_FOR_VALUE = sizeof(uint32_t) + sizeof(struct Node);
    const int BITS_OF_NODE = sizeof(struct Node);
    const int BITS_OF_TT = sizeof(uint32_t);

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
            return msg_len + sizeof(request->batch_size) + sizeof(request->req_nr);
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

    class ErrorCode
    {
    public:
        static const int SUCCESS = 0;
        static const int TIMEOUT = 1;
        static const int ERROR = 2;
        static const int ABORT = 3;
    };

    using resp_continuation_t =
        std::function<void(char *respBuf)>;
    using basic_continuation_t =
        std::function<void(char *respBuf)>;
    using continuation_t =
        std::function<void(const std::string &request, const std::string &reply)>;
    using error_continuation_t =
        std::function<void(const std::string &request, ErrorCode err)>;

    static char* encode_vec_uint32(std::vector<uint32_t> a, int nshards) {
        char *cc=(char*)malloc(sizeof(uint32_t)*nshards);
        int pos=0;
        for(int i=0;i<nshards;i++){
            memcpy(cc+pos, &a[i], sizeof(uint32_t));
            pos+=sizeof(uint32_t);
        }
        return cc;
    } 

    // to avoid copying, using decode_vec_uint32(...).swap(a);
    static std::vector<uint32_t> decode_vec_uint32(const char*cc, int nshards) {
        std::vector<uint32_t> ret(nshards);
        int pos=0;
        for (int i=0;i<nshards;i++){
            uint32_t tmp=0;
            memcpy(&tmp, cc+pos, sizeof(uint32_t));
            pos+=sizeof(uint32_t);
            ret[i] = tmp;
        }
        return ret;
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
        uint64_t rax;
        uint64_t rdx;
        asm volatile("rdtsc" : "=a"(rax), "=d"(rdx));
        return static_cast<size_t>((rdx << 32) | rax);
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

    static uint64_t getCurrentTimeMillis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
    }
}

#endif
