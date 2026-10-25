//! Drives the actual durable native actors and transfer loops through the
//! conditional file-backed interfaces. No control response or row scan is faked.
use super::native::directory::RouteTable;
use super::native::durable_coordinator::{DurableCoordinator, Operation, Response, ResumeAction, ResumeWork};
use super::native::durable_kv::{Admission, Namespace};
use super::native::durable_participant::DurableParticipant;
use super::native::migration::Request;
use super::native::participant::{Cleanup, ControlResult};
use super::native::types::{Boundary, Certificate, Command, Grant, KeyRange, MigrationPlan, Phase, Row, Status, TxnId};
use super::engine::DiskEngine;
use super::store::DiskKv;
use std::path::Path;

pub const TABLE: u64 = 31;
pub const NONCE: TxnId = TxnId { client: 9001, sequence: 1 };
pub fn range() -> KeyRange { KeyRange { table: TABLE, lo: b"m".to_vec(), hi: Some(b"n".to_vec()) } }
pub fn row(key: &[u8], value: &[u8]) -> Row {
    Row { coordinate: key.to_vec(), key: key.to_vec(), value: value.to_vec() }
}
pub fn tables() -> Vec<RouteTable> {
    vec![RouteTable { table: TABLE, boundaries: vec![Boundary { start: vec![], grant: Grant { owner: 0, ownership_version: 0 } }] }]
}
pub fn copy_plan(plan: &MigrationPlan) -> MigrationPlan {
    MigrationPlan { generation: plan.generation, nonce: plan.nonce, source: plan.source,
        destination: plan.destination, range: KeyRange { table: plan.range.table,
            lo: plan.range.lo.clone(), hi: plan.range.hi.clone() },
        old: plan.old.iter().map(|boundary| Boundary { start: boundary.start.clone(), grant: boundary.grant }).collect() }
}

pub struct Owner {
    pub kv: DiskKv,
    pub engine: DiskEngine,
    pub participant: DurableParticipant,
    pub admission: Admission,
}
impl Owner {
    fn open(root: &Path, owner: u32, fresh: bool) -> Self {
        let base = root.join(format!("owner-{owner}"));
        let mut kv = DiskKv::open(&base.join("system"));
        let mut engine = DiskEngine::open(&base.join("data"), owner);
        let mut participant = if fresh {
            DurableParticipant::create(&mut kv, owner, tables()).ok().expect("participant create")
        } else {
            DurableParticipant::recover(&mut kv, &mut engine, owner).ok().expect("participant recovery")
        };
        let admission = participant.activate(&mut kv, &mut engine).ok().expect("participant admission");
        Self { kv, engine, participant, admission }
    }
}
pub struct Cluster {
    pub coordinator: DurableCoordinator,
    pub admission: Admission,
    pub owners: [Owner; 2],
    read_sequence: u64,
}
impl Cluster {
    pub fn open(root: &Path, fresh: bool) -> Self {
        let mut owners = [Owner::open(root, 0, fresh), Owner::open(root, 1, fresh)];
        let namespace = Namespace { owner: 0, coordinator: true };
        let mut coordinator = if fresh {
            DurableCoordinator::create(&mut owners[0].kv, namespace, vec![0, 1], tables())
                .ok().expect("coordinator create")
        } else {
            DurableCoordinator::recover(&mut owners[0].kv, namespace).ok().expect("coordinator recovery")
        };
        let admission = coordinator.activate(&mut owners[0].kv).ok().expect("coordinator admission");
        Self { coordinator, admission, owners, read_sequence: 0 }
    }
    pub fn seed(&mut self) {
        self.owners[0].engine.seed(TABLE, row(b"a", b"left"));
        self.owners[0].engine.seed(TABLE, row(b"m\0", b"first\0value"));
        self.owners[0].engine.seed(TABLE, row(b"m\0x", b"second\xffvalue"));
        self.owners[0].engine.seed(TABLE, row(b"mzz", b"third"));
        self.owners[0].engine.seed(TABLE, row(b"n", b"right"));
        self.owners[1].engine.seed(TABLE, row(b"m-stale", b"destination-only"));
    }
    pub fn execute(&mut self, operation: Operation) -> Result<Response, String> {
        self.coordinator.execute(&mut self.owners[0].kv, &self.admission, operation)
            .map_err(|_| "coordinator operation rejected or fenced".to_owned())
    }
    pub fn begin(&mut self, nonce: TxnId, source: u32, destination: u32) -> Result<u64, String> {
        match self.execute(Operation::Begin { nonce, source, destination, range: range() })? {
            Response::Begun(generation) => Ok(generation),
            _ => Err("begin returned a non-generation response".to_owned()),
        }
    }
    pub fn plan(&self, generation: u64) -> MigrationPlan {
        copy_plan(self.coordinator.actor(&self.admission).ok().expect("active coordinator")
            .plan(generation).expect("retained migration plan"))
    }
    pub fn phase(&self) -> Option<Phase> {
        self.coordinator.actor(&self.admission).ok().expect("active coordinator").current_phase()
    }
    pub fn route(&self, coordinate: &[u8]) -> Grant {
        self.coordinator.actor(&self.admission).ok().expect("active coordinator")
            .lookup(TABLE, coordinate).expect("canonical table")
    }
    fn control(&mut self, generation: u64, owner: u32, command: Command) -> Result<ControlResult, String> {
        let plan = self.plan(generation);
        let owner = &mut self.owners[owner as usize];
        owner.participant.bind_plan(&mut owner.kv, &owner.admission, plan)
            .map_err(|_| "immutable plan binding failed")?;
        let result = owner.participant.deliver(&mut owner.kv, &owner.admission, generation, command)
            .map_err(|_| "participant control rejected or fenced")?;
        if result.status != Status::Ok { return Err("native control guard rejected".to_owned()); }
        if !matches!(result.cleanup, Cleanup::None) {
            owner.participant.execute_cleanup(&mut owner.kv, &mut owner.engine, &owner.admission, generation)
                .map_err(|_| "cleanup incomplete or unknown".to_owned())
        } else { Ok(result) }
    }
    fn receive(&mut self, generation: u64, owner: u32, result: ControlResult, expected: Certificate) -> Result<(), String> {
        if result.status != Status::Ok || result.certificate != Some(expected) {
            return Err("native participant did not emit the required certificate".to_owned());
        }
        self.execute(Operation::Receive { generation, owner, certificate: expected })?;
        Ok(())
    }
    fn final_copy(&mut self, generation: u64, destination: u32) -> Result<ControlResult, String> {
        self.control(generation, destination, Command::Final)?;
        let source_owner = self.coordinator.actor(&self.admission).ok().expect("active coordinator")
            .plan(generation).expect("retained migration plan").source;
        let (first, second) = self.owners.split_at_mut(1);
        let (source, destination) = if source_owner == 0 { (&first[0], &mut second[0]) }
            else { (&second[0], &mut first[0]) };
        let capture = source.participant.source(&source.engine, source.admission, generation)
            .map_err(|_| "source is not durably admitted and frozen")?;
        destination.participant.execute_final(&mut destination.kv, &mut destination.engine,
            &destination.admission, generation, &capture).map_err(|_| "final copy incomplete or unknown".to_owned())
    }
    pub fn drive_one(&mut self) -> Result<bool, String> {
        let work = self.coordinator.resume(&self.admission).map_err(|_| "resume requires recovery")?;
        let ResumeWork::Active { generation, action, .. } = work else { return Ok(false); };
        match action {
            ResumeAction::StartDestination { destination } => {
                self.control(generation, destination, Command::Start)?;
                self.execute(Operation::Request(Request::Freeze))?;
            }
            ResumeAction::FreezeAndDrain { source } => {
                self.control(generation, source, Command::Freeze)?;
                let owner = &self.owners[source as usize];
                let result = owner.participant.drain(&owner.admission, generation)
                    .map_err(|_| "source drain fenced")?;
                self.receive(generation, source, result, Certificate::Drained)?;
            }
            ResumeAction::FinalCopy { destination } => {
                let result = self.final_copy(generation, destination)?;
                self.receive(generation, destination, result, Certificate::Ready)?;
            }
            ResumeAction::RetireSource { source } => {
                let result = self.control(generation, source, Command::Retire)?;
                self.receive(generation, source, result, Certificate::Retired)?;
            }
            ResumeAction::DeliverDecision { owner, command, certificate } => {
                let result = self.control(generation, owner, command)?;
                self.receive(generation, owner, result, certificate)?;
            }
            ResumeAction::Advance(request) => { self.execute(Operation::Request(request))?; }
        }
        Ok(true)
    }
    pub fn finish(&mut self) -> Result<(), String> {
        for _ in 0..32 {
            if !self.drive_one()? { return Ok(()); }
        }
        Err("native scheduler failed to reach idle".to_owned())
    }
    /// Ordinary read uses actual native admission/grant checks, not a physical
    /// diagnostic read of an unowned copy. The reference TP durably settles its
    /// read-only transaction before the lease is released.
    pub fn read(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>, String> {
        let grant = self.route(key);
        let owner = &mut self.owners[grant.owner as usize];
        self.read_sequence += 1;
        let id = TxnId { client: (1u64 << 62) | (owner.admission.incarnation << 16) | grant.owner as u64,
            sequence: self.read_sequence };
        let status = owner.participant.begin(&mut owner.kv, &owner.admission, id)
            .map_err(|_| "read begin fenced")?;
        if status != Status::Ok { return Err("read begin rejected".to_owned()); }
        let status = owner.participant.acquire(&mut owner.kv, &owner.admission, id,
            KeyRange { table: TABLE, lo: key.to_vec(), hi: None }, true, grant)
            .map_err(|_| "read grant fenced")?;
        if status != Status::Ok { return Err("read grant rejected".to_owned()); }
        let value = owner.engine.value(TABLE, key, key).map(<[u8]>::to_vec);
        assert!(owner.engine.transaction(id, Some(true), Vec::new()));
        let status = owner.participant.finish(&mut owner.kv, &mut owner.engine, &owner.admission, id)
            .map_err(|_| "read settlement fenced")?;
        if status != Status::Ok { return Err("read settlement rejected".to_owned()); }
        Ok(value)
    }
    pub fn check_destination(&mut self, owner: u32) {
        assert_eq!(self.route(b"m\0").owner, owner);
        assert_eq!(self.read(b"m\0").unwrap(), Some(b"first\0value".to_vec()));
        assert_eq!(self.read(b"m\0x").unwrap(), Some(b"second\xffvalue".to_vec()));
        assert_eq!(self.read(b"mzz").unwrap(), Some(b"third".to_vec()));
        assert_eq!(self.read(b"m-stale").unwrap(), None);
        assert_eq!(self.read(b"a").unwrap(), Some(b"left".to_vec()));
        assert_eq!(self.read(b"n").unwrap(), Some(b"right".to_vec()));
    }
}
