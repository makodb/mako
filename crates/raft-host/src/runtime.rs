use crate::{durable::Disk, snapshot::{self, Image, Incoming, Reference}, transport::{self, Job, ReplyToken, Tag}, wire::{Batch, Command, Envelope, Request, Response}, AdmissionError, Config, Host, HostError, Position, Proposal, ProposalStatus, Shared, StateMachine, Status};
use raft_core::{CoreActionKind, CoreOutput, Event, RaftCore, Reply, RAFT_INDEX_LIMIT};
use parking_lot::Mutex;
use std::collections::{BTreeMap, BTreeSet};
use std::hash::BuildHasher;
use std::sync::{mpsc, Arc, Weak};
use std::time::{Duration, Instant};

type AdmissionReply = mpsc::SyncSender<Result<Proposal, AdmissionError>>;
type CheckpointReply = mpsc::SyncSender<Result<Position, HostError>>;
pub(crate) enum Message {
    Propose { term: u64, command: Command, reply: AdmissionReply },
    ReadBarrier { term: u64, deadline: Instant, reply: CheckpointReply },
    Inbound(Envelope, ReplyToken),
    Network(Tag, Result<Response, String>),
    Applied(Position, Result<(), String>),
    Created(Result<Image, String>, CheckpointReply),
    Staged(Result<Image, String>, u16, u64, ReplyToken),
    Published(Image, Result<(), String>, ReplyToken),
    Checkpoint(CheckpointReply),
    Shutdown,
}
enum Application {
    Apply(Position, Command),
    Create(Position, CheckpointReply),
    Begin { position: Position, digest: [u8;32], term: u64, token: ReplyToken },
    Chunk { position: Position, file: String, offset: u64, bytes: Vec<u8>, last: bool, term: u64, token: ReplyToken },
    Stage { position: Position, sender: u16, term: u64, token: ReplyToken },
    Publish(Image, ReplyToken),
}
struct Campaign { term: u64, votes: BTreeMap<u16, (bool,u64)>, deadline: Instant }
struct ReadWaiter {
    term: u64,
    // heartbeat_round_ names the NEXT generation, not the last launched one.
    minimum_round: u64,
    applied_through: Option<Position>,
    deadline: Instant,
    reply: CheckpointReply,
}
impl ReadWaiter {
    fn needs_authority(&self, term:u64, authority:(u64,u64))->bool {
        self.term==term && self.applied_through.is_none()
            && (authority.0!=term || authority.1<self.minimum_round)
    }
    fn ready(&mut self, commit:Position, authority:(u64,u64), applied:u64)->Option<Position> {
        // A current-term committed entry is mandatory before ReadIndex:
        // the election no-op establishes the complete prior prefix.
        if self.applied_through.is_none() && commit.term==self.term
            && authority.0==self.term && authority.1>=self.minimum_round {
            self.applied_through=Some(commit);
        }
        self.applied_through.filter(|position|applied>=position.index)
    }
}
// A fresh round must be able to launch a quorum together. Kicking on every
// freed slot would fragment evidence into one-peer rounds with five voters.
fn can_launch_read_quorum(core:&RaftCore<Command>)->bool {
    let quorum=raft_core::raft_quorum_majority_count(core.config_members_.len());
    let mut available=1;
    for ordinal in 0..core.peers_.len() {
        if core.peers_.next_index(ordinal)>=core.raft_log_.base()
            && core.pending_rpcs_.in_flight(core.peer_site_at(ordinal)).count_==0 {
            available+=1;
        }
    }
    available>=quorum
}

fn next_election_delay(source:&impl BuildHasher,group:u64,node:u16,draw:&mut u64,base:u64)->u64 {
    let sample=source.hash_one((group,node,*draw));
    *draw=draw.wrapping_add(1);
    base+sample%base
}

struct Group {
    cfg: Config,
    core: RaftCore<Command>,
    disk: Disk,
    shared: Weak<Shared>,
    status: Arc<Mutex<Status>>,
    isolation: crate::Isolation,
    application: mpsc::Sender<Application>,
    peers: BTreeMap<u16,mpsc::SyncSender<Job>>,
    snapshot_peers: BTreeMap<u16,mpsc::SyncSender<Job>>,
    proposals: BTreeMap<u64,Proposal>,
    reads: Vec<ReadWaiter>,
    applied: Position,
    applying: bool,
    maintenance: bool,
    checkpoint_waiter: Option<CheckpointReply>,
    snapshot_sending: BTreeSet<u16>,
    image: Option<Image>,
    retired_images: Vec<Image>,
    campaign: Option<Campaign>,
    election_random: std::collections::hash_map::RandomState,
    election_draw: u64,
    origin: Instant,
    last_tick: Instant,
    failure: Option<String>,
}
pub(crate) fn start(mut cfg: Config, machine: Arc<dyn StateMachine>) -> Result<Host, HostError> {
    cfg.peers.sort_by_key(|p| p.id);
    if cfg.peers.is_empty() || cfg.node == u16::MAX || !cfg.peers.iter().any(|p| p.id == cfg.node)
        || cfg.peers.iter().any(|p| p.id == u16::MAX || p.address.contains('\0'))
        || cfg.peers.windows(2).any(|w| w[0].id == w[1].id)
        || cfg.directory.as_os_str().is_empty() || cfg.queue_capacity == 0
        || cfg.snapshot_chunk_bytes == 0 || cfg.snapshot_chunk_bytes > 4 << 20
        || cfg.max_command_bytes == 0 || cfg.max_command_bytes > crate::MAX_COMMAND_BYTES
        || cfg.heartbeat_interval.is_zero() || cfg.rpc_timeout.is_zero()
        || cfg.election_timeout <= cfg.heartbeat_interval.saturating_mul(3) {
        return Err(HostError::Unavailable("invalid Raft host configuration".into()));
    }
    let (tx, rx) = mpsc::sync_channel(cfg.queue_capacity);
    let status=Arc::new(Mutex::new(Status::default()));
    let isolation=crate::Isolation::default();
    let shared = Arc::new(Shared { tx: tx.clone(), status: status.clone(),
        thread: Mutex::new(None), max_command_bytes: cfg.max_command_bytes,
        #[cfg(feature = "test-hooks")]
        isolation: isolation.clone() });
    let weak = Arc::downgrade(&shared);
    let (ready, started) = mpsc::sync_channel(1);
    let handle = std::thread::Builder::new().name(format!("raft-group-{}-{}", cfg.group,cfg.node)).spawn(move || {
        let recovered = recover(&cfg, &*machine);
        let (disk, core, applied, image) = match recovered { Ok(v) => v, Err(e) => { let _ = ready.send(Err(e)); return; } };
        let poll = srpc::reactor::PollThread::create();
        let server = match transport::listen(&cfg, tx.clone(), poll.clone(), isolation.clone()) {
            Ok(s) => s, Err(e) => { let _ = ready.send(Err(e)); poll.shutdown(); return; }
        };
        let (application, app_rx) = mpsc::channel();
        let app_tx = tx.clone();
        let app_cfg = cfg.clone();
        let app_thread = std::thread::Builder::new().name(format!("raft-apply-{}", cfg.group))
            .spawn(move || application_loop(app_cfg, machine, app_rx, app_tx)).expect("spawn Raft application thread");
        let mut peer_threads = Vec::new();
        let mut peers = BTreeMap::new();
        let mut snapshot_peers = BTreeMap::new();
        for peer in cfg.peers.iter().filter(|p| p.id != cfg.node) {
            let (sender, thread) = transport::peer_thread(cfg.clone(), peer.clone(), tx.clone(), poll.clone(), isolation.clone());
            peers.insert(peer.id, sender); peer_threads.push(thread);
            let (sender, thread) = transport::peer_thread(cfg.clone(), peer.clone(), tx.clone(), poll.clone(), isolation.clone());
            snapshot_peers.insert(peer.id, sender); peer_threads.push(thread);
        }
        let mut group = Group { cfg, core, disk, shared: weak, status, isolation, application, peers, snapshot_peers,
            proposals: BTreeMap::new(), applied, applying: false, maintenance: false,
            reads: Vec::new(),
            checkpoint_waiter: None, snapshot_sending: BTreeSet::new(), image, retired_images: Vec::new(), campaign: None,
            origin: Instant::now(), last_tick: Instant::now(), failure: None,
            election_random: std::collections::hash_map::RandomState::new(), election_draw: 0,
        };
        group.reset_timer();
        group.publish_status();
        let _ = ready.send(Ok(()));
        'group: loop {
            let message = rx.recv_timeout(Duration::from_millis(5));
            match message {
                Ok(Message::Shutdown) | Err(mpsc::RecvTimeoutError::Disconnected) => break,
                Ok(message) => { if let Err(e) = group.handle(message) { group.fence(e); } },
                Err(mpsc::RecvTimeoutError::Timeout) => {},
            }
            if group.shared.strong_count() == 0 { break; }
            // A slow fsync or scheduler pause must not let a timer overtake
            // leader contact already waiting in this FIFO. Only drain when an
            // election is due, so leader replication still ticks between
            // proposals. The queue capacity bounds work under continuous input.
            for _ in 0..group.cfg.queue_capacity {
                if group.failure.is_some() || !group.election_due() { break; }
                match rx.try_recv() {
                    Ok(Message::Shutdown) | Err(mpsc::TryRecvError::Disconnected) => break 'group,
                    Ok(message) => { if let Err(e)=group.handle(message) {group.fence(e);} },
                    Err(mpsc::TryRecvError::Empty) => break,
                }
            }
            if group.failure.is_none() {
                if let Err(e) = group.clock().and_then(|_| group.schedule_apply()).and_then(|_| group.collect_retired()) { group.fence(e); }
            }
            group.resolve_reads();
            group.publish_status();
        }
        if group.failure.is_none() {
            group.failure=Some("host stopped".into());
            for proposal in group.proposals.values() { proposal.set(ProposalStatus::Unknown); }
            if let Some(waiter)=group.checkpoint_waiter.take() {
                let _=waiter.send(Err(HostError::Unavailable("host stopped".into())));
            }
        }
        group.resolve_reads();
        group.publish_status();
        // Drop receiver before joining senders: a completion must never block
        // shutdown behind an abandoned full group queue.
        drop(rx);
        group.peers.clear();
        group.snapshot_peers.clear();
        drop(group.application);
        drop(server);
        for thread in peer_threads { let _ = thread.join(); }
        let _ = app_thread.join();
        poll.shutdown();
    }).map_err(|e| HostError::Unavailable(e.to_string()))?;
    *shared.thread.lock() = Some(handle);
    match started.recv() {
        Ok(Ok(())) => Ok(Host { shared }),
        result => {
            if let Some(thread) = shared.thread.lock().take() { let _ = thread.join(); }
            Err(HostError::Storage(match result { Ok(Err(e)) => e, _ => "host start interrupted".into() }))
        }
    }
}
fn recover(cfg: &Config, machine: &dyn StateMachine) -> Result<(Disk,RaftCore<Command>,Position,Option<Image>),String> {
    let disk = Disk::open(cfg)?;
    let saved = &disk.state;
    let image = match &saved.image {
        Some(name) => {
            let bytes = raft_store::images::read(&*disk.fs, &disk.path.join("images"), name)?;
            let reference: Reference = crate::wire::decode(&bytes)?;
            if reference.group != cfg.group || reference.position != (Position { index: saved.snap_index, term: saved.snap_term }) {
                return Err("durable checkpoint reference disagrees with Raft authority".into());
            }
            Some(Image::load(&cfg.directory, reference)?)
        },
        None if saved.snap_index != 0 => return Err("Raft snapshot lacks application generation".into()),
        None => None,
    };
    let mut applied = machine.applied_position()?;
    if applied.index > saved.hard.commit { return Err("application is newer than recoverable committed authority".into()); }
    if let Some(image) = &image {
        if applied.index <= image.reference.position.index {
            machine.validate_checkpoint(&image.directory, image.reference.position)?;
            machine.publish_checkpoint(&image.directory, image.reference.position)?;
            applied = machine.applied_position()?;
            if applied != image.reference.position { return Err("published checkpoint position mismatch".into()); }
        }
    }
    if applied.index < saved.snap_index { return Err("application behind compacted history".into()); }
    let term = if applied.index == saved.snap_index { saved.snap_term }
        else { saved.entries.get((applied.index-saved.snap_index-1) as usize).ok_or("application prefix absent from log")?.0 };
    if applied.term != term { return Err("application applied term disagrees with committed authority".into()); }
    let mut core = RaftCore::new();
    core.step::<Batch>(Event::SetIdentity { loc_id: cfg.node as u32, site_id: cfg.node, partition_id: 0 }, &mut CoreOutput::new());
    let members = cfg.peers.iter().map(|p| p.id).collect::<Vec<_>>();
    core.step::<Batch>(Event::Configure { members: &members }, &mut CoreOutput::new());
    let entries_rev = saved.entries.iter().rev().map(|(term,cmd)| cmd.entry(*term)).collect();
    if !core.step::<Batch>(Event::Restore { term: saved.hard.term, vote: saved.hard.vote, commit: saved.hard.commit,
        d: disk.sequence, snap_index: saved.snap_index, snap_term: saved.snap_term as i64, entries_rev }, &mut CoreOutput::new()).into_restored() {
        return Err("core refused recovered Raft state".into());
    }
    if !core.step::<Batch>(Event::EnterGates { unsaved_restart: false, failover: true }, &mut CoreOutput::new()).into_gates() {
        return Err("core refused Raft membership".into());
    }
    for index in applied.index+1..=saved.hard.commit {
        let entry = core.raft_log_.get(index).ok_or("committed recovery entry missing")?;
        let position = Position { index, term: entry.term() as u64 };
        machine.apply(position, entry.cmd().0.as_deref())?;
        applied = position;
    }
    core.step::<Batch>(Event::Applied { index: applied.index, published: 0 }, &mut CoreOutput::new());
    snapshot::cleanup(&cfg.directory, image.as_ref(), cfg.sync_writes)?;
    Ok((disk,core,applied,image))
}
impl Group {
    fn now(&self) -> u64 { self.origin.elapsed().as_micros().min(u64::MAX as u128) as u64 }
    fn reset_timer(&mut self) {
        let base = self.cfg.election_timeout.as_micros() as u64;
        // Independent per-group entropy breaks phase locking between survivors.
        // Elapsed time plus node/term constants is not randomness: a fixed
        // clock-origin offset can cancel the node salt in every campaign.
        let timeout_us=next_election_delay(&self.election_random,self.cfg.group,self.cfg.node,&mut self.election_draw,base);
        self.core.step::<Batch>(Event::ResetElectionTimer { now: self.now(), timeout_us }, &mut CoreOutput::new());
    }
    fn step(&mut self, event: Event<'_,Command,Batch>, image: Option<String>) -> Result<Reply<Command>,String> {
        let mut output = CoreOutput::new();
        let reply = self.core.step_checked(event, &mut output).ok_or("core rejected invalid host/wire event")?;
        self.finish_output(&mut output, image)?;
        Ok(reply)
    }
    fn finish_output(&mut self, output: &mut CoreOutput, image: Option<String>) -> Result<(),String> {
        // This must precede replies, APPLY_RANGE, role publication and NOOP.
        self.disk.persist(&self.core, output, image)?;
        let mut noop = false;
        let mut reset = false;
        for i in 0..output.len() {
            match output.at(i).kind() {
                CoreActionKind::APPEND_NOOP => noop = true,
                CoreActionKind::RESET_ELECTION => reset = true,
                _ => {},
            }
        }
        if reset { self.reset_timer(); }
        if noop {
            self.step(Event::Propose { cmd: Command(None), has_value: true, is_tpc_commit: false, kind: 0, payload_bytes: 0 }, None)?;
        }
        self.publish_status();
        self.resolve();
        Ok(())
    }
    fn position(&self, index: u64) -> Position {
        let term = if index == self.core.snapidx_ { self.core.snapterm_ as u64 }
            else { self.core.raft_log_.get(index).map_or(0, |e| e.term() as u64) };
        Position { index, term }
    }
    fn resolve(&mut self) {
        let mut finished = Vec::new();
        for (&index, proposal) in &self.proposals {
            let state = if index <= self.applied.index {
                // Snapshots can cover an unresolved prior leader suffix without
                // proving that suffix survived. Only a known committed proposal
                // may be declared applied when its entry was compacted away.
                if index < self.core.raft_log_.base() {
                    if matches!(proposal.status(), ProposalStatus::Committed | ProposalStatus::Applied) { ProposalStatus::Applied }
                    else { ProposalStatus::Unknown }
                } else if self.position(index).term == proposal.position.term { ProposalStatus::Applied }
                else { ProposalStatus::Discarded }
            } else if self.core.raft_log_.get(index).is_some_and(|e| e.term() as u64 != proposal.position.term) {
                ProposalStatus::Discarded
            } else if index <= self.core.commit_index_ && self.position(index).term == proposal.position.term {
                ProposalStatus::Committed
            } else if !self.core.is_leader_ || self.core.current_term_ != proposal.position.term { ProposalStatus::Unknown }
            else { ProposalStatus::Pending };
            if matches!(state, ProposalStatus::Applied | ProposalStatus::Discarded) { finished.push(index); }
            proposal.set(state);
        }
        for index in finished { self.proposals.remove(&index); }
    }
    fn resolve_reads(&mut self) {
        // RoundEnd already validates/settles the existing authority ledger and
        // publishes these exact-generation fields after commit advancement.
        // Settling a second time here would consume an already-empty ledger.
        let authority=(self.core.read_quorum_confirmed_term_,self.core.read_quorum_confirmed_round_);
        let commit=self.position(self.core.commit_index_);
        let now=Instant::now();
        let mut index=0;
        while index<self.reads.len() {
            let waiter=&mut self.reads[index];
            let result=if let Some(error)=&self.failure {
                Some(Err(HostError::Storage(error.clone())))
            } else if !self.core.is_leader_ || self.core.current_term_!=waiter.term {
                Some(Err(HostError::NotLeader))
            } else if self.maintenance {
                Some(Err(HostError::Unavailable("snapshot publication".into())))
            } else if now>=waiter.deadline {
                Some(Err(HostError::Timeout))
            } else {
                waiter.ready(commit,authority,self.applied.index).map(Ok)
            };
            if let Some(result)=result {
                let waiter=self.reads.swap_remove(index);
                let _=waiter.reply.send(result);
            } else {index+=1;}
        }
    }
    fn fence(&mut self, why: String) {
        if self.failure.is_none() {
            eprintln!("raft-host fenced group={} node={} term={} committed={} applied={} error={}",
                self.cfg.group,self.cfg.node,self.core.current_term_,self.core.commit_index_,self.applied.index,why);
            self.failure = Some(why.clone());
        }
        for proposal in self.proposals.values() { proposal.set(ProposalStatus::StorageFailed(why.clone())); }
        if let Some(waiter) = self.checkpoint_waiter.take() { let _ = waiter.send(Err(HostError::Storage(why))); }
    }
    fn publish_status(&self) {
        *self.status.lock() = Status { term: self.core.current_term_,
            leader: (self.core.current_leader_id_ != u16::MAX).then_some(self.core.current_leader_id_),
            is_leader: self.core.is_leader_ && self.failure.is_none() && !self.maintenance,
            committed: self.position(self.core.commit_index_), applied: self.applied,
            durable_sequence: self.disk.sequence, failure: self.failure.clone() };
    }
    fn handle(&mut self, message: Message) -> Result<(),String> {
        match message {
            Message::ReadBarrier{term,deadline,reply}=>{
                if let Some(error)=&self.failure {
                    let _=reply.send(Err(HostError::Storage(error.clone())));
                } else if !self.core.is_leader_ || self.core.current_term_!=term {
                    let _=reply.send(Err(HostError::NotLeader));
                } else if self.maintenance || self.reads.len()>=self.cfg.queue_capacity
                    || self.core.heartbeat_round_==u64::MAX {
                    let _=reply.send(Err(HostError::Unavailable("read authority unavailable".into())));
                } else {
                    if self.reads.is_empty() {self.last_tick=Instant::now()-self.cfg.heartbeat_interval;}
                    self.reads.push(ReadWaiter{term,minimum_round:self.core.heartbeat_round_,
                        applied_through:None,deadline,reply});
                }
            },
            Message::Propose { term, command, reply } => {
                if self.failure.is_some() || self.maintenance {
                    let _ = reply.send(Err(AdmissionError::Unavailable(self.failure.clone().unwrap_or("snapshot publication".into()))));
                } else if !self.core.is_leader_ || self.core.current_term_ != term {
                    let _ = reply.send(Err(AdmissionError::NotLeader { term: self.core.current_term_,
                        leader: (self.core.current_leader_id_ != u16::MAX).then_some(self.core.current_leader_id_) }));
                } else if self.core.raft_log_.last_index() >= RAFT_INDEX_LIMIT-2 {
                    let _ = reply.send(Err(AdmissionError::Unavailable("Raft index exhausted".into())));
                } else {
                    let size = command.0.as_ref().map_or(0, |b| b.len() as u64);
                    let index = match self.step(Event::Propose { cmd: command, has_value: true, is_tpc_commit: false, kind: 0, payload_bytes: size },None) {
                        Ok(r) => r.into_proposed()+1,
                        Err(e) => { let _ = reply.send(Err(AdmissionError::Unknown)); return Err(e); }
                    };
                    let proposal = Proposal { position: Position { index,term }, completion: Arc::new((Mutex::new(ProposalStatus::Pending), parking_lot::Condvar::new())),
                        committed: Arc::new(std::sync::atomic::AtomicBool::new(false)) };
                    self.proposals.insert(index, proposal.clone());
                    let _ = reply.send(Ok(proposal));
                    self.last_tick = Instant::now()-self.cfg.heartbeat_interval;
                }
            },
            Message::Inbound(envelope, token) => self.inbound(envelope, token)?,
            Message::Network(tag, response) => if self.failure.is_none() { self.network(tag,response)?; },
            Message::Applied(position,result) => {
                result?;
                if position.index != self.applied.index+1 || position.index > self.disk.state.hard.commit {
                    return Err("application completion outside durable committed prefix".into());
                }
                self.step(Event::Applied { index: position.index, published: self.applied.index },None)?;
                self.applied=position; self.applying=false;
                self.publish_status(); self.resolve();
            },
            Message::Checkpoint(reply) => {
                if self.failure.is_some() || self.maintenance || self.checkpoint_waiter.is_some() {
                    let _ = reply.send(Err(HostError::Unavailable("checkpoint unavailable".into())));
                } else { self.checkpoint_waiter=Some(reply); }
            },
            Message::Created(result,reply) => {
                let outcome = (|| {
                    let image=result?;
                    let position=image.reference.position;
                    let name=self.disk.save_image(position,&crate::wire::encode(&image.reference)?)?;
                    if !self.step(Event::CreateSnapshot { index: position.index, term: position.term as i64 },Some(name))?.into_created() {
                        return Err("core rejected checkpoint cut".into());
                    }
                    self.disk.checkpoint_base()?;
                    crash_point(&self.cfg, "snapshot.authority");
                    if let Some(old) = self.image.replace(image) { self.retired_images.push(old); }
                    self.maintenance=false;
                    Ok(position)
                })();
                let _=reply.send(outcome.clone().map_err(HostError::Storage));
                outcome?;
            },
            Message::Staged(result,sender,term,token) => {
                let image=result?;
                crash_point(&self.cfg, "snapshot.staged");
                let p=image.reference.position;
                let (_,contact,install)=self.step(Event::RecvInstallSnapshot { term,leader_site_id:sender,
                    last_included_index:p.index,last_included_term:p.term,applied:self.applied.index,stopped:false,failover:true },None)?.into_install_contact();
                if contact { self.reset_timer(); }
                if !contact || !install || self.applying {
                    self.maintenance=false;
                    std::fs::remove_dir_all(&image.directory).map_err(|e|e.to_string())?;
                    token.send(Response::Snapshot { term:self.core.current_term_,accepted:false });
                } else {
                    let name=self.disk.save_image(p,&crate::wire::encode(&image.reference)?)?;
                    if !self.step(Event::InstallSnapshot { index:p.index,term:p.term },Some(name))?.into_installed().0 {
                        return Err("core refused staged snapshot installation".into());
                    }
                    crash_point(&self.cfg, "snapshot.authority");
                    // WAL above is recovery authority BEFORE application generation swap.
                    self.application.send(Application::Publish(image,token)).map_err(|_|"application worker stopped")?;
                }
            },
            Message::Published(image,result,token) => {
                result?;
                let p=image.reference.position;
                self.step(Event::Applied { index:p.index,published:self.applied.index },None)?;
                self.applied=p; self.maintenance=false;
                self.disk.checkpoint_base()?;
                if let Some(old) = self.image.replace(image) { self.retired_images.push(old); }
                self.publish_status();
                self.resolve();
                token.send(Response::Snapshot { term:self.core.current_term_,accepted:true });
            },
            Message::Shutdown => {},
        }
        Ok(())
    }
    fn schedule_apply(&mut self) -> Result<(),String> {
        if self.applying || self.maintenance { return Ok(()); }
        if let Some(reply)=self.checkpoint_waiter.take() {
            if self.applied.index==0 {
                let _=reply.send(Err(HostError::Unavailable("no applied checkpoint prefix".into())));
            } else if self.image.as_ref().is_some_and(|i|i.reference.position==self.applied) {
                let _=reply.send(Ok(self.applied));
            } else {
                self.maintenance=true;
                self.application.send(Application::Create(self.applied,reply)).map_err(|_|"application worker stopped")?;
            }
            return Ok(());
        }
        if self.applied.index<self.disk.state.hard.commit {
            let index=self.applied.index+1;
            let entry=self.core.raft_log_.get(index).ok_or("committed apply entry unavailable")?;
            let position=Position { index,term:entry.term() as u64 };
            self.application.send(Application::Apply(position,entry.cmd().clone())).map_err(|_|"application worker stopped")?;
            self.applying=true;
        }
        Ok(())
    }
    fn inbound(&mut self, envelope:Envelope, token:ReplyToken)->Result<(),String> {
        if self.isolation.active() {
            token.send(Response::Error("Raft peer isolated by test hook".into())); return Ok(());
        }
        if envelope.version!=crate::wire::version(self.cfg.sync_writes) || envelope.group!=self.cfg.group || !self.core.is_config_member(envelope.sender) || envelope.sender==self.cfg.node {
            token.send(Response::Error("wrong Raft group/version/durability/member".into())); return Ok(());
        }
        if let Some(error)=&self.failure { token.send(Response::Error(error.clone())); return Ok(()); }
        let sender=envelope.sender;
        match envelope.request {
            Request::Vote { term,last } => {
                if term>=RAFT_INDEX_LIMIT || last.term>=RAFT_INDEX_LIMIT || last.index>=RAFT_INDEX_LIMIT { token.send(Response::Error("Raft position out of range".into())); return Ok(()); }
                let (term,granted)=self.step(Event::RecvRequestVote { stopped:false,candidate_is_current_voter:true,lst_log_idx:last.index,
                    lst_log_term:last.term as i64,can_id:sender,can_term:term as i64,failover:true,election_debug:false },None)?.into_vote();
                token.send(Response::Vote { term:term as u64,granted:granted!=0 });
            },
            Request::Append { term,previous,commit,batch } => {
                if term>=RAFT_INDEX_LIMIT || previous.index>=RAFT_INDEX_LIMIT || previous.term>=RAFT_INDEX_LIMIT || commit>=RAFT_INDEX_LIMIT
                    || batch.0.iter().any(|(t,c)|*t>=RAFT_INDEX_LIMIT || c.0.as_ref().is_some_and(|b|b.len()>self.cfg.max_command_bytes)) {
                    token.send(Response::Error("invalid append bounds".into())); return Ok(());
                }
                let (_,accepted,term,last)=self.step(Event::RecvAppendEntries { wire:&batch,stopped:false,sender_is_current_voter:true,
                    has_cmd:!batch.0.is_empty(),leader_current_term:term,leader_site_id:sender,leader_prev_log_index:previous.index,
                    leader_prev_log_term:previous.term,leader_commit_index:commit,failover:true },None)?.into_append();
                token.send(Response::Append { term,accepted:accepted!=0,last });
            },
            request => {
                let (term,position)=match &request {
                    Request::SnapshotBegin{term,position,..}|Request::SnapshotChunk{term,position,..}
                    |Request::SnapshotFinish{term,position}|Request::SnapshotContact{term,position} =>(*term,*position),
                    _=>unreachable!(),
                };
                if term>=RAFT_INDEX_LIMIT || position.index>=RAFT_INDEX_LIMIT || position.term>=RAFT_INDEX_LIMIT {
                    token.send(Response::Snapshot{term:self.core.current_term_,accepted:false});return Ok(());
                }
                let (reply_term,contact,install)=self.step(Event::RecvInstallSnapshot { term,leader_site_id:sender,last_included_index:position.index,
                    last_included_term:position.term,applied:self.applied.index,stopped:false,failover:true },None)?.into_install_contact();
                // Unlike AppendEntries, the existing core's snapshot contact
                // only resets on an actual role transition. Every accepted
                // chunk/keepalive is leader contact, even while already following.
                if contact { self.reset_timer(); }
                if matches!(request, Request::SnapshotContact { .. }) {
                    token.send(Response::Snapshot { term:reply_term,accepted:false });
                    return Ok(());
                }
                if self.maintenance {
                    token.send(Response::Snapshot{term:reply_term,accepted:false});return Ok(());
                }
                if contact && !install && self.applied.index >= position.index {
                    token.send(Response::SnapshotCurrent { term:reply_term });return Ok(());
                }
                if !contact || !install {
                    token.send(Response::Snapshot{term:reply_term,accepted:false});return Ok(());
                }
                let job=match request {
                    Request::SnapshotBegin{digest,..}=>Application::Begin{position,digest,term,token},
                    Request::SnapshotChunk{file,offset,bytes,last,..}=>{
                        if bytes.len()>self.cfg.snapshot_chunk_bytes {token.send(Response::Error("checkpoint chunk too large".into()));return Ok(());}
                        Application::Chunk{position,file,offset,bytes,last,term,token}
                    },
                    Request::SnapshotFinish{..}=>{
                        if self.applying { token.send(Response::Snapshot{term:reply_term,accepted:false});return Ok(()); }
                        self.maintenance=true;
                        Application::Stage{position,sender,term,token}
                    },
                    _=>unreachable!(),
                };
                self.application.send(job).map_err(|_|"application worker stopped")?;
            },
        }
        Ok(())
    }
    fn send(&mut self,peer:u16,job:Job) {
        let sender = if matches!(job, Job::Snapshot { .. }) { self.snapshot_peers.get(&peer) } else { self.peers.get(&peer) };
        if let Some(sender)=sender {
            if let Err(error)=sender.try_send(job) {
                let (mpsc::TrySendError::Full(job) | mpsc::TrySendError::Disconnected(job)) = error;
                let tag=match job{Job::Rpc{tag,..}|Job::Snapshot{tag,..}=>tag};
                // Called by the group itself: never block its bounded inbox.
                let _=self.network(tag,Err("peer transport queue busy".into()));
            }
        }
    }
    fn election_due(&self)->bool {
        !self.core.is_leader_ && self.campaign.is_none()
            && self.now().saturating_sub(self.core.last_heartbeat_time_)>=self.core.election_timeout_us_
    }
    fn clock(&mut self)->Result<(),String> {
        if self.campaign.as_ref().is_some_and(|c|Instant::now()>=c.deadline) { self.settle(true)?; }
        if self.election_due() {
            let campaign=self.step(Event::StartElection { timer_guarded:true,expected_generation:self.core.election_timer_generation_,now:self.now(),stopped:false },None)?.into_campaign();
            if campaign.started_ {
                self.campaign=Some(Campaign { term:campaign.term_,votes:BTreeMap::new(),deadline:Instant::now()+self.cfg.election_timeout });
                let peers=self.peers.keys().copied().collect::<Vec<_>>();
                for peer in peers { self.send(peer,Job::Rpc { request:Request::Vote { term:campaign.term_,last:Position{index:campaign.lst_idx_,term:campaign.lst_term_ as u64}},tag:Tag::Vote{campaign:campaign.term_,peer} }); }
                if self.peers.is_empty() { self.settle(false)?; }
            }
        }
        if self.core.is_leader_ && self.last_tick.elapsed()>=self.cfg.heartbeat_interval {
            self.last_tick=Instant::now();
            let tick=self.step(Event::TickHeartbeat { is_leader:true,snapshot_configured:self.image.is_some(),batching:false,
                max_batch_entries:1,max_batch_bytes:self.cfg.max_command_bytes as u64,now_us:self.now(),pipeline_after_us:0 },None)?.into_tick();
            for send in tick.sends_ {
                let entries=if send.cmds_.is_empty(){Vec::new()}else{send.cmds_.into_iter().enumerate().map(|(i,c)|{
                    let term=send.terms_.get(i).copied().map_or(send.entry_term_,|t|t as u64);(term,c)
                }).collect()};
                self.send(send.site_id_,Job::Rpc { request:Request::Append { term:send.term_,previous:Position{index:send.prev_log_index_,term:send.prev_log_term_},commit:send.commit_index_,batch:Batch(entries)},
                    tag:Tag::Append{slot:send.slot_,term:send.term_,round:send.sent_round_} });
            }
            for send in tick.snapshots_ {
                self.send(send.site_id_,Job::Rpc{request:Request::SnapshotContact{term:send.term_,position:Position{index:send.index_,term:send.snap_term_ as u64}},
                    tag:Tag::Contact{ordinal:send.ord_,peer:send.site_id_,term:send.term_}});
                if self.snapshot_sending.insert(send.site_id_) {
                    if let Some(image)=self.image.clone() {
                        self.send(send.site_id_,Job::Snapshot{image,term:send.term_,tag:Tag::Snapshot{ordinal:send.ord_,peer:send.site_id_,term:send.term_,position:Position{index:send.index_,term:send.snap_term_ as u64}}});
                    }
                }
            }
            self.step(Event::RoundEnd{is_leader:self.core.is_leader_},None)?;
        }
        Ok(())
    }
    fn settle(&mut self,timed_out:bool)->Result<(),String> {
        let Some(campaign)=self.campaign.take() else{return Ok(());};
        let voters=campaign.votes.keys().copied().collect::<Vec<_>>();
        let granted=campaign.votes.values().map(|v|v.0).collect::<Vec<_>>();
        let terms=campaign.votes.values().map(|v|v.1 as i64).collect::<Vec<_>>();
        self.step(Event::SettleElection{term:campaign.term,loc_id:self.cfg.node as u32,voters:&voters,granted:&granted,reply_terms:&terms,
            n_total:self.cfg.peers.len() as u64,timed_out,stopped:false,looping:true,failover:true,election_debug:false},None)?;
        self.reset_timer();
        Ok(())
    }
    fn network(&mut self,tag:Tag,result:Result<Response,String>)->Result<(),String> {
        let result=if self.isolation.active() {Err("Raft peer isolated by test hook".into())} else {result};
        // Higher terms remain authoritative even after a campaign or an append
        // slot has been retired. Transport correlation fences replication credit,
        // never the core's higher-term rule.
        let response_term=match &result {
            Ok(Response::Vote{term,..}|Response::Append{term,..}|Response::Snapshot{term,..}|Response::SnapshotCurrent{term})=>Some(*term),
            _=>None,
        };
        if let Some(term)=response_term.filter(|t|*t>self.core.current_term_ && *t<RAFT_INDEX_LIMIT) {
            let mut output=CoreOutput::new();
            self.core.observe_term(term,false,true,&mut output);
            output.finish_persist(true,self.core.current_term_,self.core.vote_for_,self.core.commit_index_);
            self.finish_output(&mut output,None)?;
        }
        match tag {
            Tag::Vote{campaign,peer}=>{
                if let Some(current)=&mut self.campaign {
                    if current.term==campaign {
                        if let Ok(Response::Vote{term,granted})=result {
                            if term<RAFT_INDEX_LIMIT {current.votes.insert(peer,(granted,term));}
                        }
                        if current.votes.values().any(|v|v.1>campaign)
                            || current.votes.values().filter(|v|v.0).count()>=self.cfg.peers.len()/2
                            || current.votes.len()==self.peers.len() {self.settle(false)?;}
                    }
                }
            },
            Tag::Append{slot,term,round}=>{
                if slot>=self.core.pending_rpcs_.len() || self.core.pending_rpcs_.sent_term(slot)!=term || self.core.pending_rpcs_.sent_round(slot)!=round {return Ok(());}
                let (status,term,last)=match result {Ok(Response::Append{accepted,term,last}) if term<RAFT_INDEX_LIMIT=>(accepted,term,last),_=>(false,0,0)};
                let before_commit=self.core.commit_index_;
                self.step(Event::RecvAppendReply{ord:slot,status,term,last_log_index:last,is_leader:self.core.is_leader_,stopped:false,failover:true},None)?;
                self.step(Event::RoundEnd{is_leader:self.core.is_leader_},None)?;
                let authority=(self.core.read_quorum_confirmed_term_,self.core.read_quorum_confirmed_round_);
                let fresh_read_waiting=self.core.is_leader_
                    && self.reads.iter().any(|read|read.needs_authority(self.core.current_term_,authority));
                // Retiring a pre-admission RPC may make the required fresh
                // quorum launchable. Do not strand that read until the next
                // periodic heartbeat on an otherwise idle log.
                if term!=0 && (last<self.core.raft_log_.last_index() || before_commit!=self.core.commit_index_
                    || (fresh_read_waiting && can_launch_read_quorum(&self.core))) {
                    self.last_tick=Instant::now()-self.cfg.heartbeat_interval;
                }
            },
            Tag::Snapshot{ordinal,peer,term,position}=>{
                self.snapshot_sending.remove(&peer);
                if let Err(error)=&result {
                    eprintln!("raft-host snapshot group={} node={} peer={} position={:?} send_term={} current_term={} error={}",
                        self.cfg.group,self.cfg.node,peer,position,term,self.core.current_term_,error);
                }
                if let Ok(Response::Snapshot{term:follower_term,accepted})=result {
                    if follower_term<RAFT_INDEX_LIMIT && (accepted || follower_term>self.core.current_term_) {
                        self.step(Event::RecvSnapshotReply{ord:ordinal,site_id:peer,snap_last_idx:if accepted{position.index}else{0},send_term:term,follower_term,stopped:false,failover:true},None)?;
                    }
                }
            },
            Tag::Contact{ordinal,peer,term}=>{
                if let Ok(Response::Snapshot{term:follower_term,..})=result {
                    if follower_term>self.core.current_term_ && follower_term<RAFT_INDEX_LIMIT {
                        self.step(Event::RecvSnapshotReply{ord:ordinal,site_id:peer,snap_last_idx:0,send_term:term,follower_term,stopped:false,failover:true},None)?;
                    }
                }
            },
        }
        Ok(())
    }
}
fn application_loop(cfg:Config,machine:Arc<dyn StateMachine>,rx:mpsc::Receiver<Application>,tx:mpsc::SyncSender<Message>) {
    let mut incoming:Option<Incoming>=None;
    while let Ok(job)=rx.recv() {
        let message=match job {
            Application::Apply(position,command)=>Message::Applied(position,machine.apply(position,command.0.as_deref())),
            Application::Create(position,reply)=>Message::Created(snapshot::create(&cfg.directory,cfg.group,position,&*machine,cfg.sync_writes),reply),
            Application::Begin{position,digest,term,token}=>{
                if let Some(previous) = incoming.take() {
                    let path = previous.image.directory.clone();
                    drop(previous);
                    if let Err(e) = std::fs::remove_dir_all(path) { token.send(Response::Error(e.to_string())); continue; }
                }
                match Incoming::begin(&cfg.directory,cfg.group,position,digest) {
                    Ok(stage)=>{incoming=Some(stage);token.send(Response::Snapshot{term,accepted:true});},
                    Err(e)=>token.send(Response::Error(e)),
                }
                continue;
            },
            Application::Chunk{position,file,offset,bytes,last,term,token}=>{
                let result=incoming.as_mut().filter(|i|i.image.reference.position==position).ok_or_else(||"snapshot staging not found".to_string())
                    .and_then(|stage|stage.chunk(file,offset,&bytes,last));
                match result {Ok(())=>token.send(Response::Snapshot{term,accepted:true}),Err(e)=>{incoming=None;token.send(Response::Error(e));}}
                continue;
            },
            Application::Stage{position,sender,term,token}=>{
                let result=incoming.take().filter(|i|i.image.reference.position==position).ok_or_else(||"snapshot staging not found".to_string())
                    .and_then(|stage|stage.finish(&*machine,cfg.sync_writes));
                Message::Staged(result,sender,term,token)
            },
            Application::Publish(image,token)=>{
                let result=machine.publish_checkpoint(&image.directory,image.reference.position).and_then(|()|{
                    if machine.applied_position()?==image.reference.position {Ok(())}else{Err("snapshot publication position mismatch".into())}
                });
                if result.is_ok() { crash_point(&cfg, "snapshot.published"); }
                Message::Published(image,result,token)
            },
        };
        let failed=matches!(&message,Message::Applied(_,Err(_))|Message::Created(Err(_),_)|Message::Staged(Err(_),_,_,_)|Message::Published(_,Err(_),_));
        if tx.send(message).is_err() || failed {break;}
    }
}

/// Explicit process-crash cutpoints for real-server recovery qualification.
/// Unset by default; no consensus decision depends on them.
fn crash_point(cfg: &Config, point: &str) {
    if std::env::var("RAFT_HOST_CRASH_POINT").as_deref() == Ok(point)
        && std::env::var("RAFT_HOST_CRASH_GROUP").map_or(true, |g| g == cfg.group.to_string())
    {
        eprintln!("raft-host group {} crash point {point}", cfg.group);
        std::process::abort();
    }
}

impl Group {
    /// A retired image is eligible only after the new authority has reached a
    /// flushed base, and after every outgoing stream releases its immutable
    /// image lease. Application generations are separately owned by the adapter.
    fn collect_retired(&mut self) -> Result<(), String> {
        let mut index = 0;
        while index < self.retired_images.len() {
            if !self.retired_images[index].exclusively_retained() { index += 1; continue; }
            let image = &self.retired_images[index];
            std::fs::remove_dir_all(&image.directory).map_err(|e| e.to_string())?;
            let p = image.reference.position;
            let image_file = self.cfg.directory.join("images").join(raft_store::images::name(p.index, p.term));
            std::fs::remove_file(image_file).map_err(|e| e.to_string())?;
            self.disk.fs.sync_dir(&self.cfg.directory.join("checkpoints")).map_err(|e|e.to_string())?;
            self.disk.fs.sync_dir(&self.cfg.directory.join("images")).map_err(|e|e.to_string())?;
            self.retired_images.swap_remove(index);
        }
        Ok(())
    }
}

#[cfg(test)]
mod read_tests {
    use super::*;
    use raft_core::{AuthorityLedger,AuthorityReply,HeartbeatAuthority};

    #[derive(Clone,Copy)]
    struct FixedSample(u64);
    impl std::hash::Hasher for FixedSample {
        fn finish(&self)->u64 {self.0}
        fn write(&mut self,_:&[u8]) {}
    }
    impl BuildHasher for FixedSample {
        type Hasher=Self;
        fn build_hasher(&self)->Self { *self }
    }

    #[test]
    fn production_timer_draws_separate_survivors_and_allow_either_to_win() {
        let base=600_000u64;
        let origins=[0,35_769];
        let wall=10_000_000u64;
        for samples in [[0,base/2],[base/2,0]] {
            let mut nodes=[RaftCore::<Command>::new(),RaftCore::<Command>::new()];
            let mut draws=[0,0];
            let mut delays=[0,0];
            for (index,node) in nodes.iter_mut().enumerate() {
                node.step::<Batch>(Event::SetIdentity{loc_id:index as u32+1,site_id:index as u16+1,partition_id:0},&mut CoreOutput::new());
                node.step::<Batch>(Event::Configure{members:&[1,2,3]},&mut CoreOutput::new());
                assert!(node.step::<Batch>(Event::EnterGates{unsaved_restart:false,failover:true},&mut CoreOutput::new()).into_gates());
                delays[index]=next_election_delay(&FixedSample(samples[index]),107,index as u16+1,&mut draws[index],base);
                assert_eq!(draws[index],1);
                assert!((base..2*base).contains(&delays[index]));
                node.step::<Batch>(Event::ResetElectionTimer{now:wall-origins[index],timeout_us:delays[index]},&mut CoreOutput::new());
            }
            let early=if delays[0]<delays[1] {0}else{1};
            let late=1-early;
            let due=wall+delays[early]+1;
            let generation=nodes[late].election_timer_generation_;
            let premature=nodes[late].step::<Batch>(Event::StartElection{
                timer_guarded:true,expected_generation:generation,now:due-origins[late],stopped:false},
                &mut CoreOutput::new()).into_campaign();
            assert!(!premature.started_);
            let generation=nodes[early].election_timer_generation_;
            let campaign=nodes[early].step::<Batch>(Event::StartElection{
                timer_guarded:true,expected_generation:generation,now:due-origins[early],stopped:false},
                &mut CoreOutput::new()).into_campaign();
            assert!(campaign.started_);
            let candidate=early as u16+1;
            let voter=late as u16+1;
            let (reply_term,granted)=nodes[late].step::<Batch>(Event::RecvRequestVote{
                stopped:false,candidate_is_current_voter:true,lst_log_idx:0,lst_log_term:0,
                can_id:candidate,can_term:campaign.term_ as i64,failover:true,election_debug:false},
                &mut CoreOutput::new()).into_vote();
            assert_ne!(granted,0);
            nodes[early].step::<Batch>(Event::SettleElection{
                term:campaign.term_,loc_id:candidate as u32,voters:&[voter],granted:&[true],reply_terms:&[reply_term],
                n_total:3,timed_out:false,stopped:false,looping:true,failover:true,election_debug:false},
                &mut CoreOutput::new());
            assert!(nodes[early].is_leader_);
            assert!(!nodes[late].is_leader_);
            // Every reset consumes a new draw, even when wall time is unchanged.
            let _=next_election_delay(&FixedSample(base-1),107,candidate,&mut draws[early],base);
            assert_eq!(draws[early],2);
        }
        let mut draw=0;
        for sample in [0,1,base-1,base,u64::MAX] {
            assert!((base..2*base).contains(&next_election_delay(&FixedSample(sample),107,1,&mut draw,base)));
        }
        assert_eq!(draw,5);
    }

    #[test]
    fn fresh_read_launch_waits_for_enough_free_peers_in_one_generation() {
        let mut core=RaftCore::<Command>::new();
        core.step::<Batch>(Event::SetIdentity{loc_id:1,site_id:1,partition_id:0},&mut CoreOutput::new());
        core.step::<Batch>(Event::Configure{members:&[1,2,3,4,5]},&mut CoreOutput::new());
        assert_eq!(core.peers_.len(),4);
        core.pending_rpcs_.resize(4);
        for ordinal in 0..4 {
            core.peers_.set_next_index(ordinal,1);
            core.pending_rpcs_.place(ordinal,raft_core::PendingAppend::new(
                core.peer_site_at(ordinal),7,4,0,false,0,0,0));
        }
        assert!(!can_launch_read_quorum(&core));
        core.pending_rpcs_.release(0);
        assert!(!can_launch_read_quorum(&core));
        core.pending_rpcs_.release(1);
        assert!(can_launch_read_quorum(&core));
    }

    #[test]
    fn late_reader_requires_new_generation_current_term_commit_and_application() {
        let members=[1,2,3];
        let mut ledger=AuthorityLedger::new();
        assert!(ledger.open(4,&members,HeartbeatAuthority::new(7,3,1)));
        assert!(ledger.launch(4,2));
        // The read arrives after round4 was launched but before its reply.
        let (reply,_receiver)=mpsc::sync_channel(1);
        let mut read=ReadWaiter{term:7,minimum_round:5,applied_through:None,
            deadline:Instant::now()+Duration::from_secs(3),reply};
        ledger.record_reply(&AuthorityReply::new(4,2,7,7,7,true,true));
        let older=ledger.settle(true,7,&members,0,0);
        assert!(older.confirmed());
        let commit=Position{index:11,term:7};
        assert!(read.ready(commit,(older.term(),older.round_id()),11).is_none());
        assert!(read.applied_through.is_none());
        assert!(read.needs_authority(7,(older.term(),older.round_id())));

        assert!(ledger.open(5,&members,HeartbeatAuthority::new(7,3,1)));
        assert!(ledger.launch(5,2));
        ledger.record_reply(&AuthorityReply::new(5,2,7,7,7,true,true));
        let fresh=ledger.settle(true,7,&members,older.term(),older.round_id());
        assert!(fresh.confirmed());
        let authority=(fresh.term(),fresh.round_id());
        // Fresh quorum alone cannot replace the election's current-term commit.
        assert!(read.ready(Position{index:10,term:6},authority,10).is_none());
        assert!(read.applied_through.is_none());
        assert!(!read.needs_authority(7,authority));
        // Nor can it bypass local application of the captured committed cut.
        assert!(read.ready(commit,authority,10).is_none());
        assert_eq!(read.applied_through,Some(commit));
        // Later commits do not keep moving the original read's apply target.
        assert_eq!(read.ready(Position{index:12,term:7},authority,11),Some(commit));
    }
}
