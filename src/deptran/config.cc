#include <std_compat.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include "config.h"
#include "srpc_log.h"

using namespace std;
using namespace srpc;

/*
 * Config owns only process/topology and replication settings. The retired
 * DepTran benchmark/schema/sharding path used to pull MemDB into every Config
 * consumer through __dep__.h; keep those dependencies out of this translation
 * unit as well as out of config.h.
 */

namespace janus {
namespace {
// @safe - in-place ASCII/locale-independent lowercase transform for config keys.
void to_lower_in_place(std::string& s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
}
}  // namespace

Config *Config::config_s = nullptr;
size_t bulkBatchCount=10000;

Config::SiteInfo::SiteInfo(uint32_t id, std::string& site_addr) : id(id) {
  const auto pos = site_addr.find(':');
  verify(pos != std::string::npos);
  name = site_addr.substr(0, pos);
  const std::string port_str = site_addr.substr(pos + 1);
  port = std::stoi(port_str);
}


Config * Config::GetConfig() {
  verify(config_s != nullptr);
  return config_s;
}

int Config::CreateConfig(int argc, char **argv) {
  if (config_s != NULL) return -1;

  vector<string> config_paths;
  std::string proc_name = "localhost"; // default as "localhost"
  std::string exp_setting_name = "not_set"; // used to dump Distribution to file
  // removed `std::string logging_path =
  // "./disk_log/";` local — only fed the now-deleted `logging_path_`
  // field on Config; only assignment via the `-r` CLI flag (also
  // gone in this phase).
  char *end_ptr    = NULL;

  char *hostspath               = NULL;
  char *ctrl_hostname           = NULL;
  char *ctrl_key                = NULL;
  char *ctrl_init               = NULL /*, *ctrl_run = NULL*/;
  uint32_t ctrl_port        = 0;
  uint32_t ctrl_timeout     = 0;
  uint32_t duration         = 10;
  bool heart_beat               = false;
  single_server_t single_server = SS_DISABLED;
  int server_or_client          = -1;
  int32_t tot_req_num           = 10000;
  int16_t n_concurrent          = 1;
  bulkBatchCount = 10000;

  string timeouts;
  size_t pos;

  int c;
  optind = 1;
  string filename;
  // dropped `r:` from getopt string —
  // `case 'r':` (logging path) handler was the only consumer.
  while ((c = getopt(argc, argv, "bc:d:f:h:i:k:p:P:s:S:t:H:T:n:A:F:O:a:N:")) != -1) {
    switch (c) {
      case 'b': // heartbeat to controller
        heart_beat = true;
        break;
      case 'A':  // kPaxosBatchSize
        bulkBatchCount = strtoul(optarg, &end_ptr, 10);
		break;
      case 'd': // duration
        duration = strtoul(optarg, &end_ptr, 10);

        if ((end_ptr == NULL) || (*end_ptr != '\0'))
          return -4;
        break;
      case 'P':
        proc_name = std::string(optarg);
        break;
      case 'N':
        exp_setting_name = std::string(optarg);
        break;
      case 'f': // properties.xml
        filename = std::string(optarg);
        config_paths.push_back(filename);
        break;
      case 't':
        ctrl_timeout = strtoul(optarg, &end_ptr, 10);
        if ((end_ptr == NULL) || (*end_ptr != '\0')) return -4;
        break;
      case 'c': // client id
        // TODO remove
        if ((end_ptr == NULL) || (*end_ptr != '\0'))
          return -4;
        if (server_or_client != -1)
          return -4;
        server_or_client = 1;
        break;
      case 'h': // ctrl_hostname
        // TODO remove
        ctrl_hostname = (char *)malloc((strlen(optarg) + 1) * sizeof(char));
        strcpy(ctrl_hostname, optarg);
        break;
      case 'H': // ctrl_host
        // TODO remove
        hostspath = (char *)malloc((strlen(optarg) + 1) * sizeof(char));
        strcpy(hostspath, optarg);
        break;
      case 'i': // ctrl_init
        // TODO remove
        ctrl_init = (char *)malloc((strlen(optarg) + 1) * sizeof(char));
        strcpy(ctrl_init, optarg);
        break;
      case 'k':
        // TODO remove
        ctrl_key = (char *)malloc((strlen(optarg) + 1) * sizeof(char));
        strcpy(ctrl_key, optarg);
        break;
      // removed `case 'r':` (logging path)
      // CLI flag — `logging_path` local + Config::logging_path_
      // field both gone.
      case 'p':
        // TODO remove
        ctrl_port = strtoul(optarg, &end_ptr, 10);
        if ((end_ptr == NULL) || (*end_ptr != '\0')) return -4;
        break;
      case 's': // site id
        // TODO remove
        if ((end_ptr == NULL) || (*end_ptr != '\0')) return -4;

        if (server_or_client != -1) return -4;
        server_or_client = 0;
        break;
      case 'S': // client touch only single server
      {
        // TODO remove
        int single_server_buf = strtoul(optarg, &end_ptr, 10);

        if ((end_ptr == NULL) || (*end_ptr != '\0')) return -4;

        switch (single_server_buf) {
          case 0:
            single_server = SS_DISABLED;
            break;
          case 1:
            single_server = SS_THREAD_SINGLE;
            break;
          case 2:
            single_server = SS_PROCESS_SINGLE;
            break;
          default:
            return -4;
        }
        break;
      }
      case 'T':
        tot_req_num = strtoul(optarg, &end_ptr, 10);
        if ((end_ptr == NULL) || (*end_ptr != '\0'))
          return -4;
        break;
      case 'n':
        n_concurrent = strtoul(optarg, &end_ptr, 10);
        if ((end_ptr == NULL) || (*end_ptr != '\0'))
          return -4;
        break;
      case '?':
        // TODO remove
        if ((optopt == 'c') ||
            (optopt == 'd') ||
            (optopt == 'f') ||
            (optopt == 'h') ||
            (optopt == 'i') ||
            (optopt == 'k') ||
            (optopt == 'p') ||
            (optopt == 'r') ||
            (optopt == 's') ||
            (optopt == 't')) Log_error("Option -{:c} requires an argument.",
                                       optopt);
        else if (isprint(optopt)) Log_error("Unknown option -{:c}.", optopt);
        else Log_error("Unknown option \\x{:x}", optopt);
        return -2;
      default:
        return -3;
    }
  }

//  if ((server_or_client != 0) && (server_or_client != 1)) return -5;
  verify(config_s == nullptr);
  config_s = new Config(
    ctrl_hostname,
    ctrl_port,
    ctrl_timeout,
    ctrl_key,
    ctrl_init,
    tot_req_num,
    n_concurrent,
    // ctrl_run,
    duration,
    heart_beat,
    single_server);
  config_s->proc_name_ = proc_name;
  config_s->exp_setting_name_ = exp_setting_name;
  config_s->config_paths_ = config_paths;
  config_s->Load();
  return SUCCESS;
}

void Config::DestroyConfig() {
  if (config_s) {
    delete config_s;
    config_s = NULL;
  }
}

//Config::Config() {}

Config::Config(char           *ctrl_hostname,
               uint32_t        ctrl_port,
               uint32_t        ctrl_timeout,
               char           *ctrl_key,
               char           *ctrl_init,
               int32_t         tot_req_num,
               int16_t         n_concurrent,
               uint32_t        duration,
               bool            heart_beat,
               single_server_t single_server) :
  heart_beat_(heart_beat),
  ctrl_hostname_(ctrl_hostname),
  ctrl_port_(ctrl_port),
  ctrl_timeout_(ctrl_timeout),
  ctrl_key_(ctrl_key),

  ctrl_init_(ctrl_init),
  tot_req_num_(tot_req_num),
  duration_(duration),
  config_paths_(vector<string>()),
  tx_proto_(-1),
  proc_id_(0),
  benchmark_(0),
  scale_factor_(1),
  txn_weight_(vector<double>()),
  txn_weights_(map<string, double>()),
  proc_name_(string()),
  exp_setting_name_(string()),
  batch_start_(false),
  // removed `logging_path_(logging_path),`
  // initializer — field gone.
  single_server_(single_server),
  n_concurrent_(n_concurrent),
  max_retry_(1),
  num_site_(0),
  start_coordinator_id_(0),
  site_(vector<string>()),
  site_threads_(vector<uint32_t>()),
  num_coordinator_threads_(1),
  sid_(1),
  cid_(1),
  next_site_id_(0),
  proc_host_map_(map<string, string>())

{
}

void Config::Load() {
  for (auto &name: config_paths_) {
    if (name.ends_with("yml")) {
      LoadYML(name);
    } else {
      verify(0);
    }
  }

}

void Config::LoadYML(std::string &filename) {
  Log_info("{}: {}", __FUNCTION__, filename.c_str());
  const YAML::Node yaml_config = YAML::LoadFile(filename);

  if (yaml_config["process"]) {
    BuildSiteProcMap(yaml_config["process"]);
  }
  if (yaml_config["site"]) {
    LoadSiteYML(yaml_config["site"]);
  }
  if (yaml_config["process"]) {
    LoadProcYML(yaml_config["process"]);
  }
  if (yaml_config["host"]) {
    LoadHostYML(yaml_config["host"]);
  }
  if (yaml_config["mode"]) {
    LoadModeYML(yaml_config["mode"]);
  }
  if (yaml_config["client"]) {
    LoadClientYML(yaml_config["client"]);
  }
  if (yaml_config["failover"]) {
    LoadFailoverYML(yaml_config["failover"]);
  }
  if (yaml_config["n_concurrent"]) {
    n_concurrent_ = yaml_config["n_concurrent"].as<uint16_t>();
    Log_info("# of concurrent requests: {}", n_concurrent_);
  }
  if (yaml_config["n_parallel_dispatch"]) {
    n_parallel_dispatch_ = yaml_config["n_parallel_dispatch"].as<int32_t>();
  }
}

void Config::LoadSiteYML(YAML::Node config) {
  auto servers = config["server"];
  // Start from current sizes to support loading multiple config files
  int partition_id = replica_groups_.size();
  int site_id = sites_.size();
  int locale_id = 0;

  // Note: Using deque for sites_ so no reserve needed - push_back doesn't invalidate pointers

  for (auto server_it = servers.begin(); server_it != servers.end(); server_it++) {
    auto group = *server_it;
    locale_id=0;
    ReplicaGroup replica_group(partition_id);
    for (auto group_it = group.begin(); group_it != group.end(); group_it++) {
      auto site_addr = group_it->as<string>();
      SiteInfo info(site_id++, site_addr);
      info.partition_id_ = replica_group.partition_id;
      info.locale_id = locale_id;
      info.type_ = SERVER;
      info.proc_name = site_proc_map_[info.name];
      if (info.proc_name.compare("localhost")==0) {
        info.role = 0;
      } else if (info.proc_name.compare("learner")==0) {
        info.role = 2;
      } else {
        info.role = 1;
      }
      sites_.push_back(info);
      replica_group.replicas.push_back(&sites_.back());
      locale_id++;
    }
    replica_groups_.push_back(replica_group);
    partition_id++;
  }

  auto clients = config["client"];
  for (auto client_it = clients.begin(); client_it != clients.end(); client_it++) {
    auto group = *client_it;
    int locale_id = 0;
    for (auto group_it = group.begin(); group_it != group.end(); group_it++) {
      auto site_name = group_it->as<string>();
      SiteInfo info(site_id++);
      info.name = site_name;
      info.proc_name = site_proc_map_[info.name];
      if (info.proc_name.compare("localhost")==0) {
        info.role = 0;
      } else if (info.proc_name.compare("learner")==0) {
        info.role = 2;
      } else {
        info.role = 1;
      }
      info.type_ = CLIENT;
      info.locale_id = locale_id;
      info.port = GetClientPort(site_name);
      par_clients_.push_back(info);
      locale_id++;
    }
  }
}

// TODO: inefficient -- do not call during testing
// port assignment should match run.py get_process_info function
int Config::GetClientPort(std::string site_name) {
  auto config = Config::GetConfig();
  std::vector<std::string> sites;
  for (auto site_host_pair : site_proc_map_) {
    sites.push_back(site_host_pair.first);
  }
  verify(sites.size() > 0);
  std::sort(sites.begin(), sites.end());
  std::vector<std::string> hosts;
  for (auto s : sites) {
    if (std::find(hosts.begin(), hosts.end(), site_proc_map_[s]) == hosts.end()) {
      if (s == site_name) {
        return Config::BASE_CLIENT_CTRL_PORT + hosts.size();
      } else {
        hosts.push_back(site_proc_map_[s]);
      }
    }
  }
//  verify(0);
  return -1;
}

void Config::BuildSiteProcMap(YAML::Node process) {
  for (auto it = process.begin(); it != process.end(); it++) {
    auto site_name = it->first.as<string>();
    auto proc_name = it->second.as<string>();

    site_proc_map_[site_name] = proc_name;
  }
}

void Config::LoadProcYML(YAML::Node config) {
  for (auto it = config.begin(); it != config.end(); it++) {
    auto site_name = it->first.as<string>();
    auto proc_name = it->second.as<string>();
    auto info = SiteByName(site_name);
//    verify(info != nullptr);
    if (info != nullptr && proc_name != "") {
      info->proc_name = proc_name;
    }
  }
}

void Config::LoadHostYML(YAML::Node config) {
  for (auto it = config.begin(); it != config.end(); it++) {
    auto proc_name = it->first.as<string>();
    auto host_name = it->second.as<string>();
    proc_host_map_[proc_name] = host_name;
    for (auto& group : replica_groups_) {
      for (auto& server : group.replicas) {
        if (server->proc_name == proc_name) {
          server->host = host_name;
        }
      }
    }
    for (auto& client : par_clients_) {
        if (client.proc_name == proc_name) {
          client.host = host_name;
        }
    }
  }
}

std::string Config::site2host_addr(std::string& siteaddr) {
  auto pos = siteaddr.find_first_of(':');

  verify(pos != std::string::npos);
  std::string sitename = siteaddr.substr(0, pos);
  std::string hostname = site2host_name(sitename);
  std::string hostaddr = siteaddr.replace(0, pos, hostname);
  return hostaddr;
}

std::string Config::site2host_name(std::string& sitename) {
  //    Log_debug("find host name by site name: {}", sitename.c_str());
  auto it = proc_host_map_.find(sitename);

  if (it != proc_host_map_.end()) {
    return it->second;
  } else {
    return sitename;
  }
}


void Config::LoadModeYML(YAML::Node config) {
  if (config["cc"]) {
    Log_error("mode.cc is no longer supported; Mako selects only mode.ab");
    verify(0);
  }
  auto ab_str = config["ab"].as<string>();
  to_lower_in_place(ab_str);
  if (ab_str == "none") {
    replica_proto_ = MODE_NONE;
  } else if (ab_str == "multi_paxos" || ab_str == "paxos") {
    replica_proto_ = MODE_MULTI_PAXOS;
  } else if (ab_str == "raft") {
    replica_proto_ = MODE_RAFT;
  } else {
    Log_error("Unsupported replication mode: {}", ab_str.c_str());
    verify(0);
  }
  max_retry_ = config["retry"].as<int32_t>();
  batch_start_ = config["batch"].as<bool>();
  if (config["timestamp"]) {
    string ts_str = config["timestamp"].as<string>();
    to_lower_in_place(ts_str);
    if (ts_str == "clock") {
      timestamp_ = CLOCK;
    } else if (ts_str == "counter") {
      timestamp_ = COUNTER;
    } else {
      verify(0);
    }
  }
  if (config["jetpack_recovery_batch_size"]) {
    jetpack_recovery_batch_size_ = config["jetpack_recovery_batch_size"].as<int>();
  }
  if (config["txn_timeout_ms"]) {
    // Convert milliseconds to microseconds
    txn_timeout_us_ = static_cast<uint64_t>(config["txn_timeout_ms"].as<int>()) * 1000;
  }
}

void Config::LoadClientYML(YAML::Node client) {
  std::string type = client["type"].as<std::string>();
  std::transform(type.begin(), type.end(), type.begin(), ::tolower);
  if (type == "open") {
    client_type_ = Open;
    client_rate_ = client["rate"].as<int>();
    client_max_undone_ = client["max_undone"].as<int>();
  } else {
    client_type_ = Closed;
    client_rate_ = -1;
    client_max_undone_ = -1;
  }
  forwarding_enabled_ = client["forwarding"].as<bool>(false);
  Log_info("client forwarding: {}", forwarding_enabled_);
}

void Config::LoadFailoverYML(YAML::Node config) {
  auto mode_str = config["method"].as<string>();
  to_lower_in_place(mode_str);
  auto fail_srv_str = config["failserver"].as<string>();
  to_lower_in_place(mode_str);
  failover_srv_idx_ = -1;
  if (mode_str == "none") {
    failover_ = false;
  } else {
    failover_ = true;
    failover_soft_ = mode_str == "soft";
    failover_random_ = fail_srv_str == "random";
    if (!failover_random_) {
      failover_leader_ = fail_srv_str == "leader";
      if (!failover_leader_ && fail_srv_str != "follower") {
        // should be the server index
        std::istringstream(fail_srv_str) >> failover_srv_idx_;
      }
    }
  }
  failover_run_int_ = config["run_interval"].as<int32_t>();
  failover_stop_int_ = config["stop_interval"].as<int32_t>();
}

Config::~Config() {
  if (ctrl_hostname_) {
    free(ctrl_hostname_);
    ctrl_hostname_ = NULL;
  }

  if (ctrl_key_) {
    free(ctrl_key_);
    ctrl_key_ = NULL;
  }

  if (ctrl_init_) {
    free(ctrl_init_);
    ctrl_init_ = NULL;
  }
}

unsigned int Config::get_site_id() {
  verify(0);
  return sid_;
}

unsigned int Config::get_client_id() {
  verify(0);
  return cid_;
}

unsigned int Config::get_ctrl_port() {
  return ctrl_port_;
}

unsigned int Config::get_ctrl_timeout() {
  return ctrl_timeout_;
}

const char * Config::get_ctrl_hostname() {
  return ctrl_hostname_;
}

const char * Config::get_ctrl_key() {
  return ctrl_key_;
}

const char * Config::get_ctrl_init() {
  return ctrl_init_;
}

// TODO obsolete
int Config::get_all_site_addr(std::vector<std::string>& servers) {
    const int num_servers = this->NumSites();
    for (int i=0; i<num_servers; i++) {
      auto& site = const_cast<SiteInfo&>(SiteById(i));
      servers.push_back(site.GetHostAddr());
    }
    return num_servers;
}

int Config::get_site_addr(unsigned int sid, std::string& server) {
  auto site = SiteById(sid);
  server.assign(site.GetHostAddr());
  return 1;
}

int Config::NumSites(SiteInfoType type) {
  if (type == SERVER) {
    return sites_.size();
  } else {
    return par_clients_.size();
  }
}

const Config::SiteInfo& Config::SiteById(uint32_t id) {
  verify(id >= 0);
  Config::SiteInfo* s;
  if (id<sites_.size()) {
    s = &sites_[id];
  } else {
    verify((id-sites_.size()) < par_clients_.size());
    s = &par_clients_[id-sites_.size()];
  }
  verify(s->id==id);
  return *s;
}

Config::SiteInfo Config::LeaderSiteByPartitionId(parid_t partition_id) {
  for (SiteInfo& site : sites_) {
    if (site.partition_id_ == partition_id && site.role == 0) {
      return site;
    }
  }
  verify(0);
}

void Config::UpgradeFromLearnerToLeader() {
  verify(proc_name_.compare("learner")==0); 
  for (auto &s: sites_) {
    if (s.proc_name.compare("learner")==0) {
      s.role=0;
    }
    if (s.proc_name.compare("localhost")==0) {
      s.role=2; // to skip old leader if failure occurs
    }
  }
}

void Config::UpgradeFromP1ToLeader() {
  verify(proc_name_.compare("p1")==0); 
  for (auto &s: sites_) {
    if (s.proc_name.compare("p1")==0) {
      s.role=0;
    }
    if (s.proc_name.compare("localhost")==0) {
      s.role=2; // to skip old leader if failure occurs
    }
  }
}

std::vector<Config::SiteInfo> Config::SitesByPartitionId(
    parid_t partition_id) {
  std::vector<SiteInfo> result;
  auto it = find_if(replica_groups_.begin(), replica_groups_.end(),
                    [partition_id](const ReplicaGroup& g) {
                      return g.partition_id == partition_id;
                    });
  if (it != replica_groups_.end()) {
    for (int i=0; i<it->replicas.size();i++) {
      result.push_back(*it->replicas[i]);
    }
    return result;
  }
  verify(0);
}


std::vector<int> Config::SiteIdsByPartitionId(parid_t partition_id){
  std::vector<int> result;
  auto it = find_if(replica_groups_.begin(), replica_groups_.end(),
                    [partition_id](const ReplicaGroup& g) {
                      return g.partition_id == partition_id;
                    });
  if (it != replica_groups_.end()) {
    for (auto si : it->replicas) {
      result.push_back(si->id);
    }
    return result;
  }
  verify(0);
}

//add another method here that gets a vector of id's

int Config::GetPartitionSize(parid_t partition_id) {
  auto it = find_if(replica_groups_.begin(), replica_groups_.end(),
                    [partition_id](const ReplicaGroup& g) {
                      return g.partition_id == partition_id;
                    });
  if (it != replica_groups_.end()) {
    return it->replicas.size(); 
  }
  verify(0);
}

std::vector<Config::SiteInfo>
Config::SitesByLocaleId(uint32_t locale_id, SiteInfoType type) {
  std::vector<SiteInfo> result;
  if (type == SERVER) {
    for (SiteInfo& site : sites_) {
      if (site.locale_id == locale_id) {
        result.push_back(site);
      }
    }
  } else {
    for (SiteInfo& site : par_clients_) {
      if (site.locale_id == locale_id) {
        result.push_back(site);
      }
    }
  }
  return result;
}

vector<Config::SiteInfo>
Config::SitesByProcessName(string proc_name, Config::SiteInfoType type) {
  //Log_info("SitesByProcessName proc_name={} type={}", proc_name.c_str(), type==SERVER);
  std::vector<SiteInfo> result;
  auto processFunc = [&proc_name, &result](SiteInfo& site) {
    if (site.proc_name == "") {
      Log_fatal("cannot find proc name for site {}", site.name.c_str());
    }
    if (site.proc_name == proc_name) {
      result.push_back(site);
    }
  };

  if (type == SERVER) {
    for (SiteInfo& site : sites_) {
      processFunc(site);
    }
  } else {
    for (SiteInfo& site : par_clients_) {
      processFunc(site);
    }
  }
  return result;
}

Config::SiteInfo* Config::SiteByName(std::string name) {
  for (SiteInfo& site : sites_) {
    if (site.name == name) {
      return &site;
    }
  }
  for (auto& client : par_clients_) {
    if (client.name == name) {
      return &client;
    }
  }
  return nullptr;
}

int Config::get_threads(unsigned int& threads) {
  verify(0);
  if (site_threads_.size() == 0) return -1;

  if (sid_ >= num_site_) return -2;
  threads = site_threads_[sid_];
  return 0;
}

unsigned int Config::get_duration() {
  return duration_;
}

bool Config::do_heart_beat() {
  return heart_beat_;
}

unsigned int Config::get_num_threads() {
  verify(num_coordinator_threads_ > 0);
  return num_coordinator_threads_;
}

unsigned int Config::get_start_coordinator_id() {
  return start_coordinator_id_;
}

int Config::benchmark() {
  return benchmark_;
}

unsigned int Config::GetNumPartition() {
  // TODO FIXME this should be number of partition.
  return replica_groups_.size();
//  return GetMyServers().size();
}

unsigned int Config::get_scale_factor() {
  return scale_factor_;
}

int32_t Config::get_max_retry() {
  return max_retry_;
}

Config::single_server_t Config::get_single_server() {
  return single_server_;
}

unsigned int Config::get_concurrent_txn() {
  return n_concurrent_;
}

bool Config::get_batch_start() {
  return batch_start_;
}

std::vector<double>& Config::get_txn_weight() {
  return txn_weight_;
}

std::map<string, double>& Config::get_txn_weights() {
  return txn_weights_;
};

int Config::GetProfilePath(char *prof_file) {
  if (prof_file == NULL) return -1;
  return sprintf(prof_file, "process-%s.prof", proc_name_.c_str());
}

// removed `Config::do_logging()` and
// `Config::log_path()` — only call site of `do_logging` was the
// now-deleted `else if (do_logging())` branch in
// `SchedulerClassic::Prepare`; `log_path` had no callers.
// removed `logging_path_` field, its
// constructor parameter, and the `-r` CLI flag (handled in the
// getopt loop).

bool Config::IsReplicated() {
  return (replica_proto_ != MODE_NONE);
}

int32_t Config::get_tot_req() {
  return tot_req_num_;
}

}
