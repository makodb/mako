#include "replication_log_entry.h"

#include "rrr/misc/serializable.hpp"

namespace janus {

static int volatile g_reg_log_entry =
    rrr::SerializableRegistry::reg<LogEntry>(LogEntry::static_kind());

void LogEntry::save(rrr::BinaryWriteArchive& ar) const {
  rrr::Serialize_::serialize(length, ar);
  rrr::Serialize_::serialize(log_entry, ar);
}

void LogEntry::load(rrr::BinaryReadArchive& ar) {
  rrr::Deserialize_::deserialize(length, ar);
  rrr::Deserialize_::deserialize(log_entry, ar);
}

}  // namespace janus
