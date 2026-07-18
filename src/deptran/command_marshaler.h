#pragma once

#include "__dep__.h"

namespace janus {

class SimpleCommand;
class CmdData;
// Phase 8 batch 4: serde free functions own the SimpleCommand wire format;
// the operators below are forwarders kept until the operator layer is
// deleted.
void serialize(const SimpleCommand &cmd, rrr::Marshal &m);
void deserialize(SimpleCommand &cmd, rrr::Marshal &m);
void serialize(const SimpleCommand &cmd, rrr::BinaryWriteArchive &ar);
void deserialize(SimpleCommand &cmd, rrr::BinaryReadArchive &ar);

rrr::Marshal &operator<<(rrr::Marshal &m, const SimpleCommand &cmd);
rrr::Marshal &operator>>(rrr::Marshal &m, SimpleCommand &cmd);

// archive operators for SimpleCommand.
// Mirror the Marshal-based pair byte-for-byte. Used by VecPieceData's
// Serializable save/load which serializes a
// vector of `shared_ptr<SimpleCommand>`.
rrr::BinaryWriteArchive &operator<<(rrr::BinaryWriteArchive &ar, const SimpleCommand &cmd);
rrr::BinaryReadArchive &operator>>(rrr::BinaryReadArchive &ar, SimpleCommand &cmd);

} // namespace janus
