import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('raw_driver',Path(__file__).with_name('run-raw-attribution.py'))
driver=importlib.util.module_from_spec(spec);spec.loader.exec_module(driver)


class RawChecks(unittest.TestCase):
    def sample(self,mode='engine',workers=1):
        return driver.expected(mode,workers)|dict(max_worker_nanoseconds=262144000*workers,
            transactions_per_second=1000000.,cycles_per_commit=123.,instructions_per_commit=456.,
            pmu_min_running_ratio=1.)

    def test_exact_schemas_and_counts_for_both_modes(self):
        for mode in ['engine','raw-sto']:
            for workers in [1,4]:driver.validate(self.sample(mode,workers),mode,workers)

    def test_every_echoed_field_and_missing_extra_fields_fail(self):
        for field in driver.expected('engine',1):
            record=self.sample();record[field]='wrong'
            with self.subTest(field=field),self.assertRaises(RuntimeError):driver.validate(record,'engine',1)
        for field in list(self.sample())+['extra']:
            record=self.sample()
            if field=='extra':record[field]=1
            else:del record[field]
            with self.subTest(field=field),self.assertRaises(RuntimeError):driver.validate(record,'engine',1)

    def test_pmu_and_denominator_fail_closed(self):
        for field,value in [('pmu_min_running_ratio',float('nan')),('pmu_min_running_ratio',.99),
                            ('max_worker_nanoseconds',float('inf')),('max_worker_nanoseconds',0),
                            ('cycles_per_commit',float('nan')),('instructions_per_commit',0),
                            ('transactions_per_second',999999)]:
            record=self.sample();record[field]=value
            with self.subTest(field=field),self.assertRaises(RuntimeError):driver.validate(record,'engine',1)


if __name__=='__main__':unittest.main()
