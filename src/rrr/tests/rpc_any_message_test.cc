// `AnyMessage` envelope unit tests.
//
// Validates the open-set polymorphic envelope from `any_message.hpp`:
//   1. Pack + unpack roundtrip preserves typed payload values.
//   2. `is_a<T>()` / the public `type_name_` discriminator work.
//   3. Wrong-type unpack returns None (not abort).
//   4. Direct archive roundtrip produces an AnyMessage that decodes
//      to the same typed value.
//   5. `pack_as` with explicit name overrides the registered name.
//   6. Registration with the same name twice (under same T) is fine
//      via the static-init pattern; under different T aborts.

#include <stdint.h>
#include <stdlib.h>

#include <gtest/gtest.h>
#include <rusty/arc.hpp>
#include <rusty/option.hpp>


#include "../rrr.hpp"
#include "../misc/any_message.hpp"
#include "../misc/serializable.hpp"

import std;
import rusty;

namespace rrr {
namespace {

// Test payload: a Serializable type with a few fields. We mark it
// Serializable (not Marshallable) because that's the migration case
// AnyMessage is meant to enable — open-set Serializable types whose
// kinds are name-based, not int-based.
struct GraphPayload {
  int32_t node_count{0};
  std::string label;

  // Serializable contract.
  void save(BinaryWriteArchive& ar) const {
    rrr::Serialize_::serialize(node_count, ar);
    rrr::Serialize_::serialize(label, ar);
  }
  void load(BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(node_count, ar);
    rrr::Deserialize_::deserialize(label, ar);
  }
  int32_t kind() const { return 0; /* unused for AnyMessage path */ }
};

struct OtherPayload {
  uint64_t value{0};

  void save(BinaryWriteArchive& ar) const { rrr::Serialize_::serialize(value, ar); }
  void load(BinaryReadArchive& ar) { rrr::Deserialize_::deserialize(value, ar); }
  int32_t kind() const { return 0; }
};

// Register the payloads under stable string names. Static-init: runs
// before any test body. The test fixture below clears and re-installs
// these between tests for deterministic state.
const std::string kGraphName = "rrr.test.GraphPayload";
const std::string kOtherName = "rrr.test.OtherPayload";

// Helper: ensure both types are registered. Idempotent — calls
// `register_type` once per process via the function-local static.
void EnsureRegistered() {
  static bool initialized = []() {
    reg_any_message_as<GraphPayload>(kGraphName);
    reg_any_message_as<OtherPayload>(kOtherName);
    return true;
  }();
  (void)initialized;
}

TEST(AnyMessageTest, PackUnpackRoundTripPreservesValue) {
  EnsureRegistered();

  // Unique-owner mutation window: the Arc is not shared yet.
  auto val = rusty::Arc<GraphPayload>::make();
  val.get_mut().unwrap().node_count = 42;
  val.get_mut().unwrap().label = "hello";

  auto am = AnyMessage::pack(val);
  EXPECT_EQ(am.type_name_, kGraphName);
  EXPECT_TRUE(am.is_a<GraphPayload>());
  EXPECT_FALSE(am.is_a<OtherPayload>());

  // Aliased pack: mutations on `val` reflect in the unpacked view.
  // @unsafe { aliasing canary: proves pack shares (not copies) the payload }
  const_cast<GraphPayload*>(val.get())->node_count = 99;
  const auto recovered = am.unpack<GraphPayload>();
  ASSERT_TRUE(recovered.is_some());
  EXPECT_EQ(recovered.unwrap()->node_count, 99);
  EXPECT_EQ(recovered.unwrap()->label, "hello");
}

TEST(AnyMessageTest, UnpackWrongTypeReturnsNullptr) {
  EnsureRegistered();

  // Unique-owner mutation window: the Arc is not shared yet.
  auto val = rusty::Arc<GraphPayload>::make();
  val.get_mut().unwrap().node_count = 7;
  val.get_mut().unwrap().label = "x";

  auto am = AnyMessage::pack(val);

  // Mismatched type — must return None, not abort.
  const auto wrong = am.unpack<OtherPayload>();
  EXPECT_TRUE(wrong.is_none());
}

TEST(AnyMessageTest, RegisteredNameSpoofStillRejectsWrongHolderType) {
  EnsureRegistered();

  auto val = rusty::Arc<GraphPayload>::make();

  // The carried name makes is_a<OtherPayload>() succeed, so unpack must
  // still validate the erased holder's exact payload type before casting.
  // This is the adversarial case that the old dynamic_cast protected.
  auto am = AnyMessage::pack_as<GraphPayload>(kOtherName, val);
  EXPECT_TRUE(am.is_a<OtherPayload>());
  EXPECT_TRUE(am.unpack<OtherPayload>().is_none());
}

TEST(AnyMessageTest, IsAByName) {
  EnsureRegistered();

  auto val = rusty::Arc<GraphPayload>::make();
  auto am = AnyMessage::pack(val);

  EXPECT_TRUE(am.type_name_ == kGraphName);
  EXPECT_FALSE(am.type_name_ == "nonexistent.Type");
}

TEST(AnyMessageTest, DirectArchiveRoundTripPreservesValue) {
  EnsureRegistered();

  // Unique-owner mutation window: the Arc is not shared yet.
  auto val = rusty::Arc<GraphPayload>::make();
  val.get_mut().unwrap().node_count = 1234;
  val.get_mut().unwrap().label = "wire-trip";

  // Sender side: pack into AnyMessage, serialize via the archive.
  AnyMessage outgoing = AnyMessage::pack(val);

  BufferSink sink;
  {
    BinaryWriteArchive writer(make_sink_proxy(&sink));
    rrr::Serialize_::serialize(outgoing, writer);
  }

  // Receiver side: deserialize, recover typed payload.
  AnyMessage incoming;
  {
    BufferSource src(sink.bytes.data(), sink.bytes.len());
    BinaryReadArchive reader(make_source_proxy(&src));
    rrr::Deserialize_::deserialize(incoming, reader);
  }

  EXPECT_EQ(incoming.type_name_, kGraphName);
  EXPECT_TRUE(incoming.is_a<GraphPayload>());

  const auto recovered = incoming.unpack<GraphPayload>();
  ASSERT_TRUE(recovered.is_some());
  EXPECT_EQ(recovered.unwrap()->node_count, 1234);
  EXPECT_EQ(recovered.unwrap()->label, "wire-trip");
}

TEST(AnyMessageTest, PackAsAdHocName) {
  EnsureRegistered();

  // Explicit name path — does not require a separate registration
  // (sender side). Receiver still needs the name to be registered for
  // the wire decode path to find a factory; here we register under
  // an additional ad-hoc name to test the wire side.
  static int _reg_alias =
      reg_any_message_as<GraphPayload>("graph.alias.v1");
  (void)_reg_alias;

  // Unique-owner mutation window: the Arc is not shared yet.
  auto val = rusty::Arc<GraphPayload>::make();
  val.get_mut().unwrap().node_count = 5;
  val.get_mut().unwrap().label = "alias";

  auto am = AnyMessage::pack_as<GraphPayload>("graph.alias.v1", val);
  EXPECT_EQ(am.type_name_, "graph.alias.v1");

  // Wire roundtrip under the alias name.
  AnyMessage outgoing = am;
  BufferSink sink;
  {
    BinaryWriteArchive writer(make_sink_proxy(&sink));
    rrr::Serialize_::serialize(outgoing, writer);
  }
  AnyMessage incoming;
  {
    BufferSource src(sink.bytes.data(), sink.bytes.len());
    BinaryReadArchive reader(make_source_proxy(&src));
    rrr::Deserialize_::deserialize(incoming, reader);
  }
  EXPECT_EQ(incoming.type_name_, "graph.alias.v1");

  // is_a<GraphPayload>() resolves through the FIRST registered name
  // for GraphPayload (kGraphName). Under the alias name, is_a<T>
  // returns false because the carried type_name doesn't match the
  // (single) name_for_type lookup. This is intentional: the registry
  // tracks one canonical name per type.
  EXPECT_FALSE(incoming.is_a<GraphPayload>());
  EXPECT_TRUE(incoming.type_name_ == "graph.alias.v1");
}

TEST(AnyMessageTest, PayloadUpdatesVisibleAfterEncodeDecode) {
  EnsureRegistered();

  // Unique-owner mutation window: the Arc is not shared yet.
  auto val = rusty::Arc<OtherPayload>::make();
  val.get_mut().unwrap().value = 0xDEADBEEFCAFEBABEull;

  AnyMessage outgoing = AnyMessage::pack(val);
  BufferSink sink;
  {
    BinaryWriteArchive writer(make_sink_proxy(&sink));
    rrr::Serialize_::serialize(outgoing, writer);
  }

  AnyMessage incoming;
  {
    BufferSource src(sink.bytes.data(), sink.bytes.len());
    BinaryReadArchive reader(make_source_proxy(&src));
    rrr::Deserialize_::deserialize(incoming, reader);
  }
  const auto recovered = incoming.unpack<OtherPayload>();
  ASSERT_TRUE(recovered.is_some());
  EXPECT_EQ(recovered.unwrap()->value, 0xDEADBEEFCAFEBABEull);
}

// ---------------------------------------------------------------------------
// Serializable interface (save / load + free archive
// operators).  Lets `AnyMessage` ride an RPC struct field directly,
// without the surrounding `MarshallDeputy` wrapper.
// ---------------------------------------------------------------------------

TEST(AnyMessageTest, SerializableSaveLoadRoundTrip) {
  EnsureRegistered();

  // Unique-owner mutation window: the Arc is not shared yet.
  auto val = rusty::Arc<GraphPayload>::make();
  val.get_mut().unwrap().node_count = 7;
  val.get_mut().unwrap().label = "save/load roundtrip";

  // Build an AnyMessage and save through the BinaryWriteArchive +
  // BufferSink path (the path rpcgen-generated code uses for fields
  // typed as `AnyMessage` directly).
  AnyMessage outgoing(AnyMessage::pack(val));
  BufferSink sink;
  {
    BinaryWriteArchive ar(make_sink_proxy(&sink));
    rrr::Serialize_::serialize(outgoing, ar);
  }

  // Decode through the BinaryReadArchive + BufferSource path.
  AnyMessage incoming;
  {
    BufferSource source(sink.bytes.data(), sink.bytes.len());
    BinaryReadArchive ar(make_source_proxy(&source));
    rrr::Deserialize_::deserialize(incoming, ar);
  }

  EXPECT_EQ(incoming.type_name_, kGraphName);
  EXPECT_TRUE(incoming.is_a<GraphPayload>());
  const auto recovered = incoming.unpack<GraphPayload>();
  ASSERT_TRUE(recovered.is_some());
  EXPECT_EQ(recovered.unwrap()->node_count, 7);
  EXPECT_EQ(recovered.unwrap()->label, "save/load roundtrip");
}

// removed
// `SerializableWireOmitsLeadingKindByte` — it compared bytes between
// the direct AnyMessage path and the (now-retired) MarshallDeputy-
// wrapped path.  With AnyMessage no longer inheriting Marshallable,
// the deputy-wrapping path is gone; there's nothing to compare to.

TEST(AnyMessageTest, SerializableUnpackWrongTypeReturnsNullptr) {
  EnsureRegistered();

  // Unique-owner mutation window: the Arc is not shared yet.
  auto val = rusty::Arc<GraphPayload>::make();
  val.get_mut().unwrap().node_count = 1;

  AnyMessage outgoing(AnyMessage::pack(val));
  BufferSink sink;
  {
    BinaryWriteArchive ar(make_sink_proxy(&sink));
    rrr::Serialize_::serialize(outgoing, ar);
  }

  AnyMessage incoming;
  {
    BufferSource source(sink.bytes.data(), sink.bytes.len());
    BinaryReadArchive ar(make_source_proxy(&source));
    rrr::Deserialize_::deserialize(incoming, ar);
  }

  EXPECT_TRUE(incoming.is_a<GraphPayload>());
  EXPECT_FALSE(incoming.is_a<OtherPayload>());
  EXPECT_TRUE(incoming.unpack<OtherPayload>().is_none());
}

}  // namespace
}  // namespace rrr
