#include "helper_queue.h"
#include <iostream>
#include <mutex>
#include "lib/common.h"

// keep in mind: this queue is just for 1 writer, 1 reader
// 
// 2023-04-17update:
//    control command using the queues[0]
// https://www.modernescpp.com/index.php/c-core-guidelines-be-aware-of-the-traps-of-condition-variables
namespace mako
{
using namespace std;

HelperQueue::HelperQueue(int id,bool is_req): my_atomic_int(0) {
    this->req_buffer_reader_idx = 0; // {req_cnt} requests since then
    this->req_buffer_writer_idx = 0; // next available to write slot
    this->req_cnt = 0;
    this->id=id;
    this->is_req=is_req;
 }

bool HelperQueue::add_one_req(erpc::ReqHandle *req_handle, size_t msg_size) {
    // if (is_req_buffer_full()) {
    //     Warning("the buffer is full");
    //     return false;
    // }

    std::unique_lock<std::mutex> lock(condition_mutex);
    req_buffer[req_buffer_writer_idx] = std::make_pair(req_handle, msg_size);
    req_buffer_writer_idx = (req_buffer_writer_idx+1)%HELPER_QUEUE_SIZE;
    req_cnt ++;
    if (is_req){
        cv.notify_one();
    }
    return true;
}

bool HelperQueue::fetch_one_req(erpc::ReqHandle **req_handle, size_t &msg_size) {
    std::unique_lock<std::mutex> lock(condition_mutex); // if no such lock, the TPUT is not even cross shards
    if (is_req_buffer_empty())
        return false;
    
    *req_handle = req_buffer[req_buffer_reader_idx].first;
    msg_size = req_buffer[req_buffer_reader_idx].second;
    req_buffer_reader_idx =(req_buffer_reader_idx+1)%HELPER_QUEUE_SIZE;
    req_cnt --;
    return true;
}

void HelperQueue::suspend() {
    std::unique_lock<std::mutex> lock(condition_mutex);
    cv.wait(lock, [this]{return stop_flag_.load(std::memory_order_acquire) || !is_req_buffer_empty();});
}

void HelperQueue::wakeup() { // NOTICE
    Panic("discard");
    // std::unique_lock<std::mutex> lock(condition_mutex);
    // cv.notify_one();
}

void HelperQueue::request_stop() {
    {
        std::lock_guard<std::mutex> lock(condition_mutex);
        stop_flag_.store(true, std::memory_order_release);
    }
    cv.notify_all();
}

// -------------------------------------------------------------------------------------------
bool HelperQueue::free_one_req() {
    Panic("Discard function free_one_req");
    // std::unique_lock<std::mutex> lock(condition_mutex);
    // if (is_req_buffer_empty())
    //     return false;

    // req_cnt --;
    // req_buffer_reader_idx =(req_buffer_reader_idx+1)%HELPER_QUEUE_SIZE;

    return true;
}
}
