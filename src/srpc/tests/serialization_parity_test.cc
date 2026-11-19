#include <gtest/gtest.h>
#include <unistd.h>
#include <rusty/rusty.hpp>

import std;
import rusty;
import srpc.serializable;
import srpc.any_message;
import srpc.serializable_envelope;

namespace {

struct UnsupportedSerializationField {};
struct NonDefaultDeserializationField final : srpc::Deserialize {
  explicit NonDefaultDeserializationField(int);
  void deserialize(srpc::BinaryReadArchive&) override {}
};
template<class T> concept HasSerializeAdapter = requires { sizeof(srpc::SerializeAdapter<T>); };
template<class T> concept HasDeserializeAdapter = requires { sizeof(srpc::DeserializeAdapter<T>); };
static_assert(!HasSerializeAdapter<std::vector<UnsupportedSerializationField>>);
static_assert(!HasDeserializeAdapter<std::map<int32_t, UnsupportedSerializationField>>);
static_assert(!HasDeserializeAdapter<std::string_view>);
static_assert(!HasDeserializeAdapter<std::vector<NonDefaultDeserializationField>>);

template<class T>
std::vector<uint8_t> Encode(const T& value) {
  srpc::BufferSink sink;
  srpc::BinaryWriteArchive archive{srpc::make_sink_proxy_buffer(&sink)};
  srpc::Serialize_::serialize(value, archive);
  return {sink.bytes.data(), sink.bytes.data() + sink.bytes.len()};
}

template<class T>
T Decode(const std::vector<uint8_t>& bytes) {
  auto source = srpc::BufferSource::new_(bytes.data(), bytes.size());
  srpc::BinaryReadArchive archive{srpc::make_source_proxy_buffer(&source)};
  T value{};
  srpc::Deserialize_::deserialize(value, archive);
  EXPECT_EQ(source.remaining(), 0u);
  return value;
}

TEST(SerializationParity, NestedContainersMatchRustWireVector) {
  const std::vector<std::vector<int64_t>> original{{1, 2, 3}, {}, {-9001, INT64_MAX}};
  std::vector<uint8_t> expected{3, 3};
  for (int64_t value : {INT64_C(1), INT64_C(2), INT64_C(3)}) {
    const auto bytes = std::bit_cast<std::array<uint8_t, sizeof(value)>>(value);
    expected.insert(expected.end(), bytes.begin(), bytes.end());
  }
  expected.insert(expected.end(), {0, 2});
  for (int64_t value : {INT64_C(-9001), INT64_MAX}) {
    const auto bytes = std::bit_cast<std::array<uint8_t, sizeof(value)>>(value);
    expected.insert(expected.end(), bytes.begin(), bytes.end());
  }
  EXPECT_EQ(Encode(original), expected);
  EXPECT_EQ(Decode<std::vector<int64_t>>(Encode(original.front())), original.front());
  EXPECT_EQ(Decode<std::vector<std::vector<int64_t>>>(expected), original);
  static_assert(sizeof(srpc::SerializeAdapter<std::vector<int64_t>>) > 0);
  static_assert(sizeof(srpc::DeserializeAdapter<std::vector<int64_t>>) > 0);
}

TEST(SerializationParity, ImportedStlAdaptersSupportErasedDispatch) {
  const std::vector<std::string> original{"alpha", std::string{'x', char(0), 'y'}};
  srpc::BufferSink sink;
  srpc::BinaryWriteArchive output{srpc::make_sink_proxy_buffer(&sink)};
  const srpc::SerializeAdapterRef<std::vector<std::string>> writer(original);
  static_cast<const srpc::Serialize&>(writer).serialize(output);
  auto source = srpc::BufferSource::new_(sink.bytes.data(), sink.bytes.len());
  srpc::BinaryReadArchive input{srpc::make_source_proxy_buffer(&source)};
  std::vector<std::string> restored;
  srpc::DeserializeAdapterRefMut<std::vector<std::string>> reader(restored);
  static_cast<srpc::Deserialize&>(reader).deserialize(input);
  EXPECT_EQ(restored, original);
  EXPECT_EQ(source.remaining(), 0u);
}

TEST(SerializationParity, OrderedAndUnorderedContainersPreserveKeys) {
  const std::set<int32_t> ordered_set{3, 1, 3};
  EXPECT_EQ(Decode<std::set<int32_t>>(Encode(ordered_set)), ordered_set);
  const std::map<int32_t, std::vector<int32_t>> ordered_map{{3, {9, 8}}, {1, {5}}};
  EXPECT_EQ((Decode<std::map<int32_t, std::vector<int32_t>>>(Encode(ordered_map))), ordered_map);
  const std::unordered_set<int32_t> unordered_set{3, 3, 1};
  EXPECT_EQ(Decode<std::unordered_set<int32_t>>(Encode(unordered_set)), unordered_set);
  const std::unordered_map<int32_t, std::string> unordered_map{{3, "three"}, {1, "one"}};
  EXPECT_EQ((Decode<std::unordered_map<int32_t, std::string>>(Encode(unordered_map))), unordered_map);
  const std::list<int32_t> sequence{3, 3, 1};
  EXPECT_EQ(Decode<std::list<int32_t>>(Encode(sequence)), sequence);
}

TEST(SerializationParity, RawCppStringsPreserveInvalidUtf8AndNul) {
  const std::string original{char(0xff), char(0), char(0xc0), char(0x80), 'a'};
  const std::vector<uint8_t> expected{5, 0xff, 0, 0xc0, 0x80, 'a'};
  EXPECT_EQ(Encode(original), expected);
  EXPECT_EQ(Encode(std::string_view(original)), expected);
  EXPECT_EQ(Decode<std::string>(expected), original);
  EXPECT_EQ(Decode<std::string>(Encode(std::string{})), std::string{});
}

TEST(SerializationParity, ConcreteStringAdaptersKeepErasedDispatch) {
  std::string original{char(0xff), char(0), 'a'};
  std::string_view view(original);
  const std::vector<uint8_t> expected{3, 0xff, 0, 'a'};
  const auto check_writer = [&](const srpc::Serialize& writer) {
    srpc::BufferSink sink;
    srpc::BinaryWriteArchive output{srpc::make_sink_proxy_buffer(&sink)};
    writer.serialize(output);
    EXPECT_EQ((std::vector<uint8_t>{sink.bytes.data(), sink.bytes.data() + sink.bytes.len()}), expected);
  };
  srpc::SerializeAdapter<std::string> owned_string(original);
  srpc::SerializeAdapter<std::string> moved_string(std::move(owned_string));
  check_writer(moved_string);
  check_writer(srpc::SerializeAdapterRef<std::string>(original));
  check_writer(srpc::SerializeAdapterRefMut<std::string>(original));
  srpc::SerializeAdapter<std::string_view> owned_view(view);
  srpc::SerializeAdapter<std::string_view> moved_view(std::move(owned_view));
  check_writer(moved_view);
  check_writer(srpc::SerializeAdapterRef<std::string_view>(view));
  check_writer(srpc::SerializeAdapterRefMut<std::string_view>(view));

  const auto check_reader = [&](srpc::Deserialize& reader) {
    auto source = srpc::BufferSource::new_(expected.data(), expected.size());
    srpc::BinaryReadArchive input{srpc::make_source_proxy_buffer(&source)};
    reader.deserialize(input);
    EXPECT_EQ(source.remaining(), 0u);
  };
  srpc::DeserializeAdapter<std::string> owned_reader(std::string{});
  srpc::DeserializeAdapter<std::string> moved_reader(std::move(owned_reader));
  check_reader(moved_reader);
  std::string restored;
  srpc::DeserializeAdapterRefMut<std::string> reader(restored);
  check_reader(reader);
  EXPECT_EQ(restored, original);
}

TEST(SerializationParity, BorrowedFdProxiesForwardToCallerOwnedDescriptors) {
  int descriptors[2];
  ASSERT_EQ(::pipe(descriptors), 0);
  {
    srpc::FdSink sink{descriptors[1]};
    srpc::BinaryWriteArchive output{srpc::make_sink_proxy_fd(&sink)};
    srpc::Serialize_::serialize(int32_t{73}, output);
    srpc::FdSource source{descriptors[0]};
    srpc::BinaryReadArchive input{srpc::make_source_proxy_fd(&source)};
    int32_t restored = 0;
    srpc::Deserialize_::deserialize(restored, input);
    EXPECT_EQ(restored, 73);
  }
  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

TEST(SerializationParity, ImportedPairFieldsKeepTupleWireOrder) {
  const std::pair<int32_t, std::string> original{17, "pair"};
  auto expected = Encode(original.first);
  const auto second = Encode(original.second);
  expected.insert(expected.end(), second.begin(), second.end());
  EXPECT_EQ(Encode(original), expected);
  EXPECT_EQ((Decode<std::pair<int32_t, std::string>>(expected)), original);
  static_assert(sizeof(srpc::SerializeAdapter<std::pair<int32_t, std::string>>) > 0);
  static_assert(sizeof(srpc::DeserializeAdapter<std::pair<int32_t, std::string>>) > 0);
}

TEST(SerializationParity, MapsKeepFirstDuplicateWireKey) {
  const std::vector<std::pair<int32_t, int32_t>> entries{{3, 30}, {1, 10}, {3, 99}};
  const auto bytes = Encode(entries);
  const std::map<int32_t, int32_t> expected{{1, 10}, {3, 30}};
  EXPECT_EQ((Decode<std::map<int32_t, int32_t>>(bytes)), expected);
  const std::unordered_map<int32_t, int32_t> unordered{{1, 10}, {3, 30}};
  EXPECT_EQ((Decode<std::unordered_map<int32_t, int32_t>>(bytes)), unordered);
}

struct Payload {
  int64_t value = 0;
  std::vector<int32_t> values;
  void save(srpc::BinaryWriteArchive& archive) const {
    srpc::Serialize_::serialize(value, archive);
    srpc::Serialize_::serialize(values, archive);
  }
  void load(srpc::BinaryReadArchive& archive) {
    srpc::Deserialize_::deserialize(value, archive);
    srpc::Deserialize_::deserialize(values, archive);
  }
  int32_t kind() const { return 61; }
};

struct OtherPayload {
  int64_t value = 0;
  void save(srpc::BinaryWriteArchive& archive) const { srpc::Serialize_::serialize(value, archive); }
  void load(srpc::BinaryReadArchive& archive) { srpc::Deserialize_::deserialize(value, archive); }
  int32_t kind() const { return 62; }
};
struct EnvelopePayloadSet {};

}  // namespace

namespace srpc {
template<> struct PayloadMember<EnvelopePayloadSet, Payload> {
  static constexpr bool value = true;
  static constexpr int32_t KIND = 61;
};
template<> struct PayloadMember<EnvelopePayloadSet, OtherPayload> {
  static constexpr bool value = true;
  static constexpr int32_t KIND = 62;
};
}  // namespace srpc

namespace {

TEST(SerializationParity, AnyMessageRoundTripPreservesValuesAndChecksHolderType) {
  const std::string name = "serialization.parity.payload";
  const std::string other_name = "serialization.parity.other";
  srpc::reg_any_message_as<Payload>(name);
  srpc::reg_any_message_as<OtherPayload>(other_name);
  auto value = rusty::Arc<Payload>::make();
  value.get_mut().unwrap().value = -9001;
  value.get_mut().unwrap().values = {2, 3, 5};
  auto original = srpc::AnyMessage::pack(value);
  ASSERT_TRUE(original.unpack<Payload>().is_some());
  EXPECT_EQ(original.unpack<Payload>().unwrap().get(), value.get());
  EXPECT_TRUE(original.unpack<OtherPayload>().is_none());
  auto restored = Decode<srpc::AnyMessage>(Encode(original));
  ASSERT_TRUE(restored.unpack<Payload>().is_some());
  EXPECT_NE(restored.unpack<Payload>().unwrap().get(), value.get());
  EXPECT_EQ(restored.unpack<Payload>().unwrap()->value, value->value);
  EXPECT_EQ(restored.unpack<Payload>().unwrap()->values, value->values);
  EXPECT_EQ(restored.type_name_, name);

  auto spoof = srpc::AnyMessage::pack_as<Payload>(other_name, value);
  EXPECT_TRUE(spoof.is_a<OtherPayload>());
  EXPECT_TRUE(spoof.unpack<OtherPayload>().is_none());
}

TEST(SerializationParity, ClosedEnvelopeRoundTripPreservesKindValuesAndIdentity) {
  using Envelope = srpc::SerializableEnvelope<EnvelopePayloadSet>;
  auto empty = Envelope::default_();
  EXPECT_FALSE(empty.has_value());
  EXPECT_TRUE(empty.unpack_shared<Payload>().is_none());
  srpc::SerializableRegistry::clear_for_testing();
  srpc::SerializableRegistry::reg<Payload>(61);
  auto value = rusty::Arc<Payload>::make();
  value.get_mut().unwrap().value = -9001;
  value.get_mut().unwrap().values = {2, 3, 5};
  auto original = Envelope::pack_aliased(value);
  EXPECT_EQ(original.kind(), 61);
  EXPECT_EQ(original.unpack_shared<Payload>().unwrap().get(), value.get());
  EXPECT_EQ(original.unpack<OtherPayload>(), nullptr);
  auto restored = Decode<Envelope>(Encode(original));
  EXPECT_EQ(restored.kind(), 61);
  ASSERT_TRUE(restored.unpack_shared<Payload>().is_some());
  EXPECT_NE(restored.unpack_shared<Payload>().unwrap().get(), value.get());
  EXPECT_EQ(restored.unpack<Payload>()->value, value->value);
  EXPECT_EQ(restored.unpack<Payload>()->values, value->values);
  EXPECT_TRUE(restored.unpack_shared<OtherPayload>().is_none());
  srpc::SerializableRegistry::clear_for_testing();
}

TEST(SerializationParity, HolderAndMutableFactoryPreservePayload) {
  Payload original{-9001, {2, 3, 5}};
  auto proxy = srpc::make_serializable_proxy_copy(original);
  EXPECT_EQ(proxy->kind(), 61);
  auto holder = srpc::serializable_holder_of<Payload>(proxy.get());
  ASSERT_NE(holder, nullptr);
  EXPECT_EQ(holder->ptr->value, original.value);
  EXPECT_EQ(holder->ptr->values, original.values);
  EXPECT_EQ(srpc::serializable_holder_of<int64_t>(proxy.get()), nullptr);

  srpc::SerializableRegistry::clear_for_testing();
  srpc::SerializableRegistry::reg<Payload>(61);
  auto fresh = srpc::SerializableRegistry::create(61);
  srpc::BufferSink sink;
  srpc::BinaryWriteArchive writer{srpc::make_sink_proxy_buffer(&sink)};
  proxy->save(writer);
  auto source = srpc::BufferSource::new_(sink.bytes.data(), sink.bytes.len());
  srpc::BinaryReadArchive reader{srpc::make_source_proxy_buffer(&source)};
  fresh.get_mut().unwrap().load(reader);
  auto restored = srpc::serializable_holder_of<Payload>(fresh.get());
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->ptr->value, original.value);
  EXPECT_EQ(restored->ptr->values, original.values);
  EXPECT_EQ(source.remaining(), 0u);

  srpc::serializable_registry_register_factory(62,
      srpc::SerializableRegistryFactory::from_callable([calls = 0]() mutable {
        auto value = rusty::Arc<Payload>::make();
        value.get_mut().unwrap().value = ++calls;
        return srpc::make_serializable_proxy(std::move(value));
      }));
  for (int64_t expected = 1; expected <= 2; ++expected) {
    auto value = srpc::SerializableRegistry::create(62);
    auto stored = srpc::serializable_holder_of<Payload>(value.get());
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(stored->ptr->value, expected);
  }
  srpc::SerializableRegistry::clear_for_testing();
}

struct WeakFactoryPayload {
  int32_t value = 7;
  void save(srpc::BinaryWriteArchive&) const {}
  void load(srpc::BinaryReadArchive&) { value = 99; }
  int32_t kind() const { return 196601; }
};

TEST(SerializationParity, FactoryWithWeakPayloadRejectsMutableLoad) {
  auto retained = std::make_shared<rusty::sync::Weak<WeakFactoryPayload>>();
  srpc::serializable_registry_register_factory(196601,
      srpc::SerializableRegistryFactory::from_callable([retained] {
        auto payload = rusty::Arc<WeakFactoryPayload>::make();
        *retained = rusty::downgrade(payload);
        return srpc::make_serializable_proxy(std::move(payload));
      }));
  auto proxy = srpc::SerializableRegistry::create(196601);
  auto source = srpc::BufferSource::new_(nullptr, 0);
  srpc::BinaryReadArchive archive{srpc::make_source_proxy_buffer(&source)};
  bool rejected = false;
  try {
    proxy.get_mut().unwrap().load(archive);
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "Called unwrap on None");
    rejected = true;
  }
  ASSERT_TRUE(rejected);
  ASSERT_TRUE(retained->upgrade().is_some());
  EXPECT_EQ(retained->upgrade().unwrap()->value, 7);
  retained->reset();
  proxy.get_mut().unwrap().load(archive);
  auto holder = srpc::serializable_holder_of<WeakFactoryPayload>(proxy.get());
  ASSERT_NE(holder, nullptr);
  EXPECT_EQ(holder->ptr->value, 99);
  srpc::SerializableRegistry::clear_for_testing();
}

}  // namespace
