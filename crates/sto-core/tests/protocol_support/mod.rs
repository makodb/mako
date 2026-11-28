//! Small contract implementations. No tree, RPC server, consensus or local CC.
#![allow(dead_code)]
use sto_core::protocol::{
    contracts::{ParticipantEngine, Transport},
    epoch::{EpochDomain, Pin},
    participant::Lane,
    wire::*,
};
use std::{
    collections::{BTreeMap, VecDeque},
    sync::{Arc, Mutex},
    time::Duration,
};

#[derive(Default)]
pub struct Data {
    pub rows: BTreeMap<Vec<u8>, Vec<u8>>,
    pub installed: usize,
    pub calls: usize,
    pub quarantine: Vec<Pin>,
}
pub struct Engine {
    pub data: Arc<Mutex<Data>>,
    pub epochs: Arc<EpochDomain>,
    pub pin: Option<Pin>,
    pub term: u32,
    pub fail_install: bool,
    pending: BTreeMap<Vec<u8>, Vec<u8>>,
    uncertain: bool,
}
impl Engine {
    pub fn new() -> Self {
        Self {
            data: Arc::default(),
            epochs: Arc::default(),
            pin: None,
            term: 0,
            fail_install: false,
            pending: BTreeMap::new(),
            uncertain: false,
        }
    }
}
impl ParticipantEngine for Engine {
    fn minimum_term(&self) -> u32 {
        self.term
    }
    fn execute(&mut self, request: &Request, begin: bool) -> Result<Value> {
        self.data.lock().unwrap().calls += 1;
        if begin {
            self.pending.clear();
            self.pin = Some(self.epochs.enter());
        }
        match &request.action {
            Action::Stage { writes } => {
                for w in writes {
                    assert_eq!(w.mutation, Mutation::Put, "fixture supports put only");
                    self.pending.insert(w.key.clone(), w.value.clone());
                }
                Ok(Value::Done)
            }
            Action::Timestamp => Ok(Value::Timestamp(20)),
            Action::Validate => Ok(Value::Done),
            Action::Install { .. } => {
                let mut data = self.data.lock().unwrap();
                data.rows.append(&mut self.pending);
                data.installed += 1;
                if self.fail_install {
                    Err(Error::new(ErrorCode::Transport, "lost log acknowledgement"))
                } else {
                    Ok(Value::Done)
                }
            }
            Action::Abort => {
                self.pending.clear();
                Ok(Value::Done)
            }
            Action::Get { key, .. } => Ok(Value::Point {
                value: self.data.lock().unwrap().rows.get(key).cloned(),
                time_term: 200,
            }),
            _ => panic!("unsupported fixture operation"),
        }
    }
    fn release_terminal(&mut self) {
        self.pin.take();
    }
    fn retain_uncertain(&mut self) {
        self.uncertain = true;
    }
}
impl Drop for Engine {
    fn drop(&mut self) {
        if self.uncertain {
            if let Some(pin) = self.pin.take() {
                self.data.lock().unwrap().quarantine.push(pin);
            }
        }
    }
}
#[derive(Clone, Copy, Debug)]
pub enum Delivery {
    Normal,
    Duplicate,
    DropRequest,
    DropReply,
    StaleReply,
}
pub struct World {
    pub lanes: BTreeMap<String, Lane<Engine>>,
    pub delivery: VecDeque<Delivery>,
    pub now: u64,
    pub trace: Vec<String>,
    lease: u64,
}
pub struct Network(pub Mutex<World>);
impl Network {
    pub fn new(endpoints: &[&str]) -> Self {
        Self(Mutex::new(World {
            lanes: endpoints
                .iter()
                .map(|s| (s.to_string(), Lane::new(Engine::new())))
                .collect(),
            delivery: VecDeque::new(),
            now: 0,
            trace: vec![],
            lease: 100,
        }))
    }
    pub fn fault(&self, delivery: Delivery) {
        self.0.lock().unwrap().delivery.push_back(delivery);
    }
}
impl Transport for Network {
    fn call(&self, endpoint: &str, message: Message, timeout: Duration) -> Result<Reply> {
        let mut world = self.0.lock().unwrap();
        world.now += 1;
        let at = world.now;
        world.trace.push(format!("{at} {endpoint} {message:?}"));
        world.lease += 1;
        let lease = world.lease;
        let delivery = if matches!(message, Message::Participant(_)) {
            world.delivery.pop_front().unwrap_or(Delivery::Normal)
        } else {
            Delivery::Normal
        };
        if matches!(delivery, Delivery::DropRequest) {
            world.now += timeout.as_millis() as u64;
            return Err(Error::new(ErrorCode::Transport, "virtual request deadline"));
        }
        let lane = world.lanes.get_mut(endpoint).expect("configured endpoint");
        let mut reply = match message {
            Message::OpenStream { nonce, .. } => Reply::OpenStream(lane.claim(nonce, lease)),
            Message::Participant(request) => {
                let response = lane.process(request.clone());
                if matches!(delivery, Delivery::Duplicate) {
                    assert_eq!(
                        response,
                        lane.process(request),
                        "duplicate changes retained response"
                    );
                }
                Reply::Participant(response)
            }
            _ => panic!("unsupported fixture message"),
        };
        if matches!(delivery, Delivery::DropReply) {
            world.now += timeout.as_millis() as u64;
            return Err(Error::new(ErrorCode::Transport, "virtual reply deadline"));
        }
        if matches!(delivery, Delivery::StaleReply) {
            if let Reply::Participant(response) = &mut reply {
                response.identity.sequence -= 1;
            }
        }
        Ok(reply)
    }
}
impl Drop for Network {
    fn drop(&mut self) {
        if std::thread::panicking() {
            eprintln!(
                "deterministic delivery trace: {:#?}",
                self.0.lock().unwrap().trace
            );
        }
    }
}
pub fn access() -> Access {
    Access {
        table: TableKey {
            table: 31,
            warehouse: 0,
        },
        route: None,
    }
}
pub fn request(attempt: u64, sequence: u64, action: Action) -> Request {
    Request {
        stream: Stream { node: 0, worker: 0 },
        identity: Identity {
            incarnation: 101,
            attempt,
            sequence,
            term: 0,
        },
        action,
    }
}
pub fn put(value: &[u8]) -> Action {
    Action::Stage {
        writes: vec![Write {
            access: access(),
            mutation: Mutation::Put,
            key: b"c".to_vec(),
            value: value.to_vec(),
        }],
    }
}
