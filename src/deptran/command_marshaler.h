#pragma once

#include "__dep__.h"

namespace janus {

class SimpleCommand;
class CmdData;
// Marshal-deprecation slice A: archive serde free functions own the
// SimpleCommand wire format (Marshal-form mirrors deleted, zero callers).
void serialize(const SimpleCommand &cmd, srpc::BinaryWriteArchive &ar);
void deserialize(SimpleCommand &cmd, srpc::BinaryReadArchive &ar);

// archive operators for SimpleCommand. Used by VecPieceData's
// Serializable save/load which serializes a
// vector of `shared_ptr<SimpleCommand>`.
srpc::BinaryWriteArchive &operator<<(srpc::BinaryWriteArchive &ar, const SimpleCommand &cmd);
srpc::BinaryReadArchive &operator>>(srpc::BinaryReadArchive &ar, SimpleCommand &cmd);

} // namespace janus
