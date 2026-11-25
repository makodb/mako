//! Runtime ownership of either a volatile participant or its durable wrapper.
//! Ordinary transactions use host epoch pins. Only migration controls enter the
//! durable actor's journal; no ordinary lease is synthesized during recovery.
use crate::{
    directory::RouteTable,
    durable_kv::{Admission, DurableKv, Namespace, ReadResult},
    durable_participant::{DurableParticipant, ParticipantError},
    host::Host,
    participant::{ControlResult, Participant},
    runtime_durability::{Engine, Kv, Recovery},
    storage::Source,
    transfer::{self, RawStore},
    types::*,
};

enum State {
    Live(Participant),
    Persistent {
        actor: DurableParticipant,
        kv: Kv,
        engine: Engine,
        admission: Admission,
    },
}
pub(crate) struct ParticipantState {
    state: State,
}
impl ParticipantState {
    pub fn live(owner: u32, tables: Vec<RouteTable>) -> Result<Self, Status> {
        Ok(Self {
            state: State::Live(Participant::new(owner, tables)?),
        })
    }
    pub fn recover(
        owner: u32,
        tables: Vec<RouteTable>,
        host: Host,
        recovery: Recovery,
        authority: u64,
    ) -> Result<Self, Status> {
        recovery.row_barrier(authority)?;
        let mut kv = Kv::new(recovery.metadata(false, authority)?);
        let mut engine = Engine::new(host, recovery, authority, owner);
        let namespace = Namespace {
            owner,
            coordinator: false,
        };
        let mut actor = match kv.read(&crate::durable_kv::journal_key(namespace, false, 0)) {
            ReadResult::Absent => DurableParticipant::create(&mut kv, owner, tables),
            ReadResult::Found(_) => DurableParticipant::recover(&mut kv, &mut engine, owner),
            ReadResult::Io => return Err(Status::Io),
        }
        .map_err(error)?;
        let admission = actor.activate(&mut kv, &mut engine).map_err(error)?;
        Ok(Self {
            state: State::Persistent {
                actor,
                kv,
                engine,
                admission,
            },
        })
    }
    pub fn actor(&self) -> Result<&Participant, Status> {
        match &self.state {
            State::Live(actor) => Ok(actor),
            State::Persistent {
                actor, admission, ..
            } => actor.actor(admission).map_err(error),
        }
    }
    pub fn plans(&self) -> Result<Vec<Vec<u8>>, Status> {
        match &self.state {
            State::Live(_) => Ok(Vec::new()),
            State::Persistent {
                actor, admission, ..
            } => actor
                .retained_plans(admission)
                .map_err(error)?
                .iter()
                .map(crate::wire::encode_plan)
                .collect(),
        }
    }
    pub fn bind(&mut self, plan: MigrationPlan) -> Result<(), Status> {
        match &mut self.state {
            State::Live(_) => Ok(()),
            State::Persistent {
                actor,
                kv,
                admission,
                ..
            } => actor.bind_plan(kv, admission, plan).map_err(error),
        }
    }
    pub fn deliver(
        &mut self,
        plan: &MigrationPlan,
        command: Command,
    ) -> Result<ControlResult, Status> {
        match &mut self.state {
            State::Live(actor) => Ok(actor.deliver(plan, command)),
            State::Persistent {
                actor,
                kv,
                admission,
                ..
            } => actor
                .deliver(kv, admission, plan.generation, command)
                .map_err(error),
        }
    }
    pub fn capture<R: RawStore>(
        &self,
        store: &R,
        plan: &MigrationPlan,
        after: Option<&Row>,
    ) -> Result<Option<Row>, Status> {
        match &self.state {
            State::Live(actor) => transfer::capture_frozen(actor, store, plan, after),
            State::Persistent {
                actor,
                engine,
                admission,
                ..
            } => actor
                .capture(engine, admission, plan.generation, after)
                .map_err(error),
        }
    }
    pub fn final_copy<R: RawStore, S: Source>(
        &mut self,
        store: &mut R,
        plan: &MigrationPlan,
        source: &S,
    ) -> Result<ControlResult, Status> {
        match &mut self.state {
            State::Live(actor) => Ok(transfer::execute_final(actor, store, plan, source)),
            State::Persistent {
                actor,
                kv,
                engine,
                admission,
            } => actor
                .execute_final(kv, engine, admission, plan.generation, source)
                .map_err(error),
        }
    }
    pub fn cleanup<R: RawStore>(
        &mut self,
        store: &mut R,
        plan: &MigrationPlan,
    ) -> Result<ControlResult, Status> {
        match &mut self.state {
            State::Live(actor) => Ok(transfer::execute_cleanup(actor, store, plan)),
            State::Persistent {
                actor,
                kv,
                engine,
                admission,
            } => actor
                .execute_cleanup(kv, engine, admission, plan.generation)
                .map_err(error),
        }
    }
}
fn error(error: ParticipantError) -> Status {
    match error {
        ParticipantError::Native(status) => status,
        ParticipantError::PlanConflict => Status::Invalid,
        ParticipantError::Fenced => Status::Retry,
        ParticipantError::Corrupt
        | ParticipantError::Journal(_)
        | ParticipantError::EngineUnknown => Status::Io,
    }
}
