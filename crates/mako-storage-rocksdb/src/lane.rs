use super::*;
pub(super) struct Envelope {
    pub op: Op,
    pub reply: mpsc::SyncSender<api::Result<Response>>,
}
pub(super) enum Op {
    Begin(u32),
    Get(u64, Vec<u8>),
    Mutate(u64, u16, api::Mutation, Vec<u8>, Vec<u8>),
    Scan(u64, Vec<u8>, Option<Vec<u8>>, bool, usize),
    Lock,
    Timestamp,
    Validate,
    Rows(u32),
    Install(u32),
    Finish(bool),
    Stop,
}
pub(super) enum Response {
    Unit,
    Value(api::VersionedValue),
    Bool(bool),
    Scan(api::VersionedScan),
    Time(u32),
    Rows(Vec<api::RedoRow>),
    Report(api::ParticipantReport),
}
enum Phase<'a> {
    Active(sto::Transaction<'a>),
    Prepared(Box<sto::Prepared<'a>>),
    Failed,
}
struct Attempt<'a> {
    phase: Phase<'a>,
    term: u32,
    dependency: u32,
    validated: bool,
    installed: Option<u32>,
    tables: BTreeMap<u64, u16>,
}
pub(super) fn run(
    worker: &mut sto::Worker,
    shared: &Arc<Shared>,
    receiver: &mpsc::Receiver<Envelope>,
) {
    while let Ok(message) = receiver.recv() {
        match message.op {
            Op::Stop => {
                let _ = message.reply.send(Ok(Response::Unit));
                return;
            }
            Op::Begin(term) => match worker.begin() {
                Err(e) => {
                    let _ = message.reply.send(Err(error(e)));
                }
                Ok(transaction) => {
                    let _ = message.reply.send(Ok(Response::Unit));
                    let attempt = Attempt {
                        phase: Phase::Active(transaction),
                        term,
                        dependency: 0,
                        validated: false,
                        installed: None,
                        tables: BTreeMap::new(),
                    };
                    if serve(attempt, shared, receiver) {
                        return;
                    }
                }
            },
            _ => {
                let _ = message
                    .reply
                    .send(Err(api::Error::invalid("no active participant")));
            }
        }
    }
}
fn serve(mut attempt: Attempt<'_>, shared: &Shared, receiver: &mpsc::Receiver<Envelope>) -> bool {
    while let Ok(message) = receiver.recv() {
        match message.op {
            Op::Stop => {
                let _ = message.reply.send(Ok(Response::Unit));
                return true;
            }
            Op::Finish(committed) => {
                let result = attempt.finish(committed);
                let _ = message.reply.send(result.map(Response::Report));
                return false;
            }
            op => {
                let result = attempt.apply(op, shared);
                let _ = message.reply.send(result);
            }
        }
    }
    false
}
impl<'a> Attempt<'a> {
    fn active(&mut self) -> api::Result<&mut sto::Transaction<'a>> {
        match &mut self.phase {
            Phase::Active(tx) => Ok(tx),
            _ => Err(api::Error::invalid("participant writes/reads frozen")),
        }
    }
    fn lock(&mut self) -> api::Result<()> {
        if matches!(self.phase, Phase::Prepared(_)) {
            return Ok(());
        }
        match std::mem::replace(&mut self.phase, Phase::Failed) {
            Phase::Active(tx) => {
                self.phase = Phase::Prepared(Box::new(
                    tx.prepare(sto::RequestId {
                        client: 1,
                        sequence: 1,
                    })
                    .map_err(error)?,
                ));
                Ok(())
            }
            _ => Err(api::Error::invalid("failed participant")),
        }
    }
    fn rows(&self, time_term: u32) -> api::Result<(Vec<sto::distributed::Row>, Vec<api::RedoRow>)> {
        if !self.validated
            || self.installed.is_some()
            || time_term % 10 != self.term
            || time_term / 10 <= self.dependency
        {
            return Err(api::Error::invalid("invalid prepared logical time/state"));
        }
        let Phase::Prepared(prepared) = &self.phase else {
            return Err(api::Error::invalid("participant not prepared"));
        };
        if prepared.is_read_only() {
            return Ok((Vec::new(), Vec::new()));
        }
        let command = sto::Command::decode(prepared.bytes().map_err(error)?).map_err(error)?;
        let sto::CommandBody::Transaction { mutations, .. } = command.body else {
            return Err(api::Error::invalid("nontransaction intent"));
        };
        let mut physical = Vec::new();
        let mut logical = Vec::new();
        for m in mutations {
            let table = *self
                .tables
                .get(&m.table)
                .ok_or_else(|| api::Error::invalid("unbound prepared table"))?;
            let row = api::RedoRow {
                table,
                key: m.key.clone(),
                value: m.value.clone(),
                time_term,
            };
            row.validate()
                .map_err(|e| api::Error::invalid(e.to_string()))?;
            logical.push(row);
            physical.push(sto::distributed::Row {
                table: m.table,
                key: m.key,
                value: m.value,
                time_term,
            });
        }
        Ok((physical, logical))
    }
    fn apply(&mut self, op: Op, shared: &Shared) -> api::Result<Response> {
        match op {
            Op::Get(table, key) => {
                let tx = self.active()?;
                let value = tx.get(table, &key).map_err(error)?;
                let time_term = tx.distributed_time(table, &key).map_err(error)?;
                self.dependency = self.dependency.max(time_term / 10);
                Ok(Response::Value(api::VersionedValue { value, time_term }))
            }
            Op::Mutate(table, logical, mutation, key, value) => {
                if self.tables.get(&table).is_some_and(|old| *old != logical)
                    || self
                        .tables
                        .iter()
                        .any(|(id, old)| *id != table && *old == logical)
                {
                    return Err(api::Error::invalid("conflicting logical table binding"));
                }
                let tx = self.active()?;
                let old = tx.get(table, &key).map_err(error)?;
                let time_term = tx.distributed_time(table, &key).map_err(error)?;
                let changed = match mutation {
                    api::Mutation::Insert if old.is_some() => false,
                    api::Mutation::Remove if old.is_none() => false,
                    api::Mutation::Remove => {
                        tx.delete(table, &key).map_err(error)?;
                        true
                    }
                    _ => {
                        tx.put(table, &key, &value).map_err(error)?;
                        true
                    }
                };
                self.dependency = self.dependency.max(time_term / 10);
                if changed {
                    self.tables.insert(table, logical);
                }
                Ok(Response::Bool(changed))
            }
            Op::Scan(table, lower, upper, reverse, limit) => {
                let tx = self.active()?;
                let rows = if reverse {
                    tx.reverse_scan(table, &lower, upper.as_deref(), limit)
                } else {
                    tx.scan(table, &lower, upper.as_deref(), limit)
                }
                .map_err(error)?;
                let dependency = tx
                    .distributed_range_dependency(table, &lower, upper.as_deref())
                    .map_err(error)?;
                let mut result = Vec::new();
                for (key, value) in rows {
                    let time_term = tx.distributed_time(table, &key).map_err(error)?;
                    result.push(api::VersionedRow {
                        key,
                        value,
                        time_term,
                    });
                }
                self.dependency = self.dependency.max(dependency);
                Ok(Response::Scan(api::VersionedScan {
                    rows: result,
                    read_dependency: self.dependency,
                }))
            }
            Op::Lock => {
                self.lock()?;
                Ok(Response::Unit)
            }
            Op::Timestamp => {
                if matches!(self.phase, Phase::Failed) {
                    return Err(api::Error::invalid("failed participant"));
                }
                Ok(Response::Time(shared.timestamp(self.dependency)?))
            }
            Op::Validate => {
                if self.installed.is_some() {
                    return Err(api::Error::invalid("already installed"));
                }
                if !self.validated {
                    self.lock()?;
                    let Phase::Prepared(prepared) = &mut self.phase else {
                        unreachable!()
                    };
                    if let Err(e) = prepared.certify() {
                        self.phase = Phase::Failed;
                        return Err(error(e));
                    }
                    self.validated = true;
                }
                Ok(Response::Unit)
            }
            Op::Rows(time) => Ok(Response::Rows(self.rows(time)?.1)),
            Op::Install(timestamp) => {
                let time_term = timestamp
                    .checked_mul(10)
                    .and_then(|v| v.checked_add(self.term))
                    .ok_or_else(|| api::Error::invalid("logical time overflow"))?;
                let (physical, _) = self.rows(time_term)?;
                let Phase::Prepared(prepared) = &mut self.phase else {
                    unreachable!()
                };
                if prepared.is_read_only() {
                    prepared.complete_read_only().map_err(error)?;
                } else {
                    // Production log application has already done this batch.
                    // Equal logical rows are a no-op; MemoryLog applies here.
                    shared
                        .db
                        .apply_distributed(None, &physical, &Vec::new())
                        .map_err(error)?;
                    prepared.complete_distributed(time_term).map_err(error)?;
                }
                shared.clock.fetch_max(timestamp, Ordering::AcqRel);
                self.installed = Some(time_term);
                Ok(Response::Unit)
            }
            _ => Err(api::Error::invalid("invalid active operation")),
        }
    }
    fn finish(mut self, committed: bool) -> api::Result<api::ParticipantReport> {
        if committed && self.installed.is_none() {
            match &mut self.phase {
                Phase::Prepared(p) if p.is_read_only() && self.validated => {
                    p.complete_read_only().map_err(error)?;
                }
                _ => return Err(api::Error::invalid("commit before installation")),
            }
        }
        Ok(api::ParticipantReport {
            disposition: if committed || self.installed.is_some() {
                1
            } else {
                2
            },
            time_term: self.installed.unwrap_or(0),
            read_dependency: self.dependency,
            empty: true,
            phase: 0,
        })
    }
}
