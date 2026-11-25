//! The same request lane runs over the native worker and the small test engine.
use crate::{
    admission::{Admission, Phase, Session},
    contracts::ParticipantEngine,
    wire::*,
};

pub struct Lane<E: ParticipantEngine> {
    session: Session,
    engine: E,
}
impl<E: ParticipantEngine> Lane<E> {
    pub fn new(engine: E) -> Self {
        Self {
            session: Session::default(),
            engine,
        }
    }
    pub fn claim(&mut self, nonce: u64, lease: u64) -> Result<u64> {
        self.session.claim(nonce, lease)
    }
    pub fn phase(&self) -> Phase {
        self.session.phase()
    }
    pub fn engine(&self) -> &E {
        &self.engine
    }
    pub fn process(&mut self, request: Request) -> Response {
        let admission = self.session.admit(&request, self.engine.minimum_term());
        if let Admission::Reply(response) = admission {
            return response;
        }
        let result = self
            .engine
            .execute(&request, matches!(admission, Admission::Begin));
        let response = Response {
            identity: request.identity,
            result,
        };
        let response = self.session.record(request, response);
        if mako_protocol_core::admission::release_protection(self.session.phase()) {
            self.engine.release_terminal();
        } else if self.session.phase() == Phase::Unknown {
            self.engine.retain_uncertain();
        }
        response
    }
}
