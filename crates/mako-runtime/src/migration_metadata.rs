//! Control metadata uses an internal table in the selected data engine. Every
//! atomic batch uses the same OCC/2PC log as rows; no separate metadata database
//! or second durability authority is introduced.
use crate::{
    migration::{CONTROL_LOG_ID, CONTROL_TABLE},
    migration_storage::commit_participant,
    storage::{engine, Store},
    wire::{Error, ErrorCode, Result},
};
use sto_core::protocol::journal::TransactionId;
use mako_sharding::{
    durable_kv::{CommitResult, ReadResult, WriteOp},
    runtime_durability::{MetadataKv, RecoveryHost, RowAuthority},
    types::Status,
};
use mako_storage_api as api;
use parking_lot::Mutex;
use std::{
    io::Read,
    sync::{atomic::Ordering, mpsc, Arc, Weak},
    thread::JoinHandle,
};

thread_local! {static ROW_AUTHORITY: std::cell::Cell<Option<u64>> = const {std::cell::Cell::new(None)};}
pub(crate) fn row_authority() -> Option<u64> {
    ROW_AUTHORITY.with(std::cell::Cell::get)
}
struct Scope(Option<u64>, std::marker::PhantomData<std::rc::Rc<()>>);
impl RowAuthority for Scope {}
impl Drop for Scope {
    fn drop(&mut self) {
        ROW_AUTHORITY.with(|slot| slot.set(self.0));
    }
}

pub(crate) struct Recovery {
    store: Weak<Store>,
    namespaces: Arc<Mutex<[bool; 2]>>,
}
impl Recovery {
    pub fn new(store: &Arc<Store>) -> Arc<Self> {
        Arc::new(Self {
            store: Arc::downgrade(store),
            namespaces: Arc::new(Mutex::new([false; 2])),
        })
    }
    fn store(&self) -> std::result::Result<Arc<Store>, Status> {
        self.store.upgrade().ok_or(Status::Retry)
    }
}
impl RecoveryHost for Recovery {
    fn rows(&self, authority: u64) -> std::result::Result<Box<dyn RowAuthority>, Status> {
        if self.authority()? != authority {
            return Err(Status::Retry);
        }
        let previous = ROW_AUTHORITY.with(|slot| slot.replace(Some(authority)));
        Ok(Box::new(Scope(previous, std::marker::PhantomData)))
    }
    fn authority(&self) -> std::result::Result<u64, Status> {
        let store = self.store()?;
        store.health().map_err(status)?;
        let status = store.log.replica_status(store.owner);
        if !status.leader {
            return Err(Status::Retry);
        }
        Ok(status.term)
    }
    fn metadata(
        &self,
        coordinator: bool,
        authority: u64,
    ) -> std::result::Result<Box<dyn MetadataKv>, Status> {
        let store = self.store()?;
        let role = usize::from(coordinator);
        let mut namespaces = self.namespaces.lock();
        if namespaces[role] {
            return Err(Status::Busy);
        }
        let kv =
            Kv::start(store, coordinator, authority, self.namespaces.clone()).map_err(status)?;
        namespaces[role] = true;
        Ok(Box::new(kv))
    }
    fn transactions_restored(&self) -> std::result::Result<bool, Status> {
        let store = self.store()?;
        store
            .participants
            .get()
            .ok_or(Status::Retry)?
            .as_ref()
            .map_err(|_| Status::Io)?
            .recover()
            .map_err(status)?;
        Ok(true)
    }
    fn row_barrier(&self, authority: u64) -> std::result::Result<(), Status> {
        let store = self.store()?;
        let permit = store.log.admit().map_err(status)?;
        if self.authority()? != authority {
            return Err(Status::Retry);
        }
        if !self.transactions_restored()? {
            return Err(Status::Retry);
        }
        let store = self.store()?;
        // Internal writes may have lost their final reply. Recovery owns and
        // resolves those prepares before an actor inspects its metadata map.
        if store
            .journal
            .entries()
            .map_err(status)?
            .iter()
            .any(|entry| {
                entry.transaction.id.coordinator == store.owner
                    && entry.transaction.id.worker >= u32::MAX - 2
                    && entry.unresolved()
            })
        {
            return Err(Status::Retry);
        }
        if self.authority()? != authority {
            return Err(Status::Retry);
        }
        permit.check().map_err(status)?;
        Ok(())
    }
}
fn status(error: Error) -> Status {
    match error.code {
        ErrorCode::Invalid => Status::Invalid,
        ErrorCode::Fatal => Status::Io,
        ErrorCode::Capacity => Status::Exhausted,
        _ => Status::Retry,
    }
}
enum Work {
    Read(Vec<u8>, mpsc::SyncSender<ReadResult>),
    Write(Vec<WriteOp>, mpsc::SyncSender<CommitResult>),
}
struct Kv {
    send: Option<mpsc::Sender<Work>>,
    thread: Option<JoinHandle<()>>,
    coordinator: bool,
    namespaces: Arc<Mutex<[bool; 2]>>,
}
impl Kv {
    fn start(
        store: Arc<Store>,
        coordinator: bool,
        authority: u64,
        namespaces: Arc<Mutex<[bool; 2]>>,
    ) -> Result<Self> {
        let mut nonce = [0; 8];
        std::fs::File::open("/dev/urandom")
            .and_then(|mut f| f.read_exact(&mut nonce))
            .map_err(|e| Error::new(ErrorCode::Fatal, e.to_string()))?;
        let incarnation = u64::from_ne_bytes(nonce).max(1);
        let (send, receive) = mpsc::channel();
        let (ready, started) = mpsc::sync_channel(1);
        let thread = std::thread::Builder::new()
            .name("migration-metadata".into())
            .spawn(move || {
                let mut worker = match store.database.worker().map_err(engine) {
                    Ok(worker) => worker,
                    Err(error) => {
                        let _ = ready.send(Err(error));
                        return;
                    }
                };
                let table = match store.open(&mut worker, CONTROL_TABLE, CONTROL_LOG_ID) {
                    Ok(table) => table,
                    Err(error) => {
                        let _ = ready.send(Err(error));
                        return;
                    }
                };
                let _ = ready.send(Ok(()));
                let mut attempt = 0u64;
                let key = |bytes: &[u8]| {
                    let mut key = vec![u8::from(coordinator)];
                    key.extend_from_slice(bytes);
                    key
                };
                while let Ok(work) = receive.recv() {
                    let admission = (|| {
                        store.health()?;
                        let permit = store.log.admit()?;
                        if store.log.replica_status(store.owner).term != authority {
                            return Err(Error::new(
                                ErrorCode::RouteChanged,
                                "metadata authority changed",
                            ));
                        }
                        permit.check()?;
                        Ok(permit)
                    })();
                    match work {
                        Work::Read(bytes, reply) => {
                            let result = (|| {
                                let permit = admission?;
                                let mut tx = worker
                                    .begin_participant(store.term.load(Ordering::Acquire))
                                    .map_err(engine)?;
                                let value = tx.get(&table.tree, &key(&bytes)).map_err(engine)?;
                                tx.lock().map_err(engine)?;
                                tx.validate().map_err(engine)?;
                                permit.check()?;
                                tx.finish(true).map_err(engine)?;
                                Ok::<_, Error>(value.value)
                            })();
                            let _ = reply.send(match result {
                                Ok(Some(v)) => ReadResult::Found(v),
                                Ok(None) => ReadResult::Absent,
                                Err(_) => ReadResult::Io,
                            });
                        }
                        Work::Write(batch, reply) => {
                            let result = (|| {
                                let permit = admission?;
                                attempt = attempt.checked_add(1).ok_or_else(|| {
                                    Error::new(ErrorCode::Capacity, "metadata IDs exhausted")
                                })?;
                                let term = store.term.load(Ordering::Acquire);
                                let mut tx = worker.begin_participant(term).map_err(engine)?;
                                for op in batch {
                                    tx.mutate(
                                        &table.tree,
                                        CONTROL_LOG_ID,
                                        if op.value.is_some() {
                                            api::Mutation::Put
                                        } else {
                                            api::Mutation::Remove
                                        },
                                        &key(&op.key),
                                        op.value.as_deref().unwrap_or_default(),
                                        false,
                                    )
                                    .map_err(engine)?;
                                }
                                commit_participant(
                                    tx,
                                    &store,
                                    term,
                                    TransactionId {
                                        coordinator: store.owner,
                                        worker: u32::MAX - 1 - u32::from(coordinator),
                                        incarnation,
                                        attempt,
                                    },
                                    &*permit,
                                )
                            })();
                            let _ = reply.send(match result {
                                Ok(()) => CommitResult::Committed,
                                Err(error) if error.code == ErrorCode::Unknown => {
                                    CommitResult::Unknown
                                }
                                Err(_) => CommitResult::NotCommitted,
                            });
                        }
                    }
                }
            })
            .map_err(|e| Error::new(ErrorCode::Fatal, e.to_string()))?;
        if let Err(error) = started
            .recv()
            .map_err(|_| Error::new(ErrorCode::Closed, "metadata worker stopped"))?
        {
            drop(send);
            let _ = thread.join();
            return Err(error);
        }
        Ok(Self {
            send: Some(send),
            thread: Some(thread),
            coordinator,
            namespaces,
        })
    }
}
impl MetadataKv for Kv {
    fn read(&mut self, key: &[u8]) -> ReadResult {
        let (reply, receive) = mpsc::sync_channel(1);
        if self
            .send
            .as_ref()
            .unwrap()
            .send(Work::Read(key.to_vec(), reply))
            .is_err()
        {
            return ReadResult::Io;
        }
        receive.recv().unwrap_or(ReadResult::Io)
    }
    fn submit(&mut self, batch: Vec<WriteOp>) -> CommitResult {
        let (reply, receive) = mpsc::sync_channel(1);
        if self
            .send
            .as_ref()
            .unwrap()
            .send(Work::Write(batch, reply))
            .is_err()
        {
            return CommitResult::NotCommitted;
        }
        receive.recv().unwrap_or(CommitResult::Unknown)
    }
}
impl Drop for Kv {
    fn drop(&mut self) {
        self.send.take();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
        self.namespaces.lock()[usize::from(self.coordinator)] = false;
    }
}
