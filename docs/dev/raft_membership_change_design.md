# Raft Membership Change Design

## Overview

This document describes the single-server membership change protocol for the
Mako Raft implementation, based on Section 4 of the Raft dissertation. The
protocol allows adding or removing one server at a time from the cluster.

Single-server changes are preferred over joint consensus because they are
simpler to implement and reason about. When only one server changes at a time,
there is no need for overlapping quorum phases or intermediate joint
configurations. The safety guarantees hold directly from the quorum overlap
property.

## Safety Argument

The key invariant is: **at most one server changes at a time.** This ensures
that the old and new configurations always share a majority, preventing
split-brain.

For a cluster of N servers:
- Quorum of old config (N servers): ceil(N/2) + 1 ... but more precisely, majority = floor(N/2) + 1
- Quorum of new config (N+1 servers for add, N-1 for remove): floor((N+1)/2) + 1 or floor((N-1)/2) + 1

In either case, any majority of the old config and any majority of the new
config must overlap by at least one server. This shared server guarantees that
two leaders cannot be elected simultaneously under different configurations,
because at least one server would have to vote for both -- which Raft's
term-based voting prevents.

Example: a 3-node cluster (majority = 2) adds a 4th node (majority = 3). Any
set of 2 from {A,B,C} and any set of 3 from {A,B,C,D} must share at least 1
member. This holds because 2 + 3 > 4.

## Protocol Steps

### AddServer

1. Leader receives `AddServer(new_server_id, new_server_address)` RPC.
2. If a configuration change is already pending, reject the request.
3. Leader begins replicating log entries (or an InstallSnapshot) to the new
   server to bring it up to date. The new server is a non-voting learner during
   this phase.
4. Once the new server is sufficiently caught up (its match_index is within a
   configurable number of rounds of the leader's log), the leader appends a
   **configuration change entry** to the log. This entry contains the new
   server list including the added server.
5. The new configuration takes effect when the entry is **appended** (not
   committed). From this point, the leader requires a majority of the new
   config for commits.
6. Once the configuration entry is committed, the change is permanent. The
   leader responds success to the client.

### RemoveServer

1. Leader receives `RemoveServer(server_id)` RPC.
2. If a configuration change is already pending, reject the request.
3. Leader appends a configuration change entry with the new server list
   (excluding the removed server).
4. The new configuration takes effect on append. The leader requires a majority
   of the new (smaller) config for subsequent commits.
5. Once the configuration entry is committed, the change is permanent.
6. If the leader itself is being removed, it steps down after committing the
   entry and stops sending heartbeats. A remaining server will time out and
   start an election.

## Configuration Entry Format

Configuration changes are stored as special Raft log entries distinguishable
from regular data entries.

```
Entry {
    term: uint64_t,
    index: uint64_t,
    type: EntryType::CONFIGURATION,  // new enum value
    data: ConfigurationData {
        version: uint64_t,           // monotonically increasing config version
        servers: vector<ServerInfo>,  // the complete new server list
    }
}

ServerInfo {
    site_id: uint32_t,
    address: string,                 // host:port
}
```

The entry type enum (currently supporting DATA entries) gains a CONFIGURATION
variant. On log replay and recovery, configuration entries are applied to
reconstruct the active server set.

## Quorum Calculation Changes

**Before any config change:** quorum = majority of current_config_.

**During transition (config entry appended but not committed):** the new
configuration takes effect immediately on append. The leader uses the new
config's majority for all subsequent commit decisions. This is safe because:
- If the entry commits, the new config is correct.
- If the leader crashes before commit, the new leader may or may not have the
  entry. If it does not, the old config applies. If it does, it will also use
  the new config, which is safe by the quorum overlap argument.

**After commit:** quorum = majority of new config only. The old config is
discarded.

In code, replace:
```cpp
// Old: static partition size
int quorum = Config::GetConfig()->GetPartitionSize() / 2 + 1;

// New: dynamic config size
int quorum = current_config_.size() / 2 + 1;
```

## Edge Cases

**Leader failure during config change.** If the leader crashes after appending
but before committing the configuration entry, the new leader may or may not
have the entry in its log. If the entry is not present, the old configuration
remains in effect. If present but uncommitted, the new leader will attempt to
commit it (or it may be overwritten if the new leader has a different log). In
either case, safety is maintained because only one config can be committed per
term.

**Multiple concurrent changes.** The leader must reject AddServer/RemoveServer
requests if a configuration change entry is pending (appended but not yet
committed). This is enforced by a `pending_config_change_` flag on the leader.
Concurrent requests receive an error response asking the client to retry later.

**Removing the last server.** Removing a server from a 1-node cluster is
undefined and must be rejected. The minimum cluster size is 1.

**New server crashes during catch-up.** If the new server crashes before the
configuration entry is appended, no harm is done -- it was a non-voting
learner. The leader can clean up the catch-up state and report failure.

**Removed server does not know it is removed.** A removed server may not
receive the committed configuration entry (e.g., if it was partitioned). It
will continue requesting votes, but since it is not in the new config, its
RequestVote RPCs will be ignored by servers using the new config. To prevent
disruption from these stale RPCs, servers should reject RequestVote from
unknown server IDs (not in current_config_).

## Integration Points in Mako

### New RPCs (in rcc_rpc.rpc)

```
AddServer(uint32_t server_id, string address | uint64_t success, string error_msg);
RemoveServer(uint32_t server_id | uint64_t success, string error_msg);
```

These are leader-only RPCs. Non-leaders respond with a redirect to the current
leader.

### RaftServer Fields

```cpp
// Active configuration: set of server IDs in the cluster
std::vector<ServerInfo> current_config_;

// True if a config change entry has been appended but not yet committed
bool pending_config_change_ = false;

// For AddServer: tracks catch-up progress of a server being added
rusty::Option<CatchUpState> catch_up_state_;
```

### Quorum Calculation

All quorum checks in `server.cc` (commit advancement, election vote counting)
must use `current_config_.size()` instead of the static `nservers` or
`Config::GetConfig()->GetPartitionSize()`. This includes:
- `advanceCommitIndex()`: majority calculation for commit
- `handleRequestVoteResponse()`: majority calculation for election
- `HeartbeatLoop()`: determining which servers to send AppendEntries to

### Configuration Persistence

Configuration entries are persisted as part of the Raft log. On recovery
(`RecoverFromStorage()`), the server replays all committed configuration
entries to reconstruct `current_config_`. If a snapshot exists, the snapshot
metadata must also include the configuration at the snapshot index.
