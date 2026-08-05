#pragma once

// central `MakoCommands` TypeList.
//
// Closes the closed-set half of the dual-envelope architecture
// (counterpart to the open-set `rrr::AnyMessage` envelope from L7):
// every polymorphic Command type that flows through `MarshallDeputy`
// as a closed-set Serializable is listed here, and its wire kind is
// the 1-indexed position of its forward declaration below.
//
// Mirrors Rust's `enum Foo { A(...), B(...), ... }` + bincode pattern,
// where each variant's wire discriminant is the declaration-order
// position in the enum.  The protobuf analogue is `oneof` (closed set,
// field-numbered).
//
// Adding a new closed-set Command type:
//   1. Add a forward declaration below.
//   2. Append the type name as a new entry at the END of `MakoCommands`
//      (appending preserves existing types' kind values; reordering
//      or inserting is a wire-format break).
//   3. Define `class T : public rrr::Serializable<T, janus::MakoCommands>`.
//   4. Add a registration line in T's .cc:
//      `static int volatile g_reg_T = rrr::reg_serializable_in_deputy<T>();`
//
// For OPEN-set polymorphic types (graph payloads, anything where the
// receiver may not know about every possible carried type at compile
// time), use `rrr::AnyMessage` instead — see `rrr/misc/any_message.hpp`.

#include <rusty/arc.hpp>

#include "rrr/misc/serializable.hpp"
#include "rrr/misc/serializable_envelope.hpp"

namespace janus {

// Forward declarations.  The TypeList only needs the type identity;
// concrete bodies are picked up later via #include of the declaring
// header in each Command type's translation unit.
class LogEntry;
class TpcPrepareCommand;
class TpcCommitCommand;
class VecPieceData;
class BulkPaxosCmd;
class BulkPrepareLog;
class HeartBeatLog;
class SyncLogRequest;
class SyncLogResponse;
class SyncNoOpRequest;
class PaxosPrepCmd;
class TpcEmptyCommand;
class TpcNoopCommand;
class TpcBatchCommand;
class VecRecData;
class ViewData;
class SimpleRWCommand;
class KeyCmdBatchData;
class ReplicatedDBCommand;

// `MakoCommands` — the closed-set polymorphic-payload list.  Position
// in this list = wire kind value (1-indexed; 0 is reserved for
// MarshallDeputy::UNKNOWN).
//
// Ordering roughly preserves the prior `MarshallDeputy::Kind` enum
// order for ease of reasoning during the L8 migration; reordering
// after this point is a wire-format break.
using MakoCommands = rrr::TypeList<
    LogEntry,             // pos  1 (was CONTAINER_CMD=3)
    TpcPrepareCommand,    // pos  2 (was CMD_TPC_PREPARE=4)
    TpcCommitCommand,     // pos  3 (was CMD_TPC_COMMIT=5)
    VecPieceData,         // pos  4 (was CMD_VEC_PIECE=6)
    BulkPaxosCmd,         // pos  5 (was CMD_BLK_PXS=7)
    BulkPrepareLog,       // pos  6 (was CMD_BLK_PREP_PXS=8)
    HeartBeatLog,         // pos  7 (was CMD_HRTBT_PXS=9)
    SyncLogRequest,       // pos  8 (was CMD_SYNCREQ_PXS=10)
    SyncLogResponse,      // pos  9 (was CMD_SYNCRESP_PXS=11)
    SyncNoOpRequest,      // pos 10 (was CMD_SYNCNOOP_PXS=12)
    PaxosPrepCmd,         // pos 11 (was CMD_PREP_PXS=13)
    TpcEmptyCommand,      // pos 12 (was CMD_TPC_EMPTY=14)
    TpcNoopCommand,       // pos 13 (was CMD_NOOP=15)
    TpcBatchCommand,      // pos 14 (was CMD_TPC_BATCH=16)
    VecRecData,           // pos 15 (was CMD_REC_VEC=19)
    ViewData,             // pos 16 (was CMD_VIEW_DATA=20)
    SimpleRWCommand,      // pos 17 (was CMD_KV=21)
    KeyCmdBatchData,      // pos 18 (was CMD_KEY_CMD_BATCH=22)
    ReplicatedDBCommand   // pos 19 (was CMD_REPLICATED_DB=23)
>;

// `Command` is the user-facing closed-set
// polymorphic carrier — the user's "Command type" that wraps any of
// the MakoCommands variants on the wire.  Replaces `MarshallDeputy`
// for closed-set fields:
//
//   // Before (legacy):
//   void Foo(MarshallDeputy md) { auto cmd = marshallable_cast<T>(md); }
//   // After:
//   void Foo(janus::Command cmd) { auto* p = cmd.unpack<T>(); }
//
// Wire format is byte-for-byte identical to post-L9 MarshallDeputy
// (`[v32 kind][payload bytes]`), so call-site migrations from
// MarshallDeputy → Command are pure C++ API changes with no on-the-
// wire impact for matched kind→type mappings.
// `Command` is a thin deptran-local subclass rather than a plain alias.
// rrr::SerializableEnvelope is now generated from the inline-Rust DSL,
// and a generated struct cannot host a templated converting constructor
// or a templated operator= (no Rust trait maps to either). Both are pure
// call-site ergonomics — `Command cmd = rusty::Arc<T>::make(...)` at 61
// deptran sites — so they live here, on deptran's side of the boundary,
// instead of pinning 20 hand-written lines inside rrr. Adds no data
// members (so slicing to the base is harmless) and no virtuals; the base
// remains deducible for marshallable_cast/serialize/deserialize.
class Command : public rrr::SerializableEnvelope<MakoCommands> {
 public:
  using Base = rrr::SerializableEnvelope<MakoCommands>;

  Command() = default;
  // Adopt a base-typed envelope (Base::pack/pack_aliased return Base).
  Command(const Base& base) : Base(base) {}
  Command(Base&& base) : Base(std::move(base)) {}

  // Aliased packing: the envelope retains the caller's Arc<T>.
  template <typename T>
  Command(rusty::Arc<T> sp) : Base(Base::template pack_aliased<T>(std::move(sp))) {}

  template <typename T>
  Command& operator=(rusty::Arc<T> sp) {
    Base::operator=(Base::template pack_aliased<T>(std::move(sp)));
    return *this;
  }
};

}  // namespace janus
