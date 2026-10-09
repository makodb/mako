//! Verified sequential auto-commit gateway. Engine atomicity and transport
//! completion remain explicit boundaries; UNKNOWN is not an aborted stutter.
use std::collections::{HashMap, hash_map::Entry};
use vstd::prelude::*;
use crate::bytes;

verus! {
pub const BEGIN: u32 = 20;
pub const COMMIT: u32 = 21;
pub const ROLLBACK: u32 = 22;
pub const PUT: u32 = 23;
pub const GET: u32 = 24;
pub const DELETE: u32 = 25;
pub const ROUTE: u32 = 27;
pub const INSERT: u32 = 28;
pub const UNKNOWN: u32 = 0;
pub const COMMITTED: u32 = 1;
pub const ABORTED: u32 = 2;
pub const REJECTED: u32 = 3;
pub const IN_FLIGHT: u32 = 4;

pub struct RequestView {
    pub kind: u32, pub client: u64, pub sequence: u64, pub session: u64,
    pub table: u64, pub epoch: u64, pub owner: u32, pub physical_table: u32,
    pub route_known: bool, pub fixed_coordinate: bool,
    pub coordinate: Seq<u8>, pub key: Seq<u8>, pub name: Seq<u8>, pub value: Seq<u8>,
}
#[derive(Debug, PartialEq, Eq)]
pub struct Request {
    pub kind: u32, pub client: u64, pub sequence: u64, pub session: u64,
    pub table: u64, pub epoch: u64, pub owner: u32, pub physical_table: u32,
    pub route_known: bool, pub fixed_coordinate: bool,
    pub coordinate: Vec<u8>, pub key: Vec<u8>, pub name: Vec<u8>, pub value: Vec<u8>,
}
impl View for Request {
    type V = RequestView;
    open spec fn view(&self) -> RequestView {
        RequestView { kind: self.kind, client: self.client, sequence: self.sequence,
            session: self.session, table: self.table, epoch: self.epoch,
            owner: self.owner, physical_table: self.physical_table,
            route_known: self.route_known, fixed_coordinate: self.fixed_coordinate,
            coordinate: self.coordinate@, key: self.key@, name: self.name@, value: self.value@ }
    }
}
impl Clone for Request {
    fn clone(&self) -> (out: Self)
        ensures out@ == self@,
    {
        Self { kind: self.kind, client: self.client, sequence: self.sequence,
            session: self.session, table: self.table, epoch: self.epoch,
            owner: self.owner, physical_table: self.physical_table,
            route_known: self.route_known, fixed_coordinate: self.fixed_coordinate,
            coordinate: bytes::copy_bytes(self.coordinate.as_slice()),
            key: bytes::copy_bytes(self.key.as_slice()),
            name: bytes::copy_bytes(self.name.as_slice()),
            value: bytes::copy_bytes(self.value.as_slice()) }
    }
}
pub open spec fn storage(kind: u32) -> bool {
    kind == PUT || kind == GET || kind == DELETE || kind == INSERT
}
pub open spec fn valid(r: RequestView) -> bool {
    r.client >= 0x8000_0000_0000_0000u64 && r.sequence > 0
        && (storage(r.kind) || r.kind == BEGIN || r.kind == COMMIT || r.kind == ROLLBACK)
        && (storage(r.kind) ==> r.route_known)
}
pub open spec fn same_intent(a: RequestView, b: RequestView) -> bool {
    a.kind == b.kind && a.session == b.session && a.name == b.name
        && a.key == b.key && a.value == b.value
        && (a.name.len() > 0 || a.physical_table == b.physical_table)
}
fn equal_bytes(a: &Vec<u8>, b: &Vec<u8>) -> (out: bool)
    ensures out == (a@ == b@),
{
    bytes::compare(a.as_slice(), b.as_slice()) == 0
}
impl Request {
    pub fn storage(&self) -> (out: bool)
        ensures out == storage(self.kind),
    { matches!(self.kind, PUT | GET | DELETE | INSERT) }
    fn same_intent(&self, other: &Self) -> (out: bool)
        ensures out == same_intent(self@, other@),
    {
        self.kind == other.kind && self.session == other.session
            && equal_bytes(&self.name, &other.name) && equal_bytes(&self.key, &other.key)
            && equal_bytes(&self.value, &other.value)
            && (!self.name.is_empty() || self.physical_table == other.physical_table)
    }
    fn same_request(&self, other: &Self) -> (out: bool)
        ensures out == (self@ == other@),
    {
        self.kind == other.kind && self.client == other.client && self.sequence == other.sequence
            && self.session == other.session && self.table == other.table && self.epoch == other.epoch
            && self.owner == other.owner && self.physical_table == other.physical_table
            && self.route_known == other.route_known && self.fixed_coordinate == other.fixed_coordinate
            && equal_bytes(&self.coordinate, &other.coordinate) && equal_bytes(&self.key, &other.key)
            && equal_bytes(&self.name, &other.name) && equal_bytes(&self.value, &other.value)
    }
}

pub struct ReplyView {
    pub status: i32, pub outcome: u32, pub op_result: bool, pub value: Seq<u8>,
}
#[derive(Debug, PartialEq, Eq)]
pub struct Reply {
    pub status: i32, pub outcome: u32, pub op_result: bool, pub value: Vec<u8>,
}
impl View for Reply {
    type V = ReplyView;
    open spec fn view(&self) -> ReplyView {
        ReplyView { status: self.status, outcome: self.outcome,
            op_result: self.op_result, value: self.value@ }
    }
}
pub open spec fn rejected() -> ReplyView {
    ReplyView { status: 2, outcome: REJECTED, op_result: false, value: Seq::empty() }
}
pub open spec fn success() -> ReplyView {
    ReplyView { status: 0, outcome: COMMITTED, op_result: false, value: Seq::empty() }
}
pub open spec fn in_flight() -> ReplyView {
    ReplyView { status: 4, outcome: IN_FLIGHT, op_result: false, value: Seq::empty() }
}
impl Reply {
    pub fn rejected() -> (out: Self) ensures out@ == rejected(),
    { Self { status: 2, outcome: REJECTED, op_result: false, value: Vec::new() } }
    fn success() -> (out: Self) ensures out@ == success(),
    { Self { status: 0, outcome: COMMITTED, op_result: false, value: Vec::new() } }
    fn in_flight() -> (out: Self) ensures out@ == in_flight(),
    { Self { status: 4, outcome: IN_FLIGHT, op_result: false, value: Vec::new() } }
    fn copy(&self) -> (out: Self) ensures out@ == self@,
    {
        Self { status: self.status, outcome: self.outcome, op_result: self.op_result,
            value: bytes::copy_bytes(self.value.as_slice()) }
    }
}
impl Clone for Reply {
    fn clone(&self) -> (out: Self)
        ensures out@ == self@,
    { self.copy() }
}
pub open spec fn reply_view(reply: Option<Reply>) -> Option<ReplyView> {
    match reply { Some(r) => Some(r@), None => None }
}
pub enum AdmissionView { Execute, Reply(ReplyView) }
pub enum Admission { Execute, Reply(Reply) }
impl View for Admission {
    type V = AdmissionView;
    open spec fn view(&self) -> AdmissionView {
        match self { Admission::Execute => AdmissionView::Execute,
            Admission::Reply(r) => AdmissionView::Reply(r@) }
    }
}
pub struct StreamView {
    pub request: RequestView, pub reply: Option<ReplyView>, pub session: Option<u64>,
    pub prior_terminal: Option<(RequestView, ReplyView)>, pub request_present: bool,
}
struct Stream {
    request: Option<Request>, reply: Option<Reply>, session: Option<u64>,
    prior_terminal: Option<(Request, Reply)>,
}
impl View for Stream {
    type V = StreamView;
    closed spec fn view(&self) -> StreamView {
        StreamView { request: self.request.unwrap()@, request_present: self.request.is_some(),
            reply: reply_view(self.reply),
            session: self.session, prior_terminal: match self.prior_terminal {
                Some((r, p)) => Some((r@, p@)), None => None } }
    }
}
pub open spec fn stream_wf(s: StreamView) -> bool {
    s.request_present && valid(s.request)
        && ((s.reply.is_none() || s.reply.unwrap().outcome == UNKNOWN) ==> storage(s.request.kind))
        && match s.prior_terminal {
        Some((r, p)) => valid(r) && r.client == s.request.client && r.sequence < s.request.sequence
            && p.outcome != UNKNOWN,
        None => true,
    }
}
pub open spec fn initial(r: RequestView, session: Option<u64>) -> StreamView {
    if r.kind == BEGIN && session.is_none() && r.session == r.sequence {
        StreamView { request: r, reply: Some(success()), session: Some(r.session), prior_terminal: None, request_present: true }
    } else if (r.kind == COMMIT || r.kind == ROLLBACK) && session == Some(r.session) {
        StreamView { request: r, reply: Some(success()), session: None, prior_terminal: None, request_present: true }
    } else if storage(r.kind) && ((r.session == 0 && session.is_none()) || session == Some(r.session)) {
        StreamView { request: r, reply: None, session, prior_terminal: None, request_present: true }
    } else {
        StreamView { request: r, reply: Some(rejected()), session, prior_terminal: None, request_present: true }
    }
}
pub open spec fn response(s: StreamView) -> AdmissionView {
    match s.reply { Some(r) => AdmissionView::Reply(r), None => AdmissionView::Execute }
}
pub open spec fn remote_resume(s: StreamView, r: RequestView, participant: u32) -> bool {
    r == s.request && r.owner != participant && match s.reply {
        Some(p) => p.outcome == UNKNOWN && p.status == 1, None => false }
}
pub open spec fn stream_admission(before: StreamView, after: StreamView, r: RequestView,
                                  participant: u32, out: AdmissionView) -> bool {
    if before.prior_terminal.is_some() && r.sequence == before.prior_terminal.unwrap().0.sequence {
        after == before && out == AdmissionView::Reply(
            if r == before.prior_terminal.unwrap().0 { before.prior_terminal.unwrap().1 } else { rejected() })
    } else if r.sequence == before.request.sequence {
        if remote_resume(before, r, participant) {
            after == StreamView { reply: None, ..before } && out is Execute
        } else {
            after == before && out == AdmissionView::Reply(if r != before.request { rejected() }
                else { match before.reply { Some(p) => p, None => in_flight() } })
        }
    } else if r.sequence <= before.request.sequence
        || before.reply.is_none() || before.reply.unwrap().outcome == UNKNOWN {
        after == before && out == AdmissionView::Reply(rejected())
    } else {
        let next = initial(r, before.session);
        after == StreamView { prior_terminal: if next.reply.is_none() {
            Some((before.request, before.reply.unwrap())) } else { None }, ..next }
            && out == response(next)
    }
}
impl Stream {
    fn new(request: Request, mut session: Option<u64>) -> (out: Self)
        requires valid(request@),
        ensures out@ == initial(request@, session), stream_wf(out@),
    {
        let reply;
        if request.kind == BEGIN && session.is_none() && request.session == request.sequence {
            session = Some(request.session); reply = Some(Reply::success());
        } else if (request.kind == COMMIT || request.kind == ROLLBACK) && session == Some(request.session) {
            session = None; reply = Some(Reply::success());
        } else if request.storage() && ((request.session == 0 && session.is_none())
            || session == Some(request.session)) {
            reply = None;
        } else { reply = Some(Reply::rejected()); }
        Self { request: Some(request), reply, session, prior_terminal: None }
    }
    fn response(&self) -> (out: Admission) ensures out@ == response(self@),
    {
        match &self.reply { Some(reply) => Admission::Reply(reply.copy()), None => Admission::Execute }
    }
    fn admit(&mut self, request: Request, participant: u32) -> (out: Admission)
        requires stream_wf(old(self)@), valid(request@), request.client == old(self)@.request.client,
        ensures stream_wf(final(self)@),
            stream_admission(old(self)@, final(self)@, request@, participant, out@),
    {
        if let Some((prior, reply)) = &self.prior_terminal {
            if request.sequence == prior.sequence {
                return Admission::Reply(if request.same_request(prior) { reply.copy() } else { Reply::rejected() });
            }
        }
        let current = self.request.as_ref().unwrap();
        if request.sequence == current.sequence {
            let exact = request.same_request(current);
            let resume = match &self.reply {
                Some(reply) => reply.outcome == UNKNOWN && reply.status == 1, None => false };
            if exact && request.owner != participant && resume {
                self.reply = None;
                return Admission::Execute;
            }
            if !exact { return Admission::Reply(Reply::rejected()); }
            return match &self.reply {
                Some(reply) => Admission::Reply(reply.copy()),
                None => Admission::Reply(Reply::in_flight()),
            };
        }
        let blocked = match &self.reply { Some(reply) => reply.outcome == UNKNOWN, None => true };
        if request.sequence <= current.sequence || blocked {
            return Admission::Reply(Reply::rejected());
        }
        let replacement = Self::new(request, self.session);
        let previous_request = self.request.take().unwrap();
        let previous_reply = self.reply.take();
        *self = replacement;
        if self.reply.is_none() {
            self.prior_terminal = match previous_reply {
                Some(reply) => Some((previous_request, reply)), None => None };
        }
        self.response()
    }
}

pub struct Gateway {
    streams: HashMap<u64, Stream>, participant: u32,
}
impl View for Gateway {
    type V = Map<u64, StreamView>;
    closed spec fn view(&self) -> Self::V {
        Map::new(self.streams@.dom(), |client: u64| self.streams@[client]@)
    }
}
pub open spec fn admission_effect(before: Map<u64, StreamView>, after: Map<u64, StreamView>,
                                  participant: u32, request: RequestView, out: AdmissionView) -> bool {
    if !valid(request) { after == before && out == AdmissionView::Reply(rejected()) }
    else {
        after.contains_key(request.client) && after == before.insert(request.client, after[request.client])
            && if before.contains_key(request.client) {
                stream_admission(before[request.client], after[request.client], request, participant, out)
            } else { after[request.client] == initial(request, None) && out == response(after[request.client]) }
    }
}
pub open spec fn finish_effect(before: Map<u64, StreamView>, after: Map<u64, StreamView>,
                               client: u64, sequence: u64, reply: ReplyView) -> bool {
    if before.contains_key(client) && before[client].request.sequence == sequence
        && before[client].reply.is_none() {
        after == before.insert(client, StreamView {
            reply: Some(reply),
            prior_terminal: if reply.outcome == UNKNOWN { before[client].prior_terminal } else { None },
            ..before[client] })
    } else { after == before }
}

/// These consequences are proved from the same transitions executed above,
/// not from an independent replay oracle.
pub proof fn admission_safety(before: Map<u64, StreamView>, after: Map<u64, StreamView>,
                               participant: u32, request: RequestView, out: AdmissionView)
    requires
        forall|client: u64| before.contains_key(client) ==> stream_wf(before[client]),
        admission_effect(before, after, participant, request, out),
    ensures
        out is Execute ==> valid(request) && storage(request.kind)
            && after.contains_key(request.client) && after[request.client].request == request
            && after[request.client].reply.is_none(),
        out is Execute && before.contains_key(request.client)
            && request.sequence <= before[request.client].request.sequence
            ==> remote_resume(before[request.client], request, participant),
        before.contains_key(request.client) && before[request.client].reply.is_some()
            && before[request.client].reply.unwrap().outcome == UNKNOWN
            && (request != before[request.client].request || request.owner == participant)
            ==> !(out is Execute) && after == before,
        before.contains_key(request.client) && request == before[request.client].request
            && before[request.client].reply.is_some()
            && before[request.client].reply.unwrap().outcome != UNKNOWN
            ==> out == AdmissionView::Reply(before[request.client].reply.unwrap()) && after == before,
        before.contains_key(request.client) && request.sequence < before[request.client].request.sequence
            ==> !(out is Execute) && after == before,
        before.contains_key(request.client) && before[request.client].reply.is_none()
            ==> !(out is Execute) && after == before,
        before.contains_key(request.client) && request.sequence == before[request.client].request.sequence
            && request != before[request.client].request
            ==> out == AdmissionView::Reply(rejected()) && after == before,
        before.contains_key(request.client) && before[request.client].prior_terminal.is_some()
            && request == before[request.client].prior_terminal.unwrap().0
            ==> out == AdmissionView::Reply(before[request.client].prior_terminal.unwrap().1)
                && after == before,
{
    if before.contains_key(request.client) {
        assert(stream_wf(before[request.client]));
        if valid(request) && after[request.client] == before[request.client] {
            assert(after =~= before);
        }
    }
}

/// The engine supplies this result; no storage mutation, rollback, or lease
/// refinement is assumed here. The source gateway preserves its exact binding
/// to the admitted identity, grant, operation, key/value and returned bytes.
pub proof fn retained_callback_result(before: Map<u64, StreamView>, after: Map<u64, StreamView>,
                                       admitted: RequestView, result: ReplyView)
    requires
        before.contains_key(admitted.client), before[admitted.client].request == admitted,
        before[admitted.client].reply.is_none(),
        finish_effect(before, after, admitted.client, admitted.sequence, result),
    ensures
        after.contains_key(admitted.client), after[admitted.client].request == admitted,
        after[admitted.client].reply == Some(result),
        after.dom() == before.dom(),
        forall|other: u64| before.contains_key(other) && other != admitted.client
            ==> after[other] == before[other],
{}
impl Gateway {
    pub open spec fn wf(&self) -> bool {
        forall|client: u64| self@.contains_key(client) ==>
            stream_wf(self@[client]) && self@[client].request.client == client
    }
    pub closed spec fn owner(&self) -> u32 { self.participant }
    pub fn new(participant: u32) -> (out: Self)
        ensures out.wf(), out@ == Map::<u64, StreamView>::empty(), out.owner() == participant,
    { Self { streams: HashMap::new(), participant } }
    pub fn admit(&mut self, request: Request) -> (out: Admission)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).owner() == old(self).owner(),
            admission_effect(old(self)@, final(self)@, old(self).owner(), request@, out@),
    {
        if request.client < 0x8000_0000_0000_0000u64 || request.sequence == 0
            || (!request.storage() && !matches!(request.kind, BEGIN | COMMIT | ROLLBACK))
            || (request.storage() && !request.route_known) {
            return Admission::Reply(Reply::rejected());
        }
        let ghost before = self@;
        let client = request.client;
        let out = match self.streams.entry(client) {
            Entry::Vacant(entry) => {
                let stream = Stream::new(request, None);
                let out = stream.response();
                entry.insert(stream);
                out
            }
            Entry::Occupied(entry) => {
                let stream = entry.into_mut();
                proof { assert(stream@ == before[client]); }
                stream.admit(request, self.participant)
            }
        };
        proof {
            assert(self@ =~= before.insert(client, self@[client]));
            assert forall|c: u64| self@.contains_key(c) implies
                stream_wf(self@[c]) && self@[c].request.client == c by {
                if c != client { assert(self@[c] == before[c]); }
            }
        }
        out
    }
    /// The callback's result is an engine boundary input, never inferred from
    /// transport timeout. Only the exact currently admitted operation can close.
    pub fn finish(&mut self, client: u64, sequence: u64, reply: Reply)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).owner() == old(self).owner(),
            finish_effect(old(self)@, final(self)@, client, sequence, reply@),
    {
        let ghost before = self@;
        match self.streams.entry(client) {
            Entry::Vacant(_) => {}
            Entry::Occupied(entry) => {
                let stream = entry.into_mut();
                proof { assert(stream@ == before[client]); }
                if stream.request.as_ref().unwrap().sequence == sequence && stream.reply.is_none() {
                    if reply.outcome != UNKNOWN { stream.prior_terminal = None; }
                    stream.reply = Some(reply);
                }
            }
        }
        proof {
            if before.contains_key(client) {
                assert(self@ =~= before.insert(client, self@[client]));
                if self@[client] == before[client] { assert(self@ =~= before); }
            } else { assert(self@ =~= before); }
            assert forall|c: u64| self@.contains_key(c) implies
                stream_wf(self@[c]) && self@[c].request.client == c by {
                if c != client { assert(self@[c] == before[c]); }
            }
        }
    }
}
impl Default for Gateway {
    fn default() -> (out: Self) ensures out.wf(), out@ == Map::<u64, StreamView>::empty(),
    { Self::new(0) }
}

pub struct ClientView {
    pub client: u64, pub next: Option<u64>, pub pending: Option<RequestView>,
}
pub struct Client { client: u64, next: Option<u64>, pending: Option<Request> }
pub open spec fn request_view(r: Option<Request>) -> Option<RequestView> {
    match r { Some(r) => Some(r@), None => None }
}
impl View for Client {
    type V = ClientView;
    closed spec fn view(&self) -> ClientView {
        ClientView { client: self.client, next: self.next, pending: request_view(self.pending) }
    }
}
pub open spec fn prepare_effect(before: ClientView, after: ClientView, intent: RequestView, ok: bool) -> bool {
    if before.pending.is_some() {
        let r = before.pending.unwrap();
        after == before && ok == same_intent(r, RequestView {
            session: if intent.kind == BEGIN { r.session } else { intent.session }, ..intent })
    } else if before.next.is_none() { !ok && after == before }
    else {
        ok && after == ClientView { pending: Some(RequestView {
            client: before.client, sequence: before.next.unwrap(),
            session: if intent.kind == BEGIN { before.next.unwrap() } else { intent.session },
            ..intent }), ..before }
    }
}
pub open spec fn terminal(outcome: u32) -> bool {
    outcome == COMMITTED || outcome == ABORTED || outcome == REJECTED
}
pub open spec fn completion_effect(before: ClientView, after: ClientView,
                                   client: u64, sequence: u64, outcome: u32, ok: bool) -> bool {
    ok == (before.pending.is_some() && before.pending.unwrap().client == client
        && before.pending.unwrap().sequence == sequence && terminal(outcome))
        && (!ok ==> after == before)
        && (ok ==> after == ClientView { client: before.client, pending: None,
            next: if sequence == u64::MAX { None } else { Some((sequence + 1) as u64) } })
}
pub proof fn pending_client_request_retained(before: ClientView, after: ClientView,
                                              intent: RequestView, ok: bool)
    requires before.pending.is_some(), prepare_effect(before, after, intent, ok),
    ensures after == before,
{}
pub proof fn unresolved_client_reply_retained(before: ClientView, after: ClientView,
                                              client: u64, sequence: u64, outcome: u32, ok: bool)
    requires completion_effect(before, after, client, sequence, outcome, ok), !terminal(outcome),
    ensures !ok, after == before,
{}
impl Client {
    pub open spec fn wf(&self) -> bool {
        self@.client >= 0x8000_0000_0000_0000u64
            && (self@.next.is_some() ==> self@.next.unwrap() > 0)
            && (self@.pending.is_some() ==> self@.next == Some(self@.pending.unwrap().sequence)
                && self@.pending.unwrap().client == self@.client)
    }
    pub fn new(client: u64, first: u64) -> (out: Option<Self>)
        ensures
            out.is_some() == (client >= 0x8000_0000_0000_0000u64 && first > 0),
            out.is_some() ==> out.unwrap().wf() && out.unwrap()@ == (ClientView {
                client, next: Some(first), pending: None }),
    {
        if client < 0x8000_0000_0000_0000u64 || first == 0 { return None; }
        Some(Self { client, next: Some(first), pending: None })
    }
    pub fn prepare(&mut self, mut intent: Request) -> (out: Result<&Request, ()>)
        requires old(self).wf(),
        ensures final(self).wf(), prepare_effect(old(self)@, final(self)@, intent@, out.is_ok()),
            out.is_ok() ==> final(self)@.pending == Some(out.unwrap()@),
    {
        if let Some(pending) = &self.pending {
            if intent.kind == BEGIN { intent.session = pending.session; }
            if !pending.same_intent(&intent) { return Err(()); }
        } else {
            let sequence = match self.next { Some(sequence) => sequence, None => return Err(()) };
            intent.client = self.client; intent.sequence = sequence;
            if intent.kind == BEGIN { intent.session = sequence; }
            self.pending = Some(intent);
        }
        match &self.pending { Some(request) => Ok(request), None => Err(()) }
    }
    pub fn set_route(&mut self, client: u64, sequence: u64, table: u64, epoch: u64,
                     owner: u32, physical_table: u32, fixed: bool,
                     coordinate: Vec<u8>) -> (out: Result<&Request, ()>)
        requires old(self).wf(),
        ensures final(self).wf(),
            out.is_ok() == (old(self)@.pending.is_some()
                && old(self)@.pending.unwrap().client == client
                && old(self)@.pending.unwrap().sequence == sequence
                && storage(old(self)@.pending.unwrap().kind) && !old(self)@.pending.unwrap().route_known),
            !out.is_ok() ==> final(self)@ == old(self)@,
            out.is_ok() ==> final(self)@ == (ClientView { pending: Some(RequestView {
                table, epoch, owner, physical_table, fixed_coordinate: fixed,
                coordinate: coordinate@, route_known: true, ..old(self)@.pending.unwrap() }), ..old(self)@ }),
            out.is_ok() ==> final(self)@.pending == Some(out.unwrap()@),
    {
        match &mut self.pending {
            None => Err(()),
            Some(request) => {
                if request.client != client || request.sequence != sequence || !request.storage()
                    || request.route_known { return Err(()); }
                request.table = table; request.epoch = epoch; request.owner = owner;
                request.physical_table = physical_table; request.fixed_coordinate = fixed;
                request.coordinate = coordinate; request.route_known = true;
                Ok(request)
            }
        }
    }
    pub fn complete(&mut self, client: u64, sequence: u64, outcome: u32) -> (out: bool)
        requires old(self).wf(),
        ensures final(self).wf(),
            completion_effect(old(self)@, final(self)@, client, sequence, outcome, out),
    {
        let pending = match &self.pending { Some(pending) => pending, None => return false };
        if pending.client != client || pending.sequence != sequence { return false; }
        if matches!(outcome, COMMITTED | ABORTED | REJECTED) {
            self.next = if sequence == u64::MAX { None } else { Some(sequence + 1) };
            self.pending = None;
            true
        } else { false }
    }
}
} // verus!

#[cfg(test)]
mod tests {
    use super::*;
    fn request(sequence: u64, kind: u32) -> Request {
        Request { kind, client: 1u64 << 63, sequence, session: 0, table: 1, epoch: 9,
            owner: 1, physical_table: 7, route_known: true, fixed_coordinate: false,
            coordinate: b"k".to_vec(), key: b"k".to_vec(), name: b"t".to_vec(),
            value: b"v".to_vec() }
    }
    #[test]
    fn lost_success_is_replayed_and_old_sequence_is_fenced() {
        let mut gateway = Gateway::default();
        let put = request(1, PUT);
        assert!(matches!(gateway.admit(put.clone()), Admission::Execute));
        let result = Reply { status: 0, outcome: COMMITTED, op_result: true, value: vec![] };
        gateway.finish(put.client, 1, result.clone());
        assert!(matches!(gateway.admit(put.clone()), Admission::Reply(r) if r == result));
        assert!(matches!(gateway.admit(request(2, GET)), Admission::Execute));
        assert!(matches!(gateway.admit(put.clone()), Admission::Reply(r) if r == result));
        gateway.finish(put.client, 2, Reply::success());
        assert!(matches!(gateway.admit(put), Admission::Reply(r) if r.outcome == REJECTED));
    }
    #[test]
    fn changed_grant_and_inflight_cannot_execute() {
        let mut gateway = Gateway::default();
        let put = request(1, PUT);
        assert!(matches!(gateway.admit(put.clone()), Admission::Execute));
        assert!(matches!(gateway.admit(put.clone()), Admission::Reply(r) if r.outcome == IN_FLIGHT));
        let mut changed = put;
        changed.epoch += 1;
        assert!(matches!(gateway.admit(changed), Admission::Reply(r) if r.outcome == REJECTED));
        assert!(matches!(gateway.admit(request(2, PUT)), Admission::Reply(r) if r.outcome == REJECTED));
    }
    #[test]
    fn sdk_timeout_keeps_identity_and_exhaustion_never_wraps() {
        let mut client = Client::new(1u64 << 63, u64::MAX).unwrap();
        let intent = request(0, PUT);
        let first = client.prepare(intent.clone()).unwrap().clone();
        assert_eq!(client.prepare(intent).unwrap(), &first);
        assert!(client.prepare(request(0, DELETE)).is_err());
        assert!(!client.complete(first.client, first.sequence, UNKNOWN));
        assert!(client.complete(first.client, first.sequence, COMMITTED));
        assert!(client.prepare(request(0, PUT)).is_err());
    }
    #[test]
    fn session_control_replays_and_never_undoes_operations() {
        let mut gateway = Gateway::default();
        let mut begin = request(1, BEGIN); begin.session = 1;
        assert!(matches!(gateway.admit(begin.clone()), Admission::Reply(r) if r.status == 0));
        assert!(matches!(gateway.admit(begin), Admission::Reply(r) if r.status == 0));
        let mut put = request(2, PUT); put.session = 1;
        assert!(matches!(gateway.admit(put.clone()), Admission::Execute));
        gateway.finish(put.client, 2, Reply::success());
        let mut rollback = request(3, ROLLBACK); rollback.session = 1;
        assert!(matches!(gateway.admit(rollback.clone()), Admission::Reply(r) if r.status == 0));
        assert!(matches!(gateway.admit(rollback), Admission::Reply(r) if r.status == 0));
        put.sequence = 4;
        assert!(matches!(gateway.admit(put), Admission::Reply(r) if r.outcome == REJECTED));
    }
    #[test]
    fn only_forwarded_transport_ambiguity_can_resume_identical_request() {
        let put = request(1, PUT); // captured owner 1
        let timeout = Reply { status: 1, outcome: UNKNOWN, op_result: false, value: vec![] };
        let mut ingress = Gateway::new(0);
        assert!(matches!(ingress.admit(put.clone()), Admission::Execute));
        ingress.finish(put.client, 1, timeout.clone());
        assert!(matches!(ingress.admit(request(2, PUT)), Admission::Reply(r) if r.outcome == REJECTED));
        assert!(matches!(ingress.admit(put.clone()), Admission::Execute));
        ingress.finish(put.client, 1, Reply::success());
        assert!(matches!(ingress.admit(put.clone()), Admission::Reply(r) if r.outcome == COMMITTED));
        let mut local = Gateway::new(1);
        assert!(matches!(local.admit(put.clone()), Admission::Execute));
        local.finish(put.client, 1, timeout);
        assert!(matches!(local.admit(put), Admission::Reply(r) if r.outcome == UNKNOWN));
    }
}
