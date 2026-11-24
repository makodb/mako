//! Sequential participant identity and terminal receipts. This is application
//! protocol state only; it neither reads a tree nor implements transaction CC.
use crate::wire::{Action, ErrorCode, Request, Response, Value};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Phase {
    Idle,
    Active,
    Sealed,
    Aborted,
    Committed,
    Unknown,
}
pub enum Admission {
    Begin,
    Continue,
    Reply(Response),
}
pub struct Session {
    phase: Phase,
    last: Option<Request>,
    response: Option<Response>,
}
impl Default for Session {
    fn default() -> Self {
        Self {
            phase: Phase::Idle,
            last: None,
            response: None,
        }
    }
}
impl Session {
    pub fn phase(&self) -> Phase {
        self.phase
    }
    pub fn active(&self) -> bool {
        matches!(self.phase, Phase::Active | Phase::Sealed)
    }

    pub fn admit(&self, request: &Request, minimum_term: u32) -> Admission {
        let identity = request.identity;
        let rejected = if matches!(request.action, Action::Install { .. }) {
            ErrorCode::Unknown
        } else {
            ErrorCode::Conflict
        };
        let reject = |code, message| Admission::Reply(Response::error(identity, code, message));
        if !identity.valid() || !request.action.valid() || identity.term < minimum_term {
            return reject(rejected, "invalid or stale participant request");
        }
        // Exact terminal receipts remain available in a quarantined lane.
        if let Some(last) = &self.last {
            if last == request {
                if let Some(response) = &self.response {
                    return Admission::Reply(response.clone());
                }
            }
        }
        if self.phase == Phase::Unknown {
            return reject(ErrorCode::Unknown, "participant outcome is unresolved");
        }
        if let Some(last) = &self.last {
            if request.stream != last.stream {
                return reject(rejected, "stream does not own this lane");
            }
            if identity.same_attempt(last.identity) {
                if identity.sequence <= last.identity.sequence {
                    return reject(rejected, "stale or mismatched duplicate");
                }
                if self.phase == Phase::Aborted && request.action == Action::Abort {
                    return Admission::Reply(Response {
                        identity,
                        result: Ok(Value::Done),
                    });
                }
                if matches!(self.phase, Phase::Aborted | Phase::Committed) {
                    return reject(
                        if self.phase == Phase::Committed {
                            ErrorCode::Unknown
                        } else {
                            rejected
                        },
                        "attempt is terminal",
                    );
                }
                if matches!(request.action, Action::Install { .. }) && self.phase != Phase::Sealed {
                    return reject(ErrorCode::Unknown, "install requires validated participant");
                }
                if self.phase == Phase::Sealed
                    && !matches!(request.action, Action::Install { .. } | Action::Abort)
                {
                    return reject(rejected, "participant is sealed");
                }
                return Admission::Continue;
            }
            let newer_term = identity.term > last.identity.term;
            if !newer_term
                && (identity.incarnation != last.identity.incarnation
                    || identity.attempt <= last.identity.attempt
                    || self.active())
            {
                return reject(rejected, "prior attempt has not released this lane");
            }
        }
        if matches!(request.action, Action::Install { .. }) {
            return reject(ErrorCode::Unknown, "install has no retained participant");
        }
        Admission::Begin
    }

    /// Record only after physical effects and terminal cleanup. An INSTALL
    /// failure is never retryable, even if its underlying error was a conflict.
    pub fn record(&mut self, request: Request, mut response: Response) -> Response {
        debug_assert_eq!(request.identity, response.identity);
        self.phase = match &request.action {
            Action::Install { .. } => {
                if response.result.is_ok() {
                    Phase::Committed
                } else {
                    if let Err(error) = &mut response.result {
                        error.code = ErrorCode::Unknown;
                    }
                    Phase::Unknown
                }
            }
            Action::Abort => {
                if response.result.is_ok() {
                    Phase::Aborted
                } else {
                    Phase::Unknown
                }
            }
            _ if response.result.is_err() => {
                if matches!(&response.result, Err(e) if matches!(e.code, ErrorCode::Unknown | ErrorCode::CommittedFailure))
                {
                    Phase::Unknown
                } else {
                    Phase::Aborted
                }
            }
            Action::Validate => Phase::Sealed,
            _ => Phase::Active,
        };
        self.last = Some(request);
        self.response = Some(response.clone());
        response
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire::{Access, Identity, Stream, TableKey};
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
        let mut session = Session::default();
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
        let mut session = Session::default();
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
        let mut session = Session::default();
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
        let mut session = Session::default();
        record(&mut session, read(1));
        let mut fresh = request(1, Action::Abort);
        fresh.identity.incarnation = 8;
        fresh.identity.term = 1;
        assert!(matches!(session.admit(&fresh, 0), Admission::Begin));
        record(&mut session, fresh.clone());
        fresh.identity.sequence += 1;
        assert!(matches!(session.admit(&fresh, 1), Admission::Reply(r) if r.result.is_ok()));
        assert!(matches!(session.admit(&read(2), 1), Admission::Reply(r) if r.result.is_err()));
    }
}
