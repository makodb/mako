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

Communicator::Communicator(
    rusty::Option<rusty::Arc<srpc::PollThread>> poll_thread_worker) {
  Log_info("setup replication communicator");
  if (poll_thread_worker.is_none()) {
    rpc_poll_ = rusty::Some(srpc::PollThread::create());
    owns_poll_thread_ = true;
  } else {
    rpc_poll_ = rusty::Some(
        poll_thread_worker.as_ref().unwrap().clone());
  }

  auto config = Config::GetConfig();
  verify(config != nullptr);
  for (const auto par_id : config->GetAllPartitionIds()) {
    Peers partition;
    for (auto& site : config->SitesByPartitionId(par_id)) {
      auto connected = ConnectToAddress(
          site.GetHostAddr(), std::chrono::milliseconds(CONNECT_TIMEOUT_MS));
      verify(connected.is_some());
      auto peer = std::make_shared<RpcPeer>(
          site.id, site.GetHostAddr(), connected.unwrap());
      const auto inserted = peers_.emplace(site.id, peer);
      verify(inserted.second);
      partition.push_back(std::move(peer));
    }
    const auto inserted = partition_peers_.emplace(
        par_id, std::move(partition));
    verify(inserted.second);
  }
}

Communicator::~Communicator() {
  SetNetworkEnabled(false);
  for (auto& [site_id, peer] : peers_) {
    (void)site_id;
    peer->Close();
  }
  partition_peers_.clear();
  peers_.clear();

  if (rpc_poll_.is_some() && owns_poll_thread_) {
    Log_info("[COMMUNICATOR] Shutting down owned poll thread");
    rpc_poll_.as_ref().unwrap()->shutdown();
  }
}

rusty::Option<rusty::Arc<srpc::Client>> Communicator::ConnectToAddress(
    const std::string& address,
    std::chrono::milliseconds timeout) const {
  verify(rpc_poll_.is_some());
  auto client = srpc::Client::create(rpc_poll_.as_ref().unwrap());
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

Communicator::Peers Communicator::PeersForPartition(parid_t par_id) const {
  if (!NetworkEnabled()) {
    return {};
  }
  const auto it = partition_peers_.find(par_id);
  if (it == partition_peers_.end()) {
    return {};
  }
  return it->second;
}

Communicator::Peer Communicator::PeerForSite(
    parid_t par_id, siteid_t site_id) const {
  if (!NetworkEnabled()) {
    return nullptr;
  }
  const auto partition_it = partition_peers_.find(par_id);
  if (partition_it == partition_peers_.end()) {
    return nullptr;
  }
  const auto it = peers_.find(site_id);
  if (it == peers_.end()) {
    return nullptr;
  }
  const auto belongs_to_partition = std::any_of(
      partition_it->second.begin(), partition_it->second.end(),
      [site_id](const Peer& peer) { return peer->site_id() == site_id; });
  if (!belongs_to_partition) {
    return nullptr;
  }
  return it->second;
}

bool Communicator::ReconnectToSite(siteid_t site_id, parid_t par_id) {
  const auto partition_it = partition_peers_.find(par_id);
  if (partition_it == partition_peers_.end()) {
    Log_error("[RECONNECT] Unknown partition {} for site {}", par_id, site_id);
    return false;
  }

  const auto peer_it = peers_.find(site_id);
  if (peer_it == peers_.end()) {
    Log_error("[RECONNECT] Unknown site {} for partition {}", site_id, par_id);
    return false;
  }
  const auto& peer = peer_it->second;
  const auto belongs_to_partition = std::find_if(
      partition_it->second.begin(), partition_it->second.end(),
      [site_id](const Peer& candidate) {
        return candidate->site_id() == site_id;
      });
  if (belongs_to_partition == partition_it->second.end()) {
    Log_error("[RECONNECT] Site {} is not in partition {}", site_id, par_id);
    return false;
  }

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

rusty::Option<rusty::Arc<srpc::PollThread>> Communicator::PollThread() const {
  if (rpc_poll_.is_none()) {
    return rusty::None;
  }
  return rusty::Some(rpc_poll_.as_ref().unwrap().clone());
}

}  // namespace janus
