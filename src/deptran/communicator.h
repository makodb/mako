#pragma once

#include "__dep__.h"
#include "constants.h"
#include "config.h"

#include <memory>
#include <mutex>
#include <utility>

#include <rusty/arc.hpp>
#include <rusty/option.hpp>

namespace janus {

// A stable, protocol-neutral endpoint. Generated protocol proxies are cheap
// non-owning wrappers and are constructed on the stack by derived
// communicators while WithClient holds request-side synchronization.
class RpcPeer {
 public:
  RpcPeer(siteid_t site_id,
          std::string address,
          rusty::Arc<srpc::Client> client)
      : site_id_(site_id),
        address_(std::move(address)),
        client_(std::move(client)) {}

  RpcPeer(const RpcPeer&) = delete;
  RpcPeer& operator=(const RpcPeer&) = delete;

  siteid_t site_id() const { return site_id_; }
  const std::string& address() const { return address_; }

  template <typename Fn>
  decltype(auto) WithClient(Fn&& fn) const {
    std::lock_guard<std::mutex> lock(request_mutex_);
    return std::forward<Fn>(fn)(
        const_cast<srpc::Client*>(client_.get()));
  }

 private:
  friend class Communicator;

  void ReplaceClient(rusty::Arc<srpc::Client> client);
  void Close();

  const siteid_t site_id_;
  const std::string address_;
  mutable std::mutex request_mutex_;
  std::mutex reconnect_mutex_;
  bool closed_ = false;  // guarded by reconnect_mutex_
  rusty::Arc<srpc::Client> client_;
};

class Communicator {
 public:
  static constexpr int CONNECT_TIMEOUT_MS = 120 * 1000;
  static constexpr int CONNECT_SLEEP_MS = 1000;

  explicit Communicator(
      rusty::Option<rusty::Arc<srpc::PollThread>> rpc_poll = rusty::None);
  virtual ~Communicator();

  Communicator(const Communicator&) = delete;
  Communicator& operator=(const Communicator&) = delete;

  // Replaces a peer's connection only after the new connection succeeds.
  // The partition argument is retained for the existing Raft recovery RPC
  // boundary and is validated against the immutable partition topology.
  bool ReconnectToSite(siteid_t site_id, parid_t par_id);

  void SetNetworkEnabled(bool enabled) {
    network_enabled_.store(enabled, std::memory_order_release);
  }
  bool NetworkEnabled() const {
    return network_enabled_.load(std::memory_order_acquire);
  }

  // Return an owned PollThread handle without exposing communicator storage.
  rusty::Option<rusty::Arc<srpc::PollThread>> PollThread() const;

 protected:
  using Peer = std::shared_ptr<RpcPeer>;
  using Peers = std::vector<Peer>;

  Peers PeersForPartition(parid_t par_id) const;
  Peer PeerForSite(parid_t par_id, siteid_t site_id) const;

 private:
  rusty::Option<rusty::Arc<srpc::Client>> ConnectToAddress(
      const std::string& address,
      std::chrono::milliseconds timeout) const;

  rusty::Option<rusty::Arc<srpc::PollThread>> rpc_poll_;
  bool owns_poll_thread_ = false;
  std::map<siteid_t, Peer> peers_;
  std::map<parid_t, Peers> partition_peers_;
  std::atomic_bool network_enabled_{true};
};

}  // namespace janus
