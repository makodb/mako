#include <gtest/gtest.h>
#include <type_traits>
#include "../rrr.hpp"
#include "../misc/marshal_serializable_bridge.hpp"  // wrap_serializable, serializable_cast
#include "deptran/classic/tpc_command.h"
#include "deptran/procedure.h"
#include "deptran/raft/replicated_db.h"
#include "deptran/rcc/dep_graph.h"
#include "deptran/paxos_worker.h"

using namespace rrr;

// Workstream N Phase 5b-5: removed the `TypedOnlyPayload` test
// fixture (struct + namespace + adapter typedef +
// `TypedMarshallableAdapterTraits` specialization +
// `EnsureTypedOnlyPayloadInitializer` helper +
// `TypedPayloadRoundTripsViaDeputyAdapter` /
// `TypedPayloadInitializerStateContainsAdapter` tests). Its sole
// purpose was to validate the `TypedMarshallableAdapter` trait
// path; that path went away with Phase 4 (every production type
// migrated to Serializable) and is now deleted.

namespace {

constexpr int32_t kTestMarshallableKind = 420042;

static_assert(!std::is_base_of_v<Marshallable, janus::VecPieceData>);
static_assert(!std::is_base_of_v<Marshallable, janus::VecRecData>);
static_assert(!std::is_base_of_v<Marshallable, janus::ViewData>);
static_assert(!std::is_base_of_v<Marshallable, janus::KeyCmdBatchData>);
static_assert(!std::is_base_of_v<Marshallable, janus::TpcPrepareCommand>);
static_assert(!std::is_base_of_v<Marshallable, janus::TpcCommitCommand>);
static_assert(!std::is_base_of_v<Marshallable, janus::TpcEmptyCommand>);
static_assert(!std::is_base_of_v<Marshallable, janus::TpcNoopCommand>);
static_assert(!std::is_base_of_v<Marshallable, janus::TpcBatchCommand>);
static_assert(!std::is_base_of_v<Marshallable, janus::ReplicatedDBCommand>);
static_assert(!std::is_base_of_v<Marshallable, janus::EmptyGraph>);
static_assert(!std::is_base_of_v<Marshallable, janus::RccGraph>);
static_assert(!std::is_base_of_v<Marshallable, janus::BulkPrepareLog>);
static_assert(!std::is_base_of_v<Marshallable, janus::PaxosPrepCmd>);
static_assert(!std::is_base_of_v<Marshallable, janus::HeartBeatLog>);
static_assert(!std::is_base_of_v<Marshallable, janus::SyncLogRequest>);
static_assert(!std::is_base_of_v<Marshallable, janus::SyncLogResponse>);
static_assert(!std::is_base_of_v<Marshallable, janus::SyncNoOpRequest>);
static_assert(!std::is_base_of_v<Marshallable, janus::LogEntry>);
static_assert(!std::is_base_of_v<Marshallable, janus::BulkPaxosCmd>);

class TestMarshallable : public Marshallable {
 public:
  int32_t value = 0;

  explicit TestMarshallable(int32_t v = 0)
      : Marshallable(kTestMarshallableKind), value(v) {}

  Marshal& to_marshal(Marshal& m) const override {
    m << kind_ << value;
    return m;
  }

  Marshal& from_marshal(Marshal& m) override {
    m >> kind_ >> value;
    return m;
  }
  // Workstream N Phase 5b-6: removed unused `entity_size` /
  // `write_to_fd` overrides. The virtual base methods on
  // Marshallable went away in the same commit.
};

void EnsureTestMarshallableInitializer() {
  static bool initialized = []() {
    MarshallDeputy::reg_initializer<TestMarshallable>(kTestMarshallableKind);
    return true;
  }();
  (void)initialized;
}

template <typename T>
std::shared_ptr<T> RoundTripTypedDeputyPayload(const std::shared_ptr<T>& src) {
  // Phase 4d-2: types migrated to Serializable lose their
  // TypedMarshallableAdapter trait, so the
  // `MarshallDeputy(shared_ptr<T>)` ctor's requires clause no longer
  // matches. Route through `wrap_typed_marshallable` — its bridge
  // overload (Phase 4a-prep) handles both legacy and Serializable T.
  MarshallDeputy outgoing(wrap_typed_marshallable(src));
  Marshal m;
  m << outgoing;

  MarshallDeputy incoming;
  m >> incoming;

  return marshallable_cast<T>(incoming);
}

std::shared_ptr<janus::TpcCommitCommand> MakeTypedTpcCommitPayload(
    txnid_t tx_id,
    int ret,
    ballot_t term,
    bool_t recovery) {
  auto commit = std::make_shared<janus::TpcCommitCommand>();
  commit->tx_id_ = tx_id;
  commit->ret_ = ret;
  commit->term = term;

  auto vec_piece = std::make_shared<janus::VecPieceData>();
  vec_piece->sp_vec_piece_data_ =
      std::make_shared<std::vector<std::shared_ptr<janus::SimpleCommand>>>();
  vec_piece->is_recovery_command_ = recovery;
  commit->cmd_ = wrap_typed_marshallable(vec_piece);

  auto view_data = std::make_shared<janus::ViewData>();
  view_data->view_.n_ = 3;
  view_data->view_.view_id_ = 19;
  view_data->view_.timestamp_ = 777;
  view_data->view_.leaders_ = {0, 1, 2};
  view_data->partition_id_ = 11;
  commit->sp_view_data_ = view_data;

  return commit;
}

}  // namespace

// Workstream N Phase 5b-4: removed six tests
// (`AdapterForwardsToMarshal`, `AdapterForwardsFromMarshal`,
// `AdapterForwardsKind`, `AdapterForwardsEntitySize`,
// `ProxyIsMoveOnly`, `RoundTripThroughProxy`) that exercised the
// `MarshallableProxy` / `MarshallableSharedPtrAdapter` /
// `make_marshallable_proxy` infrastructure. That infrastructure was
// the last remaining user-visible surface of the proxy facade after
// Phase 5b-3 removed `MarshallDeputy::data_proxy()` and the
// bypass-to-socket fast path; the proxy + adapter + helper went
// away in the same Phase 5b-4 commit.

TEST(MarshallableProxyFacadeTest, DeputyDefaultsToNoMarshallable) {
  MarshallDeputy deputy;
  EXPECT_FALSE(deputy.has_marshallable());
  EXPECT_EQ(deputy.inner(), nullptr);
}

TEST(MarshallableProxyFacadeTest, DeputyStoresProxyAndPreservesInnerSharedPtr) {
  auto sp = std::make_shared<TestMarshallable>(88);
  MarshallDeputy deputy(sp);

  EXPECT_TRUE(deputy.has_marshallable());
  EXPECT_EQ(deputy.kind_, kTestMarshallableKind);
  EXPECT_EQ(marshallable_cast<TestMarshallable>(deputy), sp);
}

TEST(MarshallableProxyFacadeTest, DeputyRoundTripPreservesDerivedMarshallable) {
  EnsureTestMarshallableInitializer();

  auto src = std::make_shared<TestMarshallable>(321);
  MarshallDeputy outgoing(src);

  Marshal m;
  m << outgoing;

  MarshallDeputy incoming;
  m >> incoming;

  EXPECT_TRUE(incoming.has_marshallable());
  auto decoded = marshallable_cast<TestMarshallable>(incoming);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->kind_, kTestMarshallableKind);
  EXPECT_EQ(decoded->value, 321);
}

// Phase 5b-9: simplified — the factory now returns
// `shared_ptr<Marshallable>` directly (no MarInitializerState wrapper).
// Kind is verified via `m->kind()`.
TEST(MarshallableProxyFacadeTest, InitializerReturnsProxyBackedMetadata) {
  EnsureTestMarshallableInitializer();

  auto m = MarshallDeputy::create_initializer(kTestMarshallableKind);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->kind(), kTestMarshallableKind);
}

TEST(MarshallableProxyFacadeTest, MarshallableCastFromSharedPtrKeepsType) {
  std::shared_ptr<Marshallable> base = std::make_shared<TestMarshallable>(17);
  auto typed = marshallable_cast<TestMarshallable>(base);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->value, 17);
}

TEST(MarshallableProxyFacadeTest, MarshallableCastFromMarshallableRefKeepsType) {
  TestMarshallable payload(23);
  Marshallable& base = payload;
  auto typed = marshallable_cast<TestMarshallable>(base);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed.get(), &payload);
  EXPECT_EQ(typed->value, 23);
}

TEST(MarshallableProxyFacadeTest,
     MarshallableCastFromMarshallablePointerHandlesNullAndValue) {
  Marshallable* null_base = nullptr;
  EXPECT_EQ(marshallable_cast<TestMarshallable>(null_base), nullptr);

  TestMarshallable payload(29);
  Marshallable* base = &payload;
  auto typed = marshallable_cast<TestMarshallable>(base);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed.get(), &payload);
  EXPECT_EQ(typed->value, 29);
}

TEST(MarshallableProxyFacadeTest, MarshallableCastFromNullDeputyPointerIsNull) {
  MarshallDeputy* deputy = nullptr;
  EXPECT_EQ(marshallable_cast<TestMarshallable>(deputy), nullptr);
}

// Workstream N Phase 5b-5: removed
// `TypedPayloadRoundTripsViaDeputyAdapter` and
// `TypedPayloadInitializerStateContainsAdapter` tests — they
// exercised the `TypedMarshallableAdapter` trait path, which is
// gone now (every production payload uses Serializable).

TEST(MarshallableProxyFacadeTest, DeptranVecPieceDataUsesTypedAdapterPath) {
  auto payload = std::make_shared<janus::VecPieceData>();
  payload->sp_vec_piece_data_ =
      std::make_shared<std::vector<std::shared_ptr<janus::SimpleCommand>>>();
  payload->time_sent_from_client_ = 42.5;
  payload->is_recovery_command_ = 1;

  auto wrapped = wrap_typed_marshallable(payload);
  ASSERT_NE(wrapped, nullptr);
  EXPECT_EQ(wrapped->kind(), janus::VecPieceData::static_kind());

  // Phase 4d-6: VecPieceData migrated to Serializable via the
  // value-semantic `wrap_serializable` path (no aliased adapter — the
  // production sites in fpga_raft/server.cc, raft/server.h,
  // mongodb_connection_thread_pool.h, etc. recover decoded state from
  // round-tripped wire bytes, not in-memory shared_ptr identity).
  // Use `wrap_serializable_aliased` here to keep the in-memory
  // identity check that this regression test guards.
  MarshallDeputy deputy(wrap_serializable_aliased(payload));
  EXPECT_EQ(deputy.kind_, janus::VecPieceData::static_kind());
  auto decoded = marshallable_cast<janus::VecPieceData>(deputy);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded, payload);
  EXPECT_DOUBLE_EQ(decoded->time_sent_from_client_, 42.5);
  EXPECT_EQ(decoded->is_recovery_command_, 1);
}

// Phase 4d-6: round-trip a populated VecPieceData (with a real
// SimpleCommand carrying TxWorkspace input + map<int32_t,Value> output
// fields) through Marshal serialization to exercise the new archive
// operators end-to-end. This stresses the SimpleCommand,
// TxWorkspace, and mdb::Value archive operators that were added
// alongside the VecPieceData migration.
TEST(MarshallableProxyFacadeTest, DeptranVecPieceDataNonEmptyRoundTrip) {
  auto payload = std::make_shared<janus::VecPieceData>();
  payload->sp_vec_piece_data_ =
      std::make_shared<std::vector<std::shared_ptr<janus::SimpleCommand>>>();
  payload->time_sent_from_client_ = 17.5;
  payload->is_recovery_command_ = 1;

  // SimpleCommand 1: input has 2 keys mapped to Value, output is empty.
  auto cmd1 = std::make_shared<janus::SimpleCommand>();
  cmd1->id_ = 1001;
  cmd1->type_ = 7;
  cmd1->inn_id_ = 5;
  cmd1->root_id_ = 999;
  cmd1->root_type_ = 3;
  cmd1->client_id_ = 42;
  cmd1->cmd_id_in_client_ = 99;
  cmd1->rule_mode_on_and_is_original_path_only_command_ = 1;
  cmd1->input.keys_.insert(10);
  cmd1->input.keys_.insert(20);
  (*cmd1->input.values_)[10] = mdb::Value(static_cast<i32>(123));
  (*cmd1->input.values_)[20] = mdb::Value(std::string("hello"));
  cmd1->output[3] = mdb::Value(static_cast<i64>(456));
  cmd1->output_size = 1;
  cmd1->partition_id_ = 11;
  cmd1->timestamp_ = 7777;
  cmd1->rank_ = 2;
  payload->sp_vec_piece_data_->push_back(cmd1);

  // SimpleCommand 2: input/output both empty (edge case).
  auto cmd2 = std::make_shared<janus::SimpleCommand>();
  cmd2->id_ = 2002;
  cmd2->type_ = 8;
  cmd2->partition_id_ = 22;
  payload->sp_vec_piece_data_->push_back(cmd2);

  // Serialize via the new Serializable path (SerializableMarshallableAdapter::to_marshal
  // delegates to VecPieceData::save through a MarshalSink + BinaryWriteArchive).
  MarshallDeputy outgoing(wrap_typed_marshallable(payload));
  Marshal m;
  m << outgoing;

  // Deserialize via the new path (SerializableMarshallableAdapter::from_marshal
  // delegates to VecPieceData::load through a MarshalSource + BinaryReadArchive).
  MarshallDeputy incoming;
  m >> incoming;
  auto decoded = marshallable_cast<janus::VecPieceData>(incoming);
  ASSERT_NE(decoded, nullptr);

  EXPECT_DOUBLE_EQ(decoded->time_sent_from_client_, 17.5);
  EXPECT_EQ(decoded->is_recovery_command_, 1);
  ASSERT_EQ(decoded->sp_vec_piece_data_->size(), 2u);

  auto& d1 = *(*decoded->sp_vec_piece_data_)[0];
  EXPECT_EQ(d1.id_, 1001);
  EXPECT_EQ(d1.type_, 7);
  EXPECT_EQ(d1.inn_id_, 5);
  EXPECT_EQ(d1.root_id_, 999);
  EXPECT_EQ(d1.root_type_, 3);
  EXPECT_EQ(d1.client_id_, 42);
  EXPECT_EQ(d1.cmd_id_in_client_, 99);
  EXPECT_EQ(d1.rule_mode_on_and_is_original_path_only_command_, 1);
  EXPECT_EQ(d1.partition_id_, 11u);
  EXPECT_EQ(d1.timestamp_, 7777u);
  EXPECT_EQ(d1.rank_, 2);
  ASSERT_EQ(d1.input.keys_.count(10), 1u);
  ASSERT_EQ(d1.input.keys_.count(20), 1u);
  EXPECT_EQ(d1.input.at(10).get_i32(), 123);
  EXPECT_EQ(d1.input.at(20).get_str(), "hello");
  ASSERT_EQ(d1.output.count(3), 1u);
  EXPECT_EQ(d1.output.at(3).get_i64(), 456);
  EXPECT_EQ(d1.output_size, 1);

  auto& d2 = *(*decoded->sp_vec_piece_data_)[1];
  EXPECT_EQ(d2.id_, 2002);
  EXPECT_EQ(d2.type_, 8);
  EXPECT_EQ(d2.partition_id_, 22u);
  EXPECT_TRUE(d2.input.keys_.empty());
  EXPECT_TRUE(d2.output.empty());
}

TEST(MarshallableProxyFacadeTest, DeptranViewDataMarshalRoundTrip) {
  janus::ViewData src;
  src.view_.n_ = 3;
  src.view_.view_id_ = 9;
  src.view_.timestamp_ = 12345;
  src.view_.leaders_ = {1, 2, 1};
  src.partition_id_ = 7;

  // Phase 4d-3: ViewData migrated to Serializable. Use Archive
  // round-trip — the Marshal-based to_marshal/from_marshal methods
  // are gone.
  Marshal m;
  MarshalSink sink(&m);
  BinaryWriteArchive writer(&sink);
  src.save(writer);

  janus::ViewData dst;
  MarshalSource source(&m);
  BinaryReadArchive reader(&source);
  dst.load(reader);

  EXPECT_EQ(dst.view_.n_, 3);
  EXPECT_EQ(dst.view_.view_id_, 9);
  EXPECT_EQ(dst.view_.timestamp_, 12345);
  ASSERT_EQ(dst.view_.leaders_.size(), 3u);
  EXPECT_EQ(dst.view_.leaders_[0], 1);
  EXPECT_EQ(dst.view_.leaders_[1], 2);
  EXPECT_EQ(dst.view_.leaders_[2], 1);
  EXPECT_EQ(dst.partition_id_, 7);
}

TEST(MarshallableProxyFacadeTest, DeptranVecRecAndBatchUseTypedAdapterPath) {
  auto vec_rec = std::make_shared<janus::VecRecData>();
  vec_rec->key_data_ = std::make_shared<std::vector<key_t>>();
  vec_rec->key_data_->push_back(11);
  vec_rec->key_data_->push_back(12);

  MarshallDeputy vec_rec_deputy(vec_rec);
  EXPECT_EQ(vec_rec_deputy.kind_, janus::VecRecData::static_kind());
  auto decoded_vec_rec = marshallable_cast<janus::VecRecData>(vec_rec_deputy);
  ASSERT_NE(decoded_vec_rec, nullptr);
  ASSERT_NE(decoded_vec_rec->key_data_, nullptr);
  ASSERT_EQ(decoded_vec_rec->key_data_->size(), 2u);
  EXPECT_EQ((*decoded_vec_rec->key_data_)[0], 11);
  EXPECT_EQ((*decoded_vec_rec->key_data_)[1], 12);

  auto batch = std::make_shared<janus::KeyCmdBatchData>();
  auto nested = std::make_shared<TestMarshallable>(77);
  batch->AddEntry(1001, nested);

  MarshallDeputy batch_deputy(batch);
  EXPECT_EQ(batch_deputy.kind_, janus::KeyCmdBatchData::static_kind());
  auto decoded_batch = marshallable_cast<janus::KeyCmdBatchData>(batch_deputy);
  ASSERT_NE(decoded_batch, nullptr);
  ASSERT_EQ(decoded_batch->Size(), 1u);
  EXPECT_EQ(decoded_batch->GetKey(0), 1001);
  auto nested_decoded =
      marshallable_cast<TestMarshallable>(decoded_batch->GetCommand(0));
  ASSERT_NE(nested_decoded, nullptr);
  EXPECT_EQ(nested_decoded->value, 77);
}

TEST(MarshallableProxyFacadeTest, DeptranTpcCommitRoundTripUsesTypedAdapter) {
  auto src = MakeTypedTpcCommitPayload(/*tx_id=*/321, /*ret=*/9, /*term=*/17,
                                       /*recovery=*/1);

  // Phase 4a-3b: TpcCommitCommand is now Serializable. The
  // `MarshallDeputy(shared_ptr<T>)` ctor's requires clause checks
  // `kHasTypedMarshallableAdapter<T>` (no longer satisfied for
  // migrated types). Explicitly route through wrap_typed_marshallable
  // — its bridge overload (Phase 4a-prep) handles Serializable T's.
  MarshallDeputy outgoing(wrap_typed_marshallable(src));
  EXPECT_EQ(outgoing.kind_, janus::TpcCommitCommand::static_kind());

  Marshal m;
  m << outgoing;

  MarshallDeputy incoming;
  m >> incoming;
  EXPECT_EQ(incoming.kind_, janus::TpcCommitCommand::static_kind());

  auto decoded = marshallable_cast<janus::TpcCommitCommand>(incoming);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->tx_id_, 321);
  EXPECT_EQ(decoded->ret_, 9);
  EXPECT_EQ(decoded->term, 17);

  auto decoded_vec_piece = marshallable_cast<janus::VecPieceData>(decoded->cmd_);
  ASSERT_NE(decoded_vec_piece, nullptr);
  EXPECT_EQ(decoded_vec_piece->is_recovery_command_, 1);

  ASSERT_NE(decoded->sp_view_data_, nullptr);
  EXPECT_EQ(decoded->sp_view_data_->partition_id_, 11);
  EXPECT_EQ(decoded->sp_view_data_->view_.view_id_, 19);
}

TEST(MarshallableProxyFacadeTest, DeptranTpcBatchAndNoopEmptyUseTypedAdapter) {
  auto batch = std::make_shared<janus::TpcBatchCommand>();
  std::vector<std::shared_ptr<janus::TpcCommitCommand>> commits{
      MakeTypedTpcCommitPayload(/*tx_id=*/101, /*ret=*/1, /*term=*/3,
                                /*recovery=*/0),
      MakeTypedTpcCommitPayload(/*tx_id=*/202, /*ret=*/2, /*term=*/4,
                                /*recovery=*/1)};
  batch->AddCmds(commits);

  // Phase 4a-3c: TpcBatchCommand migrated to Serializable; route
  // through wrap_typed_marshallable as in the Commit case above.
  MarshallDeputy batch_outgoing(wrap_typed_marshallable(batch));
  EXPECT_EQ(batch_outgoing.kind_, janus::TpcBatchCommand::static_kind());
  Marshal batch_marshaled;
  batch_marshaled << batch_outgoing;

  MarshallDeputy batch_incoming;
  batch_marshaled >> batch_incoming;
  auto decoded_batch = marshallable_cast<janus::TpcBatchCommand>(batch_incoming);
  ASSERT_NE(decoded_batch, nullptr);
  ASSERT_EQ(decoded_batch->Size(), 2u);
  EXPECT_EQ(decoded_batch->cmds_.at(0)->tx_id_, 101);
  EXPECT_EQ(decoded_batch->cmds_.at(1)->tx_id_, 202);

  // Workstream N Phase 4a-2: TpcEmptyCommand migrated to Serializable.
  // Construction goes through `wrap_serializable_aliased` (preserves
  // event-member aliasing); recovery uses `serializable_cast<T>`.
  auto empty_cmd = std::make_shared<janus::TpcEmptyCommand>();
  MarshallDeputy empty_deputy(wrap_serializable_aliased(empty_cmd));
  EXPECT_EQ(empty_deputy.kind_, janus::TpcEmptyCommand::static_kind());
  ASSERT_NE(serializable_cast<janus::TpcEmptyCommand>(empty_deputy),
            nullptr);
  EXPECT_EQ(serializable_cast<janus::TpcEmptyCommand>(empty_deputy),
            empty_cmd.get())
      << "aliased wrap: serializable_cast should return the same "
         "instance as the caller's shared_ptr";

  // Workstream N Phase 4a-1: TpcNoopCommand was migrated from
  // Marshallable to Serializable. Construction goes through
  // `wrap_serializable` (returns shared_ptr<Marshallable>); recovery
  // uses `serializable_cast<T>` instead of `marshallable_cast<T>`.
  auto noop_cmd = std::make_shared<janus::TpcNoopCommand>();
  MarshallDeputy noop_deputy(wrap_serializable(noop_cmd));
  EXPECT_EQ(noop_deputy.kind_, janus::TpcNoopCommand::static_kind());
  ASSERT_NE(serializable_cast<janus::TpcNoopCommand>(noop_deputy), nullptr);
}

TEST(MarshallableProxyFacadeTest,
     ReplicatedDbCommandRoundTripUsesTypedAdapter) {
  auto put_cmd = janus::ReplicatedDBCommand::CreatePut("k1", "v1");
  ASSERT_NE(put_cmd, nullptr);

  MarshallDeputy outgoing(put_cmd);
  EXPECT_EQ(outgoing.kind_, janus::ReplicatedDBCommand::static_kind());

  Marshal m;
  m << outgoing;

  MarshallDeputy incoming;
  m >> incoming;
  EXPECT_EQ(incoming.kind_, janus::ReplicatedDBCommand::static_kind());

  auto decoded = marshallable_cast<janus::ReplicatedDBCommand>(incoming);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->op_, janus::ReplicatedDBOp::PUT);
  EXPECT_EQ(decoded->key_, "k1");
  EXPECT_EQ(decoded->value_, "v1");
}

TEST(MarshallableProxyFacadeTest, EmptyGraphRoundTripUsesAnyMessageEnvelope) {
  auto payload = std::make_shared<janus::EmptyGraph>();
  ASSERT_NE(payload, nullptr);

  // Workstream N L7: graph payloads moved from kind-tagged Serializable
  // to the open-set `AnyMessage` envelope.  All graph types now serialize
  // under MarshallDeputy::ANY_MESSAGE; the type identity ("janus.EmptyGraph"
  // vs "janus.RccGraph") is a string inside the envelope.
  MarshallDeputy outgoing(rrr::AnyMessage::pack(payload));
  EXPECT_EQ(outgoing.kind_, MarshallDeputy::ANY_MESSAGE);

  Marshal m;
  m << outgoing;

  MarshallDeputy incoming;
  m >> incoming;
  EXPECT_EQ(incoming.kind_, MarshallDeputy::ANY_MESSAGE);
  auto am = rrr::AnyMessage::try_cast(incoming);
  ASSERT_NE(am, nullptr);
  EXPECT_TRUE(am->is_a<janus::EmptyGraph>());
  ASSERT_NE(am->unpack<janus::EmptyGraph>(), nullptr);
}

TEST(MarshallableProxyFacadeTest, RccGraphRoundTripUsesAnyMessageEnvelope) {
  auto payload = std::make_shared<janus::RccGraph>();
  ASSERT_NE(payload, nullptr);

  MarshallDeputy outgoing(rrr::AnyMessage::pack(payload));
  EXPECT_EQ(outgoing.kind_, MarshallDeputy::ANY_MESSAGE);

  Marshal m;
  m << outgoing;

  MarshallDeputy incoming;
  m >> incoming;
  EXPECT_EQ(incoming.kind_, MarshallDeputy::ANY_MESSAGE);

  auto am = rrr::AnyMessage::try_cast(incoming);
  ASSERT_NE(am, nullptr);
  EXPECT_TRUE(am->is_a<janus::RccGraph>());
  auto decoded = am->unpack<janus::RccGraph>();
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->size(), 0u);
}

TEST(MarshallableProxyFacadeTest,
     PaxosControlPayloadsUseTypedAdapterConstructionPath) {
  auto bulk_prepare = std::make_shared<janus::BulkPrepareLog>();
  auto prep_cmd = std::make_shared<janus::PaxosPrepCmd>();
  auto heartbeat = std::make_shared<janus::HeartBeatLog>();
  auto sync_req = std::make_shared<janus::SyncLogRequest>();
  auto sync_resp = std::make_shared<janus::SyncLogResponse>();
  auto sync_noop = std::make_shared<janus::SyncNoOpRequest>();

  EXPECT_EQ(wrap_typed_marshallable(bulk_prepare)->kind(),
            janus::BulkPrepareLog::static_kind());
  EXPECT_EQ(wrap_typed_marshallable(prep_cmd)->kind(),
            janus::PaxosPrepCmd::static_kind());
  EXPECT_EQ(wrap_typed_marshallable(heartbeat)->kind(),
            janus::HeartBeatLog::static_kind());
  EXPECT_EQ(wrap_typed_marshallable(sync_req)->kind(),
            janus::SyncLogRequest::static_kind());
  EXPECT_EQ(wrap_typed_marshallable(sync_resp)->kind(),
            janus::SyncLogResponse::static_kind());
  EXPECT_EQ(wrap_typed_marshallable(sync_noop)->kind(),
            janus::SyncNoOpRequest::static_kind());
}

TEST(MarshallableProxyFacadeTest,
     PaxosControlPayloadsRoundTripViaTypedAdapters) {
  EnsureTestMarshallableInitializer();

  auto bulk_prepare = std::make_shared<janus::BulkPrepareLog>();
  bulk_prepare->min_prepared_slots = {{0u, 10}, {1u, 20}};
  bulk_prepare->leader_id = 3;
  bulk_prepare->epoch = 7;
  auto bulk_prepare_decoded = RoundTripTypedDeputyPayload(bulk_prepare);
  ASSERT_NE(bulk_prepare_decoded, nullptr);
  EXPECT_EQ(bulk_prepare_decoded->leader_id, 3u);
  EXPECT_EQ(bulk_prepare_decoded->epoch, 7);
  ASSERT_EQ(bulk_prepare_decoded->min_prepared_slots.size(), 2u);
  EXPECT_EQ(bulk_prepare_decoded->min_prepared_slots[1].second, 20);

  auto prep_cmd = std::make_shared<janus::PaxosPrepCmd>();
  prep_cmd->slots = {5, 6};
  prep_cmd->ballots = {11, 12};
  prep_cmd->leader_id = 2;
  auto prep_cmd_decoded = RoundTripTypedDeputyPayload(prep_cmd);
  ASSERT_NE(prep_cmd_decoded, nullptr);
  EXPECT_EQ(prep_cmd_decoded->leader_id, 2);
  ASSERT_EQ(prep_cmd_decoded->slots.size(), 2u);
  ASSERT_EQ(prep_cmd_decoded->ballots.size(), 2u);
  EXPECT_EQ(prep_cmd_decoded->slots[0], 5);
  EXPECT_EQ(prep_cmd_decoded->ballots[1], 12);

  auto heartbeat = std::make_shared<janus::HeartBeatLog>();
  heartbeat->leader_id = 9;
  heartbeat->epoch = 13;
  auto heartbeat_decoded = RoundTripTypedDeputyPayload(heartbeat);
  ASSERT_NE(heartbeat_decoded, nullptr);
  EXPECT_EQ(heartbeat_decoded->leader_id, 9u);
  EXPECT_EQ(heartbeat_decoded->epoch, 13);

  auto sync_req = std::make_shared<janus::SyncLogRequest>();
  sync_req->leader_id = 1;
  sync_req->epoch = 44;
  sync_req->sync_commit_slot = {100, 120, 140};
  auto sync_req_decoded = RoundTripTypedDeputyPayload(sync_req);
  ASSERT_NE(sync_req_decoded, nullptr);
  EXPECT_EQ(sync_req_decoded->leader_id, 1);
  EXPECT_EQ(sync_req_decoded->epoch, 44);
  ASSERT_EQ(sync_req_decoded->sync_commit_slot.size(), 3u);
  EXPECT_EQ(sync_req_decoded->sync_commit_slot[2], 140);

  auto sync_resp = std::make_shared<janus::SyncLogResponse>();
  sync_resp->sync_data.push_back(
      std::make_shared<MarshallDeputy>(std::make_shared<TestMarshallable>(55)));
  sync_resp->missing_slots = {{4, 8}, {15}};
  auto sync_resp_decoded = RoundTripTypedDeputyPayload(sync_resp);
  ASSERT_NE(sync_resp_decoded, nullptr);
  ASSERT_EQ(sync_resp_decoded->sync_data.size(), 1u);
  auto nested =
      marshallable_cast<TestMarshallable>(sync_resp_decoded->sync_data[0].get());
  ASSERT_NE(nested, nullptr);
  EXPECT_EQ(nested->value, 55);
  ASSERT_EQ(sync_resp_decoded->missing_slots.size(), 2u);
  ASSERT_EQ(sync_resp_decoded->missing_slots[0].size(), 2u);
  EXPECT_EQ(sync_resp_decoded->missing_slots[0][1], 8);

  auto sync_noop = std::make_shared<janus::SyncNoOpRequest>();
  sync_noop->leader_id = 6;
  sync_noop->epoch = 77;
  sync_noop->sync_slots = {21, 22};
  auto sync_noop_decoded = RoundTripTypedDeputyPayload(sync_noop);
  ASSERT_NE(sync_noop_decoded, nullptr);
  EXPECT_EQ(sync_noop_decoded->leader_id, 6);
  EXPECT_EQ(sync_noop_decoded->epoch, 77);
  ASSERT_EQ(sync_noop_decoded->sync_slots.size(), 2u);
  EXPECT_EQ(sync_noop_decoded->sync_slots[1], 22);
}

TEST(MarshallableProxyFacadeTest, PaxosLogEntryRoundTripUsesTypedAdapter) {
  auto log_entry = std::make_shared<janus::LogEntry>();
  log_entry->length = 5;
  log_entry->log_entry = "abcde";

  auto decoded = RoundTripTypedDeputyPayload(log_entry);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->length, 5);
  EXPECT_EQ(decoded->log_entry, "abcde");
}

TEST(MarshallableProxyFacadeTest, PaxosBulkPaxosCmdRoundTripUsesTypedAdapter) {
  EnsureTestMarshallableInitializer();

  auto payload = std::make_shared<janus::BulkPaxosCmd>();
  payload->leader_id = 4;
  payload->slots = {10, 11};
  payload->ballots = {20, 21};
  payload->cmds.push_back(
      std::make_shared<MarshallDeputy>(std::make_shared<TestMarshallable>(88)));
  payload->cmds.push_back(
      std::make_shared<MarshallDeputy>(std::make_shared<TestMarshallable>(99)));

  auto decoded = RoundTripTypedDeputyPayload(payload);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->leader_id, 4);
  ASSERT_EQ(decoded->slots.size(), 2u);
  ASSERT_EQ(decoded->ballots.size(), 2u);
  ASSERT_EQ(decoded->cmds.size(), 2u);
  EXPECT_EQ(decoded->slots[1], 11);
  EXPECT_EQ(decoded->ballots[0], 20);

  auto nested0 = marshallable_cast<TestMarshallable>(decoded->cmds[0].get());
  auto nested1 = marshallable_cast<TestMarshallable>(decoded->cmds[1].get());
  ASSERT_NE(nested0, nullptr);
  ASSERT_NE(nested1, nullptr);
  EXPECT_EQ(nested0->value, 88);
  EXPECT_EQ(nested1->value, 99);
}
