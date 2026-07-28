#ifndef _LIB_HELPER_QUEUE_H_
#define _LIB_HELPER_QUEUE_H_

// Only the OFF (rrr-transport) build must avoid pulling eRPC's <queue>-heavy
// headers in here: under clang-22 + import-std they redeclare std::queue with
// mismatched abi_tags. HelperQueue uses erpc::ReqHandle only through pointers,
// so the forward declaration below is sufficient; the full type is included
// only when eRPC is actually enabled.
#ifdef MAKO_ENABLE_ERPC
#include "rpc.h"
#endif

// Forward declare erpc::ReqHandle to break a circular include cycle:
//   rpc.h → cc/timing_wheel.h → cc/timely.h → "common.h"
// resolves to mako/benchmarks/common.h (not erpc/src/common.h) because of
// include path ordering. mako's common.h pulls benchmark_config.h which
// pulls this file before rpc.h has finished declaring erpc::ReqHandle.
namespace erpc { class ReqHandle; }
#include <mutex>
#include <condition_variable>
#include <atomic>

#define HELPER_QUEUE_SIZE 100

namespace erpc {
class ReqHandle;
}

namespace mako
{
using namespace std;

// 1 writer + 1 reader queue
class HelperQueue {
public:
    HelperQueue(int id,bool is_req);

    bool is_req_buffer_full() { return req_cnt == HELPER_QUEUE_SIZE;};
    bool is_req_buffer_empty() { return req_cnt == 0;};
    size_t get_size() {return req_cnt;} ;
    bool add_one_req(erpc::ReqHandle *req_handle, size_t msg_size);
    bool free_one_req();
    void suspend();
    void wakeup();
    bool fetch_one_req(erpc::ReqHandle **req_handle, size_t &msg_size);
    void request_stop();
    bool should_stop() const { return stop_flag_.load(std::memory_order_acquire); }
    int req_buffer_reader_idx;
    int req_buffer_writer_idx;
    int req_cnt;

private:
    std::pair<erpc::ReqHandle*, size_t>req_buffer[HELPER_QUEUE_SIZE];
    std::mutex condition_mutex;

    /* used for wakeup*/
    std::condition_variable cv;
    int id;
    bool is_req;
    std::atomic<int> my_atomic_int;
    std::atomic<bool> stop_flag_{false};
};

}
#endif
