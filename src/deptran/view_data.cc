#include "view_data.h"

namespace janus {

void EnsureViewDataRegistered() {
  static const int registered =
      rrr::SerializableRegistry::reg<ViewData>(ViewData::static_kind());
  (void)registered;
}

namespace {

const int register_view_data_at_startup = (EnsureViewDataRegistered(), 0);

}  // namespace
}  // namespace janus
