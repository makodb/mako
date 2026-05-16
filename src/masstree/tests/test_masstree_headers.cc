// Tier 9 of docs/masstree-test-plan.md — build & API gates.
//
// Header self-containment / transitive-include sentinel. This TU
// includes every public Masstree header (plus the mako-side wrappers
// that consumers use to instantiate trees) and asserts nothing more
// than "this compiles". If a refactor accidentally drops a forward
// declaration or relies on a transitive include from an unrelated
// header, the compile fails here before it fails in a downstream
// consumer.
//
// This is intentionally minimal — no runtime behavior, just compile
// coverage. The cost is one TU per build; the value is a fast,
// targeted failure when header layout regresses.

#include "masstree/btree_leaflink.hh"
#include "masstree/circular_int.hh"
#include "masstree/compiler.hh"
#include "masstree/kpermuter.hh"
#include "masstree/ksearch.hh"
#include "masstree/kvthread.hh"
#include "masstree/masstree.hh"
#include "masstree/masstree_get.hh"
#include "masstree/masstree_insert.hh"
#include "masstree/masstree_key.hh"
#include "masstree/masstree_print.hh"
#include "masstree/masstree_remove.hh"
#include "masstree/masstree_scan.hh"
#include "masstree/masstree_split.hh"
#include "masstree/masstree_struct.hh"
#include "masstree/masstree_tcursor.hh"
#include "masstree/nodeversion.hh"
#include "masstree/string_slice.hh"
#include "masstree/timestamp.hh"

#include "mako/masstree_btree.h"
#include "mako/varkey.h"

import std;

// Required by Masstree's RCU machinery when concurrent_btree is
// instantiated below.
volatile mrcu_epoch_type globalepoch = 1;

// Touch both tree types so the compiler instantiates the templates
// from the headers above. Without these instantiations the test
// would only catch include-order bugs, not template-body bugs.
namespace {

void Instantiate() {
  single_threaded_btree st;
  concurrent_btree mt;
  (void)st;
  (void)mt;
}

}  // namespace

int main() {
  Instantiate();
  return 0;
}
