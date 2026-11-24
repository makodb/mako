#include <std_compat.hpp>  // textual STL before `import std` (abi_tag fix)
#include <stdint.h>

#include <gtest/gtest.h>
#include "../srpc.hpp"
#include "deptran/tpc_command.h"
#include "deptran/replication_log_entry.h"

import std;
import rusty;
import srpc.serializable;  // wrap_serializable, serializable_cast

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
static_assert(static_cast<int32_t>(janus::MakoCommandKind::VecPieceData) == 4);
static_assert(static_cast<int32_t>(janus::MakoCommandKind::VecRecData) == 15);
static_assert(static_cast<int32_t>(janus::MakoCommandKind::SimpleRWCommand) == 17);
static_assert(static_cast<int32_t>(janus::MakoCommandKind::KeyCmdBatchData) == 18);
// Kind 19 stays reserved for the retired ReplicatedDBCommand payload; the
// class itself is gone with memory-only Raft, so only the enum value is pinned.
static_assert(static_cast<int32_t>(janus::MakoCommandKind::ReplicatedDBCommand) == 19);

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
ASSERT_MAKO_PAYLOAD_KIND(TpcEmptyCommand, 12);
ASSERT_MAKO_PAYLOAD_KIND(TpcNoopCommand, 13);
ASSERT_MAKO_PAYLOAD_KIND(TpcBatchCommand, 14);
ASSERT_MAKO_PAYLOAD_KIND(ViewData, 16);

#undef ASSERT_MAKO_PAYLOAD_KIND

static_assert(!srpc::PayloadMember<janus::MakoCommands,
                                  UnregisteredMakoPayload>::value);
static_assert(!MakoPackable<UnregisteredMakoPayload>);

void ExpectRawMakoKindWireByte(int32_t expected) {
  srpc::BufferSink sink;
  srpc::BinaryWriteArchive writer(srpc::make_sink_proxy_buffer(&sink));
  const srpc::v32 tag(expected);
  srpc::Serialize_::serialize(tag, writer);

  ASSERT_EQ(sink.bytes.len(), 1u);
  EXPECT_EQ(sink.bytes[0], static_cast<uint8_t>(expected));
}

template <typename T>
void ExpectMakoKindWireByte(int32_t expected) {
  ASSERT_EQ(T::static_kind(), expected);
  ExpectRawMakoKindWireByte(expected);
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
    std::string log_bytes) {
  // Fill a local value first, then wrap in the const-view Arc.
  janus::TpcCommitCommand commit;
  commit.tx_id_ = tx_id;
  commit.ret_ = ret;
  commit.term = term;

  janus::LogEntry log_entry;
  log_entry.length = static_cast<int>(log_bytes.size());
  log_entry.log_entry = std::move(log_bytes);
  commit.cmd_ = janus::Command::pack_aliased(
      rusty::Arc<janus::LogEntry>::make(std::move(log_entry)));

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
  // Retired payload kinds remain wire tombstones and must never be reused.
  ExpectRawMakoKindWireByte(4);
  ExpectMakoKindWireByte<janus::TpcEmptyCommand>(12);
  ExpectMakoKindWireByte<janus::TpcNoopCommand>(13);
  ExpectMakoKindWireByte<janus::TpcBatchCommand>(14);
  ExpectRawMakoKindWireByte(15);
  ExpectMakoKindWireByte<janus::ViewData>(16);
  ExpectRawMakoKindWireByte(17);
  ExpectRawMakoKindWireByte(18);
  ExpectRawMakoKindWireByte(19);
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

TEST(MarshallableProxyFacadeTest, DeptranTpcCommitRoundTripUsesTypedAdapter) {
  auto src = MakeTypedTpcCommitPayload(/*tx_id=*/321, /*ret=*/9, /*term=*/17,
                                       "raft-entry");

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

  const auto decoded_log_entry =
      marshallable_cast<janus::LogEntry>(decoded.unwrap()->cmd_);
  ASSERT_TRUE(decoded_log_entry.is_some());
  EXPECT_EQ(decoded_log_entry.unwrap()->length, 10);
  EXPECT_EQ(decoded_log_entry.unwrap()->log_entry, "raft-entry");

  ASSERT_TRUE(decoded.unwrap()->sp_view_data_.is_some());
  EXPECT_EQ(decoded.unwrap()->sp_view_data_.unwrap()->partition_id_, 11);
  EXPECT_EQ(decoded.unwrap()->sp_view_data_.unwrap()->view_.view_id_, 19);
}

TEST(MarshallableProxyFacadeTest, DeptranTpcBatchAndNoopEmptyUseTypedAdapter) {
  // Fill a local value first, then wrap in the const-view Arc below.
  janus::TpcBatchCommand batch;
  std::vector<rusty::Arc<janus::TpcCommitCommand>> commits{
      MakeTypedTpcCommitPayload(/*tx_id=*/101, /*ret=*/1, /*term=*/3,
                                "entry-one"),
      MakeTypedTpcCommitPayload(/*tx_id=*/202, /*ret=*/2, /*term=*/4,
                                "entry-two")};
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
  const auto first_log_entry = marshallable_cast<janus::LogEntry>(
      decoded_batch.unwrap()->cmds_.at(0)->cmd_);
  const auto second_log_entry = marshallable_cast<janus::LogEntry>(
      decoded_batch.unwrap()->cmds_.at(1)->cmd_);
  ASSERT_TRUE(first_log_entry.is_some());
  ASSERT_TRUE(second_log_entry.is_some());
  EXPECT_EQ(first_log_entry.unwrap()->log_entry, "entry-one");
  EXPECT_EQ(second_log_entry.unwrap()->log_entry, "entry-two");

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

TEST(MarshallableProxyFacadeTest, ReplicationLogEntryRoundTripUsesTypedAdapter) {
  janus::LogEntry log_entry;
  log_entry.length = 5;
  log_entry.log_entry = "abcde";

  const auto decoded = RoundTripTypedPayload(
      rusty::Arc<janus::LogEntry>::make(std::move(log_entry)));
  ASSERT_TRUE(decoded.is_some());
  EXPECT_EQ(decoded.unwrap()->length, 5);
  EXPECT_EQ(decoded.unwrap()->log_entry, "abcde");
}
