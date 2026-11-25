# Upper-layer contracts

The protocol is separate from the implementations of local transaction
storage, transport, replicated logging and clocks. Qualification evidence and
completion status are in [the plan](../plans/upper-layer-verification.md).

## What the upper layer proves

The distributed coordinator orders staging, timestamp selection, certification,
installation, logging and cleanup. It must certify both local and remote
participants before installation, use a decision greater than observed read
dependencies, and never turn an uncertain installed outcome into an abort that
may be retried. Participant admission binds stream leases, attempts, terms and
request sequences; exact terminal receipts may be replayed without repeating
effects. Unknown outcomes retain their migration admission protection.

The existing sharding/migration actors prove routing, metadata transitions,
drain requirements and transfer/publication against their storage and source
contracts. Production and the test driver call these same actors.

## Local transaction and ordered storage

`mako-protocol-core::coordinator::Effects` separates local lock/certify/install
operations from remote callbacks. Its stages record completed protocol facts.
Validation succeeds only while the local transaction's observations and locks
support the promised commit. Installation can fail after effects have begun;
neither an error nor a later cleanup proves rollback. A successful terminal
close releases the local transaction. Timestamp and status constants must agree
with the existing STO implementation.

Migration uses the existing `mako_sharding::transfer::RawStore` and
`storage::Source` contracts. Ordered scans return the exact next row, and EOF
certifies absence over the remaining range. Successful raw mutations change
only the requested cell; errors preserve the selected window. Admission and
completed drain provide exclusion for that window. A global database snapshot
or exclusion of unrelated ranges is not assumed.

A returned raw-store error must be definite and leave the selected window
unchanged. The native storage host maps ambiguous installation or cleanup to
`Mapped::FailStop` and aborts the process. Its callback contract has no unknown
outcome variant. The distributed participant interface does represent unknown
outcomes and retains the epoch guard in the live store's quarantine.

The Masstree/STO adapter supplies these facts. RocksDB is behind a persistence
adapter. Verifying the upper layer does not verify either storage implementation.
Component tests check their internal behavior; adapter integration tests check
the mappings at this boundary.

## Message delivery

The transport contract exchanges owned, bounded requests and replies with a
configured endpoint. Delivery may fail, duplicate, reorder or lose responses.
An endpoint response must come from that endpoint; transaction replies must
match the pending identity. The protocol checks identity and phase before
effects, and replays retained receipts only for the exact request.

A send or timeout error does not prove that a remote operation did not happen.
In particular, installation and gateway write uncertainty must remain unknown.
SRPC supplies real transport and cancellation. The deterministic driver supplies
a controllable queue and virtual deadlines. Safety does not assume eventual
delivery; any progress claim states the required availability separately.

## Raft and durability

The log/admission interface is independent of Raft. A permit must reject a
changed or unhealthy authority before new transaction effects. A successful
append meets the selected durability policy and preserves the chosen decision.
For Raft this requires current-term quorum admission and the committed log
record. A failure after local installation fences the attempt.

Receipt lookup and append must agree on exact request identity. A backend that
claims durable receipts must persist the receipt atomically with the associated
redo. The explicit volatile implementation makes no restart guarantee.
Replication failures may return unknown; they are never evidence of an abort.

Raft consensus and RocksDB recovery are component obligations, not algorithms
to simulate inside every upper-layer test. A small contract implementation may
choose success, definite refusal or uncertainty without implementing consensus.

## Time and limits

The timestamp allocator returns a positive, encodable value greater than the
requested lower bound, or fails with exhaustion. It never wraps. A request
deadline bounds a transport wait, not the time at which a remote effect must
stop. Test scenarios advance virtual time and choose delivery explicitly.

No new crash-recoverable distributed coordinator is introduced. Live migration
retains the current restriction against replication and local persistence.
Conditional durable reference models do not establish production support for
that combination.

## Evidence and trust

Trait contracts are assumptions about the named adapters. A passing proof is
conditional on them. The fast gate must verify actual executable decisions,
audit source/dependency correspondence, and reject semantic negative controls.
Its deterministic tests exercise the same decisions against small contract
implementations. Separate real-adapter and process tests check that production
supplies the required behavior. A text audit or a simulator pass alone is not a
whole-system proof.

| Contract or decision | Production implementation | Isolated evidence |
| --- | --- | --- |
| Coordinator ordering and outcome | `mako-protocol-core/src/coordinator.rs`, called by `sto-tpcc-ffi/src/protocol.rs` | Complete Verus proof; timestamp, certification, installation and logging fault cases |
| Local transaction effects | `CoordinatorEffects` and existing STO operations | Trusted trait contracts; separate 84-case application adapter suite |
| Participant lease, sequence and receipt | `mako-protocol-core/src/admission.rs`, `mako-protocol/src/admission.rs` | Complete core proof; duplicates, changed payload, retired leases and delayed abort scenarios |
| Physical participant and retained epoch | `mako-runtime/src/receiver.rs::NativeEngine` implements `ParticipantEngine`; generic `Lane` owns protocol state | In-memory lane tests; native unknown-install teardown regression |
| Delivery and leader discovery | `mako-runtime/src/rpc.rs::Network` implements `Transport` | Virtual deadlines, dropped requests/replies, stale reply rejection; separate SRPC suite |
| Remote sequencing and routing binding | `mako-protocol/src/remote.rs` and gateway SDK | Production code in isolated tests; native runtime tests retain real transport coverage |
| Log admission and durable receipts | `mako-runtime/src/replication.rs::{LocalLog,ReplicatedLog}` and `gateway.rs::GatewayLog` implement `CommitLog` | Failure injection at log acknowledgement; separate RocksDB/Raft restart and receipt tests |
| Migration storage and source | Existing `RawStore` / `Source`, implemented by native storage host | Complete production sharding/transfer proof; ordered in-memory cells with byte, absence and outside-range oracles |
| Clock reservation | `mako-clock`, called by `CoordinatorEffects` | Bounded timestamp contract, exhaustion scenario, and separate clock component tests |

`coordinate` has an initial collecting-state precondition. Its native wrapper
checks that state before calling it. The trait's ghost stages describe completed
effects; the native implementation is trusted to supply those facts. Admission
entry points are total. Payload validation, exact byte comparison, serialization
and trait-to-native mappings are ordinary Rust, covered by executable scenarios
and real-adapter regressions rather than claimed as whole-function proofs.

The dependency audit checks Cargo's selected graph before compilation and its
actual artifacts afterward. It permits pure `mako-engine-api` types and
`mako-clock`; it excludes transaction engines and physical backends. C and C++
compilers are disabled in that gate. Source audits reject proof escapes and
alternate core entry files and check the explicit production call mappings.
These audits support correspondence; they do not prove the trusted adapters.
