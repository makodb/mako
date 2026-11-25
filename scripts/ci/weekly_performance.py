#!/usr/bin/env python3
"""Weekly/manual workload measurements. Never called by compile or upper.

Preserve raw samples and environment metadata; compare medians only when the
environment and workload configuration match. Results are diagnostic, not a
correctness gate or a performance claim about shared CI hardware.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import time

ROOT=Path(__file__).resolve().parents[2]

def read(path):
    try: return Path(path).read_text().strip()
    except OSError: return 'unavailable'

def environment(seconds, samples):
    cpu=next((line.split(':',1)[1].strip() for line in read('/proc/cpuinfo').splitlines() if line.startswith('model name')), 'unknown')
    return {'cpu':cpu,'logical_cpus':len(os.sched_getaffinity(0)),
        'kernel':platform.release(),'architecture':platform.machine(),
        'cpu_quota':read('/sys/fs/cgroup/cpu.max'),'memory_limit':read('/sys/fs/cgroup/memory.max'),
        'governor':read('/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor'),
        'rustc':subprocess.check_output(['rustc','--version'],text=True).strip(),
        'profile':'Release','seconds':seconds,'samples':samples,
        'topologies':[{'workload':'micro','shards':2,'replicas':1,'workers':3},
                      {'workload':'tpcc','shards':2,'replicas':1,'workers':3},
                      {'workload':'micro','shards':2,'replicas':3,'workers':2}]}

def compare(current,baseline):
    if current['environment']!=baseline.get('environment'):
        return {'comparable':False,'reason':'environment or workload configuration changed'}
    ratios={name:current['medians'][name]/value for name,value in baseline['medians'].items()
            if value>0 and name in current['medians']}
    if len(ratios)!=len(current['medians']):
        return {'comparable':False,'reason':'baseline is incomplete'}
    return {'comparable':True,'throughput_ratio':ratios,'baseline_revision':baseline['revision']}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,default=Path(os.environ.get('BUILD_DIR','build_docker')))
    parser.add_argument('--output',type=Path,default=Path('build_docker/performance'))
    parser.add_argument('--baseline',type=Path)
    parser.add_argument('--update-baseline',action='store_true')
    parser.add_argument('--seconds',type=int,default=20)
    parser.add_argument('--samples',type=int,default=3)
    parser.add_argument('--fingerprint-only',action='store_true')
    args=parser.parse_args()
    if not (1<=args.seconds<=300 and 1<=args.samples<=10): parser.error('invalid duration/samples')
    os.sched_setaffinity(0,sorted(os.sched_getaffinity(0))[:4])
    env=environment(args.seconds,args.samples)
    fingerprint=hashlib.sha256(json.dumps(env,sort_keys=True).encode()).hexdigest()
    if args.fingerprint_only:
        print(fingerprint); return
    if not (Path('/.dockerenv').exists() or Path('/run/.containerenv').exists()):
        parser.error('run measurements inside the build container')
    args.output.mkdir(parents=True,exist_ok=True)
    binary=(ROOT/args.build/'mako_bench').resolve()
    report={'environment':env,'fingerprint':fingerprint,
        'revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
        'samples':{},'medians':{},'passed':False}
    started=time.monotonic()
    try:
        for topology in env['topologies']:
            name=f"{topology['workload']}-{topology['shards']}shards-{topology['replicas']}replicas"
            report['samples'][name]=[]
            # A separate warmup keeps startup/cache effects out of the samples.
            for sample in range(args.samples+1):
                seconds=min(5,args.seconds) if sample==0 else args.seconds
                command=['python3','examples/run_native.py',topology['workload'],'--build',str(args.build),
                    '--shards',str(topology['shards']),'--replicas',str(topology['replicas']),
                    '--workers',str(topology['workers']),'--duration-only','--seconds',str(seconds),'--keep',
                    '--timeout',str(seconds+180)]
                result=subprocess.run(command,cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,check=False)
                (args.output/f'{name}-{sample}.log').write_text(result.stdout)
                if result.returncode: raise RuntimeError(f'{name}: workload failed; see retained log')
                rows=[json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
                if len(rows)!=topology['shards']: raise RuntimeError('missing workload results')
                throughput=sum(sum(row['committed'])/row['elapsed_seconds'] for row in rows)
                if sample:
                    report['samples'][name].append({'commits_per_second':throughput,'shards':rows})
            report['medians'][name]=statistics.median(s['commits_per_second'] for s in report['samples'][name])
        if args.baseline and args.baseline.exists():
            report['comparison']=compare(report,json.loads(args.baseline.read_text()))
        else:
            report['comparison']={'comparable':False,'reason':'no baseline yet'}
        report['passed']=True
        if args.update_baseline:
            if not args.baseline: raise RuntimeError('--update-baseline requires --baseline')
            args.baseline.parent.mkdir(parents=True,exist_ok=True)
            args.baseline.write_text(json.dumps(report,indent=2)+'\n')
    finally:
        report['seconds']=round(time.monotonic()-started,3)
        (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'medians':report['medians'],'comparison':report['comparison']},indent=2))

if __name__=='__main__': main()
