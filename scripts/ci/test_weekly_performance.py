import unittest
from weekly_performance import compare

class Baselines(unittest.TestCase):
    def test_changed_environment_is_not_a_regression(self):
        self.assertFalse(compare({'environment':{'cpu':'new'}},{'environment':{'cpu':'old'}})['comparable'])
    def test_same_environment_compares_all_metrics(self):
        result=compare({'environment':{},'medians':{'micro':90,'tpcc':110}},
                       {'environment':{},'medians':{'micro':100,'tpcc':100},'revision':'baseline'})
        self.assertTrue(result['comparable'])
        self.assertEqual(result['throughput_ratio'],{'micro':.9,'tpcc':1.1})
    def test_missing_or_zero_baseline_does_not_claim_comparability(self):
        for medians in ({},{'micro':0}):
            self.assertFalse(compare({'environment':{},'medians':{'micro':100}},
                                    {'environment':{},'medians':medians})['comparable'])

if __name__=='__main__': unittest.main()
