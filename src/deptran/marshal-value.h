
#pragma once

#include "__dep__.h"

namespace rrr {

Marshal& operator << (Marshal& m, const mdb::Value &value);

Marshal& operator >> (Marshal& m, mdb::Value &value);

// archive operators for mdb::Value, mirroring
// the Marshal-based ones byte-for-byte. Used by Phase 4 type
// migrations (TxWorkspace, SimpleCommand) that need to write Value
// fields through BinaryWriteArchive / BinaryReadArchive.
BinaryWriteArchive& operator << (BinaryWriteArchive& ar, const mdb::Value &value);

BinaryReadArchive& operator >> (BinaryReadArchive& ar, mdb::Value &value);

} // namespace rrr

