#pragma once

// Central `MakoCommands` closed payload set.
//
// Closes the closed-set half of the dual-envelope architecture
// (counterpart to the open-set `rrr::AnyMessage` envelope from L7):
// Every polymorphic Command type carried by the closed-set Serializable
// envelope has one explicit marker registration here. Its numeric kind is
// an immutable wire value rather than an implicit position in a type list.
//
// Mirrors a Rust enum with explicit discriminants. The protobuf analogue is
// `oneof`: a closed set whose field numbers must never be reused or changed.
//
// Adding a new closed-set Command type:
//   1. Add a forward declaration below.
//   2. Append a new explicitly numbered `MakoCommandKind` variant and a
//      `PayloadMember<MakoCommands>` impl. Never change an existing value.
//   3. Define `class T : public rrr::Serializable<
//          rrr::PayloadMember<janus::MakoCommands, T>::KIND>`.
//   4. Add a registration line in T's .cc:
//      `static int volatile g_reg_T =
//          rrr::SerializableRegistry::reg<T>(T::static_kind());`
//
// For OPEN-set polymorphic types (graph payloads, anything where the
// receiver may not know about every possible carried type at compile
// time), use `rrr::AnyMessage` instead — see `rrr/misc/any_message.hpp`.

#include <rusty/arc.hpp>

#include "rrr/misc/serializable.hpp"
#include "rrr/misc/serializable_envelope.hpp"

namespace janus {

// Forward declarations. Marker registrations need only type identity;
// concrete bodies are picked up later by the declaring headers.
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

}  // namespace janus

// Source of truth for all closed-set membership and wire values. Explicit
// values preserve the former TypeList's 1..19 mapping; 0 remains the empty /
// unknown sentinel. Marker impls intentionally live at global scope so their
// generated C++ explicit specializations are legal.
#if RUSTYCPP_RUST
mod janus {
    #[repr(i32)]
    pub enum MakoCommandKind {
        Unknown = 0,
        LogEntry = 1,
        TpcPrepareCommand = 2,
        TpcCommitCommand = 3,
        VecPieceData = 4,
        BulkPaxosCmd = 5,
        BulkPrepareLog = 6,
        HeartBeatLog = 7,
        SyncLogRequest = 8,
        SyncLogResponse = 9,
        SyncNoOpRequest = 10,
        PaxosPrepCmd = 11,
        TpcEmptyCommand = 12,
        TpcNoopCommand = 13,
        TpcBatchCommand = 14,
        VecRecData = 15,
        ViewData = 16,
        SimpleRWCommand = 17,
        KeyCmdBatchData = 18,
        ReplicatedDBCommand = 19,
    }

    pub struct MakoCommands {}
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::LogEntry {
    const KIND: i32 = janus::MakoCommandKind::LogEntry as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::TpcPrepareCommand {
    const KIND: i32 = janus::MakoCommandKind::TpcPrepareCommand as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::TpcCommitCommand {
    const KIND: i32 = janus::MakoCommandKind::TpcCommitCommand as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::VecPieceData {
    const KIND: i32 = janus::MakoCommandKind::VecPieceData as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::BulkPaxosCmd {
    const KIND: i32 = janus::MakoCommandKind::BulkPaxosCmd as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::BulkPrepareLog {
    const KIND: i32 = janus::MakoCommandKind::BulkPrepareLog as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::HeartBeatLog {
    const KIND: i32 = janus::MakoCommandKind::HeartBeatLog as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::SyncLogRequest {
    const KIND: i32 = janus::MakoCommandKind::SyncLogRequest as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::SyncLogResponse {
    const KIND: i32 = janus::MakoCommandKind::SyncLogResponse as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::SyncNoOpRequest {
    const KIND: i32 = janus::MakoCommandKind::SyncNoOpRequest as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::PaxosPrepCmd {
    const KIND: i32 = janus::MakoCommandKind::PaxosPrepCmd as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::TpcEmptyCommand {
    const KIND: i32 = janus::MakoCommandKind::TpcEmptyCommand as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::TpcNoopCommand {
    const KIND: i32 = janus::MakoCommandKind::TpcNoopCommand as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::TpcBatchCommand {
    const KIND: i32 = janus::MakoCommandKind::TpcBatchCommand as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::VecRecData {
    const KIND: i32 = janus::MakoCommandKind::VecRecData as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::ViewData {
    const KIND: i32 = janus::MakoCommandKind::ViewData as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::SimpleRWCommand {
    const KIND: i32 = janus::MakoCommandKind::SimpleRWCommand as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::KeyCmdBatchData {
    const KIND: i32 = janus::MakoCommandKind::KeyCmdBatchData as i32;
}

#[cfg_attr(any(), cpp_marker_impl)]
impl rrr::PayloadMember<janus::MakoCommands> for janus::ReplicatedDBCommand {
    const KIND: i32 = janus::MakoCommandKind::ReplicatedDBCommand as i32;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=mako_commands.1 version=1 rust_sha256=ca969b1f29a6c1bd909b75dce30ecf9da225f0a57ff1baf39a3488a46c4070c2*/
namespace janus {
    enum class MakoCommandKind;
    constexpr MakoCommandKind MakoCommandKind_Unknown();
    constexpr MakoCommandKind MakoCommandKind_LogEntry();
    constexpr MakoCommandKind MakoCommandKind_TpcPrepareCommand();
    constexpr MakoCommandKind MakoCommandKind_TpcCommitCommand();
    constexpr MakoCommandKind MakoCommandKind_VecPieceData();
    constexpr MakoCommandKind MakoCommandKind_BulkPaxosCmd();
    constexpr MakoCommandKind MakoCommandKind_BulkPrepareLog();
    constexpr MakoCommandKind MakoCommandKind_HeartBeatLog();
    constexpr MakoCommandKind MakoCommandKind_SyncLogRequest();
    constexpr MakoCommandKind MakoCommandKind_SyncLogResponse();
    constexpr MakoCommandKind MakoCommandKind_SyncNoOpRequest();
    constexpr MakoCommandKind MakoCommandKind_PaxosPrepCmd();
    constexpr MakoCommandKind MakoCommandKind_TpcEmptyCommand();
    constexpr MakoCommandKind MakoCommandKind_TpcNoopCommand();
    constexpr MakoCommandKind MakoCommandKind_TpcBatchCommand();
    constexpr MakoCommandKind MakoCommandKind_VecRecData();
    constexpr MakoCommandKind MakoCommandKind_ViewData();
    constexpr MakoCommandKind MakoCommandKind_SimpleRWCommand();
    constexpr MakoCommandKind MakoCommandKind_KeyCmdBatchData();
    constexpr MakoCommandKind MakoCommandKind_ReplicatedDBCommand();
    struct MakoCommands;
}

namespace rrr {
}

// mod janus
namespace janus {

    enum class MakoCommandKind;
    constexpr MakoCommandKind MakoCommandKind_Unknown();
    constexpr MakoCommandKind MakoCommandKind_LogEntry();
    constexpr MakoCommandKind MakoCommandKind_TpcPrepareCommand();
    constexpr MakoCommandKind MakoCommandKind_TpcCommitCommand();
    constexpr MakoCommandKind MakoCommandKind_VecPieceData();
    constexpr MakoCommandKind MakoCommandKind_BulkPaxosCmd();
    constexpr MakoCommandKind MakoCommandKind_BulkPrepareLog();
    constexpr MakoCommandKind MakoCommandKind_HeartBeatLog();
    constexpr MakoCommandKind MakoCommandKind_SyncLogRequest();
    constexpr MakoCommandKind MakoCommandKind_SyncLogResponse();
    constexpr MakoCommandKind MakoCommandKind_SyncNoOpRequest();
    constexpr MakoCommandKind MakoCommandKind_PaxosPrepCmd();
    constexpr MakoCommandKind MakoCommandKind_TpcEmptyCommand();
    constexpr MakoCommandKind MakoCommandKind_TpcNoopCommand();
    constexpr MakoCommandKind MakoCommandKind_TpcBatchCommand();
    constexpr MakoCommandKind MakoCommandKind_VecRecData();
    constexpr MakoCommandKind MakoCommandKind_ViewData();
    constexpr MakoCommandKind MakoCommandKind_SimpleRWCommand();
    constexpr MakoCommandKind MakoCommandKind_KeyCmdBatchData();
    constexpr MakoCommandKind MakoCommandKind_ReplicatedDBCommand();
    struct MakoCommands;

    enum class MakoCommandKind {
        Unknown = 0,
    LogEntry = 1,
    TpcPrepareCommand = 2,
    TpcCommitCommand = 3,
    VecPieceData = 4,
    BulkPaxosCmd = 5,
    BulkPrepareLog = 6,
    HeartBeatLog = 7,
    SyncLogRequest = 8,
    SyncLogResponse = 9,
    SyncNoOpRequest = 10,
    PaxosPrepCmd = 11,
    TpcEmptyCommand = 12,
    TpcNoopCommand = 13,
    TpcBatchCommand = 14,
    VecRecData = 15,
    ViewData = 16,
    SimpleRWCommand = 17,
    KeyCmdBatchData = 18,
    ReplicatedDBCommand = 19
    };
    inline constexpr MakoCommandKind MakoCommandKind_Unknown() { return MakoCommandKind::Unknown; }
    inline constexpr MakoCommandKind MakoCommandKind_LogEntry() { return MakoCommandKind::LogEntry; }
    inline constexpr MakoCommandKind MakoCommandKind_TpcPrepareCommand() { return MakoCommandKind::TpcPrepareCommand; }
    inline constexpr MakoCommandKind MakoCommandKind_TpcCommitCommand() { return MakoCommandKind::TpcCommitCommand; }
    inline constexpr MakoCommandKind MakoCommandKind_VecPieceData() { return MakoCommandKind::VecPieceData; }
    inline constexpr MakoCommandKind MakoCommandKind_BulkPaxosCmd() { return MakoCommandKind::BulkPaxosCmd; }
    inline constexpr MakoCommandKind MakoCommandKind_BulkPrepareLog() { return MakoCommandKind::BulkPrepareLog; }
    inline constexpr MakoCommandKind MakoCommandKind_HeartBeatLog() { return MakoCommandKind::HeartBeatLog; }
    inline constexpr MakoCommandKind MakoCommandKind_SyncLogRequest() { return MakoCommandKind::SyncLogRequest; }
    inline constexpr MakoCommandKind MakoCommandKind_SyncLogResponse() { return MakoCommandKind::SyncLogResponse; }
    inline constexpr MakoCommandKind MakoCommandKind_SyncNoOpRequest() { return MakoCommandKind::SyncNoOpRequest; }
    inline constexpr MakoCommandKind MakoCommandKind_PaxosPrepCmd() { return MakoCommandKind::PaxosPrepCmd; }
    inline constexpr MakoCommandKind MakoCommandKind_TpcEmptyCommand() { return MakoCommandKind::TpcEmptyCommand; }
    inline constexpr MakoCommandKind MakoCommandKind_TpcNoopCommand() { return MakoCommandKind::TpcNoopCommand; }
    inline constexpr MakoCommandKind MakoCommandKind_TpcBatchCommand() { return MakoCommandKind::TpcBatchCommand; }
    inline constexpr MakoCommandKind MakoCommandKind_VecRecData() { return MakoCommandKind::VecRecData; }
    inline constexpr MakoCommandKind MakoCommandKind_ViewData() { return MakoCommandKind::ViewData; }
    inline constexpr MakoCommandKind MakoCommandKind_SimpleRWCommand() { return MakoCommandKind::SimpleRWCommand; }
    inline constexpr MakoCommandKind MakoCommandKind_KeyCmdBatchData() { return MakoCommandKind::KeyCmdBatchData; }
    inline constexpr MakoCommandKind MakoCommandKind_ReplicatedDBCommand() { return MakoCommandKind::ReplicatedDBCommand; }

    struct MakoCommands {
        // Rust derives Send/Sync from the field types; C++ cannot see them.
        static constexpr bool is_send = true;
        static constexpr bool is_sync = true;
    };

}

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::LogEntry> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_LogEntry());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::TpcPrepareCommand> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_TpcPrepareCommand());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::TpcCommitCommand> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_TpcCommitCommand());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::VecPieceData> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_VecPieceData());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::BulkPaxosCmd> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_BulkPaxosCmd());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::BulkPrepareLog> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_BulkPrepareLog());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::HeartBeatLog> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_HeartBeatLog());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::SyncLogRequest> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_SyncLogRequest());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::SyncLogResponse> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_SyncLogResponse());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::SyncNoOpRequest> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_SyncNoOpRequest());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::PaxosPrepCmd> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_PaxosPrepCmd());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::TpcEmptyCommand> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_TpcEmptyCommand());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::TpcNoopCommand> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_TpcNoopCommand());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::TpcBatchCommand> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_TpcBatchCommand());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::VecRecData> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_VecRecData());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::ViewData> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_ViewData());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::SimpleRWCommand> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_SimpleRWCommand());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::KeyCmdBatchData> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_KeyCmdBatchData());
};

template<>
struct rrr::PayloadMember<::janus::MakoCommands, janus::ReplicatedDBCommand> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = static_cast<int32_t>(janus::MakoCommandKind_ReplicatedDBCommand());
};
/*RUSTYCPP:GEN-END id=mako_commands.1*/

namespace janus {

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
