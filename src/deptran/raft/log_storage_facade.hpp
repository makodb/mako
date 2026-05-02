#pragma once

/**
 * @file log_storage_facade.hpp
 * @brief Proxy facade for raft::LogStorage.
 *
 * Mirrors every LogStorage method so RaftServer can move from a virtual
 * interface pointer to a proxy-backed storage boundary (Phase 8.4).
 */

#include <string>
#include <vector>

// deptran/constants.h defines macro RR, which can collide with template
// parameter names inside proxy headers. Protect proxy includes.
#ifdef RR
#pragma push_macro("RR")
#undef RR
#define RAFT_LOG_STORAGE_FACADE_RESTORE_RR_MACRO 1
#endif
#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>
#ifdef RAFT_LOG_STORAGE_FACADE_RESTORE_RR_MACRO
#pragma pop_macro("RR")
#undef RAFT_LOG_STORAGE_FACADE_RESTORE_RR_MACRO
#endif

#include "log_storage.hpp"

namespace janus {
namespace raft {

PRO_DEF_MEM_DISPATCH(LsGet,         get);
PRO_DEF_MEM_DISPATCH(LsPut,         put);
PRO_DEF_MEM_DISPATCH(LsRemove,      remove);
PRO_DEF_MEM_DISPATCH(LsGetRange,    get_range);
PRO_DEF_MEM_DISPATCH(LsPutBatch,    put_batch);
PRO_DEF_MEM_DISPATCH(LsRemoveRange, remove_range);
PRO_DEF_MEM_DISPATCH(LsFirstIndex,  get_first_index);
PRO_DEF_MEM_DISPATCH(LsLastIndex,   get_last_index);
PRO_DEF_MEM_DISPATCH(LsGetTerm,     get_term);
PRO_DEF_MEM_DISPATCH(LsSize,        size);
PRO_DEF_MEM_DISPATCH(LsEmpty,       empty);
PRO_DEF_MEM_DISPATCH(LsSetMetadata, set_metadata);
PRO_DEF_MEM_DISPATCH(LsGetMetadata, get_metadata);
PRO_DEF_MEM_DISPATCH(LsSync,        sync);
PRO_DEF_MEM_DISPATCH(LsClose,       close);
PRO_DEF_MEM_DISPATCH(LsIsOpen,      is_open);
PRO_DEF_MEM_DISPATCH(LsClear,       clear);

struct LogStorageFacade : pro::facade_builder
    ::add_convention<LsGet, rusty::Option<LogEntry>(slotid_t) const>
    ::add_convention<LsPut, bool(const LogEntry&)>
    ::add_convention<LsRemove, bool(slotid_t)>
    ::add_convention<LsGetRange, std::vector<LogEntry>(slotid_t, slotid_t) const>
    ::add_convention<LsPutBatch, bool(const std::vector<LogEntry>&)>
    ::add_convention<LsRemoveRange, bool(slotid_t, slotid_t)>
    ::add_convention<LsFirstIndex, slotid_t() const>
    ::add_convention<LsLastIndex, slotid_t() const>
    ::add_convention<LsGetTerm, rusty::Option<ballot_t>(slotid_t) const>
    ::add_convention<LsSize, size_t() const>
    ::add_convention<LsEmpty, bool() const>
    ::add_convention<LsSetMetadata, bool(const std::string&, const std::string&)>
    ::add_convention<LsGetMetadata, rusty::Option<std::string>(const std::string&) const>
    ::add_convention<LsSync, bool()>
    ::add_convention<LsClose, bool()>
    ::add_convention<LsIsOpen, bool() const>
    ::add_convention<LsClear, bool()>
    ::build {};

using LogStorageProxy = pro::proxy<LogStorageFacade>;

}  // namespace raft
}  // namespace janus
