#pragma once

#include <vector>
#include "macros.h"

/**
 * T - node type. needs to implement is_lock_owner()
 */
template <typename Scope, typename T>
class ownership_checker {
public:
  // @unsafe - clears thread-local list tracking raw node pointers
  static void
  NodeLockRegionBegin()
  {
    MyLockedNodes(true)->clear();
  }

  // is used to signal the end of a tuple lock region
  // @unsafe - asserts raw nodes no longer locked
  static void
  AssertAllNodeLocksReleased()
  {
    std::vector<const T *> *nodes = MyLockedNodes(false);
    ALWAYS_ASSERT(nodes);
    for (auto p : *nodes)
      ALWAYS_ASSERT(!p->is_lock_owner());
    nodes->clear();
  }

  // @unsafe - records raw node pointer into thread-local lock set
  static void
  AddNodeToLockRegion(const T *n)
  {
    ALWAYS_ASSERT(n->is_locked());
    ALWAYS_ASSERT(n->is_lock_owner());
    std::vector<const T *> *nodes = MyLockedNodes(false);
    if (nodes)
      nodes->push_back(n);
  }

private:
  // @unsafe - manages thread-local vector of raw pointers
  static std::vector<const T *> *
  MyLockedNodes(bool create)
  {
    static __thread std::vector<const T *> *tl_locked_nodes = nullptr;
    if (unlikely(!tl_locked_nodes) && create)
      tl_locked_nodes = new std::vector<const T *>;
    return tl_locked_nodes;
  }
};
