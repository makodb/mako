// Mako-independent isolation repro for the ShardManagerTest.LargeRangeMigration-
// ConservesEveryKey failure. It exercises the module-form btree_port BTreeMap at
// multi-node scale (200 std::string keys) -- the exact instantiation the
// cluster's Shard.data uses -- with a plain insert-then-get workload.
//
// The existing tests/btree_port_module_test.cpp inserts only ONE key (a single
// B-tree node), so the multi-node path is otherwise unexercised. If the
// module-form BTreeMap corrupts its tree once it grows past a single node, a
// get() below returns None for a key that was definitely inserted (or an
// internal Option::unwrap panics), reproducing the cluster failure with zero
// mako/cluster code involved.
import btree_port.btree.map;
import rusty;

#include <string>
#include <cstdio>

namespace btree_port { using btree::map::BTreeMap; }  // same alias as src/cluster/shard.h

int main() {
    auto m = btree_port::BTreeMap<std::string, std::string>::new_();
    for (int i = 0; i < 200; ++i) {
        std::string k = std::string("m") + std::to_string(i);
        std::string v = std::string("v") + std::to_string(i);
        m.insert(std::move(k), std::move(v));
    }
    int lost = 0;
    for (int i = 0; i < 200; ++i) {
        auto r = m.get(std::string("m") + std::to_string(i));
        if (!r.is_some()) { std::printf("LOST key m%d\n", i); ++lost; }
    }
    std::printf("btree_port 200-key insert+get: lost=%d (want 0)\n", lost);
    return lost == 0 ? 0 : 1;
}
