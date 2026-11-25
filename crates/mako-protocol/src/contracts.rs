//! Backend contracts; see docs/developer/upper-layer-contracts.md.
use crate::wire::*;
use std::time::Duration;

/// Owned request/reply transport. Replies may be lost and a failed call may
/// already have executed remotely. Implementations use configured trusted peers
/// and never silently replay transaction bodies. Per-request identity
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

/// One affine execution worker. The lane validates identity, bounds and sequence
/// before execute. A new attempt requires cleanup of previous unprepared work,
/// fresh admission and retained table/epoch ownership. The native runtime handles
/// Lock, Prepare and Decide outside this execution interface. Frozen work must
/// remain owned by the retained service until its saved outcome completes;
/// ordinary request or listener teardown cannot roll it back.
pub trait ParticipantEngine {
    /// A frozen participant must reject further execution without replacing its
    /// retained admission/receipt state. Terminal 2PC commands have a separate
    /// transaction identity and are dispatched by the retained worker service.
    fn accepts(&self, _action: &Action) -> bool {
        true
    }
    fn minimum_term(&self) -> u32;
    fn execute(&mut self, request: &Request, begin: bool) -> Result<Value>;
    fn release_terminal(&mut self);
    fn retain_uncertain(&mut self);
}

/// Shared transaction-log contract for MemoryLog, LocalLog and ReplicatedLog.
/// Every backend owns its journal; callers never synthesize a memory fallback.
pub trait CommitLog: crate::journal::Journal {
    /// Publish a recoverable engine/protocol checkpoint and compact the log.
    fn checkpoint(&self) -> Result<(u64, u64)> {
        Err(crate::wire::Error::new(
            crate::wire::ErrorCode::Invalid,
            "this log has no durable checkpoint",
        ))
    }
    /// Serialize physical installation with checkpoint export. The callback
    /// runs on the participant's affine worker, exactly once. Persistent logs
    /// retain the saved Complete effect until this publication succeeds.
    fn publish(
        &self,
        _: crate::journal::TransactionId,
        effect: &mut dyn FnMut() -> Result<()>,
    ) -> Result<()> {
        effect()
    }
    fn ready(&self) -> bool {
        self.health().is_ok()
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
