import importlib.util
import builtins
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

path = Path(__file__).with_name('run-resolved-variants.py')
spec = importlib.util.spec_from_file_location('resolved_driver', path)
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


class WorkloadChecks(unittest.TestCase):
    def test_three_arm_position_balance(self):
        labels = ['base', 'cache', 'cache-prefetch']
        for start in (1, 4):
            orders = [driver.variant_order(labels, r) for r in range(start, start+3)]
            for position in range(3):
                self.assertEqual({order[position] for order in orders}, set(labels))
        self.assertEqual(driver.variant_order(labels, 1), labels)
        self.assertEqual(driver.variant_order(labels, 2), ['cache', 'cache-prefetch', 'base'])

    def test_machine_policy_and_binary_identity(self):
        valid = {'boost':'0', 'governors':{str(cpu):'schedutil' for cpu in range(64)}}
        driver.validate_machine(valid)
        for invalid in ({**valid, 'boost':'1'},
                        {**valid, 'governors':{str(cpu):'performance' for cpu in range(64)}},
                        {**valid, 'governors':{}}):
            with self.assertRaises(RuntimeError):
                driver.validate_machine(invalid)
        with mock.patch.object(driver, 'digest', return_value='frozen'):
            self.assertEqual(driver.validate_binary(Path('test'), 'frozen'), 'frozen')
            with self.assertRaises(RuntimeError):
                driver.validate_binary(Path('test'), 'changed')

    def test_cell_failure_retains_only_attempted_scratch(self):
        for defect in ('hash-before', 'hash-after', 'mode', 'workers', 'pmu', None):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as temporary, tempfile.TemporaryFile() as lock:
                root=Path(temporary); output=root/'output'; output.mkdir()
                binaries=[root/'base',root/'cache']
                for binary in binaries:
                    binary.touch()
                launched=[]; hashes={binary:0 for binary in binaries}
                snapshot={'boost':'0','governors':{str(cpu):'schedutil' for cpu in range(64)}}
                argv=['driver','--output',str(output),'--scenarios','hot256',
                      '--modes','cache-ack','--iterations','1','--warmup','256','--repetitions','1']
                for binary in binaries:
                    argv+=['--variant',f'{binary.name}={binary}']
                real_open=builtins.open
                def open_lock(path,*args,**kwargs):
                    return contextlib.nullcontext(lock) if str(path)=='/var/tmp/mako-zoo2-perf-shuai.lock' else real_open(path,*args,**kwargs)
                def fake_digest(path):
                    if path in hashes:
                        hashes[path]+=1
                        if defect=='hash-before' and hashes[path]>=2 or defect=='hash-after' and hashes[path]>=3:
                            return 'changed'
                    return 'frozen'
                def fake_execute(command,directory):
                    launched.append(command);(directory/'rocks').mkdir()
                    fields=driver.expected_fields('hot256',1,'cache-ack',1,256,driver.SEED)
                    measurement=dict(fields,pmu_min_running_ratio=1.0,max_worker_nanoseconds=1000,
                                     transactions_per_second=1000000.0,cycles_per_commit=123.0,
                                     instructions_per_commit=456.0,applied_before_abort=0,queued_before_abort=257)
                    if defect=='mode': measurement['mode']='engine'
                    if defect=='workers': measurement['workers']=2
                    if defect=='pmu': measurement['pmu_min_running_ratio']=float('nan')
                    (directory/'stdout.txt').write_text(json.dumps(measurement)+'\n')
                    return {'completed':True,'returncode':0}
                error=None
                with contextlib.ExitStack() as stack:
                    stack.enter_context(mock.patch.object(sys,'argv',argv))
                    stack.enter_context(mock.patch.object(driver.socket,'gethostname',return_value='zoo-002'))
                    stack.enter_context(mock.patch.object(driver.os,'sched_setaffinity'))
                    stack.enter_context(mock.patch.object(driver.fcntl,'flock'))
                    stack.enter_context(mock.patch.object(driver,'machine',side_effect=lambda:dict(snapshot)))
                    stack.enter_context(mock.patch.object(driver,'digest',side_effect=fake_digest))
                    stack.enter_context(mock.patch.object(driver,'execute',side_effect=fake_execute))
                    stack.enter_context(mock.patch('builtins.open',side_effect=open_lock))
                    stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                    try: driver.main()
                    except RuntimeError as failure: error=failure
                self.assertEqual(len(launched),0 if defect=='hash-before' else 2 if defect is None else 1)
                self.assertEqual(len(list(output.glob('*/rocks'))),0 if defect in ('hash-before',None) else 1)
                self.assertEqual(error is None,defect is None)

    def test_schedule_vectors_and_mixed_tables(self):
        self.assertEqual([driver.identity('uniform', 100, 2, driver.SEED, i) for i in range(8)],
                         [(0,78),(1,16),(0,88),(0,13),(0,23),(0,54),(0,73),(0,75)])
        self.assertEqual(driver.identity('hot-cycle',256,2,driver.SEED,256),(1,0))
        self.assertEqual(driver.identity('hot-cycle',256,2,driver.SEED,512),(0,0))
        self.assertEqual(driver.distinct('hot-cycle',256,2,driver.SEED,1024),512)

    def test_scenario_distinct_counts(self):
        expected = {'hot256':256, 'hot1024':1024, 'cycle16k':16384,
                    'uniform16k':16384, 'skew16k':14105, 'stream':327680,
                    'width32':256, 'width33':256, 'width64':256, 'mixed2':512}
        for name, count in expected.items():
            fields=driver.expected_fields(name,1,'engine',262144,65536,driver.SEED)
            self.assertEqual(fields['distinct_identities_per_worker'],count)
            self.assertEqual(fields['verified_keys'],count)
        stream=driver.expected_fields('stream',1,'engine',262144,65536,driver.SEED)
        self.assertIsNone(stream['requested_keys_per_worker'])
        self.assertEqual(stream['effective_keys_per_worker'],327680)

    def test_domain_limits_and_unsupported_mode(self):
        for arguments in [('hot256',0,'engine',262144,65536),
                          ('hot256',33,'engine',262144,65536),
                          ('stream',32,'engine',262144,65536),
                          ('mixed2',1,'cache-ack',262144,65536),
                          ('hot256',1,'engine',0,65536),
                          ('hot256',1,'engine',1,255)]:
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                driver.expected_fields(*arguments,driver.SEED)

    def sample(self):
        expected=driver.expected_fields('hot256',1,'cache-ack',262144,65536,driver.SEED)
        measurement=dict(expected,pmu_min_running_ratio=1.0,max_worker_nanoseconds=262144000,
                         transactions_per_second=1000000.0,cycles_per_commit=123.0,
                         instructions_per_commit=456.0,applied_before_abort=1024,
                         queued_before_abort=expected['acknowledged']-1088)
        return measurement,expected

    def test_valid_sample_allows_nonatomic_tail_snapshot(self):
        measurement,expected=self.sample()
        driver.validate(measurement,expected)

    def test_every_echoed_field_is_checked(self):
        for field in self.sample()[1]:
            measurement,expected=self.sample()
            measurement[field]='mismatch'
            with self.subTest(field=field),self.assertRaises(RuntimeError):
                driver.validate(measurement,expected)

    def test_pmu_time_and_tail_fail_closed(self):
        for field,value in [('pmu_min_running_ratio',0.994),('pmu_min_running_ratio',float('nan')),
                            ('pmu_min_running_ratio',float('inf')),('max_worker_nanoseconds',0),
                            ('max_worker_nanoseconds',float('inf')),('max_worker_nanoseconds',float('nan')),
                            ('transactions_per_second',1000001.0),('cycles_per_commit',float('nan')),
                            ('instructions_per_commit',0),('queued_before_abort',999999999)]:
            measurement,expected=self.sample();measurement[field]=value
            with self.subTest(field=field,value=value),self.assertRaises(RuntimeError):
                driver.validate(measurement,expected)


if __name__=='__main__':
    unittest.main()
