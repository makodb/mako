module;

#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <list>
#include <string>
#include <utility>
#include <vector>


#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <chrono>


module rrr:impl.misc.recorder;
import rrr;

/*
 *
 * Here is how it works, there is a queue, a flush thread, and a callback thread.
 *
 */




// @external: {
//   open: [unsafe],
//   write: [unsafe],
//   fdatasync: [unsafe],
//   strerror: [unsafe],
//   new: [unsafe],
//   delete: [unsafe],
//   std::thread: [unsafe],
//   std::mutex: [unsafe],
//   std::condition_variable: [unsafe]
// }

// NOTE: This file is a disk recorder/logger. It uses file I/O system calls,
// threading primitives, and manual memory management. All functions are @unsafe.

namespace rrr {

// @unsafe - Uses open() system call, new, std::thread
Recorder::Recorder(const char *path) {
    Log::debug("disk log into %s", path);  // @unsafe

    fd_ = open(path, O_RDWR | O_CREAT, 0644);  // @unsafe
    if (errno == EINVAL) {
	Log::error("Open record file failed, are"  // @unsafe
		   " yo2u trying to write into a tmpfs?");
	fd_ = open(path, O_RDWR | O_CREAT, 0644);  // @unsafe
    }
    if (fd_ <= 0) {
	Log::error("Open record file failed, errno:"  // @unsafe
		   " %d, %s", errno, strerror(errno));  // @unsafe
	verify(fd_ > 0);  // @unsafe
    }

    flush_reqs_ = new std::list<io_req_t*>();  // @unsafe
    callback_reqs_ = new std::list<io_req_t*>();  // @unsafe


    th_flush_ = new std::thread(&Recorder::flush_loop, this);  // @unsafe

    timer_.start();

//    th_flush_ = new std::thread([this] () {
//	    this->flush_loop();
//	});
    //    th_pool_ = new base::ThreadPool(1);
}

// @unsafe - Uses std::mutex and condition_variable
void Recorder::flush_loop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mtx_cd_flush_);  // @unsafe
//        auto now = std::chrono::system_clock::now();
            cd_flush_.wait(lock);  // @unsafe
        }
        flush_buf();  // @unsafe
    }
}

//void Recorder::submit(const std::string &buf) {
//    std::function<void(void)> empty_func;
//    submit(buf, empty_func);
//}

// @unsafe - Uses new and std::mutex
void Recorder::submit(const std::string &buf,
		      const std::function<void(void)> &cb) {

    io_req_t *req = new io_req_t(buf, cb);  // @unsafe
    std::lock_guard<std::mutex> lock(this->mtx_);  // @unsafe
    flush_reqs_->push_back(req);  // @unsafe

//    if (cb) {
//        cd_flush_.notify_one();
//    }
}

// @unsafe - Uses new and std::mutex
void Recorder::submit(Marshal &m,
                      const std::function<void(void)> &cb) {
    io_req_t *req = new io_req_t();  // @unsafe
    std::string &s = req->first;
    req->second = cb;

    s.resize(m.content_size());
    m.write((void*)s.data(), m.content_size());  // @unsafe

    std::lock_guard<std::mutex> lock(this->mtx_);  // @unsafe
    flush_reqs_->push_back(req);  // @unsafe
}

// @unsafe - Uses write() system call, fdatasync, new
void Recorder::flush_buf() {
    int cnt_flush = 0;
    int sz_flush = 0;
    int sz;
    std::list<io_req_t*>* reqs;

    {
        std::lock_guard<std::mutex> lock(mtx_);  // @unsafe
        sz = flush_reqs_->size();
        reqs = flush_reqs_;

        if (sz > 0) {
            flush_reqs_ = new std::list<io_req_t*>;  // @unsafe
        }
    }

    if (sz == 0) {
	return;
    }

    for (auto &p: *reqs) {
	std::string &s = p->first;
	int ret = write(fd_, s.data(), s.size());  // @unsafe
	verify(ret == s.size());  // @unsafe
        cnt_flush ++;
        sz_flush += ret;
    }
#ifndef __APPLE__
    fdatasync(fd_);  // @unsafe
#endif

    stat_cnt_.sample(cnt_flush);
    stat_sz_.sample(sz_flush);

    // push to call back reqs.

    {
        std::lock_guard<std::mutex> lock(mtx_);  // @unsafe
        callback_reqs_->insert(callback_reqs_->end(),  // @unsafe
                               reqs->begin(), reqs->end());
    }
    return;

}

// @unsafe - Uses std::mutex, new, delete
void Recorder::invoke_cb() {
    verify(0);  // @unsafe
    int sz;
    std::list<io_req_t*>* reqs;
    {
        std::lock_guard<std::mutex> lock(mtx_);  // @unsafe
        sz = callback_reqs_->size();
        reqs = callback_reqs_;
        if (sz > 0) {
            callback_reqs_ = new std::list<io_req_t*>;  // @unsafe
        }
    }

    if (sz == 0) {
        return;
    }

    for (auto &p: *reqs) {
        auto &cb = p->second;
        if (cb) {
            cb();  // @unsafe
        }
        delete p;  // @unsafe
    }
    delete reqs;  // @unsafe
}

// @safe - Empty destructor
Recorder::~Recorder() {
}

} // namespace rrr
