#pragma once

#include <string>

#include "mako_commands.h"
#include "rrr/misc/serializable.hpp"

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
    : public rrr::Serializable<
          rrr::PayloadMember<MakoCommands, LogEntry>::KIND> {
 public:
  int length = 0;
  std::string log_entry;

  LogEntry() = default;

  void save(rrr::BinaryWriteArchive& ar) const;
  void load(rrr::BinaryReadArchive& ar);
};

}  // namespace janus
