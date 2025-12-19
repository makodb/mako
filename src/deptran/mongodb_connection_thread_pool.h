#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <cmath>
#include "mongodb_kv_table_handler.h"
#include "RW_command.h"

namespace janus {

class MongodbConnectionThreadPool {

  class CommandQueue {
   private:
    std::queue<shared_ptr<Marshallable>> queue;
    std::mutex mutex;
    std::condition_variable cond_var;
   public:
    void push(const shared_ptr<Marshallable>& cmd) {
      std::lock_guard<std::mutex> lock(mutex);
      queue.push(cmd);
      cond_var.notify_one();
    }
    shared_ptr<Marshallable> pop() {
      std::unique_lock<std::mutex> lock(mutex);
      cond_var.wait(lock, [this]{ return !queue.empty(); });
      shared_ptr<Marshallable> cmd = queue.front();
      queue.pop();
      return cmd;
    }
    bool empty() {
      return queue.empty();
    }
    void close() {
      push(nullptr);
    }
  };

  int thread_num_;
  int round_robin_ = 0;

  std::vector<std::thread> threads_;
  std::vector<std::shared_ptr<MongodbKVTableHandler>> mongodb_handlers_{10000};
  std::vector<std::shared_ptr<CommandQueue>> request_queues_;
  std::vector<std::shared_ptr<Distribution>> durations_;

  CommandQueue finished_queue_;

 public:

  void MongodbHandler(int thread_id) {
    while (true) {
      shared_ptr<Marshallable> cmd = request_queues_[thread_id]->pop();
      
      if (cmd == nullptr)
        break;

      SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd);

      auto start_time = std::chrono::high_resolution_clock::now();

      if (parsed_cmd.IsRead())
        mongodb_handlers_[thread_id]->Read(parsed_cmd.key_);
      else if (parsed_cmd.IsWrite())
        mongodb_handlers_[thread_id]->Write(parsed_cmd.key_, parsed_cmd.value_);
      else
        break;

      auto end_time = std::chrono::high_resolution_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
      durations_[thread_id]->append(duration.count());

      // finished_queue_.push(cmd);
      shared_ptr<TxPieceData> cmd_content = *(((VecPieceData*)(dynamic_pointer_cast<TpcCommitCommand>(cmd)->cmd_.get()))->sp_vec_piece_data_->begin());
#ifdef MONGODB_DEBUG
      Log_info("Before cmd_content->mongodb_finished->Set(1);");
#endif
      cmd_content->mongodb_finished->Set(1);
#ifdef MONGODB_DEBUG
      Log_info("After cmd_content->mongodb_finished->Set(1);");
#endif
    }
  }

  void static createHandlers(int i, std::vector<std::shared_ptr<janus::MongodbKVTableHandler>>& handlers) {
    handlers[i] = std::make_shared<janus::MongodbKVTableHandler>();
  }

  MongodbConnectionThreadPool(int thread_num) : thread_num_(thread_num) {
    for (int i = 0; i < thread_num; i++) {
      request_queues_.push_back(std::make_shared<CommandQueue>());
      durations_.push_back(std::make_shared<Distribution>());
    }
    std::vector<std::thread> create_connection_threads;
    for (int i = 0; i < thread_num; i++) {
      create_connection_threads.emplace_back(createHandlers, i, std::ref(mongodb_handlers_));
    }
    for (auto& t : create_connection_threads) {
      t.join();
    }
    for (int i = 0; i < thread_num; i++)
      threads_.push_back(std::thread([this, i]() {
        MongodbHandler(i);
      }));
  }

  ~MongodbConnectionThreadPool() {

  }

  void MongodbRequest(const shared_ptr<Marshallable>& cmd) {
    request_queues_[round_robin_]->push(cmd);
    round_robin_++;
    if (round_robin_ >= thread_num_)
      round_robin_ = 0;
  }

  shared_ptr<Marshallable> MongodbFinishedPop() {
    return finished_queue_.pop();
  }

  bool MongodbFinishedEmpty() {
    return finished_queue_.empty();
  }

  void Close() {
    for (int i = 0; i < thread_num_; i++)
      request_queues_[i]->close();
    for (int i = 0; i < thread_num_; i++)
      threads_[i].join();
    finished_queue_.close();
  }

  double LatencyMs() {
    Distribution all;
    for (int i = 0; i < thread_num_; i++)
      all.merge(*durations_[i]);
    return all.pct50();
  }
};


}
