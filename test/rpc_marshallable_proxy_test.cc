#include <gtest/gtest.h>
#include "rrr.hpp"
#include "misc/marshallable_proxy.h"

using namespace rrr;

namespace {

constexpr int32_t kTestMarshallableKind = 420042;

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

  size_t entity_size() const override {
    return sizeof(kind_) + sizeof(value);
  }

  size_t write_to_fd(int fd, size_t written) const override {
    return 0;
  }
};

void EnsureTestMarshallableInitializer() {
  static bool initialized = []() {
    MarshallDeputy::reg_initializer(
        kTestMarshallableKind,
        []() -> Marshallable* { return new TestMarshallable(); });
    return true;
  }();
  (void)initialized;
}

}  // namespace

TEST(MarshallableProxyFacadeTest, AdapterForwardsToMarshal) {
  auto sp = std::make_shared<TestMarshallable>(99);
  auto proxy = make_marshallable_proxy(sp);

  Marshal m;
  proxy->to_marshal(m);

  int32_t out_kind = 0, out_val = 0;
  m >> out_kind >> out_val;
  EXPECT_EQ(out_kind, kTestMarshallableKind);
  EXPECT_EQ(out_val, 99);
}

TEST(MarshallableProxyFacadeTest, AdapterForwardsFromMarshal) {
  auto sp = std::make_shared<TestMarshallable>();
  auto proxy = make_marshallable_proxy(sp);

  Marshal m;
  int32_t kind = 42, val = 77;
  kind = kTestMarshallableKind;
  m << kind << val;

  proxy->from_marshal(m);
  EXPECT_EQ(sp->kind_, kTestMarshallableKind);
  EXPECT_EQ(sp->value, 77);
}

TEST(MarshallableProxyFacadeTest, AdapterForwardsKind) {
  auto sp = std::make_shared<TestMarshallable>(0);
  auto proxy = make_marshallable_proxy(sp);

  EXPECT_EQ(proxy->kind(), kTestMarshallableKind);
}

TEST(MarshallableProxyFacadeTest, AdapterForwardsEntitySize) {
  auto sp = std::make_shared<TestMarshallable>(0);
  auto proxy = make_marshallable_proxy(sp);

  EXPECT_EQ(proxy->entity_size(), sizeof(int32_t) * 2);
}

TEST(MarshallableProxyFacadeTest, ProxyIsMoveOnly) {
  auto sp = std::make_shared<TestMarshallable>(5);
  auto proxy = make_marshallable_proxy(sp);

  auto moved = std::move(proxy);
  EXPECT_EQ(moved->kind(), kTestMarshallableKind);

  Marshal m;
  moved->to_marshal(m);
  int32_t k = 0, v = 0;
  m >> k >> v;
  EXPECT_EQ(v, 5);
}

TEST(MarshallableProxyFacadeTest, RoundTripThroughProxy) {
  auto src = std::make_shared<TestMarshallable>(123);
  auto src_proxy = make_marshallable_proxy(src);

  Marshal m;
  src_proxy->to_marshal(m);

  auto dst = std::make_shared<TestMarshallable>();
  auto dst_proxy = make_marshallable_proxy(dst);
  dst_proxy->from_marshal(m);

  EXPECT_EQ(dst->kind_, kTestMarshallableKind);
  EXPECT_EQ(dst->value, 123);
}

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
  EXPECT_EQ(std::dynamic_pointer_cast<TestMarshallable>(deputy.inner()), sp);
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
  auto decoded = std::dynamic_pointer_cast<TestMarshallable>(incoming.inner());
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->kind_, kTestMarshallableKind);
  EXPECT_EQ(decoded->value, 321);
}
