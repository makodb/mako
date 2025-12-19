#pragma once

// This header provides the same API as s_main.h but uses Raft instead of Paxos
// It should only be included when MAKO_USE_RAFT is defined (via mako.hh)

#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <tuple>
#include <vector>
#include <cstdint>

std::vector<std::string> setup(int argc, char *argv[]);

// setup2 - overloaded for default arguments
int setup2(int action, int shardIndex);
inline int setup2() { return setup2(0, -1); }

std::map<std::string, std::string> getHosts(std::string);
int get_outstanding_logs(uint32_t);
int shutdown_paxos();
void microbench_paxos();
void register_for_follower(std::function<void(const char *, int)>, uint32_t);
void register_for_follower_par_id(std::function<void(const char *&, int, int)>,
                                  uint32_t);
void register_for_follower_par_id_return(
    std::function<int(const char *&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char *>> &)>,
    uint32_t);
void register_for_leader(std::function<void(const char *, int)>, uint32_t);
void register_leader_election_callback(std::function<void(int)>);
void register_for_leader_par_id(std::function<void(const char *&, int, int)>,
                                uint32_t);
void register_for_leader_par_id_return(
    std::function<int(const char *&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char *>> &)>,
    uint32_t);
void submit(const char *, int, uint32_t);
void add_log(const char *, int, uint32_t);
void add_log_without_queue(const char *, int, uint32_t);

void add_log_to_nc(const char *, int, uint32_t, int = 0);

void wait_for_submit(uint32_t);
void microbench_paxos_queue();
void pre_shutdown_step();
int get_epoch();

// set_epoch - overloaded for default argument
void set_epoch(int epoch);
inline void set_epoch() { set_epoch(-1); }

void upgrade_p1_to_leader();
void worker_info_stats(size_t);

// ============================================================================
// PREFERRED REPLICA SYSTEM API
// ============================================================================
// Set the preferred leader for all Raft workers
// @param site_id The site ID of the preferred leader replica
void set_preferred_leader(int site_id);

void nc_setup_server(int, std::string);
std::vector<std::vector<int>> *nc_get_new_order_requests(int);
std::vector<std::vector<int>> *nc_get_payment_requests(int);
std::vector<std::vector<int>> *nc_get_delivery_requests(int);
std::vector<std::vector<int>> *nc_get_order_status_requests(int);
std::vector<std::vector<int>> *nc_get_stock_level_requests(int);
std::vector<std::vector<int>> *nc_get_read_requests(int);
std::vector<std::vector<int>> *nc_get_rmw_requests(int);
