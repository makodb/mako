#pragma once

#include <functional>
#include <memory>
#include <queue>
#include <tuple>

int setup(int argc, char* argv[]);
int setup2();
int shutdown_paxos();
void microbench_paxos();
void register_for_follower(std::function<void(const char*, int)>, uint32_t);
void register_for_follower_par_id(std::function<void(const char*&, int, int)>, uint32_t);
void register_for_follower_par_id_return(std::function<unsigned long long int(const char*&, int, int, std::queue<std::tuple<unsigned long long int, int, int, const char *>> &)>, uint32_t);
void register_for_leader(std::function<void(const char*, int)>, uint32_t);
void register_leader_election_callback(std::function<void()>);
void register_for_leader_par_id(std::function<void(const char*&, int, int)>, uint32_t);
void register_for_leader_par_id_return(std::function<unsigned long long int(const char*&, int, int, std::queue<std::tuple<unsigned long long int, int, int, const char *>> &)>, uint32_t);
void submit(const char*, int, uint32_t);
void add_log(const char*, int, uint32_t);
void add_log_without_queue(const char*, int, uint32_t);
void add_log_to_nc(const char*, int, uint32_t);
void wait_for_submit(uint32_t);
void microbench_paxos_queue();
void pre_shutdown_step();
int get_epoch();

// auxiliary functions
void worker_info_stats(size_t);
