#include "__dep__.h"
#include "frame.h"
#include "client_worker.h"
#include "procedure.h"
#include "command_marshaler.h"
#include "benchmark_control_rpc.h"
#include "client_status.h"
#include "server_worker.h"
#include "scheduler.h"

#include "rrr/rrr.hpp"

// #define CPU_PROFILE 1

#ifdef CPU_PROFILE
# include <gperftools/profiler.h>
#endif // ifdef CPU_PROFILE

using namespace janus;

// Shared client status for synchronization and statistics
// Can be accessed by both RPC service and external callers (ClientWorker, Coordinators)
static rusty::Option<rusty::Arc<ClientStatus>> client_status_g = rusty::None;
static rusty::Option<rusty::Arc<rrr::PollThread>> cli_poll_thread_worker_g = rusty::None;
static rrr::Server *cli_hb_server_g = nullptr;

static vector<ServerWorker> svr_workers_g = {};
vector<unique_ptr<ClientWorker>> client_workers_g = {};
static std::vector<std::thread> client_threads_g = {}; // TODO remove this?
static std::vector<std::thread> failover_threads_g = {};
bool* volatile failover_triggers;
volatile bool failover_server_quit = false;
volatile locid_t failover_server_idx;
volatile double total_throughput = 0;
// Request latency statistics count only the middle third of a run.
Distribution request_latency;
// commit_time for all (default 30s) duration
vector<std::pair<double, double>> commit_time; // <dispatch_time, duration>
#ifdef LATENCY_DEBUG
  Distribution client2leader, client2leader_send, client2test_point;
#endif

void client_setup_heartbeat(int num_clients) {
  Log_info("{} in client_setup_heartbeat", __FUNCTION__);
  std::map<int32_t, std::string> txn_types;
  Frame* f = Frame::GetFrame(Config::GetConfig()->tx_proto_);
  f->GetTxTypes(txn_types);
  delete f;

  // Always create shared client status (needed for ClientWorker/Coordinator stats)
  client_status_g = rusty::Some(rusty::Arc<ClientStatus>::make(num_clients, txn_types));

  bool hb = Config::GetConfig()->do_heart_beat();
  if (hb) {
    // setup controller rpc server
    cli_poll_thread_worker_g = rusty::Some(rrr::PollThread::create());
    cli_hb_server_g = new rrr::Server(rrr::Server::new_(rusty::Some(cli_poll_thread_worker_g.as_ref().unwrap().clone())));

    // Create service with Arc<ClientStatus>, register as owned Box<Service>
    cli_hb_server_g->reg_service_typed(rusty::make_box<ClientControlServiceImpl>(
        client_status_g.as_ref().unwrap().clone()));

    auto ctrl_port = std::to_string(Config::GetConfig()->get_ctrl_port());
    std::string server_address = std::string("0.0.0.0:").append(ctrl_port);
    Log_info("Start control server on port {}", ctrl_port.c_str());
    cli_hb_server_g->start(reinterpret_cast<const int8_t*>(server_address.c_str()));
  }
}

void client_launch_workers(vector<Config::SiteInfo> &client_sites) {
  // load some common configuration
  // start client workers in new threads.
  Log_info("client enabled, number of sites: {}", client_sites.size());
  vector<ClientWorker*> workers;

  failover_triggers = new bool[client_sites.size()]() ;
#ifdef SIMULATE_WAN
  int core_id = 10; // [JetPack] usually run within 5 replicas, 5 + 3 cores are enough for aws test
#endif
#ifndef SIMULATE_WAN
  int core_id = 0; // [JetPack] usually run 1 replica on each process on cloud setting
#endif
  for (uint32_t client_id = 0; client_id < client_sites.size(); client_id++) {
    // Pass Arc<ClientStatus> to ClientWorker for synchronization and statistics
    auto client_status = client_status_g.is_some()
        ? rusty::Some(client_status_g.as_ref().unwrap().clone())
        : rusty::None;
    ClientWorker* worker = new ClientWorker(client_id,
                                            client_sites[client_id],
                                            Config::GetConfig(),
                                            std::move(client_status),
                                            rusty::None,
                                            &(failover_triggers[client_id]),
                                            &failover_server_quit,
                                            &failover_server_idx,
                                            &total_throughput);
    workers.push_back(worker);
    auto th_ = std::thread(&ClientWorker::Work, worker);
#ifdef AWS
    //in AWS test, distrubute client workers on different cores except physical core 1(core_id == 1 || 5), which is the server worker core
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    int rc = pthread_setaffinity_np(th_.native_handle(),
                                    sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
      std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    } else {
      Log_info("start a client thread on core {}, client-id:{}", core_id, client_id);
    }
    core_id ++;
    if(core_id % 4 == 1){
      core_id++;
    }
#endif
#ifndef AWS
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    int rc = pthread_setaffinity_np(th_.native_handle(),
                                    sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
      std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    } else {
      Log_info("start a client thread on core {}, client-id:{}", core_id, client_id);
    }
    core_id ++;

#endif
    client_threads_g.push_back(std::move(th_));
    client_workers_g.push_back(std::unique_ptr<ClientWorker>(worker));
  }

}

// start servers in new threads.
void server_launch_worker(vector<Config::SiteInfo>& server_sites) {
  auto config = Config::GetConfig();
  Log_info("server enabled, number of sites: {}", server_sites.size());
  svr_workers_g.resize(server_sites.size());
  int i=0;
  vector<std::thread> setup_ths;
  int core_id = 1;
#ifdef SIMULATE_WAN
  core_id = 5; //
#endif
  for (auto& site_info : server_sites) {
    auto th_ = std::thread([&site_info, &i, &config] () {
      Log_info("launching site: {:x}, bind address {}",
               site_info.id,
               site_info.GetBindAddress().c_str());
      auto& worker = svr_workers_g[i++];
      worker.site_info_ = const_cast<Config::SiteInfo*>(&config->SiteById(site_info.id));
      Log_info("start SetupBase");
      worker.SetupBase();
      // register txn piece logic
      Log_info("start RegisterWorkload");
      worker.RegisterWorkload();
      // populate table according to benchmarks
      worker.PopTable();
      Log_info("table popped for site {}", (int)worker.site_info_->id);
#ifdef DB_CHECKSUM
      worker.DbChecksum();
#endif
      // start server service
#ifdef RAFT_TEST_CORO
      // In test mode, only initialize replication services
      if (worker.rep_sched_)
        worker.rep_sched_->svr_workers_g = &svr_workers_g;
#else
      worker.tx_sched_->svr_workers_g = &svr_workers_g;
      if (worker.rep_sched_)
        worker.rep_sched_->svr_workers_g = &svr_workers_g;
#endif
      worker.SetupService();
      Log_info("start communication for site {}", (int)worker.site_info_->id);
      worker.SetupCommo();
      Log_info("site {} launched!", (int)site_info.id);
      worker.launched_ = true;
    });
#ifdef AWS
    // for better performance, bind each server thread to a cpu core
    // in AWS test, each instance has at most one server worker
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    int rc = pthread_setaffinity_np(th_.native_handle(),
                                    sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
      std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    } else {
      Log_info("start a server thread on core {}, site-id:{}", core_id, site_info.id);
    }
#endif
#ifndef AWS
    // for better performance, bind each server thread to a cpu core
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    int rc = pthread_setaffinity_np(th_.native_handle(),
                                    sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
      std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    } else {
      Log_info("start a server thread on core {}, site-id:{}", core_id, site_info.id);
    }
    core_id ++;
#endif
    setup_ths.push_back(std::move(th_));
  }

  for (auto& worker : svr_workers_g) {
    while (!worker.launched_) {
      sleep(1);
    }
  }

  Log_info("waiting for server setup threads.");
  for (auto& th: setup_ths) {
    th.join();
  }
  Log_info("done waiting for server setup threads.");

  for (ServerWorker& worker : svr_workers_g) {
    // start communicator after all servers are running
    // setup communication between controller script
    worker.SetupHeartbeat();
  }
  Log_info("server workers' communicators setup");
}

void client_shutdown() {
  for (const unique_ptr<ClientWorker>& client: client_workers_g) {
    // removed commented-out
    // `// client->retrive_statistic();` — method deleted.
    request_latency.merge(client->request_latency_);
    commit_time.insert(commit_time.end(), client->commit_time_.begin(), client->commit_time_.end());
#ifdef LATENCY_DEBUG
    client2leader.merge(client->client2leader_);
    client2test_point.merge(client->client2test_point_);
    client2leader_send.merge(client->client2leader_send_);
#endif
  }
  client_workers_g.clear();
}

void server_shutdown() {
  Log_info("server_shutdown");
  for (auto &worker : svr_workers_g) {
    worker.ShutDown();
  }
}

void check_current_path() {
  auto path = std::filesystem::current_path();
  Log_info("PWD : ", path.string().c_str());
}

void wait_for_clients() {
  Log_info("{}: wait for client threads to exit.", __FUNCTION__);
  for (auto &th: client_threads_g) {
    th.join();
  }
}

int find_current_leader() {
    for (int i = 0; i < svr_workers_g.size(); i++) {
        if (svr_workers_g[i].rep_sched_) {
            // Cast to RaftServer to access IsLeader()
            if (TxLogServer* server = dynamic_cast<TxLogServer*>(svr_workers_g[i].rep_sched_)) {
                if (server->IsLeader()) {
                    Log_info("Current leader found: index {}, site_id {}, locale_id {}",
                              i, svr_workers_g[i].site_info_->id, svr_workers_g[i].site_info_->locale_id);
                    return i;
                }
            }
        }
    }
    Log_info("No current leader found");
    return -1;
}

void server_failover_co(bool random, bool leader, int srv_idx)
{
#ifdef FAILOVER_DEBUG
    Log_info("!!!!!!!!!!!!!!!enter server_failover_co");
#endif
    int idx = -1 ;
    int expected_idx = -1 ;
    int run_int = Config::GetConfig()->get_failover_run_interval() ;
    int stop_int = Config::GetConfig()->get_failover_stop_interval() ;

    if (srv_idx != -1)
    {
        expected_idx = srv_idx ;
    }
    else if(!random)
    {
        // temporary solution, assign id 0 as leader, id 1 as follower
        if(leader)
        {
            expected_idx = 0 ;
        }
        else
        {
            expected_idx = 1 ;
        }
    }
    else
    {
        // do nothing
    }

    for(int i=0;i<svr_workers_g.size();i++)
    {
      Log_debug("failover at index {}, id {}, loc id {} part id {}", 
        i, svr_workers_g[i].site_info_->id,
        svr_workers_g[i].site_info_->locale_id,  
        svr_workers_g[i].site_info_->partition_id_ );
    }

    for(int i=0;i<svr_workers_g.size();i++)
    {
        if(svr_workers_g[i].site_info_->locale_id == expected_idx )
        {
            idx = i ;
            break ;
        }
    }    
#ifdef FAILOVER_DEBUG
    Log_info("!!!!!!!!!!!!!!!!! failover_server_quit {}", failover_server_quit);
#endif
    while(!failover_server_quit)
    {
        if(random)
        {
            idx = rand() % svr_workers_g.size() ;
        }
        failover_server_idx = idx ;
#ifdef FAILOVER_DEBUG
        Log_info("!!!!!!!!!!!!!!!!!!!! before run_int wait {} s", run_int);
#endif
        sleep(run_int) ;
        // auto r = create_sp_timeout_event(run_int * 1000 * 1000);
        // r->wait();
#ifdef FAILOVER_DEBUG
        Log_info("!!!!!!!!!!!!!!!!!!!! after run_int wait");
#endif
        if(idx == -1) 
        {
          // TODO other types
          if (!leader) break ;
          idx = 0 ;
        }        
        if(failover_server_quit)
        {
#ifdef FAILOVER_DEBUG
          Log_info("!!!!!!!!!!!!!!!!!!!! quit here");
#endif
          break ;
        }
        for (int i = 0; i < client_workers_g.size() ; ++i)
        {
          failover_triggers[i] = true ;
        }
        // for (int i = 0; i < client_workers_g.size() ; ++i)
        // {
        //   while(failover_triggers[i]) {
        //     if (failover_server_quit) {
        //       Log_info("!!!!!!!!!!!!!!!!!!!! quit here");
        //       return ;
        //     }
        //   }
        // }
        // TODO the idx of client
        idx = find_current_leader();
        if (idx == -1) break; // [Jetpack] If no leader found, do not need to pause.
#ifdef FAILOVER_DEBUG
        Log_info("@@@@@@@@@@@@@@@@@@@@@@@@ before pause {}", svr_workers_g[idx].site_info_->id);
        // Log_info("client_workers_g size: {}", client_workers_g.size());
#endif
        // client_workers_g[0]->Pause(idx) ;
        // Log_info("@@@@@@@@@@@@@@@@@@@@@@@@ client_workers_g paused");
        svr_workers_g[idx].Pause() ;
        Log_info("@@@@@@@@@@@@@@@@@@@@@@@@ svr_workers_g {} paused", idx);
        for (int i = 0; i < client_workers_g.size() ; ++i)
        {
          failover_triggers[i] = true ;
        }
#ifdef FAILOVER_DEBUG
        Log_info("!!!!!!!!!!!!!! before stop_int wait");
#endif
        Log_info("server {} paused for failover test", idx);
        sleep(stop_int) ;
        // auto s = create_sp_timeout_event(stop_int * 1000 * 1000);
        // s->wait() ;      
#ifdef FAILOVER_DEBUG  
        Log_info("!!!!!!!!!!!!!! after stop_int wait");
#endif
        // for (int i = 0; i < client_workers_g.size() ; ++i)
        // {
        //   while(failover_triggers[i]) {
        //     if (failover_server_quit) return ;
        //   }
        // }        
#ifdef FAILOVER_DEBUG
        Log_info("@@@@@@@@@@@@@@@@@@@@@@@@ before resume {}", svr_workers_g[idx].site_info_->id);
#endif
        // client_workers_g[0]->Resume(idx) ;
        Log_info("@@@@@@@@@@@@@@@@@@@@@@@@ failover resumed");
        svr_workers_g[idx].Resume() ;
#ifdef FAILOVER_DEBUG
        Log_info("server {} resumed for failover test", idx);
#endif
        if(leader)
        {
          // get current leader
          idx = failover_server_idx ;
        }
        break; // [Jetpack] Only simulate failure once
    }

}

void server_failover_thread(bool random, bool leader, int srv_idx) {
#ifdef AWS
    sleep(15); // [JetPack] This is add for aws server test, this place need to wait the same time as client_launch_workers
#endif
#ifdef FAILOVER_DEBUG
  Log_info("!!!!!!!!!!!!!!!enter server_failover_thread");
#endif
  Fiber::create_run([&, random, leader, srv_idx]() { 
    server_failover_co(random, leader, srv_idx) ;
  }) ;
}

void server_failover()
{
    bool failover = Config::GetConfig()->get_failover();
    bool random = Config::GetConfig()->get_failover_random() ;
    bool leader = Config::GetConfig()->get_failover_leader() ;
    int idx = Config::GetConfig()->get_failover_srv_idx() ;
    if(failover)
    {
      /*Fiber::create_run([&, random, leader, idx]() { 
        server_failover_co(random, leader, idx) ;
      }) ;*/
      // TODO only consider the partition 0 now
      failover_threads_g.push_back(
          std::thread(&server_failover_thread, random, leader, idx)) ;
    }
}

void setup_ulimit() {
  struct rlimit limit;
  /* Get max number of files. */
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    Log_fatal("getrlimit() failed with errno={}", errno);
  }
  Log_info("ulimit -n is {}", (int)limit.rlim_cur);
}
double getMemoryUsageMB() {
    std::ifstream status_file("/proc/self/status");
    std::string line;
    double rss_kb = 0.0;

    if (!status_file.is_open()) {
        std::cerr << "Error: Could not open /proc/self/status" << std::endl;
        return -1.0;
    }

    while (std::getline(status_file, line)) {
        // Find the line that starts with "VmRSS:"
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line);
            std::string key;
            double value;
            std::string unit;
            iss >> key >> value >> unit;

            if (unit == "kB") {
                rss_kb = value;
            }
            break;
        }
    }

    status_file.close();

    // Convert from kilobytes to megabytes
    return rss_kb / 1024.0;
}
//for AWS test cpu usage monitoring
struct CPUStats { 
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;

    unsigned long long total() const {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }

    unsigned long long idleTime() const {
        return idle + iowait;
    }
};
//for AWS test cpu usage monitoring
CPUStats getCPUStats(int core) {
    std::ifstream file("/proc/stat");
    std::string line;
    CPUStats stats;

    // Skip to the line corresponding to the specified core (e.g., "cpu0", "cpu1", etc.)
    for (int i = 0; i <= core + 1; ++i) {
        std::getline(file, line);
    }
    
    std::istringstream iss(line);
    std::string cpuLabel;
    iss >> cpuLabel >> stats.user >> stats.nice >> stats.system >> stats.idle
        >> stats.iowait >> stats.irq >> stats.softirq >> stats.steal;

    return stats;
}
//for AWS test cpu usage monitoring
double calculateCPUUsage(const CPUStats& oldStats, const CPUStats& newStats) {
    unsigned long long totalDiff = newStats.total() - oldStats.total();
    unsigned long long idleDiff = newStats.idleTime() - oldStats.idleTime();
    // Log_info("idlediff : {:.4f}, totaldiff : {:.4f}",static_cast<double>(idleDiff) ,static_cast<double>(totalDiff));
    return 100.0 * (1.0 - static_cast<double>(idleDiff) / totalDiff);
}
double median(std::vector<double> values) {
    // Ensure the vector is not empty
    if (values.empty()) {
        throw std::invalid_argument("Cannot compute the median of an empty vector.");
    }

    // Sort the vector
    std::sort(values.begin(), values.end());

    size_t size = values.size();
    if (size % 2 == 0) {
        // If even number of elements, return the average of the two middle elements
        return (values[size / 2 - 1] + values[size / 2]) / 2.0;
    } else {
        // If odd number of elements, return the middle element
        return values[size / 2];
    }
}
//for AWS test cpu usage monitoring
vector<double> getUsage(int core_id, int duration){
  // Get initial CPU stats
  int first_phase = duration / 3;
  int second_phase = 2 * (duration / 3);
   
  std::vector<double> cpu_usages;
  //addition: add memory usage to this function
  std::vector<double> memory_usage;
  for (int i = 0; i < duration; ++i) {
      // Measure CPU usage if within the middle third of the duration
      if (i >= first_phase && i < second_phase) {
          CPUStats oldStats = getCPUStats(core_id);
          std::this_thread::sleep_for(std::chrono::seconds(1)); // Wait 1 second
          memory_usage.push_back(getMemoryUsageMB());
          CPUStats newStats = getCPUStats(core_id);
          double cpuUsage = calculateCPUUsage(oldStats, newStats);
          cpu_usages.push_back(cpuUsage);
          continue;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  cpu_usages.push_back(median(memory_usage));
  return cpu_usages;

}

int main(int argc, char *argv[]) {
  check_current_path();
  Log_info("starting process {}", getpid());
  setup_ulimit();

  // read configuration
  int ret = Config::CreateConfig(argc, argv);
  if (ret == SUCCESS) {
    Log_info("Read config finish");
  } else {
    Log_fatal("Read config failed");
    return ret;
  }



  auto client_infos = Config::GetConfig()->GetMyClients();
  Log_info("!!!!!!!!!!!! client_infos size {}", client_infos.size());
  if (client_infos.size() > 0) {
    client_setup_heartbeat(client_infos.size());
  }

#ifdef CPU_PROFILE
  char prof_file[1024];
  Config::GetConfig()->GetProfilePath(prof_file);
  // start to profile
  ProfilerStart(prof_file);
  Log_info("started to profile cpu");
#endif // ifdef CPU_PROFILE
  auto server_infos = Config::GetConfig()->GetMyServers();
  // std::cout << "!!!!!!!!!!!!!!!" << server_infos[0].id << " " << server_infos[0].locale_id << std::endl;
  if (!server_infos.empty()) {
    server_launch_worker(server_infos);
    server_failover() ;
  } else {
    Log_info("no servers on this process");
  }

  if (!client_infos.empty()) {
    //client_setup_heartbeat(client_infos.size());
#ifdef AWS
    sleep(15); // Give remote servers time to initialize before starting clients.
#endif
    Log_info("!!!!!!!!!!!!! before client_launch_workers(client_infos);");
    client_launch_workers(client_infos);

#ifdef AWS
    int server_core_id = 1;
    std::vector<double> cpu_usage = getUsage(server_core_id, Config::GetConfig()->duration_);
    double memory_during_test = cpu_usage[cpu_usage.size() - 1];
    Log_info("CORE {} USAGE: ", server_core_id);
    for(int i = 0; i < cpu_usage.size(); i++){
      Log_info("{:.4f}", cpu_usage[i]);
    }
    Log_info("server median : {:.3f}", median(cpu_usage));
    Log_info("memory during test: {:.3f}", memory_during_test);
#endif
#ifndef AWS

    sleep(Config::GetConfig()->duration_);
#endif
    wait_for_clients();
    failover_server_quit = true;
    Log_info("all clients have shut down.");
  }
  Log_info("Total throughtput is {:.2f}", total_throughput);
#ifdef DB_CHECKSUM
  sleep(90); // hopefully servers can finish hanging RPCs in 90 seconds.
#endif
  sleep(10); // hopefully servers can finish reset of work in 10 seconds

  for (auto& worker : svr_workers_g) {
    worker.WaitForShutdown();
  }

  Log_info("After worker.WaitForShutdown();");

  for (auto& ft : failover_threads_g) {
    ft.join();
  }

#ifdef DB_CHECKSUM
  map<parid_t, vector<int>> checksum_results = {};
  for (auto& worker : svr_workers_g) {
    auto p = worker.site_info_->partition_id_;
    int sum = worker.DbChecksum();
    checksum_results[p].push_back(sum);
    Log_info("partition {} checksum {}", p, sum);
  }
  bool checksum_fail = false;
  for (auto& pair : checksum_results) {
    auto& vec = pair.second;
    for (auto checksum: vec) {
      if (checksum != vec[0]) {
        checksum_fail = true;
      }
    }
  }
  if (checksum_fail) {
    Log_warn("checksum match failed...perhaps wait longer before checksum?");
  } else {
    Log_info("checksum success");
  }
#endif
#ifdef CPU_PROFILE
  // stop profiling
  ProfilerStop();
#endif // ifdef CPU_PROFILE
  client_shutdown();
  Log_info("After client_shutdown");
  
  Log_info("Request-latency statistics {}", request_latency.statistics().c_str());
  Log_info("Request-latency distribution {}", request_latency.distribution().c_str());
  Log_info("Mid throughput is {:.2f}", request_latency.count() / (Config::GetConfig()->duration_ / 3.0));
  string dump_file_name = "results/recent_csv/" + Config::GetConfig()->exp_setting_name_ + ".csv";
  std::ofstream file(dump_file_name);
  if (!file.is_open()) {
    Log_info("Failed to open file for writing {}", dump_file_name.c_str());
  } else {
    file << "Request-Latency,Start-Time,End2End-Latency\n";
    size_t max_size = std::max(request_latency.count(), commit_time.size());
    std::sort(commit_time.begin(),
              commit_time.end(),
              [](auto const& a, auto const& b) {
                  return a.first < b.first;
              });
    for (size_t i = 0; i < max_size; ++i) {
        if (i < request_latency.count())
          file << request_latency.data_[i];
        file << ",";
        if (i < commit_time.size()) {
          file << commit_time[i].first;
          file << ",";
          file << commit_time[i].second;
        }
        file << "\n";
    }
    Log_info("Dumped to {} with {} lines data", dump_file_name.c_str(), max_size);
    file.close();
    Log_info("{} closed", dump_file_name.c_str());
  }

#ifdef LATENCY_DEBUG
  Log_info("client2leader 50pct {:.2f} 90pct {:.2f} 99pct {:.2f}", client2leader.pct50(), client2leader.pct90(), client2leader.pct99());
  Log_info("client2test_point 50pct {:.2f} 90pct {:.2f} 99pct {:.2f}", client2test_point.pct50(), client2test_point.pct90(), client2test_point.pct99());
  Log_info("client2leader_send 50pct {:.2f} 90pct {:.2f} 99pct {:.2f}", client2leader_send.pct50(), client2leader_send.pct90(), client2leader_send.pct99());
#endif
  server_shutdown();
  // TODO, FIXME pending_future in rpc cause error.
  fflush(stderr);
  fflush(stdout);
  exit(0);
  return 0;
  
  Log_info("all server workers have shut down.");

  RandomGenerator::destroy();
  Config::DestroyConfig();

  Log_debug("exit process.");

  return 0;
}
