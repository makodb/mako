
#pragma once

#include "__dep__.h"

namespace rrr {

// Marshal-deprecation slice A: archive serde free functions own the
// mdb::Value wire format (Marshal-form mirrors deleted).
void serialize(const mdb::Value &value, BinaryWriteArchive &ar);
void deserialize(mdb::Value &value, BinaryReadArchive &ar);

// archive operators for mdb::Value. Used by Phase 4 type
// migrations (TxWorkspace, SimpleCommand) that need to write Value
// fields through BinaryWriteArchive / BinaryReadArchive.
BinaryWriteArchive& operator << (BinaryWriteArchive& ar, const mdb::Value &value);

BinaryReadArchive& operator >> (BinaryReadArchive& ar, mdb::Value &value);

} // namespace rrr

