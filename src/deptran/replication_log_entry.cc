#include "replication_log_entry.h"

#include "srpc/misc/serializable.hpp"

namespace janus {

static int volatile g_reg_log_entry =
    srpc::SerializableRegistry::reg<LogEntry>(LogEntry::static_kind());

void LogEntry::save(srpc::BinaryWriteArchive& ar) const {
  srpc::Serialize_::serialize(length, ar);
  srpc::Serialize_::serialize(log_entry, ar);
}

void LogEntry::load(srpc::BinaryReadArchive& ar) {
  srpc::Deserialize_::deserialize(length, ar);
  srpc::Deserialize_::deserialize(log_entry, ar);
}

}  // namespace janus
