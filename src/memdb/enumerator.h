#pragma once

// `Enumerator<T>` — memdb's query-cursor interface (ResultSet, table
// scans, txn queries). Relocated verbatim from rrr/base/basetypes.cpp:
// memdb was its only consumer, and the no-manual-cpp goal keeps rrr
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

} // namespace mdb
