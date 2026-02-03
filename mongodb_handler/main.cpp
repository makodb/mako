#include <chrono>
#include "mongocxx/instance.hpp"
#include "mongodb_kv_table_handler.h"
#include "mongodb_connection_thread_pool.h"

class callback_class {
 public:
  std::mutex callback_mtx;
  std::vector<mongodb_handler::SampleCommand> executed_commands;
  void callback(mongodb_handler::SampleCommand cmd) {
    std::lock_guard<std::mutex> lock(callback_mtx);
    executed_commands.push_back(cmd);
  }
};


int main() {
  // mongocxx::instance instance;
  // mongodb_handler::MongodbKVTableHandler handler;
  // std::cout << handler.Read(1) << std::endl;
  // std::cout << handler.Write(1, 3) << std::endl;
  // std::cout << handler.Read(1) << std::endl;
  // std::cout << handler.Write(1, 5) << std::endl;
  // std::cout << handler.Read(1) << std::endl;
  // std::cout << handler.Write(3, 13) << std::endl;
  // std::cout << handler.Write(5, 15) << std::endl;
  // std::cout << handler.Read(3) << std::endl;

  auto launch_time = std::chrono::high_resolution_clock::now();

  const int pool_size = 4000;
  callback_class callback_ins;
  std::function<void(mongodb_handler::SampleCommand)> callback_func = std::bind(&callback_class::callback, &callback_ins, std::placeholders::_1);
  mongodb_handler::MongodbConnectionThreadPool pool(pool_size, callback_func);

//   mongodb_handler::MongodbConnectionThreadPool pool(100);

  srand(time(0));

  int total_num = 100000;

  auto start_time = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < total_num; i++) {
    mongodb_handler::SampleCommand cmd{rand() % 2, i, rand() % 1000000};
    pool.MongodbRequest(cmd);
  }

  pool.Close();

  auto end_time = std::chrono::high_resolution_clock::now();

  auto connection_duration = std::chrono::duration_cast<std::chrono::milliseconds>(start_time - launch_time);
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
  std::cout << "Connection Duration: " << connection_duration.count() << " ms" << std::endl;
  std::cout << "Connection Throughput: " << pool_size * 1000.0 / connection_duration.count()<< " connection/s" << std::endl;
  std::cout << "Duration: " << duration.count() << " ms" << std::endl;
  std::cout << "Throughput: " << total_num * 1000.0 / duration.count()<< " req/s" << std::endl;
  std::cout << "Medium MongoDB Latency: " << pool.LatencyMs() << " ms" << std::endl;
  std::cout << "Executed command count: " << callback_ins.executed_commands.size() << std::endl;

  return 0;
}