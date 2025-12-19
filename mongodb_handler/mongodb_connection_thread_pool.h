#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <cmath>
#include "mongodb_kv_table_handler.h"

namespace mongodb_handler {

class SampleCommand {
 public:
  int type, key, value;
};

class Distribution {
  std::vector<double> data_;
 public:
  void append(double x) {
    data_.push_back(x);
  }
  void merge(Distribution &o) {
    for (int i = 0; i < o.count(); i++)
      data_.push_back(o.data_[i]);
  }
  size_t count() {
    return data_.size();
  }
  double pct(double pct) {
    if (data_.size() == 0)
      return -1;
    sort(data_.begin(), data_.end());
    return data_[floor(data_.size() * pct)];
  }
  double pct50() {
    return pct(0.5);
  }
  double pct90() {
    return pct(0.9);
  }
  double pct99() {
    return pct(0.99);
  }
  double ave() {
    if (data_.size() == 0)
      return -1;
    double sum = 0;
    for (int i = 0; i < data_.size(); i++)
      sum += data_[i];
    return sum / data_.size();
  }
};

class MongodbConnectionThreadPool {

  using CallbackType = std::function<void(SampleCommand)>;

  int thread_num_;
  int round_robin_ = 0;

  std::vector<std::thread> threads_;
  std::vector<std::unique_ptr<std::mutex>> mtxs_;
  std::vector<std::unique_ptr<std::condition_variable>> conditions_;
  std::vector<std::shared_ptr<janus::MongodbKVTableHandler>> mongodb_handlers_{10000};
  std::vector<std::shared_ptr<Distribution>> durations_;
  std::vector<std::queue<SampleCommand>> request_queues_;
  SampleCommand terminate_cmd = SampleCommand{2, 0, 0};

  CallbackType callback_;

public:

  void MongodbConnection(int thread_id) {
    while (true) {
      {
        std::unique_lock<std::mutex> lk(*mtxs_[thread_id]);
        conditions_[thread_id]->wait(lk,
          [this, thread_id]{
            return !request_queues_[thread_id].empty(); 
          });
      }
      SampleCommand cmd;
      {
        std::lock_guard<std::mutex> lk(*mtxs_[thread_id]);
        cmd = request_queues_[thread_id].front();
        request_queues_[thread_id].pop();
      }
      
      auto start_time = std::chrono::high_resolution_clock::now();

      if (cmd.type == 0) {
        // usleep(40000);
        mongodb_handlers_[thread_id]->Read(cmd.key);
      }
      else if (cmd.type == 1) {
        // usleep(40000);
        mongodb_handlers_[thread_id]->Write(cmd.key, cmd.value);
      }
      else
        break;

      auto end_time = std::chrono::high_resolution_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
      durations_[thread_id]->append(duration.count());

      callback_(cmd);
    }
  }

  void MongodbRequest(SampleCommand cmd) {
    {
        std::lock_guard<std::mutex> lk(*mtxs_[round_robin_]);
        request_queues_[round_robin_].push(cmd);
    }
    conditions_[round_robin_]->notify_one();

    round_robin_++;
    if (round_robin_ >= thread_num_)
      round_robin_ = 0;
  }

  void Close() {
    for (int i = 0; i < thread_num_; i++) {
      std::unique_lock<std::mutex> lk(*mtxs_[i]);
      request_queues_[i].push(terminate_cmd);
      lk.unlock();
      conditions_[i]->notify_one();
    }
    for (int i = 0; i < thread_num_; i++) {
      threads_[i].join();
    }
  }

  double LatencyMs() {
    Distribution all;
    for (int i = 0; i < thread_num_; i++) {
      all.merge(*durations_[i]);
    }
    return all.pct50();
  }

  void static createHandlers(int i, std::vector<std::shared_ptr<janus::MongodbKVTableHandler>>& handlers) {
    handlers[i] = std::make_shared<janus::MongodbKVTableHandler>();
  }

  MongodbConnectionThreadPool(int thread_num, CallbackType callback)
    : thread_num_(thread_num), callback_(callback) {
    auto time1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < thread_num; i++) {
      mtxs_.push_back(std::make_unique<std::mutex>());
      request_queues_.push_back(std::queue<SampleCommand>());
      conditions_.push_back(std::make_unique<std::condition_variable>());
      durations_.push_back(std::make_shared<Distribution>());
    }
    auto time2 = std::chrono::high_resolution_clock::now();
    std::cout << "MongodbConnectionThreadPool Phase 1 Duration: " << std::chrono::duration_cast<std::chrono::milliseconds>(time2 - time1).count() << " ms" << std::endl;
    std::vector<std::thread> create_connection_threads;
    for (int i = 0; i < thread_num; i++) {
      create_connection_threads.emplace_back(createHandlers, i, std::ref(mongodb_handlers_));
      // mongodb_handlers_.push_back(std::make_shared<janus::MongodbKVTableHandler>());
    }
    for (auto& t : create_connection_threads) {
        t.join();
    }
    auto time3 = std::chrono::high_resolution_clock::now();
    std::cout << "MongodbConnectionThreadPool Phase 2 Duration: " << std::chrono::duration_cast<std::chrono::milliseconds>(time3 - time2).count() << " ms" << std::endl;
    for (int i = 0; i < thread_num; i++) {
      threads_.push_back(std::thread([this, i]() {
        MongodbConnection(i);
      }));
    }
    auto time4 = std::chrono::high_resolution_clock::now();
    std::cout << "MongodbConnectionThreadPool Phase 3 Duration: " << std::chrono::duration_cast<std::chrono::milliseconds>(time4 - time3).count() << " ms" << std::endl;
  }

  ~MongodbConnectionThreadPool() {

  }
};


}