module;
#include <rusty/array.hpp>
#include <rusty/slice.hpp>
export module reduced_repro;
import rusty;
import btree_port.btree.map;
namespace btree_port { using btree::map::BTreeMap; }
export void trigger(btree_port::BTreeMap<std::string, std::string>& staged,
                    btree_port::BTreeMap<std::string, bool>& deleted,
                    btree_port::BTreeMap<std::string, std::string>& dst) {
    for (auto&& kv : rusty::for_in(staged)) {
        dst.insert(std::get<0>(kv), std::get<1>(kv));
    }
    for (auto&& dk : rusty::for_in(deleted)) {
        dst.remove(std::get<0>(dk));
    }
}
