#pragma once

#include <string>

#include "mako_commands.h"
#include "srpc/misc/serializable.hpp"

namespace janus {

// Raw application bytes carried by the replication protocols.
//
// The wire format is intentionally unchanged from the former definition in
// paxos_worker.h:
//   int length
//   std::string log_entry
//
// Keeping this payload independent of Paxos lets Raft use the raw-byte command
// without pulling the legacy transaction/storage headers into its live log
// path.
class LogEntry
    : public srpc::Serializable<
          srpc::PayloadMember<MakoCommands, LogEntry>::KIND> {
 public:
  int length = 0;
  std::string log_entry;

  LogEntry() = default;

  void save(srpc::BinaryWriteArchive& ar) const;
  void load(srpc::BinaryReadArchive& ar);
};

}  // namespace janus
