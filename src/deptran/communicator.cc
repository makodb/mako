#include <stddef.h>
#include <stdint.h>

#include "communicator.h"

import std;

namespace janus {

void RpcPeer::ReplaceClient(rusty::Arc<srpc::Client> client) {
  std::unique_lock<std::mutex> lock(request_mutex_);
  rusty::Arc<srpc::Client> old_client = std::move(client_);
  client_ = std::move(client);
  lock.unlock();
  old_client->close();
}

void RpcPeer::Close() {
  std::lock_guard<std::mutex> reconnect_lock(reconnect_mutex_);
  if (closed_) {
    return;
  }
  closed_ = true;
  std::lock_guard<std::mutex> request_lock(request_mutex_);
  client_->close();
}

namespace {

// The poll thread the registry will own: the caller's if it gave one,
// otherwise a fresh one this communicator is then responsible for shutting
// down. A free function because it has to run before the member initialiser
// list reaches registry_, which has no default constructor.
// @unsafe - srpc::PollThread::create is reactor API.
rusty::Option<rusty::Arc<srpc::PollThread>> ResolvePollThread(
    const rusty::Option<rusty::Arc<srpc::PollThread>>& given) {
  if (given.is_none()) {
    return rusty::Some(srpc::PollThread::create());
  }
  return rusty::Some(given.as_ref().unwrap().clone());
}

}  // namespace

// @unsafe - Config walk and ConnectToAddress; the table itself is Rust's.
Communicator::Communicator(
    rusty::Option<rusty::Arc<srpc::PollThread>> poll_thread_worker)
    : registry_(PeerRegistry::new_(ResolvePollThread(poll_thread_worker),
                                   poll_thread_worker.is_none())) {
  Log_info("setup replication communicator");

  auto config = Config::GetConfig();
  verify(config != nullptr);
  for (const auto par_id : config->GetAllPartitionIds()) {
    // Bound to a local first: `verify` must not be handed a side-effecting
    // call, or a build that compiles it out silently skips the insert.
    const bool partition_is_new = registry_.begin_partition(par_id);
    verify(partition_is_new);
    for (auto& site : config->SitesByPartitionId(par_id)) {
      auto connected = ConnectToAddress(
          site.GetHostAddr(), std::chrono::milliseconds(CONNECT_TIMEOUT_MS));
      verify(connected.is_some());
      auto peer = std::make_shared<RpcPeer>(
          site.id, site.GetHostAddr(), connected.unwrap());
      const bool site_is_new =
          registry_.add_peer(par_id, site.id, std::move(peer));
      verify(site_is_new);
    }
  }
}

// @unsafe - client teardown and poll-thread shutdown.
Communicator::~Communicator() {
  SetNetworkEnabled(false);
  const size_t peer_count = registry_.peer_count();
  for (size_t i = 0; i < peer_count; i++) {
    registry_.peer_at(i)->Close();
  }
  registry_.clear();

  auto poll = registry_.poll_thread();
  if (poll.is_some() && registry_.owns_poll_thread()) {
    Log_info("[COMMUNICATOR] Shutting down owned poll thread");
    poll.as_ref().unwrap()->shutdown();
  }
}

// @unsafe - srpc::Client connect/close, chrono and sleep. A kernel: this is
// the one operation in this file the DSL genuinely cannot express.
rusty::Option<rusty::Arc<srpc::Client>> Communicator::ConnectToAddress(
    const std::string& address,
    std::chrono::milliseconds timeout) const {
  auto poll = registry_.poll_thread();
  verify(poll.is_some());
  auto client = srpc::Client::create(poll.as_ref().unwrap());
  const auto start = std::chrono::steady_clock::now();
  int attempt = 0;

  do {
    Log_debug("connect to site: {} (attempt {})", address.c_str(), attempt++);
    if (client->connect(
            reinterpret_cast<const int8_t*>(address.c_str()), false) == SUCCESS) {
      Log_info("connect to site: {} success!", address.c_str());
      return rusty::Some(std::move(client));
    }
    if (timeout.count() <= 0) {
      break;
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(CONNECT_SLEEP_MS));
  } while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start) < timeout);

  Log_warn("timeout connecting to {}", address.c_str());
  client->close();
  return rusty::None;
}

// @safe - one registry lookup; the null convention the callers test is
// restored here from the Option the registry returns.
Communicator::Peer Communicator::PeerForSite(
    parid_t par_id, siteid_t site_id) const {
  auto found = registry_.peer_for_site(par_id, site_id);
  if (found.is_none()) {
    return nullptr;
  }
  return found.unwrap();
}

// @unsafe - takes the peer's reconnect mutex and reconnects.
bool Communicator::ReconnectToSite(siteid_t site_id, parid_t par_id) {
  if (!registry_.has_partition(par_id)) {
    Log_error("[RECONNECT] Unknown partition {} for site {}", par_id, site_id);
    return false;
  }

  auto found = registry_.peer_by_site(site_id);
  if (found.is_none()) {
    Log_error("[RECONNECT] Unknown site {} for partition {}", site_id, par_id);
    return false;
  }
  if (!registry_.site_in_partition(par_id, site_id)) {
    Log_error("[RECONNECT] Site {} is not in partition {}", site_id, par_id);
    return false;
  }
  const auto peer = found.unwrap();

  // Only one replacement attempt per peer. The request mutex is deliberately
  // not held while connect retries, so existing traffic can keep using the old
  // client until a replacement is ready.
  std::lock_guard<std::mutex> reconnect_lock(peer->reconnect_mutex_);
  if (peer->closed_) {
    Log_warn("[RECONNECT] Site {} peer is already closed", site_id);
    return false;
  }
  Log_info("[RECONNECT] Attempting to reconnect to site {} at {}",
           site_id, peer->address().c_str());
  // Recovery notifications run on an RPC poll thread. Make exactly one
  // non-sleeping attempt here; startup construction retains the bounded
  // retry loop above, while later notifications can retry independently.
  auto connected = ConnectToAddress(
      peer->address(), std::chrono::milliseconds(0));
  if (connected.is_none()) {
    Log_error("[RECONNECT] Failed to reconnect to site {}; retaining old client",
              site_id);
    return false;
  }

  peer->ReplaceClient(connected.unwrap());
  Log_info("[RECONNECT] Successfully reconnected to site {}", site_id);
  return true;
}

}  // namespace janus
