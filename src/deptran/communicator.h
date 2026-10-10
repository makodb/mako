#pragma once

#include "__dep__.h"
#include "constants.h"
#include "config.h"

#include <memory>
#include <mutex>
#include <utility>

// rusty::Vec is a vec_port C++20 MODULE, not a header: <rusty/vec.hpp> is
// empty and says so. src/deptran/raft/server.h imports it from a header for
// the same reason -- a DSL struct in a header names rusty::Vec, so every TU
// that includes the header needs the module, and the header is the only place
// that can guarantee it. Global scope, before `namespace janus`: inside it,
// the import would name janus::rusty and shadow ::rusty for the whole file.
import rusty;
#include <rusty/arc.hpp>
#include <rusty/option.hpp>
#include <rusty/array.hpp>   // rusty::len, which the emitted loops call
#include <rusty/move.hpp>    // rusty::clone, which the emitted lookups call
#include <rusty/rusty.hpp>   // rusty::sync::atomic::AtomicBool and Ordering
#include <rusty/slice.hpp>   // rusty::detail::deref_if_pointer_like
#include <rusty/sync/atomic.hpp>

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

}  // namespace janus

// The carriers PeerRegistry names, in the namespace the emitted code spells.
// Same contract as src/deptran/raft/rust_facade_types.h: inline mode has no
// --type-map, so a foreign type reaches C++ under the exact path the Rust
// writes, and both languages have to agree on one name. ReactorPollThread is
// declared identically there; a repeated identical alias is well-formed, so
// both headers may appear in one translation unit.
namespace rusty {
using ReactorPollThread = ::srpc::PollThread;
using CommoPeerPtr = ::std::shared_ptr<::janus::RpcPeer>;
}  // namespace rusty

// The Rust side models CommoPeerPtr as sixteen opaque bytes
// (src/rusty-rustc/src/lib.rs). It never looks inside one; these pin the claim
// rather than leaving it a convention.
static_assert(sizeof(rusty::CommoPeerPtr) == 16,
              "CommoPeerPtr's rustc-side carrier pins 16 bytes");
static_assert(alignof(rusty::CommoPeerPtr) == 8,
              "CommoPeerPtr's rustc-side carrier pins align 8");

namespace janus {

// ===========================================================================
// The peer table, written once in Rust and compiled twice.
//
// WHY IT IS HERE AND NOT IN A COMMUNICATOR SUBCLASS. `Communicator` is a
// data-carrying base with two subclasses -- MultiPaxosCommo
// (src/deptran/paxos/commo.h) and RaftCommo (src/deptran/raft/commo.h) -- and
// Rust has no implementation inheritance. Flattening the base into each
// subclass gives two definitions that can drift, and only one of the two
// engines is being converted. So the base's DATA moves into one Rust-authored
// value type that the base holds by composition, while the base itself stays
// a C++ class with the same name and the same public surface. Nothing about
// Paxos changes.
//
// WHY A DSL BLOCK AND NOT A RUSTC-ONLY MODULE. This one source is compiled by
// both toolchains: rusty-cpp translates it into the C++ below, which is what
// both engines link, and the same block is extracted for rustc into the Raft
// crate. That is the mechanism src/deptran/scheduler.h already uses for the
// TxLogServer interface. One definition, two lanes, nothing duplicated.
//
// WHAT CHANGED IN THE DATA. The C++ stored each peer TWICE -- once in
// `peers_` keyed by site, and again inside `partition_peers_[par]` -- so a
// peer's membership lived in two containers that had to agree, and the
// `belongs_to_partition` scan existed to check they did. Here a partition
// owns site ids and the peer table owns peers, so there is exactly one place
// a peer can be, and "is this site in this partition" is answered from the
// ids alone.
//
// WHY VECTORS AND NOT MAPS. `std::map` became a linear scan over a contiguous
// vector. A replica group is a handful of sites -- five in the lab config --
// and at that size a scan of packed u16s beats a red-black tree's pointer
// chasing. It is also what the DSL can express faithfully in both lanes:
// rusty::Vec re-exports std::vec::Vec for rustc and is the real vec_port for
// C++, whereas rusty::BTreeMap's rustc facade has no remove and no mutable
// get (the note beside PeerTable in src/deptran/raft/server.h).
// ===========================================================================

#if RUSTYCPP_RUST
// One partition's membership, as site ids in config order.
pub struct PartitionSites {
    pub par_id: u32,
    pub sites: rusty::Vec<u16>,
}

// One site's endpoint. `peer` is carried, never followed: the Rust side
// models it as opaque bytes and only hands it back to C++, which is where
// RpcPeer::WithClient takes the request lock.
pub struct PeerEntry {
    pub site_id: u16,
    pub peer: rusty::CommoPeerPtr,
}

pub struct PeerRegistry {
    entries: rusty::Vec<PeerEntry>,
    partitions: rusty::Vec<PartitionSites>,
    // Named net_enabled, not network_enabled: a field and a method may not
    // share a name, or the emitter renames the field out from under callers.
    net_enabled: rusty::sync::atomic::AtomicBool,
    rpc_poll: rusty::Option<rusty::Arc<rusty::ReactorPollThread>>,
    owns_poll: bool,
}

impl PeerRegistry {
    pub fn new(poll: rusty::Option<rusty::Arc<rusty::ReactorPollThread>>,
               owns: bool) -> PeerRegistry {
        PeerRegistry {
            entries: rusty::Vec::new(),
            partitions: rusty::Vec::new(),
            net_enabled: rusty::sync::atomic::AtomicBool::new(true),
            rpc_poll: poll,
            owns_poll: owns,
        }
    }

    // --- construction, once per process at startup

    // False when the partition was already declared, which the C++ asserted
    // through `partition_peers_.emplace(...).second`.
    pub fn begin_partition(&mut self, par_id: u32) -> bool {
        let mut i: usize = 0;
        while i < self.partitions.len() {
            if self.partitions[i].par_id == par_id {
                return false;
            }
            i += 1;
        }
        self.partitions.push(PartitionSites {
            par_id,
            sites: rusty::Vec::new(),
        });
        true
    }

    // False when the site is already known, which the C++ asserted through
    // `peers_.emplace(...).second`, or when the partition was never begun.
    pub fn add_peer(&mut self, par_id: u32, site_id: u16,
                    peer: rusty::CommoPeerPtr) -> bool {
        let mut i: usize = 0;
        while i < self.entries.len() {
            if self.entries[i].site_id == site_id {
                return false;
            }
            i += 1;
        }
        let mut p: usize = 0;
        while p < self.partitions.len() {
            if self.partitions[p].par_id == par_id {
                self.partitions[p].sites.push(site_id);
                self.entries.push(PeerEntry { site_id, peer });
                return true;
            }
            p += 1;
        }
        false
    }

    // --- the network-enabled flag, which the lab suite toggles to cut a
    // replica off. Release/Acquire, as the std::atomic_bool it replaces used.

    pub fn set_network_enabled(&self, enabled: bool) {
        self.net_enabled.store(enabled, rusty::sync::atomic::Ordering::Release);
    }

    pub fn network_enabled(&self) -> bool {
        self.net_enabled.load(rusty::sync::atomic::Ordering::Acquire)
    }

    // --- topology

    pub fn has_partition(&self, par_id: u32) -> bool {
        let mut i: usize = 0;
        while i < self.partitions.len() {
            if self.partitions[i].par_id == par_id {
                return true;
            }
            i += 1;
        }
        false
    }

    pub fn site_in_partition(&self, par_id: u32, site_id: u16) -> bool {
        let mut i: usize = 0;
        while i < self.partitions.len() {
            if self.partitions[i].par_id == par_id {
                let mut j: usize = 0;
                while j < self.partitions[i].sites.len() {
                    if self.partitions[i].sites[j] == site_id {
                        return true;
                    }
                    j += 1;
                }
                return false;
            }
            i += 1;
        }
        false
    }

    // --- selection. Both honour the network flag, exactly where the C++
    // PeersForPartition and PeerForSite tested NetworkEnabled() first.

    pub fn peers_for_partition(&self, par_id: u32)
        -> rusty::Vec<rusty::CommoPeerPtr> {
        let mut out: rusty::Vec<rusty::CommoPeerPtr> = rusty::Vec::new();
        if !self.network_enabled() {
            return out;
        }
        let mut i: usize = 0;
        while i < self.partitions.len() {
            if self.partitions[i].par_id == par_id {
                let mut j: usize = 0;
                while j < self.partitions[i].sites.len() {
                    let mut k: usize = 0;
                    while k < self.entries.len() {
                        if self.entries[k].site_id == self.partitions[i].sites[j] {
                            out.push(self.entries[k].peer.clone());
                        }
                        k += 1;
                    }
                    j += 1;
                }
                return out;
            }
            i += 1;
        }
        out
    }

    pub fn peer_for_site(&self, par_id: u32, site_id: u16)
        -> rusty::Option<rusty::CommoPeerPtr> {
        if !self.network_enabled() {
            return rusty::None;
        }
        if !self.site_in_partition(par_id, site_id) {
            return rusty::None;
        }
        self.peer_by_site(site_id)
    }

    // Ungated by the network flag and by partition: ReconnectToSite repairs a
    // connection whether or not traffic is currently allowed through it.
    pub fn peer_by_site(&self, site_id: u16)
        -> rusty::Option<rusty::CommoPeerPtr> {
        let mut i: usize = 0;
        while i < self.entries.len() {
            if self.entries[i].site_id == site_id {
                return rusty::Some(self.entries[i].peer.clone());
            }
            i += 1;
        }
        rusty::None
    }

    // --- teardown and the poll thread

    pub fn peer_count(&self) -> usize {
        self.entries.len()
    }

    pub fn peer_at(&self, index: usize) -> rusty::CommoPeerPtr {
        self.entries[index].peer.clone()
    }

    pub fn clear(&mut self) {
        self.entries.clear();
        self.partitions.clear();
    }

    // One clone, no is_some/unwrap dance. Cloning the Option IS the handle
    // copy in both lanes: std's Option<Arc<T>>::clone for rustc, and
    // rusty::clone -- whose SFINAE falls back to copy construction -- for
    // C++, which is a refcount bump either way. Writing it as
    // `if is_some() { Some(as_ref().unwrap().clone()) }` is what clippy's
    // unnecessary_unwrap rejects, and `if let` is the shape the emitter
    // lowers with a dot instead of an arrow (the TODO on
    // ReplicationWakeGate::wake_on_owner in src/deptran/raft/shell/server_h.rs).
    pub fn poll_thread(&self)
        -> rusty::Option<rusty::Arc<rusty::ReactorPollThread>> {
        self.rpc_poll.clone()
    }

    pub fn owns_poll_thread(&self) -> bool {
        self.owns_poll
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=deptran_communicator.peer_registry version=1 rust_sha256=c541ad23bac51bb035e29f87ef372cbd5165a10d55ff888cd8a8ef0a54f1a7cb*/
struct PartitionSites;
struct PeerEntry;
struct PeerRegistry;

struct PartitionSites {
    uint32_t par_id;
    rusty::Vec<uint16_t> sites;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct PeerEntry {
    uint16_t site_id;
    rusty::CommoPeerPtr peer;
};

struct PeerRegistry {
    rusty::Vec<PeerEntry> entries;
    rusty::Vec<PartitionSites> partitions;
    rusty::sync::atomic::AtomicBool net_enabled;
    rusty::Option<rusty::Arc<rusty::ReactorPollThread>> rpc_poll;
    bool owns_poll;

    static PeerRegistry new_(rusty::Option<rusty::Arc<rusty::ReactorPollThread>> poll, bool owns);
    bool begin_partition(uint32_t par_id);
    bool add_peer(uint32_t par_id, uint16_t site_id, rusty::CommoPeerPtr peer);
    void set_network_enabled(bool enabled) const;
    bool network_enabled() const;
    bool has_partition(uint32_t par_id) const;
    bool site_in_partition(uint32_t par_id, uint16_t site_id) const;
    rusty::Vec<rusty::CommoPeerPtr> peers_for_partition(uint32_t par_id) const;
    rusty::Option<rusty::CommoPeerPtr> peer_for_site(uint32_t par_id, uint16_t site_id) const;
    rusty::Option<rusty::CommoPeerPtr> peer_by_site(uint16_t site_id) const;
    size_t peer_count() const;
    rusty::CommoPeerPtr peer_at(size_t index) const;
    void clear();
    rusty::Option<rusty::Arc<rusty::ReactorPollThread>> poll_thread() const;
    bool owns_poll_thread() const;
};


inline PeerRegistry PeerRegistry::new_(rusty::Option<rusty::Arc<rusty::ReactorPollThread>> poll, bool owns) {
    return PeerRegistry{.entries = rusty::Vec<PeerEntry>::new_(), .partitions = rusty::Vec<PartitionSites>::new_(), .net_enabled = rusty::sync::atomic::AtomicBool::new_(true), .rpc_poll = std::move(poll), .owns_poll = std::move(owns)};
}

inline bool PeerRegistry::begin_partition(uint32_t par_id) {
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(this->partitions)) {
        if (rusty::detail::deref_if_pointer_like(this->partitions[i].par_id) == rusty::detail::deref_if_pointer_like(par_id)) {
            return false;
        }
        i += 1;
    }
    this->partitions.push(PartitionSites{.par_id = std::move(par_id), .sites = rusty::Vec<uint16_t>::new_()});
    return true;
}

inline bool PeerRegistry::add_peer(uint32_t par_id, uint16_t site_id, rusty::CommoPeerPtr peer) {
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(this->entries)) {
        if (rusty::detail::deref_if_pointer_like(this->entries[i].site_id) == rusty::detail::deref_if_pointer_like(site_id)) {
            return false;
        }
        i += 1;
    }
    size_t p = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(p) < rusty::len(this->partitions)) {
        if (rusty::detail::deref_if_pointer_like(this->partitions[p].par_id) == rusty::detail::deref_if_pointer_like(par_id)) {
            this->partitions[p].sites.push(std::move(site_id));
            this->entries.push(PeerEntry{.site_id = std::move(site_id), .peer = std::move(peer)});
            return true;
        }
        p += 1;
    }
    return false;
}

inline void PeerRegistry::set_network_enabled(bool enabled) const {
    this->net_enabled.store(std::move(enabled), rusty::sync::atomic::Ordering::Release);
}

inline bool PeerRegistry::network_enabled() const {
    return this->net_enabled.load(rusty::sync::atomic::Ordering::Acquire);
}

inline bool PeerRegistry::has_partition(uint32_t par_id) const {
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(this->partitions)) {
        if (rusty::detail::deref_if_pointer_like(this->partitions[i].par_id) == rusty::detail::deref_if_pointer_like(par_id)) {
            return true;
        }
        i += 1;
    }
    return false;
}

inline bool PeerRegistry::site_in_partition(uint32_t par_id, uint16_t site_id) const {
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(this->partitions)) {
        if (rusty::detail::deref_if_pointer_like(this->partitions[i].par_id) == rusty::detail::deref_if_pointer_like(par_id)) {
            size_t j = static_cast<size_t>(0);
            while (rusty::detail::deref_if_pointer_like(j) < rusty::len(this->partitions[i].sites)) {
                if (this->partitions[i].sites[j] == rusty::detail::deref_if_pointer_like(site_id)) {
                    return true;
                }
                j += 1;
            }
            return false;
        }
        i += 1;
    }
    return false;
}

inline rusty::Vec<rusty::CommoPeerPtr> PeerRegistry::peers_for_partition(uint32_t par_id) const {
    rusty::Vec<rusty::CommoPeerPtr> out = rusty::Vec<rusty::CommoPeerPtr>::new_();
    if (!this->network_enabled()) {
        return std::move(out);
    }
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(this->partitions)) {
        if (rusty::detail::deref_if_pointer_like(this->partitions[i].par_id) == rusty::detail::deref_if_pointer_like(par_id)) {
            size_t j = static_cast<size_t>(0);
            while (rusty::detail::deref_if_pointer_like(j) < rusty::len(this->partitions[i].sites)) {
                size_t k = static_cast<size_t>(0);
                while (rusty::detail::deref_if_pointer_like(k) < rusty::len(this->entries)) {
                    if (rusty::detail::deref_if_pointer_like(this->entries[k].site_id) == this->partitions[i].sites[j]) {
                        out.push(rusty::clone(this->entries[k].peer));
                    }
                    k += 1;
                }
                j += 1;
            }
            return std::move(out);
        }
        i += 1;
    }
    return std::move(out);
}

inline rusty::Option<rusty::CommoPeerPtr> PeerRegistry::peer_for_site(uint32_t par_id, uint16_t site_id) const {
    if (!this->network_enabled()) {
        return rusty::None;
    }
    if (!this->site_in_partition(std::move(par_id), std::move(site_id))) {
        return rusty::None;
    }
    return this->peer_by_site(std::move(site_id));
}

inline rusty::Option<rusty::CommoPeerPtr> PeerRegistry::peer_by_site(uint16_t site_id) const {
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(this->entries)) {
        if (rusty::detail::deref_if_pointer_like(this->entries[i].site_id) == rusty::detail::deref_if_pointer_like(site_id)) {
            return rusty::Option<rusty::CommoPeerPtr>(rusty::clone(this->entries[i].peer));
        }
        i += 1;
    }
    return rusty::None;
}

inline size_t PeerRegistry::peer_count() const {
    return rusty::len(this->entries);
}

inline rusty::CommoPeerPtr PeerRegistry::peer_at(size_t index) const {
    return rusty::clone(this->entries[index].peer);
}

inline void PeerRegistry::clear() {
    this->entries.clear();
    this->partitions.clear();
}

inline rusty::Option<rusty::Arc<rusty::ReactorPollThread>> PeerRegistry::poll_thread() const {
    return rusty::clone(this->rpc_poll);
}

inline bool PeerRegistry::owns_poll_thread() const {
    return this->owns_poll;
}
/*RUSTYCPP:GEN-END id=deptran_communicator.peer_registry*/

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
    registry_.set_network_enabled(enabled);
  }
  bool NetworkEnabled() const { return registry_.network_enabled(); }

  // Return an owned PollThread handle without exposing communicator storage.
  rusty::Option<rusty::Arc<srpc::PollThread>> PollThread() const {
    return registry_.poll_thread();
  }

 protected:
  using Peer = rusty::CommoPeerPtr;
  using Peers = rusty::Vec<Peer>;

  Peers PeersForPartition(parid_t par_id) const {
    return registry_.peers_for_partition(par_id);
  }
  Peer PeerForSite(parid_t par_id, siteid_t site_id) const;

 private:
  rusty::Option<rusty::Arc<srpc::Client>> ConnectToAddress(
      const std::string& address,
      std::chrono::milliseconds timeout) const;

  // The five data members this class used to carry -- rpc_poll_,
  // owns_poll_thread_, peers_, partition_peers_ and network_enabled_ -- are
  // the fields of PeerRegistry above, which Rust owns.
  PeerRegistry registry_;
};

}  // namespace janus
