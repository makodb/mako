#include <std_compat.hpp>  // textual STL before `import std` (abi_tag fix)
#include <stdint.h>

#include <gtest/gtest.h>
#include "../srpc.hpp"
#include "../misc/serializable.hpp"  // wrap_serializable, serializable_cast
#include "deptran/RW_command.h"
#include "deptran/classic/tpc_command.h"
#include "deptran/procedure.h"
#include "deptran/raft/replicated_db.h"
#include "deptran/rcc/dep_graph.h"
#include "deptran/paxos_worker.h"

import std;
import rusty;

using namespace srpc;

// removed the `TypedOnlyPayload` test
// fixture (struct + namespace + adapter typedef +
// `TypedMarshallableAdapterTraits` specialization +
// `EnsureTypedOnlyPayloadInitializer` helper +
// `TypedPayloadRoundTripsViaDeputyAdapter` /
// `TypedPayloadInitializerStateContainsAdapter` tests). Its sole
// purpose was to validate the `TypedMarshallableAdapter` trait
// path; that path went away with Phase 4 (every production type
// migrated to Serializable) and is now deleted.

namespace {

struct UnregisteredMakoPayload {};

template <typename T>
concept MakoPackable = requires(const T& value) {
  janus::Command::Base::template pack<T>(value);
};

static_assert(static_cast<int32_t>(janus::MakoCommandKind::Unknown) == 0);

#define ASSERT_MAKO_PAYLOAD_KIND(TypeName, KindValue)                         \
  static_assert(srpc::PayloadMember<janus::MakoCommands,                      \
                                   janus::TypeName>::value);                 \
  static_assert(srpc::PayloadMember<janus::MakoCommands,                      \
                                   janus::TypeName>::KIND == KindValue);     \
  static_assert(static_cast<int32_t>(janus::MakoCommandKind::TypeName) ==    \
                KindValue);                                                  \
  static_assert(janus::TypeName::static_kind() == KindValue);                \
  static_assert(MakoPackable<janus::TypeName>)

ASSERT_MAKO_PAYLOAD_KIND(LogEntry, 1);
ASSERT_MAKO_PAYLOAD_KIND(TpcPrepareCommand, 2);
ASSERT_MAKO_PAYLOAD_KIND(TpcCommitCommand, 3);
ASSERT_MAKO_PAYLOAD_KIND(VecPieceData, 4);
ASSERT_MAKO_PAYLOAD_KIND(BulkPaxosCmd, 5);
ASSERT_MAKO_PAYLOAD_KIND(BulkPrepareLog, 6);
ASSERT_MAKO_PAYLOAD_KIND(HeartBeatLog, 7);
ASSERT_MAKO_PAYLOAD_KIND(SyncLogRequest, 8);
ASSERT_MAKO_PAYLOAD_KIND(SyncLogResponse, 9);
ASSERT_MAKO_PAYLOAD_KIND(SyncNoOpRequest, 10);
ASSERT_MAKO_PAYLOAD_KIND(PaxosPrepCmd, 11);
ASSERT_MAKO_PAYLOAD_KIND(TpcEmptyCommand, 12);
ASSERT_MAKO_PAYLOAD_KIND(TpcNoopCommand, 13);
ASSERT_MAKO_PAYLOAD_KIND(TpcBatchCommand, 14);
ASSERT_MAKO_PAYLOAD_KIND(VecRecData, 15);
ASSERT_MAKO_PAYLOAD_KIND(ViewData, 16);
ASSERT_MAKO_PAYLOAD_KIND(SimpleRWCommand, 17);
ASSERT_MAKO_PAYLOAD_KIND(KeyCmdBatchData, 18);
ASSERT_MAKO_PAYLOAD_KIND(ReplicatedDBCommand, 19);

#undef ASSERT_MAKO_PAYLOAD_KIND

static_assert(!srpc::PayloadMember<janus::MakoCommands,
                                  UnregisteredMakoPayload>::value);
static_assert(!MakoPackable<UnregisteredMakoPayload>);

template <typename T>
void ExpectMakoKindWireByte(int32_t expected) {
  ASSERT_EQ(T::static_kind(), expected);

  srpc::BufferSink sink;
  srpc::BinaryWriteArchive writer(srpc::make_sink_proxy_buffer(&sink));
  const srpc::v32 tag(expected);
  srpc::Serialize_::serialize(tag, writer);

  ASSERT_EQ(sink.bytes.len(), 1u);
  EXPECT_EQ(sink.bytes[0], static_cast<uint8_t>(expected));
}

// 2 step 5 (2026-05-05): retired the
// `static_assert(!std::is_base_of_v<Marshallable, ...>)` block — the
// `Marshallable` type is gone, so the check is no longer
// expressible. The Phase 4 migration that motivated these guards is
// complete (every production payload uses Serializable directly).

template <typename T>
rusty::Option<rusty::Arc<T>> RoundTripTypedPayload(const rusty::Arc<T>& src) {
  // 2 step 5 (2026-05-05): wire round-trip via Command's
  // Marshal& archive operators (added in L10f-2 step 2; same wire
  // bytes as the legacy MarshallDeputy path).  T is auto-wrapped by
  // Command's templated non-Marshallable ctor.
  janus::Command outgoing{src};
  srpc::BufferSink sink;
  srpc::BinaryWriteArchive war(srpc::make_sink_proxy_buffer(&sink));
  srpc::Serialize_::serialize(outgoing, war);

  janus::Command incoming;
  srpc::BufferSource source(sink.bytes.data(), sink.bytes.len());
  srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&source));
  srpc::Deserialize_::deserialize(incoming, rar);

  return marshallable_cast<T>(incoming);
}

rusty::Arc<janus::TpcCommitCommand> MakeTypedTpcCommitPayload(
    txnid_t tx_id,
    int ret,
    ballot_t term,
    bool_t recovery) {
  // Fill a local value first, then wrap in the const-view Arc.
  janus::TpcCommitCommand commit;
  commit.tx_id_ = tx_id;
  commit.ret_ = ret;
  commit.term = term;

  janus::VecPieceData vec_piece;
  vec_piece.sp_vec_piece_data_ =
      std::make_shared<std::vector<std::shared_ptr<janus::SimpleCommand>>>();
  vec_piece.is_recovery_command_ = recovery;
  commit.cmd_ = janus::Command::pack_aliased(rusty::Arc<janus::VecPieceData>::make(std::move(vec_piece)));

  janus::ViewData view_data;
  view_data.view_.n_ = 3;
  view_data.view_.view_id_ = 19;
  view_data.view_.timestamp_ = 777;
  view_data.view_.leaders_ = {0, 1, 2};
  view_data.partition_id_ = 11;
  commit.sp_view_data_ = rusty::Option<rusty::Arc<janus::ViewData>>(
      rusty::Arc<janus::ViewData>::make(std::move(view_data)));

  return rusty::Arc<janus::TpcCommitCommand>::make(std::move(commit));
}

}  // namespace

TEST(MakoCommandKindTest, AllExplicitDiscriminantsAreOneByteV32) {
  ExpectMakoKindWireByte<janus::LogEntry>(1);
  ExpectMakoKindWireByte<janus::TpcPrepareCommand>(2);
  ExpectMakoKindWireByte<janus::TpcCommitCommand>(3);
  ExpectMakoKindWireByte<janus::VecPieceData>(4);
  ExpectMakoKindWireByte<janus::BulkPaxosCmd>(5);
  ExpectMakoKindWireByte<janus::BulkPrepareLog>(6);
  ExpectMakoKindWireByte<janus::HeartBeatLog>(7);
  ExpectMakoKindWireByte<janus::SyncLogRequest>(8);
  ExpectMakoKindWireByte<janus::SyncLogResponse>(9);
  ExpectMakoKindWireByte<janus::SyncNoOpRequest>(10);
  ExpectMakoKindWireByte<janus::PaxosPrepCmd>(11);
  ExpectMakoKindWireByte<janus::TpcEmptyCommand>(12);
  ExpectMakoKindWireByte<janus::TpcNoopCommand>(13);
  ExpectMakoKindWireByte<janus::TpcBatchCommand>(14);
  ExpectMakoKindWireByte<janus::VecRecData>(15);
  ExpectMakoKindWireByte<janus::ViewData>(16);
  ExpectMakoKindWireByte<janus::SimpleRWCommand>(17);
  ExpectMakoKindWireByte<janus::KeyCmdBatchData>(18);
  ExpectMakoKindWireByte<janus::ReplicatedDBCommand>(19);
}

TEST(MakoCommandKindTest, ExplicitMappingControlsExactEnvelopeWireBytes) {
  janus::TpcNoopCommand payload;
  auto envelope = janus::Command::pack(payload);

  EXPECT_EQ(payload.kind(), 13);
  EXPECT_EQ(envelope.kind(), 13);

  srpc::BufferSink sink;
  srpc::BinaryWriteArchive writer(srpc::make_sink_proxy_buffer(&sink));
  srpc::Serialize_::serialize(envelope, writer);

  ASSERT_EQ(sink.bytes.len(), 1u);
  EXPECT_EQ(sink.bytes[0], 13u);
}

// removed eight tests
// (`DeputyDefaultsToNoMarshallable`,
// `DeputyStoresProxyAndPreservesInnerSharedPtr`,
// `DeputyRoundTripPreservesDerivedMarshallable`,
// `InitializerReturnsProxyBackedMetadata`,
// `MarshallableCastFromSharedPtrKeepsType`,
// `MarshallableCastFromMarshallableRefKeepsType`,
// `MarshallableCastFromMarshallablePointerHandlesNullAndValue`,
// `MarshallableCastFromNullDeputyPointerIsNull`) plus the
// `TestMarshallable` fixture / `kTestMarshallableKind` constant /
// `EnsureTestMarshallableInitializer` helper.  All exercised the
// `MarshallDeputy` / `Marshallable` infrastructure that retires
// alongside in this same commit.
//
// removed
// `TypedPayloadRoundTripsViaDeputyAdapter` and
// `TypedPayloadInitializerStateContainsAdapter` tests — they
// exercised the `TypedMarshallableAdapter` trait path, which is
// gone now (every production payload uses Serializable).

// 2 step 5 (2026-05-05): removed
// `DeptranVecPieceDataUsesTypedAdapterPath` — exercised the
// `wrap_serializable_aliased` aliasing identity through
// `MarshallDeputy`.  Both helpers retire in this same commit;
// the remaining `DeptranVecPieceDataNonEmptyRoundTrip` test
// covers the wire-roundtrip case for VecPieceData.

// round-trip a populated VecPieceData (with a real
// SimpleCommand carrying TxWorkspace input + map<int32_t,Value> output
// fields) through Marshal serialization to exercise the new archive
// operators end-to-end. This stresses the SimpleCommand,
// TxWorkspace, and mdb::Value archive operators that were added
// alongside the VecPieceData migration.
TEST(MarshallableProxyFacadeTest, DeptranVecPieceDataNonEmptyRoundTrip) {
  // Fill a local value first, then wrap in the const-view Arc below.
  janus::VecPieceData payload;
  payload.sp_vec_piece_data_ =
      std::make_shared<std::vector<std::shared_ptr<janus::SimpleCommand>>>();
  payload.time_sent_from_client_ = 17.5;
  payload.is_recovery_command_ = 1;

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
  payload.sp_vec_piece_data_->push_back(cmd1);

  // SimpleCommand 2: input/output both empty (edge case).
  auto cmd2 = std::make_shared<janus::SimpleCommand>();
  cmd2->id_ = 2002;
  cmd2->type_ = 8;
  cmd2->partition_id_ = 22;
  payload.sp_vec_piece_data_->push_back(cmd2);

  // Serialize via Command's Marshal& archive operators (added in
  // 2 step 2; same wire bytes as the legacy MarshallDeputy
  // path).
  janus::Command outgoing{rusty::Arc<janus::VecPieceData>::make(std::move(payload))};
  srpc::BufferSink sink;
  srpc::BinaryWriteArchive war(srpc::make_sink_proxy_buffer(&sink));
  srpc::Serialize_::serialize(outgoing, war);

  janus::Command incoming;
  srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
  srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
  srpc::Deserialize_::deserialize(incoming, rar);
  const auto decoded = marshallable_cast<janus::VecPieceData>(incoming);
  ASSERT_TRUE(decoded.is_some());

  EXPECT_DOUBLE_EQ(decoded.unwrap()->time_sent_from_client_, 17.5);
  EXPECT_EQ(decoded.unwrap()->is_recovery_command_, 1);
  ASSERT_EQ(decoded.unwrap()->sp_vec_piece_data_->size(), 2u);

  auto& d1 = *(*decoded.unwrap()->sp_vec_piece_data_)[0];
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

  auto& d2 = *(*decoded.unwrap()->sp_vec_piece_data_)[1];
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

  // ViewData migrated to Serializable. Use Archive
  // round-trip — the Marshal-based to_marshal/from_marshal methods
  // are gone.
  BufferSink sink;
  BinaryWriteArchive writer(make_sink_proxy_buffer(&sink));
  src.save(writer);

  janus::ViewData dst;
  BufferSource source(sink.bytes.data(), sink.bytes.len());
  BinaryReadArchive reader(make_source_proxy_buffer(&source));
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
  // Fill local values first, then wrap in const-view Arcs.
  janus::VecRecData vec_rec;
  vec_rec.key_data_ = std::make_shared<std::vector<key_t>>();
  vec_rec.key_data_->push_back(11);
  vec_rec.key_data_->push_back(12);

  janus::Command vec_rec_envelope{
      rusty::Arc<janus::VecRecData>::make(std::move(vec_rec))};
  EXPECT_EQ(vec_rec_envelope.kind_, janus::VecRecData::static_kind());
  const auto decoded_vec_rec =
      marshallable_cast<janus::VecRecData>(vec_rec_envelope);
  ASSERT_TRUE(decoded_vec_rec.is_some());
  ASSERT_NE(decoded_vec_rec.unwrap()->key_data_, nullptr);
  ASSERT_EQ(decoded_vec_rec.unwrap()->key_data_->size(), 2u);
  EXPECT_EQ((*decoded_vec_rec.unwrap()->key_data_)[0], 11);
  EXPECT_EQ((*decoded_vec_rec.unwrap()->key_data_)[1], 12);

  janus::KeyCmdBatchData batch;
  janus::HeartBeatLog nested;
  nested.leader_id = 77;
  nested.epoch = 0;
  batch.AddEntry(1001, janus::Command::pack_aliased(rusty::Arc<janus::HeartBeatLog>::make(
                           std::move(nested))));

  janus::Command batch_envelope = janus::Command::pack_aliased(
      rusty::Arc<janus::KeyCmdBatchData>::make(std::move(batch)));
  EXPECT_EQ(batch_envelope.kind_, janus::KeyCmdBatchData::static_kind());
  const auto decoded_batch =
      marshallable_cast<janus::KeyCmdBatchData>(batch_envelope);
  ASSERT_TRUE(decoded_batch.is_some());
  ASSERT_EQ(decoded_batch.unwrap()->Size(), 1u);
  EXPECT_EQ(decoded_batch.unwrap()->GetKey(0), 1001);
  const auto nested_decoded = marshallable_cast<janus::HeartBeatLog>(
      decoded_batch.unwrap()->GetCommand(0));
  ASSERT_TRUE(nested_decoded.is_some());
  EXPECT_EQ(nested_decoded.unwrap()->leader_id, 77u);
}

TEST(MarshallableProxyFacadeTest, DeptranTpcCommitRoundTripUsesTypedAdapter) {
  auto src = MakeTypedTpcCommitPayload(/*tx_id=*/321, /*ret=*/9, /*term=*/17,
                                       /*recovery=*/1);

  janus::Command outgoing{src};
  EXPECT_EQ(outgoing.kind_, janus::TpcCommitCommand::static_kind());

  srpc::BufferSink sink;
  srpc::BinaryWriteArchive war(srpc::make_sink_proxy_buffer(&sink));
  srpc::Serialize_::serialize(outgoing, war);

  janus::Command incoming;
  srpc::BufferSource byte_src(sink.bytes.data(), sink.bytes.len());
  srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&byte_src));
  srpc::Deserialize_::deserialize(incoming, rar);
  EXPECT_EQ(incoming.kind_, janus::TpcCommitCommand::static_kind());

  const auto decoded = marshallable_cast<janus::TpcCommitCommand>(incoming);
  ASSERT_TRUE(decoded.is_some());
  EXPECT_EQ(decoded.unwrap()->tx_id_, 321);
  EXPECT_EQ(decoded.unwrap()->ret_, 9);
  EXPECT_EQ(decoded.unwrap()->term, 17);

  const auto decoded_vec_piece =
      marshallable_cast<janus::VecPieceData>(decoded.unwrap()->cmd_);
  ASSERT_TRUE(decoded_vec_piece.is_some());
  EXPECT_EQ(decoded_vec_piece.unwrap()->is_recovery_command_, 1);

  ASSERT_TRUE(decoded.unwrap()->sp_view_data_.is_some());
  EXPECT_EQ(decoded.unwrap()->sp_view_data_.unwrap()->partition_id_, 11);
  EXPECT_EQ(decoded.unwrap()->sp_view_data_.unwrap()->view_.view_id_, 19);
}

TEST(MarshallableProxyFacadeTest, DeptranTpcBatchAndNoopEmptyUseTypedAdapter) {
  // Fill a local value first, then wrap in the const-view Arc below.
  janus::TpcBatchCommand batch;
  std::vector<rusty::Arc<janus::TpcCommitCommand>> commits{
      MakeTypedTpcCommitPayload(/*tx_id=*/101, /*ret=*/1, /*term=*/3,
                                /*recovery=*/0),
      MakeTypedTpcCommitPayload(/*tx_id=*/202, /*ret=*/2, /*term=*/4,
                                /*recovery=*/1)};
  batch.AddCmds(commits);

  janus::Command batch_outgoing{
      rusty::Arc<janus::TpcBatchCommand>::make(std::move(batch))};
  EXPECT_EQ(batch_outgoing.kind_, janus::TpcBatchCommand::static_kind());
  srpc::BufferSink batch_sink;
  srpc::BinaryWriteArchive batch_war(srpc::make_sink_proxy_buffer(&batch_sink));
  srpc::Serialize_::serialize(batch_outgoing, batch_war);

  janus::Command batch_incoming;
  srpc::BufferSource batch_src(batch_sink.bytes.data(), batch_sink.bytes.len());
  srpc::BinaryReadArchive batch_rar(srpc::make_source_proxy_buffer(&batch_src));
  srpc::Deserialize_::deserialize(batch_incoming, batch_rar);
  const auto decoded_batch =
      marshallable_cast<janus::TpcBatchCommand>(batch_incoming);
  ASSERT_TRUE(decoded_batch.is_some());
  ASSERT_EQ(decoded_batch.unwrap()->Size(), 2u);
  EXPECT_EQ(decoded_batch.unwrap()->cmds_.at(0)->tx_id_, 101);
  EXPECT_EQ(decoded_batch.unwrap()->cmds_.at(1)->tx_id_, 202);

  // 2 step 5 (2026-05-05): TpcEmptyCommand round-trip via
  // Command::pack_aliased preserves the caller's Arc identity.
  auto empty_cmd = rusty::Arc<janus::TpcEmptyCommand>::make();
  janus::Command empty_envelope =
      janus::Command::pack_aliased<janus::TpcEmptyCommand>(empty_cmd);
  EXPECT_EQ(empty_envelope.kind_, janus::TpcEmptyCommand::static_kind());
  ASSERT_NE(empty_envelope.unpack<janus::TpcEmptyCommand>(), nullptr);
  EXPECT_EQ(static_cast<const janus::TpcEmptyCommand*>(
                empty_envelope.unpack<janus::TpcEmptyCommand>()),
            empty_cmd.get())
      << "pack_aliased: unpack should return the same instance as "
         "the caller's Arc";

  // TpcNoopCommand value-pack — owns a fresh copy; unpack returns
  // a different instance from the caller's Arc.
  auto noop_cmd = rusty::Arc<janus::TpcNoopCommand>::make();
  janus::Command noop_envelope = janus::Command::pack(*noop_cmd);
  EXPECT_EQ(noop_envelope.kind_, janus::TpcNoopCommand::static_kind());
  ASSERT_NE(noop_envelope.unpack<janus::TpcNoopCommand>(), nullptr);
}

TEST(MarshallableProxyFacadeTest,
     ReplicatedDbCommandRoundTripUsesTypedAdapter) {
  auto put_cmd = janus::ReplicatedDBCommand::CreatePut("k1", "v1");
  ASSERT_TRUE(put_cmd.get() != nullptr);

  janus::Command outgoing{put_cmd};
  EXPECT_EQ(outgoing.kind_, janus::ReplicatedDBCommand::static_kind());

  srpc::BufferSink sink;
  srpc::BinaryWriteArchive war(srpc::make_sink_proxy_buffer(&sink));
  srpc::Serialize_::serialize(outgoing, war);

  janus::Command incoming;
  srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
  srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
  srpc::Deserialize_::deserialize(incoming, rar);
  EXPECT_EQ(incoming.kind_, janus::ReplicatedDBCommand::static_kind());

  const auto decoded = marshallable_cast<janus::ReplicatedDBCommand>(incoming);
  ASSERT_TRUE(decoded.is_some());
  EXPECT_EQ(decoded.unwrap()->op_, janus::ReplicatedDBOp::PUT);
  EXPECT_EQ(decoded.unwrap()->key_, "k1");
  EXPECT_EQ(decoded.unwrap()->value_, "v1");
}

TEST(MarshallableProxyFacadeTest, EmptyGraphRoundTripUsesAnyMessageEnvelope) {
  auto payload = rusty::Arc<janus::EmptyGraph>::make();
  ASSERT_NE(payload.get(), nullptr);

  // graph payloads moved from kind-tagged Serializable
  // to the open-set `AnyMessage` envelope.  L10f-2 step 5 (2026-05-05):
  // AnyMessage no longer inherits Marshallable; the envelope rides
  // directly in RPC fields without a surrounding MarshallDeputy.
  srpc::AnyMessage outgoing = srpc::AnyMessage::pack(payload);

  BufferSink sink;
  {
    BinaryWriteArchive writer(make_sink_proxy_buffer(&sink));
    srpc::Serialize_::serialize(outgoing, writer);
  }

  srpc::AnyMessage incoming;
  {
    BufferSource src(sink.bytes.data(), sink.bytes.len());
    BinaryReadArchive reader(make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(incoming, reader);
  }
  EXPECT_TRUE(incoming.is_a<janus::EmptyGraph>());
  ASSERT_TRUE(incoming.unpack<janus::EmptyGraph>().is_some());
}

TEST(MarshallableProxyFacadeTest, RccGraphRoundTripUsesAnyMessageEnvelope) {
  auto payload = rusty::Arc<janus::RccGraph>::make();
  ASSERT_NE(payload.get(), nullptr);

  srpc::AnyMessage outgoing = srpc::AnyMessage::pack(payload);

  BufferSink sink;
  {
    BinaryWriteArchive writer(make_sink_proxy_buffer(&sink));
    srpc::Serialize_::serialize(outgoing, writer);
  }

  srpc::AnyMessage incoming;
  {
    BufferSource src(sink.bytes.data(), sink.bytes.len());
    BinaryReadArchive reader(make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(incoming, reader);
  }

  EXPECT_TRUE(incoming.is_a<janus::RccGraph>());
  const auto decoded = incoming.unpack<janus::RccGraph>();
  ASSERT_TRUE(decoded.is_some());
  EXPECT_EQ(decoded.unwrap()->size(), 0u);
}

TEST(MarshallableProxyFacadeTest,
     PaxosControlPayloadsUseTypedAdapterConstructionPath) {
  auto bulk_prepare = rusty::Arc<janus::BulkPrepareLog>::make();
  auto prep_cmd = rusty::Arc<janus::PaxosPrepCmd>::make();
  auto heartbeat = rusty::Arc<janus::HeartBeatLog>::make();
  auto sync_req = rusty::Arc<janus::SyncLogRequest>::make();
  auto sync_resp = rusty::Arc<janus::SyncLogResponse>::make();
  auto sync_noop = rusty::Arc<janus::SyncNoOpRequest>::make();

  // 2 step 5 (2026-05-05): construction path verifies that
  // Wrapping in `Command` produces the explicitly registered kind tag.
  EXPECT_EQ(janus::Command{bulk_prepare}.kind_,
            janus::BulkPrepareLog::static_kind());
  EXPECT_EQ(janus::Command{prep_cmd}.kind_,
            janus::PaxosPrepCmd::static_kind());
  EXPECT_EQ(janus::Command{heartbeat}.kind_,
            janus::HeartBeatLog::static_kind());
  EXPECT_EQ(janus::Command{sync_req}.kind_,
            janus::SyncLogRequest::static_kind());
  EXPECT_EQ(janus::Command{sync_resp}.kind_,
            janus::SyncLogResponse::static_kind());
  EXPECT_EQ(janus::Command{sync_noop}.kind_,
            janus::SyncNoOpRequest::static_kind());
}

TEST(MarshallableProxyFacadeTest,
     PaxosControlPayloadsRoundTripViaTypedAdapters) {
  // Each payload: fill a local value, then wrap in a const-view Arc
  // for the round-trip helper.
  janus::BulkPrepareLog bulk_prepare;
  bulk_prepare.min_prepared_slots = {{0u, 10}, {1u, 20}};
  bulk_prepare.leader_id = 3;
  bulk_prepare.epoch = 7;
  const auto bulk_prepare_decoded = RoundTripTypedPayload(
      rusty::Arc<janus::BulkPrepareLog>::make(std::move(bulk_prepare)));
  ASSERT_TRUE(bulk_prepare_decoded.is_some());
  EXPECT_EQ(bulk_prepare_decoded.unwrap()->leader_id, 3u);
  EXPECT_EQ(bulk_prepare_decoded.unwrap()->epoch, 7);
  ASSERT_EQ(bulk_prepare_decoded.unwrap()->min_prepared_slots.size(), 2u);
  EXPECT_EQ(bulk_prepare_decoded.unwrap()->min_prepared_slots[1].second, 20);

  janus::PaxosPrepCmd prep_cmd;
  prep_cmd.slots = {5, 6};
  prep_cmd.ballots = {11, 12};
  prep_cmd.leader_id = 2;
  const auto prep_cmd_decoded = RoundTripTypedPayload(
      rusty::Arc<janus::PaxosPrepCmd>::make(std::move(prep_cmd)));
  ASSERT_TRUE(prep_cmd_decoded.is_some());
  EXPECT_EQ(prep_cmd_decoded.unwrap()->leader_id, 2);
  ASSERT_EQ(prep_cmd_decoded.unwrap()->slots.size(), 2u);
  ASSERT_EQ(prep_cmd_decoded.unwrap()->ballots.size(), 2u);
  EXPECT_EQ(prep_cmd_decoded.unwrap()->slots[0], 5);
  EXPECT_EQ(prep_cmd_decoded.unwrap()->ballots[1], 12);

  janus::HeartBeatLog heartbeat;
  heartbeat.leader_id = 9;
  heartbeat.epoch = 13;
  const auto heartbeat_decoded = RoundTripTypedPayload(
      rusty::Arc<janus::HeartBeatLog>::make(std::move(heartbeat)));
  ASSERT_TRUE(heartbeat_decoded.is_some());
  EXPECT_EQ(heartbeat_decoded.unwrap()->leader_id, 9u);
  EXPECT_EQ(heartbeat_decoded.unwrap()->epoch, 13);

  janus::SyncLogRequest sync_req;
  sync_req.leader_id = 1;
  sync_req.epoch = 44;
  sync_req.sync_commit_slot = {100, 120, 140};
  const auto sync_req_decoded = RoundTripTypedPayload(
      rusty::Arc<janus::SyncLogRequest>::make(std::move(sync_req)));
  ASSERT_TRUE(sync_req_decoded.is_some());
  EXPECT_EQ(sync_req_decoded.unwrap()->leader_id, 1);
  EXPECT_EQ(sync_req_decoded.unwrap()->epoch, 44);
  ASSERT_EQ(sync_req_decoded.unwrap()->sync_commit_slot.size(), 3u);
  EXPECT_EQ(sync_req_decoded.unwrap()->sync_commit_slot[2], 140);

  janus::SyncLogResponse sync_resp;
  janus::HeartBeatLog nested_payload_55;
  nested_payload_55.leader_id = 55;
  nested_payload_55.epoch = 0;
  sync_resp.sync_data.push_back(rusty::Arc<janus::Command>::make(
      rusty::Arc<janus::HeartBeatLog>::make(std::move(nested_payload_55))));
  sync_resp.missing_slots = {{4, 8}, {15}};
  const auto sync_resp_decoded = RoundTripTypedPayload(
      rusty::Arc<janus::SyncLogResponse>::make(std::move(sync_resp)));
  ASSERT_TRUE(sync_resp_decoded.is_some());
  ASSERT_EQ(sync_resp_decoded.unwrap()->sync_data.size(), 1u);
  const auto nested = marshallable_cast<janus::HeartBeatLog>(
      *sync_resp_decoded.unwrap()->sync_data[0]);
  ASSERT_TRUE(nested.is_some());
  EXPECT_EQ(nested.unwrap()->leader_id, 55u);
  ASSERT_EQ(sync_resp_decoded.unwrap()->missing_slots.size(), 2u);
  ASSERT_EQ(sync_resp_decoded.unwrap()->missing_slots[0].size(), 2u);
  EXPECT_EQ(sync_resp_decoded.unwrap()->missing_slots[0][1], 8);

  janus::SyncNoOpRequest sync_noop;
  sync_noop.leader_id = 6;
  sync_noop.epoch = 77;
  sync_noop.sync_slots = {21, 22};
  const auto sync_noop_decoded = RoundTripTypedPayload(
      rusty::Arc<janus::SyncNoOpRequest>::make(std::move(sync_noop)));
  ASSERT_TRUE(sync_noop_decoded.is_some());
  EXPECT_EQ(sync_noop_decoded.unwrap()->leader_id, 6);
  EXPECT_EQ(sync_noop_decoded.unwrap()->epoch, 77);
  ASSERT_EQ(sync_noop_decoded.unwrap()->sync_slots.size(), 2u);
  EXPECT_EQ(sync_noop_decoded.unwrap()->sync_slots[1], 22);
}

TEST(MarshallableProxyFacadeTest, PaxosLogEntryRoundTripUsesTypedAdapter) {
  janus::LogEntry log_entry;
  log_entry.length = 5;
  log_entry.log_entry = "abcde";

  const auto decoded = RoundTripTypedPayload(
      rusty::Arc<janus::LogEntry>::make(std::move(log_entry)));
  ASSERT_TRUE(decoded.is_some());
  EXPECT_EQ(decoded.unwrap()->length, 5);
  EXPECT_EQ(decoded.unwrap()->log_entry, "abcde");
}

TEST(MarshallableProxyFacadeTest, PaxosBulkPaxosCmdRoundTripUsesTypedAdapter) {
  janus::BulkPaxosCmd payload;
  payload.leader_id = 4;
  payload.slots = {10, 11};
  payload.ballots = {20, 21};
  janus::HeartBeatLog nested_payload_88;
  nested_payload_88.leader_id = 88;
  nested_payload_88.epoch = 0;
  janus::HeartBeatLog nested_payload_99;
  nested_payload_99.leader_id = 99;
  nested_payload_99.epoch = 0;
  payload.cmds.push_back(rusty::Arc<janus::Command>::make(
      rusty::Arc<janus::HeartBeatLog>::make(std::move(nested_payload_88))));
  payload.cmds.push_back(rusty::Arc<janus::Command>::make(
      rusty::Arc<janus::HeartBeatLog>::make(std::move(nested_payload_99))));

  const auto decoded = RoundTripTypedPayload(
      rusty::Arc<janus::BulkPaxosCmd>::make(std::move(payload)));
  ASSERT_TRUE(decoded.is_some());
  EXPECT_EQ(decoded.unwrap()->leader_id, 4);
  ASSERT_EQ(decoded.unwrap()->slots.size(), 2u);
  ASSERT_EQ(decoded.unwrap()->ballots.size(), 2u);
  ASSERT_EQ(decoded.unwrap()->cmds.size(), 2u);
  EXPECT_EQ(decoded.unwrap()->slots[1], 11);
  EXPECT_EQ(decoded.unwrap()->ballots[0], 20);

  const auto nested0 =
      marshallable_cast<janus::HeartBeatLog>(*decoded.unwrap()->cmds[0]);
  const auto nested1 =
      marshallable_cast<janus::HeartBeatLog>(*decoded.unwrap()->cmds[1]);
  ASSERT_TRUE(nested0.is_some());
  ASSERT_TRUE(nested1.is_some());
  EXPECT_EQ(nested0.unwrap()->leader_id, 88u);
  EXPECT_EQ(nested1.unwrap()->leader_id, 99u);
}
