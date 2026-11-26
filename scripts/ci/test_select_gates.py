import unittest
from select_gates import select

class Selection(unittest.TestCase):
    def test_upper_change_does_not_run_components(self):
        for path in ('crates/mako-protocol/tests/delivery.rs','crates/mako-protocol-core/tests/coordinator.rs'):
            self.assertEqual({k for k,v in select([path]).items() if v},{'upper'})
    def test_shared_contract_checks_both_sides(self):
        for path in ('crates/mako-protocol/src/contracts.rs', 'crates/mako-protocol/src/range_lock.rs',
                     'src/cluster/range_lock.rs'):
            result=select([path])
            for name in ('upper','components','masstree_tx','srpc','replication','integration'): self.assertTrue(result[name])
    def test_srpc_keeps_its_own_suite(self):
        result=select(['src/srpc/src/client.rs'])
        self.assertTrue(result['srpc']); self.assertFalse(result['upper']); self.assertFalse(result['components'])
    def test_masstree_transaction_changes_have_their_own_component_gate(self):
        for path in ('crates/sto-masstree/src/lib.rs', 'crates/sto-tpcc-ffi/tests/native_api.rs',
                     'ci/masstree_tx.sh', 'cmake/MasstreeTransaction.cmake'):
            self.assertEqual({k for k,v in select([path]).items() if v}, {'masstree_tx','integration'})
    def test_shared_core_and_index_changes_check_the_adapter(self):
        for path in ('crates/sto-core/src/transaction.rs','crates/masstree/src/lib.rs','src/masstree/masstree.hh'):
            result=select([path])
            for name in ('components','masstree_tx','integration'): self.assertTrue(result[name])
    def test_unknown_build_change_selects_all(self):
        self.assertTrue(all(select(['Cargo-new-config.toml']).values()))
    def test_scheduled_and_manual_select_all(self):
        for event in ('schedule','workflow_dispatch'): self.assertTrue(all(select([],event).values()))
    def test_independent_models_keep_their_extended_check(self):
        self.assertTrue(select(['tla/mako/src/sharding.rs'])['extended'])
    def test_performance_driver_does_not_trigger_benchmarks_or_native_builds(self):
        self.assertFalse(any(select(['scripts/ci/weekly_performance.py']).values()))
    def test_docs_do_not_build(self):
        self.assertFalse(any(select(['docs/developer/upper-layer-contracts.md']).values()))

if __name__=='__main__': unittest.main()
