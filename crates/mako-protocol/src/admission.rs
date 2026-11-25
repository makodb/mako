//! Sequential participant identity and terminal receipts. This is application
//! protocol state only; it neither reads a tree nor implements transaction CC.
use crate::wire::{Action, Error, ErrorCode, Request, Response, Result, Value};

pub use core::Phase;
use mako_protocol_core::admission::{self as core, Claim, Decision, Header, Operation, Refusal};

fn operation(action: &Action) -> Operation {
    match action {
        Action::Install { .. } => Operation::Install,
        Action::Abort => Operation::Abort,
        Action::Validate => Operation::Validate,
        _ => Operation::Ordinary,
    }
}
fn header(request: &Request) -> Header {
    Header {
        incarnation: request.identity.incarnation,
        attempt: request.identity.attempt,
        sequence: request.identity.sequence,
        term: request.identity.term,
        node: request.stream.node,
        worker: request.stream.worker,
    }
}
pub enum Admission {
    Begin,
    Continue,
    Reply(Response),
}
pub struct Session {
    binding: Option<(u64, u64)>,
    phase: Phase,
    last: Option<Request>,
    response: Option<Response>,
}
impl Default for Session {
    fn default() -> Self {
        Self {
            binding: None,
            phase: Phase::Idle,
            last: None,
            response: None,
        }
    }
}
impl Session {
    pub fn owns_transaction(&self, request: &crate::wire::TransactionRequest) -> bool {
        self.binding == Some((request.id.incarnation, request.lease))
            && self.last.as_ref().is_some_and(|last| {
                last.stream.node == request.id.coordinator
                    && last.stream.worker == request.id.worker
                    && last.identity.incarnation == request.lease
                    && last.identity.attempt == request.id.attempt
                    && last.identity.term == request.term
            })
    }
    pub fn seal_external(&mut self) {
        self.phase = Phase::Sealed;
    }
    pub fn finish_external(&mut self, outcome: crate::journal::Outcome) {
        self.phase = match outcome {
            crate::journal::Outcome::Commit => Phase::Committed,
            crate::journal::Outcome::Abort => Phase::Aborted,
        };
    }
    /// Replace only a physically cleaned-up stream. The receiver mints a fresh
    /// lease, so a delayed OPEN cannot make its old data packets current again.
    /// Repeating the current OPEN returns the same lease after a lost reply.
    pub fn claim(&mut self, nonce: u64, fresh_lease: u64) -> Result<u64> {
        match core::claim(self.phase, self.binding, nonce, fresh_lease) {
            Claim::Existing(lease) => return Ok(lease),
            Claim::Invalid => return Err(Error::new(ErrorCode::Invalid, "invalid stream claim")),
            Claim::Unknown => {
                return Err(Error::new(
                    ErrorCode::Unknown,
                    "stream has an unresolved outcome",
                ))
            }
            Claim::Busy => {
                return Err(Error::new(
                    ErrorCode::Busy,
                    "stream has a retained transaction",
                ))
            }
            Claim::Collision => {
                return Err(Error::new(ErrorCode::Capacity, "stream lease collision"))
            }
            Claim::New => (),
        }
        self.binding = Some((nonce, fresh_lease));
        self.phase = Phase::Idle;
        self.last = None;
        self.response = None;
        Ok(fresh_lease)
    }

    pub fn phase(&self) -> Phase {
        self.phase
    }
    pub fn active(&self) -> bool {
        core::active(self.phase)
    }

    pub fn admit(&self, request: &Request, minimum_term: u32) -> Admission {
        let decision = core::admit(
            self.phase,
            self.binding.map(|(_, lease)| lease),
            self.last.as_ref().map(header),
            header(request),
            operation(&request.action),
            minimum_term,
            request.action.valid(),
            self.last.as_ref() == Some(request) && self.response.is_some(),
        );
        let rejected = if matches!(request.action, Action::Install { .. }) {
            ErrorCode::Unknown
        } else {
            ErrorCode::Conflict
        };
        let refusal = match decision {
            Decision::Begin => return Admission::Begin,
            Decision::Continue => return Admission::Continue,
            Decision::Replay => {
                return Admission::Reply(self.response.as_ref().expect("exact receipt").clone())
            }
            Decision::AbortedReplay => {
                return Admission::Reply(Response {
                    identity: request.identity,
                    result: Ok(Value::Done),
                })
            }
            Decision::Reject(reason) => reason,
        };
        let (code, message) = match refusal {
            Refusal::Lease => (rejected, "participant stream lease is absent or retired"),
            Refusal::Invalid => (rejected, "invalid or stale participant request"),
            Refusal::Unknown => (ErrorCode::Unknown, "participant outcome is unresolved"),
            Refusal::Stream => (rejected, "stream does not own this lane"),
            Refusal::Sequence => (rejected, "stale or mismatched duplicate"),
            Refusal::TerminalAborted => (rejected, "attempt is terminal"),
            Refusal::TerminalCommitted => (ErrorCode::Unknown, "attempt is terminal"),
            Refusal::NeedsValidation => {
                (ErrorCode::Unknown, "install requires validated participant")
            }
            Refusal::Sealed => (rejected, "participant is sealed"),
            Refusal::PriorActive => (rejected, "prior attempt has not released this lane"),
            Refusal::MissingParticipant => {
                (ErrorCode::Unknown, "install has no retained participant")
            }
        };
        Admission::Reply(Response::error(request.identity, code, message))
    }

    /// Record only after physical effects and terminal cleanup. An INSTALL
    /// failure is never retryable, even if its underlying error was a conflict.
    pub fn record(&mut self, request: Request, mut response: Response) -> Response {
        debug_assert_eq!(request.identity, response.identity);
        self.phase = core::completed(
            operation(&request.action),
            response.result.is_ok(),
            matches!(&response.result, Err(e) if matches!(e.code, ErrorCode::Unknown | ErrorCode::CommittedFailure)),
        );
        if matches!(request.action, Action::Install { .. }) {
            if let Err(error) = &mut response.result {
                error.code = ErrorCode::Unknown;
            }
        }
        self.last = Some(request);
        self.response = Some(response.clone());
        response
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire::{Access, Identity, Stream, TableKey};
    fn opened() -> Session {
        let mut session = Session::default();
        session.claim(100, 7).unwrap();
        session
    }
    fn request(sequence: u64, action: Action) -> Request {
        Request {
            stream: Stream { node: 0, worker: 0 },
            identity: Identity {
                incarnation: 7,
                attempt: 1,
                sequence,
                term: 0,
            },
            action,
        }
    }
    fn read(sequence: u64) -> Request {
        request(
            sequence,
            Action::Get {
                access: Access {
                    table: TableKey {
                        table: 1,
                        warehouse: 0,
                    },
                    route: None,
                },
                key: b"key".to_vec(),
            },
        )
    }
    fn record(session: &mut Session, request: Request) {
        let response = Response {
            identity: request.identity,
            result: Ok(Value::Done),
        };
        session.record(request, response);
    }
    #[test]
    fn duplicates_require_identical_payload_and_stale_packets_cannot_adopt_a_new_attempt() {
        let mut session = opened();
        assert!(matches!(session.admit(&read(1), 0), Admission::Begin));
        record(&mut session, read(1));
        assert!(matches!(session.admit(&read(1), 0), Admission::Reply(r) if r.result.is_ok()));
        assert!(
            matches!(session.admit(&request(1, Action::Abort), 0), Admission::Reply(r) if r.result.is_err())
        );
        let mut next = read(2);
        next.identity.attempt += 1;
        assert!(matches!(session.admit(&next, 0), Admission::Reply(r) if r.result.is_err()));
        record(&mut session, request(2, Action::Abort));
        assert!(matches!(session.admit(&next, 0), Admission::Begin));
        record(&mut session, next);
        assert!(
            matches!(session.admit(&request(3, Action::Abort), 0), Admission::Reply(r) if r.result.is_err())
        );
    }
    #[test]
    fn install_requires_validation_and_unknown_fences_even_newer_terms() {
        let mut session = opened();
        record(&mut session, read(1));
        let install = request(3, Action::Install { timestamp: 2 });
        assert!(
            matches!(session.admit(&install, 0), Admission::Reply(r) if matches!(&r.result, Err(e) if e.code == ErrorCode::Unknown))
        );
        record(&mut session, request(2, Action::Validate));
        assert!(matches!(session.admit(&install, 0), Admission::Continue));
        let response = Response::error(
            install.identity,
            ErrorCode::Transport,
            "reply lost after install",
        );
        let terminal = session.record(install.clone(), response);
        assert!(matches!(&terminal.result, Err(e) if e.code == ErrorCode::Unknown));
        assert!(matches!(session.admit(&install, 0), Admission::Reply(r) if r == terminal));
        let mut newer = read(4);
        newer.identity.term = 1;
        assert!(
            matches!(session.admit(&newer, 0), Admission::Reply(r) if matches!(&r.result, Err(e) if e.code == ErrorCode::Unknown))
        );
    }
    #[test]
    fn committed_receipt_survives_retries_and_abort_cannot_relabel_it() {
        let mut session = opened();
        record(&mut session, request(1, Action::Validate));
        let install = request(2, Action::Install { timestamp: 9 });
        record(&mut session, install.clone());
        assert!(matches!(session.admit(&install, 0), Admission::Reply(r) if r.result.is_ok()));
        assert!(
            matches!(session.admit(&request(3, Action::Abort), 0), Admission::Reply(r) if matches!(&r.result, Err(e) if e.code == ErrorCode::Unknown))
        );
    }
    #[test]
    fn higher_term_cancels_uninstalled_attempt_and_abort_creates_cancellation_fence() {
        let mut session = opened();
        record(&mut session, read(1));
        let mut fresh = request(1, Action::Abort);
        fresh.identity.term = 1;
        assert!(matches!(session.admit(&fresh, 0), Admission::Begin));
        record(&mut session, fresh.clone());
        fresh.identity.sequence += 1;
        assert!(matches!(session.admit(&fresh, 1), Admission::Reply(r) if r.result.is_ok()));
        assert!(matches!(session.admit(&read(2), 1), Admission::Reply(r) if r.result.is_err()));
    }

    #[test]
    fn stream_handoff_requires_cleanup_and_stale_open_cannot_revive_old_packets() {
        let mut session = opened();
        assert_eq!(session.claim(100, 8).unwrap(), 7); // Lost OPEN reply.
        record(&mut session, read(1));
        assert_eq!(session.claim(200, 9).unwrap_err().code, ErrorCode::Busy);
        record(&mut session, request(2, Action::Abort));
        assert_eq!(session.claim(200, 9).unwrap(), 9);
        let mut current = read(1);
        current.identity.incarnation = 9;
        assert!(matches!(session.admit(&current, 0), Admission::Begin));
        assert!(matches!(session.admit(&read(3),0),Admission::Reply(r) if r.result.is_err()));
        // A delayed OPEN from the retired client gets a new lease, not its
        // old one. Its old writes and abort packets remain stale.
        assert_eq!(session.claim(100, 10).unwrap(), 10);
        assert!(matches!(session.admit(&read(3),0),Admission::Reply(r) if r.result.is_err()));
        assert!(matches!(session.admit(&current,0),Admission::Reply(r) if r.result.is_err()));
        let mut active = read(1);
        active.identity.incarnation = 10;
        record(&mut session, active);
        let mut install = request(2, Action::Install { timestamp: 2 });
        install.identity.incarnation = 10;
        session.record(
            install.clone(),
            Response::error(install.identity, ErrorCode::Unknown, "uncertain"),
        );
        assert_eq!(session.claim(300, 11).unwrap_err().code, ErrorCode::Unknown);
        assert_eq!(session.claim(100, 11).unwrap(), 10);
        assert!(
            matches!(session.admit(&install,0),Admission::Reply(r) if matches!(&r.result,Err(e) if e.code == ErrorCode::Unknown))
        );
    }
}
