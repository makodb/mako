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
