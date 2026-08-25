#pragma once

// `Enumerator<T>` — memdb's query-cursor interface (ResultSet, table
// scans, txn queries). Relocated verbatim from srpc/base/basetypes.cpp:
// memdb was its only consumer, and the no-manual-cpp goal keeps srpc
// free of hand-written polymorphic templates. (Its sibling
// MergedEnumerator had zero users and was deleted.)

#include <cstdlib>

namespace mdb {

template<class T>
class Enumerator {
public:
    virtual ~Enumerator() {}
    virtual void reset() {
        std::abort();
    }
    virtual bool has_next() = 0;
    operator bool() {
        return this->has_next();
    }
    virtual T next() = 0;
    T operator() () {
        return this->next();
    }
};

// `NoCopy` — deleted-copy marker base for memdb's identity types
// (Row/Table/Txn/TxnMgr/snapshot_group/MergedCursor). Relocated
// verbatim from srpc/base/basetypes.cpp: memdb was its only remaining
// consumer after the srpc wire-layer DSL flips dropped their NoCopy
// bases for Cell move-only markers.
class NoCopy {
protected:
    NoCopy() = default;
    virtual ~NoCopy() = default;
public:
    NoCopy(const NoCopy&) = delete;
    NoCopy& operator=(const NoCopy&) = delete;
    NoCopy(NoCopy&&) = default;
    NoCopy& operator=(NoCopy&&) = default;
};

} // namespace mdb
