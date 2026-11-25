import unittest
from select_gates import select, NAMES

class Selection(unittest.TestCase):
    def test_upper_change_does_not_run_components(self):
        self.assertEqual({k for k,v in select(['crates/mako-protocol/tests/delivery.rs']).items() if v},{'upper'})
    def test_shared_contract_checks_both_sides(self):
        result=select(['crates/mako-protocol/src/contracts.rs'])
        for name in ('upper','components','srpc','replication','integration'): self.assertTrue(result[name])
    def test_srpc_keeps_its_own_suite(self):
        result=select(['src/srpc/src/client.rs'])
        self.assertTrue(result['srpc']); self.assertFalse(result['upper']); self.assertFalse(result['components'])
    def test_unknown_build_change_selects_all(self):
        self.assertTrue(all(select(['Cargo-new-config.toml']).values()))
    def test_scheduled_and_manual_select_all(self):
        for event in ('schedule','workflow_dispatch'): self.assertTrue(all(select([],event).values()))
    def test_docs_do_not_build(self):
        self.assertFalse(any(select(['docs/developer/upper-layer-contracts.md']).values()))

if __name__=='__main__': unittest.main()
