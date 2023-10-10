#pragma once

#include <functional>
#include <memory>
#include <queue>
#include <tuple>
#include <map>

std::vector<std::string> setup(int argc, char* argv[]);  // return proc_name on the same machine, s101, s102
int setup2(int action = 0, int shardIndex = -1);
std::map<std::string, std::string> getHosts(std::string) ;  // return hosts map
int get_outstanding_logs(uint32_t);
int shutdown_paxos();
void microbench_paxos();
void register_for_follower(std::function<void(const char*, int)>, uint32_t);
void register_for_follower_par_id(std::function<void(const char*&, int, int)>, uint32_t);
void register_for_follower_par_id_return(std::function<std::vector<uint32_t>(const char*&, int, int, int, std::queue<std::tuple<std::vector<uint32_t>, int, int, int, const char *>> &)>, 
                                         uint32_t);
void register_for_leader(std::function<void(const char*, int)>, uint32_t);
void register_leader_election_callback(std::function<void(int)>);
void register_for_leader_par_id(std::function<void(const char*&, int, int)>, uint32_t);
void register_for_leader_par_id_return(std::function<std::vector<uint32_t>(const char*&, int, int, int, std::queue<std::tuple<std::vector<uint32_t>, int, int, int, const char *>> &)>, 
                                       uint32_t);
void submit(const char*, int, uint32_t);
void add_log(const char*, int, uint32_t);
void add_log_without_queue(const char*, int, uint32_t);
void add_log_to_nc(const char*, int, uint32_t, int = 0);
void wait_for_submit(uint32_t);
void microbench_paxos_queue();
void pre_shutdown_step();
int get_epoch();
void set_epoch(int = -1);
void upgrade_p1_to_leader();

// auxiliary functions
void worker_info_stats(size_t);

// network client
void nc_setup_server(int, std::string);  // start a server accepting requests on the leader replica
std::vector<std::vector<int>>* nc_get_new_order_requests(int); 
std::vector<std::vector<int>>* nc_get_payment_requests(int); 
std::vector<std::vector<int>>* nc_get_delivery_requests(int); 
std::vector<std::vector<int>>* nc_get_order_status_requests(int); 
std::vector<std::vector<int>>* nc_get_stock_level_requests(int); 
std::vector<std::vector<int>>* nc_get_read_requests(int); 
std::vector<std::vector<int>>* nc_get_rmw_requests(int); 