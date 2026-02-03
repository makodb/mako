# Phase 4.1: Pre-Vote Protocol

## Problem Statement

In standard Raft, a partitioned node will repeatedly time out and increment its term. When the partition heals, this node's higher term can disrupt the cluster:
1. The returning node has a higher term
2. The current leader sees the higher term and steps down
3. A new election is triggered unnecessarily

Pre-Vote prevents this by requiring a "pre-election" check before incrementing the term.

## Design

### Pre-Vote Protocol

1. When election timeout fires, don't immediately start election
2. Instead, send PreVote requests with:
   - Current term (NOT incremented)
   - Last log index and term
3. Other nodes grant pre-vote if:
   - Candidate's log is at least as up-to-date
   - Node hasn't heard from a valid leader recently
4. If candidate gets majority pre-votes:
   - Increment term
   - Start real election with RequestVote
5. If pre-vote fails:
   - Stay follower
   - Don't increment term

### New RPC: PreVote

```cpp
struct PreVoteRequest {
    uint64_t term;           // Candidate's current term (NOT incremented)
    siteid_t candidate_id;
    uint64_t last_log_index;
    uint64_t last_log_term;
};

struct PreVoteResponse {
    uint64_t term;           // Responder's current term
    bool vote_granted;       // True if pre-vote granted
};
```

### Implementation Changes

1. **RaftServer::RequestVote()** - Add pre-vote phase:
   - First send PreVote requests
   - Only start real election if pre-vote succeeds

2. **RaftServer::OnPreVote()** - Handle pre-vote requests:
   - Grant if candidate's log is up-to-date
   - Grant only if no recent leader heartbeat

3. **RaftCommo** - Add PreVote RPC:
   - SendPreVote()
   - PreVoteQuorum handling

4. **Configuration** - Pre-vote can be optional:
   - Enable/disable via config
   - Default: enabled

### Key Considerations

1. **Leader Lease Integration**: Pre-vote should consider leader lease
   - Don't grant pre-vote if leader lease is valid

2. **Election Timeout**: Pre-vote uses same timeout as regular election

3. **Backwards Compatibility**: Pre-vote is optional extension

## Files Modified

- `src/deptran/raft/server.h`: Add pre-vote state and handlers
- `src/deptran/raft/server.cc`: Implement pre-vote logic
- `src/deptran/raft/commo.h`: Add PreVote RPC declarations
- `src/deptran/raft/commo.cc`: Implement PreVote RPC
- `src/deptran/raft/raft_rpc.rpc`: Add PreVote RPC definition

## LOC Estimate

~150 LOC for pre-vote implementation

## Next Steps

- Phase 4.2: Leader Lease for linearizable reads
