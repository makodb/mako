#include "__dep__.h"
#include "server_worker.h"

using namespace janus;

namespace {

std::vector<ServerWorker> server_workers;

void CheckCurrentPath() {
  const auto path = std::filesystem::current_path();
  Log_info("PWD: {}", path.string().c_str());
}

void CheckFileDescriptorLimit() {
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    Log_fatal("getrlimit() failed with errno={}", errno);
  }
  Log_info("ulimit -n is {}", static_cast<int>(limit.rlim_cur));
}

void PinServerThread(std::thread& thread,
                     int core_id,
                     const Config::SiteInfo& site) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  const int rc = pthread_setaffinity_np(thread.native_handle(),
                                        sizeof(cpu_set_t),
                                        &cpuset);
  if (rc != 0) {
    Log_warn("Could not pin server site {} to core {}: error {}",
             site.id, core_id, rc);
    return;
  }
  Log_info("Pinned server site {} to core {}", site.id, core_id);
}

// Launch the five embedded Raft servers. Each worker must create its scheduler
// and RPC service before any worker enters SetupCommo(): the Raft lab harness
// verifies that all five schedulers exist when its first communicator starts.
// SetupCommo() on site 0 then owns the reactor loop until the lab test fiber
// finishes and clears Reactor::looping_.
void RunServers(const std::vector<Config::SiteInfo>& server_sites) {
  auto* const config = Config::GetConfig();
  verify(config != nullptr);

  Log_info("Raft lab server enabled, number of sites: {}",
           server_sites.size());
  server_workers.resize(server_sites.size());

  std::mutex setup_mutex;
  std::condition_variable setup_cv;
  size_t services_ready = 0;
  std::vector<std::thread> threads;
  threads.reserve(server_sites.size());

#ifdef SIMULATE_WAN
  int core_id = 5;
#else
  int core_id = 1;
#endif

  for (size_t index = 0; index < server_sites.size(); ++index) {
    const siteid_t site_id = server_sites[index].id;
    threads.emplace_back([&, index, site_id]() {
      auto& worker = server_workers[index];
      worker.site_info_ =
          const_cast<Config::SiteInfo*>(&config->SiteById(site_id));

      Log_info("Launching Raft lab site {:x}, bind address {}",
               site_id, worker.site_info_->GetBindAddress().c_str());
      worker.SetupBase();
      worker.SetupService();

      {
        std::unique_lock<std::mutex> lock(setup_mutex);
        ++services_ready;
        if (services_ready == server_sites.size()) {
          setup_cv.notify_all();
        } else {
          setup_cv.wait(lock, [&]() {
            return services_ready == server_sites.size();
          });
        }
      }

      Log_info("Starting communication for Raft lab site {}",
               static_cast<int>(site_id));
      worker.SetupCommo();
      worker.launched_ = true;
      Log_info("Raft lab site {} stopped", static_cast<int>(site_id));
    });

    PinServerThread(threads.back(), core_id, server_sites[index]);
#ifndef AWS
    ++core_id;
#endif
  }

  Log_info("Waiting for the Raft lab reactor and server threads");
  for (auto& thread : threads) {
    thread.join();
  }
  Log_info("Raft lab server threads stopped");
}

void ShutdownServers() {
  for (auto& worker : server_workers) {
    worker.ShutDown();
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  CheckCurrentPath();
  Log_info("Starting Raft lab process {}", getpid());
  CheckFileDescriptorLimit();

  const int ret = Config::CreateConfig(argc, argv);
  if (ret != SUCCESS) {
    Log_fatal("Failed to read Raft lab configuration");
    return ret;
  }

  auto* const config = Config::GetConfig();
  verify(config != nullptr);

  const auto client_sites = config->GetMyClients();
  if (!client_sites.empty()) {
    Log_error("The Raft lab executable is server-only; found {} client sites",
              client_sites.size());
    Config::DestroyConfig();
    return FAILURE;
  }

  const auto server_sites = config->GetMyServers();
  if (server_sites.empty()) {
    Log_error("The Raft lab executable has no server sites to run");
    Config::DestroyConfig();
    return FAILURE;
  }

  RunServers(server_sites);
  ShutdownServers();

  RandomGenerator::destroy();
  Config::DestroyConfig();
  Log_info("Raft lab process finished");
  return SUCCESS;
}
