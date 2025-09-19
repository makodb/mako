#ifndef _LIB_HELPER_QUEUE_H_
#define _LIB_HELPER_QUEUE_H_

#include "rpc.h"
#include <mutex>
#include <condition_variable>
#include <atomic>

#define HELPER_QUEUE_SIZE 100

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
