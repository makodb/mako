#pragma once

/**
 * @file snapshot_manager_facade.hpp
 * @brief Proxy facade for raft::SnapshotManager.
 *
 * Mirrors every SnapshotManager method so RaftServer can migrate from virtual
 * interface pointers to proxy-backed storage boundaries (Phase 8.4).
 */

#include <memory>
#include <string>
#include <vector>

// deptran/constants.h defines macro RR, which can collide with template
// parameter names inside proxy headers. Protect proxy includes.
#ifdef RR
#pragma push_macro("RR")
#undef RR
#define RAFT_SNAPSHOT_MANAGER_FACADE_RESTORE_RR_MACRO 1
#endif
#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>
#ifdef RAFT_SNAPSHOT_MANAGER_FACADE_RESTORE_RR_MACRO
#pragma pop_macro("RR")
#undef RAFT_SNAPSHOT_MANAGER_FACADE_RESTORE_RR_MACRO
#endif

#include "snapshot_manager.hpp"

namespace janus {
namespace raft {

PRO_DEF_MEM_DISPATCH(SmBeginSnapshot, BeginSnapshot);
PRO_DEF_MEM_DISPATCH(SmTakeSnapshot, TakeSnapshot);
PRO_DEF_MEM_DISPATCH(SmBeginLoad, BeginLoad);
PRO_DEF_MEM_DISPATCH(SmLoadLatestSnapshot, LoadLatestSnapshot);
PRO_DEF_MEM_DISPATCH(SmGetLatestSnapshot, GetLatestSnapshot);
PRO_DEF_MEM_DISPATCH(SmListSnapshots, ListSnapshots);
PRO_DEF_MEM_DISPATCH(SmHasSnapshotAtOrAfter, HasSnapshotAtOrAfter);
PRO_DEF_MEM_DISPATCH(SmPruneSnapshots, PruneSnapshots);
PRO_DEF_MEM_DISPATCH(SmDeleteAllSnapshots, DeleteAllSnapshots);
PRO_DEF_MEM_DISPATCH(SmGetStoragePath, GetStoragePath);

struct SnapshotManagerFacade : pro::facade_builder
    ::add_convention<SmBeginSnapshot, std::unique_ptr<SnapshotWriter>(slotid_t, ballot_t)>
    ::add_convention<SmTakeSnapshot, bool(slotid_t, ballot_t, const char*, size_t)>
    ::add_convention<SmBeginLoad, std::unique_ptr<SnapshotReader>(const SnapshotMetadata&)>
    ::add_convention<SmLoadLatestSnapshot, bool(SnapshotMetadata*, std::string*)>
    ::add_convention<SmGetLatestSnapshot, rusty::Option<SnapshotMetadata>() const>
    ::add_convention<SmListSnapshots, std::vector<SnapshotMetadata>() const>
    ::add_convention<SmHasSnapshotAtOrAfter, bool(slotid_t) const>
    ::add_convention<SmPruneSnapshots, size_t(slotid_t)>
    ::add_convention<SmDeleteAllSnapshots, size_t()>
    ::add_convention<SmGetStoragePath, const std::string&() const>
    ::build {};

using SnapshotManagerProxy = pro::proxy<SnapshotManagerFacade>;

}  // namespace raft
}  // namespace janus
