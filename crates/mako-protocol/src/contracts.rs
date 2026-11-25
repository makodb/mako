//! Backend contracts; see docs/developer/upper-layer-contracts.md.
use crate::wire::*;
use std::time::Duration;

/// Owned request/reply transport. Replies may be lost and a failed call may
/// already have executed remotely. Implementations authenticate endpoints and
/// never silently retry transaction INSTALL requests. Per-request identity
/// validation belongs to the protocol caller. See the contract document.
pub trait Transport: Send + Sync {
    fn call(&self, endpoint: &str, message: Message, timeout: Duration) -> Result<Reply>;

    /// Discovery observes authority but does not pin it. A transaction selects
    /// its endpoint once, then retains that participant for the entire attempt.
    fn find_leader(&self, owner: u32, replicas: &ReplicaSet, timeout: Duration) -> Result<String> {
        for endpoint in &replicas.endpoints {
            if let Ok(Reply::ReplicaStatus(status)) = self.call(
                endpoint,
                Message::ReplicaStatus,
                timeout.min(Duration::from_millis(500)),
            ) {
                if status.owner == owner
                    && status.group == Some(replicas.group)
                    && status.leader
                    && status.healthy
                    && status.durable_receipts
                {
                    return Ok(endpoint.clone());
                }
            }
        }
        Err(Error::new(
            ErrorCode::RouteChanged,
            "no healthy leader in configured replica group",
        ))
    }
}

/// One affine local transaction worker. The protocol has already validated
/// identity, payload bounds and sequencing when it calls execute. `begin`
/// requires cleanup of the previous uninstalled attempt, fresh admission and
/// retained table/epoch ownership. Validation certifies all observations;
/// install success includes the configured log and terminal publication.
/// Other failures complete definite local cleanup or report uncertainty.
/// The implementation owns rollback on drop and must retain protection after
/// an uncertain outcome even when the request lane is destroyed.
pub trait ParticipantEngine {
    fn minimum_term(&self) -> u32;
    fn execute(&mut self, request: &Request, begin: bool) -> Result<Value>;
    fn release_terminal(&mut self);
    fn retain_uncertain(&mut self);
}

pub trait CommitLog: Send + Sync {
    /// None selects the store's explicit in-memory transaction journal. A
    /// persistence/replication adapter exposes its authoritative journal here.
    fn transaction_journal(&self) -> Option<&dyn crate::journal::Journal> {
        None
    }
    fn health(&self) -> Result<()> {
        Ok(())
    }
    fn shutdown(&self) {}
    fn replica_status(&self, owner: u32) -> ReplicaStatus {
        ReplicaStatus {
            owner,
            group: None,
            leader: true,
            term: 0,
            healthy: true,
            durable_receipts: false,
        }
    }
    fn retains_gateway_receipts(&self) -> bool {
        false
    }
    fn gateway_receipt(&self, _: &GatewayRequest) -> Result<Option<GatewayResponse>> {
        Ok(None)
    }
    fn append_gateway(&self, bytes: &[u8], time_term: u32, _: &GatewayReceipt) -> Result<()> {
        self.append(bytes, time_term)
    }
    fn admit(&self) -> Result<Box<dyn CommitPermit>> {
        Ok(Box::new(()))
    }
    /// Return success only after the configured durability contract is met.
    /// A failure occurs after local installation and therefore fences the lane.
    fn append(&self, bytes: &[u8], time_term: u32) -> Result<()>;
}
pub trait CommitPermit: Send + Sync {
    fn check(&self) -> Result<()>;
}
impl CommitPermit for () {
    fn check(&self) -> Result<()> {
        Ok(())
    }
}
/// Explicit policy for a non-replicated, non-persistent shard.
pub struct VolatileLog;
impl CommitLog for VolatileLog {
    fn append(&self, _: &[u8], _: u32) -> Result<()> {
        Ok(())
    }
}
