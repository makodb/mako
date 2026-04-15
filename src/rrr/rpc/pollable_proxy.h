#pragma once

#include "reactor/epoll_wrapper.h"

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

// Proxy facade definition for incremental Pollable migration.
PRO_DEF_MEM_DISPATCH(PollableMemFd, fd);
PRO_DEF_MEM_DISPATCH(PollableMemPollMode, poll_mode);
PRO_DEF_MEM_DISPATCH(PollableMemContentSize, content_size);
PRO_DEF_MEM_DISPATCH(PollableMemHandleRead, handle_read);
PRO_DEF_MEM_DISPATCH(PollableMemHandleWrite, handle_write);
PRO_DEF_MEM_DISPATCH(PollableMemHandleError, handle_error);
PRO_DEF_MEM_DISPATCH(PollableMemClose, close);
PRO_DEF_MEM_DISPATCH(PollableMemCheckPendingWriteUpdate, check_pending_write_update);
PRO_DEF_MEM_DISPATCH(PollableMemIsClosed, is_closed);

struct PollableFacade : pro::facade_builder
    ::add_convention<PollableMemFd, int() const>
    ::add_convention<PollableMemPollMode, int() const>
    ::add_convention<PollableMemContentSize, size_t()>
    ::add_convention<PollableMemHandleRead, bool()>
    ::add_convention<PollableMemHandleWrite, int()>
    ::add_convention<PollableMemHandleError, void()>
    ::add_convention<PollableMemClose, void()>
    ::add_convention<PollableMemCheckPendingWriteUpdate, bool() const>
    ::add_convention<PollableMemIsClosed, bool() const>
    ::build {};

using PollableProxy = pro::proxy<PollableFacade>;

// Compatibility bridge for incremental migration leaves.
class PollableArcAdapter {
 public:
  explicit PollableArcAdapter(rusty::Arc<Pollable> poll) : poll_(std::move(poll)) {}

  int fd() const { return poll_->fd(); }
  int poll_mode() const { return poll_->poll_mode(); }
  size_t content_size() { return mut_poll().content_size(); }
  bool handle_read() { return mut_poll().handle_read(); }
  int handle_write() { return mut_poll().handle_write(); }
  void handle_error() { mut_poll().handle_error(); }
  void close() { mut_poll().close(); }
  bool check_pending_write_update() const { return poll_->check_pending_write_update(); }
  bool is_closed() const { return poll_->is_closed(); }

 private:
  Pollable& mut_poll() { return const_cast<Pollable&>(*poll_.get()); }
  rusty::Arc<Pollable> poll_;
};

inline PollableProxy make_pollable_proxy_from_arc(rusty::Arc<Pollable> poll) {
  return pro::make_proxy<PollableFacade, PollableArcAdapter>(std::move(poll));
}

}  // namespace rrr
