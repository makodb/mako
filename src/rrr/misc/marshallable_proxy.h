#pragma once

// Forward declarations - actual types come from marshal.hpp
// This breaks the circular dependency between marshallable_proxy.h and marshal.hpp
namespace rrr {
class Marshal;
class Marshallable;
}  // namespace rrr

#ifdef RR
#pragma push_macro("RR")
#undef RR
#define RRR_RESTORE_RR_MACRO 1
#endif
#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>
#ifdef RRR_RESTORE_RR_MACRO
#pragma pop_macro("RR")
#undef RRR_RESTORE_RR_MACRO
#endif

namespace rrr {

PRO_DEF_MEM_DISPATCH(MarshallableMemToMarshal, to_marshal);
PRO_DEF_MEM_DISPATCH(MarshallableMemFromMarshal, from_marshal);
PRO_DEF_MEM_DISPATCH(MarshallableMemEntitySize, entity_size);
PRO_DEF_MEM_DISPATCH(MarshallableMemWriteToFd, write_to_fd);
PRO_DEF_MEM_DISPATCH(MarshallableMemKind, kind);

struct MarshallableFacade : pro::facade_builder
    ::add_convention<MarshallableMemToMarshal, Marshal&(Marshal&) const>
    ::add_convention<MarshallableMemFromMarshal, Marshal&(Marshal&)>
    ::add_convention<MarshallableMemEntitySize, size_t() const>
    ::add_convention<MarshallableMemWriteToFd, size_t(int, size_t) const>
    ::add_convention<MarshallableMemKind, int32_t() const>
    ::build {};

using MarshallableProxy = pro::proxy<MarshallableFacade>;

class MarshallableSharedPtrAdapter {
 public:
  explicit MarshallableSharedPtrAdapter(std::shared_ptr<Marshallable> m)
      : m_(std::move(m)) {}

  Marshal& to_marshal(Marshal& out) const { return m_->to_marshal(out); }
  Marshal& from_marshal(Marshal& in) { return m_->from_marshal(in); }
  size_t entity_size() const { return m_->entity_size(); }
  size_t write_to_fd(int fd, size_t written) const {
    return m_->write_to_fd(fd, written);
  }
  int32_t kind() const { return m_->kind(); }

  std::shared_ptr<Marshallable> inner() const { return m_; }

 private:
  std::shared_ptr<Marshallable> m_;
};

inline MarshallableProxy make_marshallable_proxy(
    std::shared_ptr<Marshallable> m) {
  return pro::make_proxy<MarshallableFacade>(
      MarshallableSharedPtrAdapter(std::move(m)));
}

}  // namespace rrr
