#include <memory>
#include <string>

#include "gtest/gtest.h"

#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>

namespace {

static_assert(__cplusplus >= 202002L,
              "ngcpp/proxy requires compiling with at least C++20");

PRO_DEF_MEM_DISPATCH(MemLabel, Label);
PRO_DEF_MEM_DISPATCH(MemValue, Value);

struct ProxyDependencyFacade : pro::facade_builder
    ::add_convention<MemLabel, const std::string&() const>
    ::add_convention<MemValue, int() const>
    ::build {};

class ProxyDependencyExample {
 public:
  ProxyDependencyExample(std::string label, int value)
      : label_(std::move(label)), value_(value) {}

  const std::string& Label() const { return label_; }
  int Value() const { return value_; }

 private:
  std::string label_;
  int value_;
};

TEST(RpcProxyDependencyTest, FacadeDispatchesToValueObject) {
  pro::proxy<ProxyDependencyFacade> object =
      pro::make_proxy<ProxyDependencyFacade, ProxyDependencyExample>("bootstrap", 7);

  EXPECT_EQ(object->Label(), "bootstrap");
  EXPECT_EQ(object->Value(), 7);
}

TEST(RpcProxyDependencyTest, FacadeDispatchesToUniquePtrObject) {
  auto impl = std::make_unique<ProxyDependencyExample>("ownership", 11);
  pro::proxy<ProxyDependencyFacade> object = std::move(impl);

  EXPECT_EQ(object->Label(), "ownership");
  EXPECT_EQ(object->Value(), 11);
}

}  // namespace
