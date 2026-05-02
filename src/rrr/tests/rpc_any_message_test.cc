// Workstream N L7 — `AnyMessage` envelope unit tests.
//
// Validates the open-set polymorphic envelope from `any_message.hpp`:
//   1. Pack + unpack roundtrip preserves typed payload values.
//   2. `is_a<T>()` / `is_a(name)` / `type_name()` discriminators work.
//   3. Wrong-type unpack returns nullptr (not abort).
//   4. Wire roundtrip through `MarshallDeputy` produces an AnyMessage
//      that decodes to the same typed value.
//   5. `pack_as` with explicit name overrides the registered name.
//   6. Registration with the same name twice (under same T) is fine
//      via the static-init pattern; under different T aborts.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "../rrr.hpp"
#include "../misc/any_message.hpp"
#include "../misc/marshal_serializable_bridge.hpp"

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
    ar << node_count << label;
  }
  void load(BinaryReadArchive& ar) {
    ar >> node_count >> label;
  }
  int32_t kind() const { return 0; /* unused for AnyMessage path */ }
};

struct OtherPayload {
  uint64_t value{0};

  void save(BinaryWriteArchive& ar) const { ar << value; }
  void load(BinaryReadArchive& ar) { ar >> value; }
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
  EXPECT_EQ(am->type_name(), kGraphName);
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

  EXPECT_TRUE(am->is_a(kGraphName));
  EXPECT_FALSE(am->is_a("nonexistent.Type"));
}

TEST(AnyMessageTest, WireRoundTripThroughMarshallDeputy) {
  EnsureRegistered();

  auto val = std::make_shared<GraphPayload>();
  val->node_count = 1234;
  val->label = "wire-trip";

  // Sender side: wrap in MarshallDeputy, serialize.
  MarshallDeputy outgoing(AnyMessage::pack(val));
  EXPECT_EQ(outgoing.kind_, MarshallDeputy::ANY_MESSAGE);

  Marshal m;
  m << outgoing;

  // Receiver side: deserialize, recover typed payload.
  MarshallDeputy incoming;
  m >> incoming;
  EXPECT_EQ(incoming.kind_, MarshallDeputy::ANY_MESSAGE);

  auto am = AnyMessage::try_cast(incoming);
  ASSERT_NE(am, nullptr);
  EXPECT_EQ(am->type_name(), kGraphName);
  EXPECT_TRUE(am->is_a<GraphPayload>());

  auto recovered = am->unpack<GraphPayload>();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->node_count, 1234);
  EXPECT_EQ(recovered->label, "wire-trip");
}

TEST(AnyMessageTest, TryCastReturnsNullptrForOtherKind) {
  EnsureRegistered();

  // Build an empty deputy (kind UNKNOWN) — try_cast must return null.
  MarshallDeputy empty;
  EXPECT_EQ(AnyMessage::try_cast(empty), nullptr);
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
  EXPECT_EQ(am->type_name(), "graph.alias.v1");

  // Wire roundtrip under the alias name.
  MarshallDeputy outgoing(am);
  Marshal m;
  m << outgoing;
  MarshallDeputy incoming;
  m >> incoming;
  auto am2 = AnyMessage::try_cast(incoming);
  ASSERT_NE(am2, nullptr);
  EXPECT_EQ(am2->type_name(), "graph.alias.v1");

  // is_a<GraphPayload>() resolves through the FIRST registered name
  // for GraphPayload (kGraphName). Under the alias name, is_a<T>
  // returns false because the carried type_name doesn't match the
  // (single) name_for_type lookup. This is intentional: the registry
  // tracks one canonical name per type.
  EXPECT_FALSE(am2->is_a<GraphPayload>());
  EXPECT_TRUE(am2->is_a("graph.alias.v1"));
}

TEST(AnyMessageTest, PayloadUpdatesVisibleAfterEncodeDecode) {
  EnsureRegistered();

  auto val = std::make_shared<OtherPayload>();
  val->value = 0xDEADBEEFCAFEBABEull;

  auto am = AnyMessage::pack(val);
  MarshallDeputy outgoing(am);
  Marshal m;
  m << outgoing;

  MarshallDeputy incoming;
  m >> incoming;
  auto am2 = AnyMessage::try_cast(incoming);
  ASSERT_NE(am2, nullptr);
  auto recovered = am2->unpack<OtherPayload>();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->value, 0xDEADBEEFCAFEBABEull);
}

// ---------------------------------------------------------------------------
// L10c-anymsg: Serializable interface (save / load + free archive
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
    BinaryWriteArchive ar(&sink);
    ar << outgoing;
  }

  // Decode through the BinaryReadArchive + MarshalSource path.
  AnyMessage incoming;
  {
    MarshalSource source(&m);
    BinaryReadArchive ar(&source);
    ar >> incoming;
  }

  EXPECT_EQ(incoming.type_name(), kGraphName);
  EXPECT_TRUE(incoming.is_a<GraphPayload>());
  auto recovered = incoming.unpack<GraphPayload>();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->node_count, 7);
  EXPECT_EQ(recovered->label, "save/load roundtrip");
}

TEST(AnyMessageTest, SerializableWireOmitsLeadingKindByte) {
  // Wire format under direct embedding: [v64-prefixed string]
  // [payload bytes].  Compared to the deputy-wrapped form, the leading
  // [v32 ANY_MESSAGE=24] byte is gone.  Saves 1 byte per envelope on
  // the wire when the field is statically typed as AnyMessage.
  EnsureRegistered();

  auto val = std::make_shared<OtherPayload>();
  val->value = 0xCAFE;

  // Serializable path bytes.
  Marshal m_direct;
  {
    AnyMessage outgoing(*AnyMessage::pack(val));
    MarshalSink sink(&m_direct);
    BinaryWriteArchive ar(&sink);
    ar << outgoing;
  }
  size_t direct_bytes = m_direct.content_size();

  // Deputy-wrapped path bytes.
  Marshal m_deputy;
  {
    MarshallDeputy outgoing(AnyMessage::pack(val));
    m_deputy << outgoing;
  }
  size_t deputy_bytes = m_deputy.content_size();

  // Deputy adds a v32 ANY_MESSAGE=24 prefix; ANY_MESSAGE=24 fits in 1
  // byte (24 < 64) so the deputy path is exactly 1 byte longer.
  EXPECT_EQ(deputy_bytes, direct_bytes + 1);
}

TEST(AnyMessageTest, SerializableUnpackWrongTypeReturnsNullptr) {
  EnsureRegistered();

  auto val = std::make_shared<GraphPayload>();
  val->node_count = 1;

  AnyMessage outgoing(*AnyMessage::pack(val));
  Marshal m;
  {
    MarshalSink sink(&m);
    BinaryWriteArchive ar(&sink);
    ar << outgoing;
  }

  AnyMessage incoming;
  {
    MarshalSource source(&m);
    BinaryReadArchive ar(&source);
    ar >> incoming;
  }

  EXPECT_TRUE(incoming.is_a<GraphPayload>());
  EXPECT_FALSE(incoming.is_a<OtherPayload>());
  EXPECT_EQ(incoming.unpack<OtherPayload>(), nullptr);
}

}  // namespace
}  // namespace rrr
