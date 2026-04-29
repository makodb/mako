#pragma once

#include "__dep__.h"

namespace janus {

class SimpleCommand;
class CmdData;
rrr::Marshal &operator<<(rrr::Marshal &m, const SimpleCommand &cmd);
rrr::Marshal &operator>>(rrr::Marshal &m, SimpleCommand &cmd);

// Workstream N Phase 4d-6: archive operators for SimpleCommand.
// Mirror the Marshal-based pair byte-for-byte. Used by VecPieceData's
// Serializable save/load (Phase 4d-6 migration) which serializes a
// vector of `shared_ptr<SimpleCommand>`.
rrr::BinaryWriteArchive &operator<<(rrr::BinaryWriteArchive &ar, const SimpleCommand &cmd);
rrr::BinaryReadArchive &operator>>(rrr::BinaryReadArchive &ar, SimpleCommand &cmd);

} // namespace janus
