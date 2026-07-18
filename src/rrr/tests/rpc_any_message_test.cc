// `AnyMessage` envelope unit tests.
//
// Validates the open-set polymorphic envelope from `any_message.hpp`:
//   1. Pack + unpack roundtrip preserves typed payload values.
//   2. `is_a<T>()` / the public `type_name_` discriminator work.
//   3. Wrong-type unpack returns nullptr (not abort).
//   4. Direct archive roundtrip produces an AnyMessage that decodes
//      to the same typed value.
//   5. `pack_as` with explicit name overrides the registered name.
//   6. Registration with the same name twice (under same T) is fine
//      via the static-init pattern; under different T aborts.

#include <stdint.h>
#include <stdlib.h>

#include <gtest/gtest.h>


#include "../rrr.hpp"
#include "../misc/any_message.hpp"
#include "../misc/serializable.hpp"

import std;

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

  auto val = std::make_shared<GraphPayload>();
  val->node_count = 42;
  val->label = "hello";

  auto am = AnyMessage::pack(val);
  ASSERT_NE(am, nullptr);
  EXPECT_EQ(am->type_name_, kGraphName);
  EXPECT_TRUE(am->is_a<GraphPayload>());
  EXPECT_FALSE(am->is_a<OtherPayload>());

  // Aliased pack: mutations on `val` reflect in the unpacked view.
  val->node_count = 99;
  auto recovered = am->unpack<GraphPayload>();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->node_count, 99);
  EXPECT_EQ(recovered->label, "hello");
}

TEST(AnyMessageTest, UnpackWrongTypeReturnsNullptr) {
  EnsureRegistered();

  auto val = std::make_shared<GraphPayload>();
  val->node_count = 7;
  val->label = "x";

  auto am = AnyMessage::pack(val);
  ASSERT_NE(am, nullptr);

  // Mismatched type — must return nullptr, not abort.
  auto wrong = am->unpack<OtherPayload>();
  EXPECT_EQ(wrong, nullptr);
}

TEST(AnyMessageTest, IsAByName) {
  EnsureRegistered();

  auto val = std::make_shared<GraphPayload>();
  auto am = AnyMessage::pack(val);

  EXPECT_TRUE(am->type_name_ == kGraphName);
  EXPECT_FALSE(am->type_name_ == "nonexistent.Type");
}

TEST(AnyMessageTest, DirectArchiveRoundTripPreservesValue) {
  EnsureRegistered();

  auto val = std::make_shared<GraphPayload>();
  val->node_count = 1234;
  val->label = "wire-trip";

  // Sender side: pack into AnyMessage, serialize via the archive.
  AnyMessage outgoing = *AnyMessage::pack(val);

  Marshal m;
  {
    MarshalSink sink(&m);
    BinaryWriteArchive writer(make_sink_proxy(&sink));
    rrr::Serialize_::serialize(outgoing, writer);
  }

  // Receiver side: deserialize, recover typed payload.
  AnyMessage incoming;
  {
    MarshalSource src(&m);
    BinaryReadArchive reader(make_source_proxy(&src));
    rrr::Deserialize_::deserialize(incoming, reader);
  }

  EXPECT_EQ(incoming.type_name_, kGraphName);
  EXPECT_TRUE(incoming.is_a<GraphPayload>());

  auto recovered = incoming.unpack<GraphPayload>();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->node_count, 1234);
  EXPECT_EQ(recovered->label, "wire-trip");
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

  auto val = std::make_shared<GraphPayload>();
  val->node_count = 5;
  val->label = "alias";

  auto am = AnyMessage::pack_as<GraphPayload>("graph.alias.v1", val);
  ASSERT_NE(am, nullptr);
  EXPECT_EQ(am->type_name_, "graph.alias.v1");

  // Wire roundtrip under the alias name.
  AnyMessage outgoing = *am;
  Marshal m;
  {
    MarshalSink sink(&m);
    BinaryWriteArchive writer(make_sink_proxy(&sink));
    rrr::Serialize_::serialize(outgoing, writer);
  }
  AnyMessage incoming;
  {
    MarshalSource src(&m);
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

  auto val = std::make_shared<OtherPayload>();
  val->value = 0xDEADBEEFCAFEBABEull;

  AnyMessage outgoing = *AnyMessage::pack(val);
  Marshal m;
  {
    MarshalSink sink(&m);
    BinaryWriteArchive writer(make_sink_proxy(&sink));
    rrr::Serialize_::serialize(outgoing, writer);
  }

  AnyMessage incoming;
  {
    MarshalSource src(&m);
    BinaryReadArchive reader(make_source_proxy(&src));
    rrr::Deserialize_::deserialize(incoming, reader);
  }
  auto recovered = incoming.unpack<OtherPayload>();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->value, 0xDEADBEEFCAFEBABEull);
}

// ---------------------------------------------------------------------------
// Serializable interface (save / load + free archive
// operators).  Lets `AnyMessage` ride an RPC struct field directly,
// without the surrounding `MarshallDeputy` wrapper.
// ---------------------------------------------------------------------------

TEST(AnyMessageTest, SerializableSaveLoadRoundTrip) {
  EnsureRegistered();

  auto val = std::make_shared<GraphPayload>();
  val->node_count = 7;
  val->label = "save/load roundtrip";

  // Build an AnyMessage and save through the BinaryWriteArchive +
  // MarshalSink path (the path rpcgen-generated code uses for fields
  // typed as `AnyMessage` directly).
  AnyMessage outgoing(*AnyMessage::pack(val));
  Marshal m;
  {
    MarshalSink sink(&m);
    BinaryWriteArchive ar(make_sink_proxy(&sink));
    rrr::Serialize_::serialize(outgoing, ar);
  }

  // Decode through the BinaryReadArchive + MarshalSource path.
  AnyMessage incoming;
  {
    MarshalSource source(&m);
    BinaryReadArchive ar(make_source_proxy(&source));
    rrr::Deserialize_::deserialize(incoming, ar);
  }

  EXPECT_EQ(incoming.type_name_, kGraphName);
  EXPECT_TRUE(incoming.is_a<GraphPayload>());
  auto recovered = incoming.unpack<GraphPayload>();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->node_count, 7);
  EXPECT_EQ(recovered->label, "save/load roundtrip");
}

// removed
// `SerializableWireOmitsLeadingKindByte` — it compared bytes between
// the direct AnyMessage path and the (now-retired) MarshallDeputy-
// wrapped path.  With AnyMessage no longer inheriting Marshallable,
// the deputy-wrapping path is gone; there's nothing to compare to.

TEST(AnyMessageTest, SerializableUnpackWrongTypeReturnsNullptr) {
  EnsureRegistered();

  auto val = std::make_shared<GraphPayload>();
  val->node_count = 1;

  AnyMessage outgoing(*AnyMessage::pack(val));
  Marshal m;
  {
    MarshalSink sink(&m);
    BinaryWriteArchive ar(make_sink_proxy(&sink));
    rrr::Serialize_::serialize(outgoing, ar);
  }

  AnyMessage incoming;
  {
    MarshalSource source(&m);
    BinaryReadArchive ar(make_source_proxy(&source));
    rrr::Deserialize_::deserialize(incoming, ar);
  }

  EXPECT_TRUE(incoming.is_a<GraphPayload>());
  EXPECT_FALSE(incoming.is_a<OtherPayload>());
  EXPECT_EQ(incoming.unpack<OtherPayload>(), nullptr);
}

}  // namespace
}  // namespace rrr
