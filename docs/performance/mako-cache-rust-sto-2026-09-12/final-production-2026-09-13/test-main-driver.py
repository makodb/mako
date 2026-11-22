import builtins
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

spec=importlib.util.spec_from_file_location('main_driver',Path(__file__).with_name('run-main-pairs.py'))
driver=importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


def sample(config,engine='sto'):
    result={field:0 for field in driver.SAMPLE_FIELDS}
    result.update(config,engine=engine)
    writes=config['workload']!='read'
    commits=config['target']*config['workers']
    total=(config['target']+config['warmup'])*config['workers']
    logs=(config['keyspace']+63)//64+(total if writes else 0)
    # Each phase spawns new threads. Only threads with applied writes persist
    # lane metadata; the parent thread always writes the seed records.
    lane_count=1+2*config['workers'] if writes else 1
    result.update(commits=commits,logical_ops=commits*config['tx']*(2 if config['workload']=='rmw' else 1),
                  measured_mutation_bytes=commits*config['tx']*136 if writes else 0,
                  total_mutation_bytes=(config['keyspace']+(total*config['tx'] if writes else 0))*136,
                  live_user_bytes=config['keyspace']*136,checksum=driver.expected_checksum(config),
                  data_keys=config['keyspace'],log_keys=logs,metadata_keys=1+lane_count,
                  backend_keys=logs+config['keyspace']+1+lane_count,
                  log_key_bytes=logs*22,log_value_bytes=logs*42+(config['keyspace']+(total*config['tx'] if writes else 0))*153,
                  backend_key_bytes=logs*22+config['keyspace']*30+14+16*lane_count,
                  backend_value_bytes=logs*42+(config['keyspace']+(total*config['tx'] if writes else 0))*153+config['keyspace']*175+46+54*lane_count,
                  duration_ns=1000,attempt_p50_ns=1,attempt_p99_ns=2,
                  logical_p50_ns=1,logical_p99_ns=2,drain_ns=1,physical_bytes=4096)
    return result


def recovery(config,engine='sto'):
    return {k:v for k,v in config.items() if k not in ('target','warmup')} | dict(
        engine=engine,checksum=driver.expected_checksum(config),open_ns=1,validation_ns=1)


def encode(record,protocol):
    return protocol+' '+ ' '.join(f'{k}={v}' for k,v in sorted(record.items()))+'\n'


class MainChecks(unittest.TestCase):
    def test_fresh_phase_worker_lanes_have_exact_metadata_extents(self):
        # Literal expectations keep this test independent of the launcher's
        # lane-count formula. Tuples hold workers, keys, key bytes, value bytes.
        writing_metadata=[(1,4,62,208),(2,6,94,316),(4,10,158,532),
                          (8,18,286,964),(16,34,542,1828),(32,66,1054,3556)]
        for workers,writing_keys,writing_key_bytes,writing_value_bytes in writing_metadata:
            for workload in ('read','write','rmw'):
                for tx in (1,4):
                    with self.subTest(workers=workers,workload=workload,tx=tx):
                        config=driver.configuration(workload,tx,'low',workers)
                        record=sample(config)
                        metadata=(2,30,100) if workload=='read' else (
                            writing_keys,writing_key_bytes,writing_value_bytes)
                        self.assertEqual(record['metadata_keys'],metadata[0])
                        self.assertEqual(record['backend_keys']-record['log_keys']-record['data_keys'],metadata[0])
                        self.assertEqual(record['backend_key_bytes']-record['log_key_bytes']-record['data_keys']*30,metadata[1])
                        self.assertEqual(record['backend_value_bytes']-record['log_value_bytes']-record['data_keys']*175,metadata[2])
                        driver.validate_sample(record,config,'sto')
                        for adjustment in (-1,1):
                            for field in ('metadata_keys','backend_keys','backend_key_bytes','backend_value_bytes'):
                                invalid=dict(record);invalid[field]+=adjustment
                                with self.subTest(field=field,adjustment=adjustment),self.assertRaises(RuntimeError):
                                    driver.validate_sample(invalid,config,'sto')
                        if workload!='read':
                            stale=dict(record,metadata_keys=2,
                                       backend_keys=record['log_keys']+record['data_keys']+2,
                                       backend_key_bytes=record['log_key_bytes']+record['data_keys']*30+30,
                                       backend_value_bytes=record['log_value_bytes']+record['data_keys']*175+100)
                            with self.assertRaises(RuntimeError):
                                driver.validate_sample(stale,config,'sto')

    def test_matrix_and_bounded_counts(self):
        self.assertEqual(len(driver.MATRIX),12)
        self.assertEqual(len(set(driver.MATRIX)),12)
        for cell in driver.MATRIX:
            config=driver.configuration(*cell)
            driver.validate_sample(sample(config),config,'sto')
            driver.validate_recovery(recovery(config),sample(config),config,'sto')
            if config['workload']!='read':
                self.assertLessEqual(config['workers']*config['target']*config['tx'],262144)

    def test_exact_protocol_schema_and_canonical_integers(self):
        record=sample(driver.configuration('rmw',4,'high',4))
        text=encode(record,driver.SAMPLE_PROTOCOL)
        self.assertEqual(driver.parse(text,driver.SAMPLE_PROTOCOL,driver.SAMPLE_FIELDS),record)
        malformed=[text+'\n',text.replace(' ', '  ',1),text.replace('\n','\r\n'),
                   text.rstrip()+' engine=sto\n',text.rstrip()+' extra=1\n',
                   text.replace(' tx=4',' tx=04'),text.replace(' tx=4',' tx=-4'),
                   text.replace(' tx=4',' tx=18446744073709551616'),
                   text.replace(' tx=4',''),text.replace('mako-m1-sample-v3','wrong',1)]
        for invalid in malformed:
            with self.subTest(text=invalid),self.assertRaises(RuntimeError):
                driver.parse(invalid,driver.SAMPLE_PROTOCOL,driver.SAMPLE_FIELDS)

    def test_exact_sample_configuration_accounting_and_durations(self):
        config=driver.configuration('rmw',4,'high',4)
        changes={key:'bad' if isinstance(value,str) else value+1 for key,value in config.items()}
        changes.update(engine='bad',commits=0,logical_ops=0,measured_mutation_bytes=0,
                       total_mutation_bytes=0,live_user_bytes=0,checksum=0,data_keys=0,log_keys=0,
                       backend_keys=0,backend_key_bytes=0,backend_value_bytes=0,
                       log_key_bytes=0,log_value_bytes=0,metadata_keys=0,
                       duration_ns=0,attempt_p50_ns=0,attempt_p99_ns=0,
                       logical_p50_ns=0,logical_p99_ns=0,drain_ns=0,physical_bytes=0)
        for field,value in changes.items():
            record=sample(config);record[field]=value
            with self.subTest(field=field),self.assertRaises(RuntimeError):
                driver.validate_sample(record,config,'sto')

    def test_every_recovery_field_is_checked(self):
        config=driver.configuration('read',4,'low',4)
        for field in driver.RECOVERY_FIELDS:
            record=recovery(config)
            record[field]='bad' if isinstance(record[field],str) else 0 if field.endswith('_ns') else record[field]+1
            with self.subTest(field=field),self.assertRaises(RuntimeError):
                driver.validate_recovery(record,sample(config),config,'sto')

    def test_failure_keeps_database_and_success_removes_it(self):
        for defect in ('sample','recovery','hash',None):
            with self.subTest(defect=defect),tempfile.TemporaryDirectory() as temporary,tempfile.TemporaryFile() as lock:
                root=Path(temporary);out=root/'out';out.mkdir()
                binaries={arm:root/arm for arm in ('c-abi','sto')}
                for binary in binaries.values():binary.touch()
                argv=['driver','--output',str(out),'--c-abi',str(binaries['c-abi']),
                      '--sto',str(binaries['sto']),'--repetitions','1']
                config=driver.configuration('read',1,'low',1)
                calls=[]
                real_open=builtins.open
                def open_lock(path,*args,**kwargs):
                    return contextlib.nullcontext(lock) if str(path)=='/var/tmp/mako-zoo2-perf-shuai.lock' else real_open(path,*args,**kwargs)
                def execute(command,directory):
                    calls.append(command);phase=directory.name
                    database=directory.parent/'rocks'
                    identity='sto' if command[6]==str(binaries['sto']) else 'mako-local/sto-masstrans'
                    if phase=='sample':
                        database.mkdir();record=sample(config,identity);protocol=driver.SAMPLE_PROTOCOL
                    else:
                        record=recovery(config,identity);protocol=driver.RECOVERY_PROTOCOL
                    if defect==phase or defect=='recovery' and phase=='recover':record['checksum']+=1
                    (directory/'stdout.txt').write_text(encode(record,protocol))
                    return {'completed':True,'returncode':0}
                with contextlib.ExitStack() as stack:
                    for target,attribute,value in [
                        (sys,'argv',argv),(driver,'MATRIX',[('read',1,'low',1)]),
                    ]:stack.enter_context(mock.patch.object(target,attribute,value))
                    stack.enter_context(mock.patch.object(driver.socket,'gethostname',return_value='zoo-002'))
                    stack.enter_context(mock.patch.object(driver.os,'sched_setaffinity'))
                    stack.enter_context(mock.patch.object(driver.fcntl,'flock'))
                    stack.enter_context(mock.patch('builtins.open',side_effect=open_lock))
                    stack.enter_context(mock.patch.object(driver.common,'machine',side_effect=lambda:{'boost':'0','governors':{str(c):'schedutil' for c in range(64)}}))
                    stack.enter_context(mock.patch.object(driver.common,'digest',side_effect=lambda path:'changed' if defect=='hash' and calls and path in binaries.values() else 'frozen'))
                    stack.enter_context(mock.patch.object(driver.common,'execute',side_effect=execute))
                    stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                    error=None
                    try:driver.main()
                    except RuntimeError as failure:error=failure
                self.assertEqual(error is None,defect is None)
                self.assertEqual(len(list(out.glob('*/rocks'))),0 if defect is None else 1)
                self.assertEqual(len(calls),4 if defect is None else 2 if defect=='recovery' else 1)


if __name__=='__main__':unittest.main()
