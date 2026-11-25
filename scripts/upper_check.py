#!/usr/bin/env python3
"""Complete upper-layer positives, selected semantic controls and small scenarios.

Default wall deadline: 300s, including tool extraction and Cargo. --bootstrap
only warms downloads/build products, never supplies a passing check. Logs and a
JSON report live under $BUILD_DIR/upper/runs. No result is reused across runs.
"""
from pathlib import Path
import argparse
import ast
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get('BUILD_DIR', ROOT/'build_docker')).resolve()
ALLOWED = set('mako-protocol mako-protocol-core mako-sharding shard-host-core mako-clock mako-engine-api bincode serde serde_core serde_derive proc-macro2 unicode-ident quote syn synstructure convert_case verus_builtin verus_builtin_macros verus_syn verus_prettyplease verus_state_machines_macros vstd indexmap equivalent hashbrown parking_lot lock_api scopeguard parking_lot_core cfg-if libc smallvec'.split())
ALLOWED.add('mako-storage-api')
PACKAGES = ['-p','mako-protocol','-p','mako-protocol-core','-p','shard-host-core','-p','mako-storage-api']
VERSION = '0.2026.08.02.b677dd5'
SHA = '4c769256e888ee84bde85aae44d95c46bccbb8cf70e1d09f537b0d05fe965dee'
SUMMARY = re.compile(r'verification results:: (\d+) verified, (\d+) errors')
FAILURES = ('assertion failed','postcondition not satisfied','precondition not satisfied','invariant not satisfied')
INVALID = ('error[E','Resource limit','resource limit','internal compiler error')

def validate_proof(result, reject=False, minimum=1):
    summary = SUMMARY.search(result.stdout)
    if reject:
        valid = (result.returncode == 1 and summary and int(summary[2]) > 0
                 and any(s in result.stdout for s in FAILURES) and not any(s in result.stdout for s in INVALID))
    else:
        valid = result.returncode == 0 and summary and int(summary[1]) >= minimum and int(summary[2]) == 0
    if not valid:
        raise RuntimeError('expected semantic rejection' if reject else 'complete positive proof did not pass')

def audit_tree(output):
    seen = set()
    for line in output.splitlines():
        if not line.strip(): continue
        match = re.match(r'([\w-]+) v[^ ]+.*?\[([^\]]*)\]', line)
        if not match or match[1] not in ALLOWED:
            raise RuntimeError(f'unreviewed upper dependency: {line}')
        if match[1] == 'mako-sharding' and set(match[2].split(',')) != {'protocol'}:
            raise RuntimeError('upper sharding must enable only protocol')
        seen.add(match[1])
    if not seen: raise RuntimeError('empty dependency graph')
    return sorted(seen)

def source_inputs():
    files=set()
    for directory in ('src/cluster','tla/mako/src','crates/mako-protocol',
                      'crates/mako-protocol-core','crates/shard-host-core','crates/mako-storage-api'):
        files.update(p for p in (ROOT/directory).rglob('*') if p.is_file() and p.suffix in ('.rs','.toml','.lock'))
    import upper_protocol_proof as core
    files.update(ROOT/p for p in core.ADAPTERS)
    files.update(ROOT/p for p in ('crates/Cargo.toml','crates/Cargo.lock','scripts/upper_check.py',
        'scripts/upper_protocol_proof.py','scripts/native_sharding_proof_sources.py',
        'scripts/shard_host_core_proof.py','scripts/shard_host_core_trusted.txt',
        'tla/mako/scripts/verify_controls.py','ci/upper.sh'))
    return {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(files)}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bootstrap', action='store_true')
    parser.add_argument('--cpus', type=int, default=os.environ.get('MAKO_UPPER_CPUS', '4'),
                        help='logical CPU budget for Cargo and Verus; default MAKO_UPPER_CPUS or 4')
    args = parser.parse_args()
    if args.cpus < 1:
        parser.error('--cpus must be a positive integer')
    if not (Path('/.dockerenv').exists() or Path('/run/.containerenv').exists()):
        raise SystemExit('Use ./docker_build.sh ci upper')
    os.chdir(ROOT)
    # All descendants share this CPU budget, including rustc/Z3. Respect a
    # smaller affinity mask imposed by the container or calling process.
    cpus = sorted(os.sched_getaffinity(0))[:args.cpus]
    workers = len(cpus)
    os.sched_setaffinity(0, cpus)
    started = time.monotonic()
    budget = 1800 if args.bootstrap else 300
    runs = BUILD/'upper/runs'; runs.mkdir(parents=True,exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%dT%H%M%S')+'-',dir=runs))
    report = {'passed':False,'bootstrap':args.bootstrap,'cpu_affinity':cpus,
              'requested_cpus':args.cpus,'workers':workers,'budget_seconds':budget,'stages':[]}
    print(f'upper: using {workers} logical CPUs; requested {args.cpus}', flush=True)
    env = {k:v for k,v in os.environ.items() if not k.startswith('MAKO_MTREE')}
    env.update(CC='/bin/false',CXX='/bin/false',CARGO_BUILD_JOBS=str(workers), CARGO_TARGET_DIR=str(BUILD/'upper/target'))

    def run(name, command, cwd=ROOT, expected=0, machine=False):
        begin=time.monotonic(); print(f'upper: {name}',flush=True)
        remaining=budget-(begin-started)
        if remaining<=0: raise TimeoutError('upper wall deadline exhausted')
        process=subprocess.Popen(command,cwd=cwd,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE if machine else subprocess.STDOUT,start_new_session=True)
        try:
            stdout,stderr=process.communicate(timeout=remaining)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid,signal.SIGKILL)
            stdout,stderr=process.communicate()
            (output/'timeout.log').write_text(stdout+(stderr or ''))
            raise TimeoutError(f'{name}: upper wall deadline exhausted')
        result=subprocess.CompletedProcess(command,process.returncode,stdout)
        log=output/(str(len(report['stages']))+'-'+re.sub('[^a-zA-Z0-9_-]+','-',name)+'.log')
        log.write_text(result.stdout+(stderr or ''))
        report['stages'].append({'name':name,'seconds':round(time.monotonic()-begin,3),'exit':result.returncode,'log':log.name})
        print(f"upper: {name}: {report['stages'][-1]['seconds']}s",flush=True)
        if expected is not None and result.returncode!=expected:
            raise RuntimeError(f'{name} failed; {log}\n{result.stdout[-5000:]}')
        return result

    tools_dir=None
    try:
        report['revision']=run('revision',['git','rev-parse','HEAD']).stdout.strip()
        report['diff_sha256']=hashlib.sha256(run('working-diff',['git','diff','--ignore-submodules=all','HEAD']).stdout.encode()).hexdigest()
        # Include new source files, not just a clean-HEAD assertion.
        untracked=run('new-files',['git','ls-files','--others','--exclude-standard']).stdout.splitlines()
        report['untracked_sha256']={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in untracked if (ROOT/p).is_file()}
        rust=run('compiler',['rustc','--version']).stdout.strip(); report['rustc']=rust
        if not rust.startswith('rustc 1.97.1 '): raise RuntimeError('pinned Verus requires Rust 1.97.1')
        sysroot=run('sysroot',['rustc','--print','sysroot']).stdout.strip()
        env.update(VERUS_USE_RUSTUP='0',VERUS_SYSROOT=sysroot,LD_LIBRARY_PATH=sysroot+'/lib:'+env.get('LD_LIBRARY_PATH',''))
        cache=BUILD/'proof-tools'; cache.mkdir(parents=True,exist_ok=True)
        archive=cache/f'verus-{VERSION}-x86-linux.zip'
        if not archive.exists():
            run('download-verifier',['curl','--fail','--location','--proto','=https','--tlsv1.2',f'https://github.com/verus-lang/verus/releases/download/release/{VERSION}/{archive.name}','--output',str(archive)+'.download'])
            Path(str(archive)+'.download').replace(archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest()!=SHA: raise RuntimeError('Verus archive checksum mismatch')
        tools_dir=Path(tempfile.mkdtemp(prefix='upper-verified-',dir=cache))
        with zipfile.ZipFile(archive) as z: z.extractall(tools_dir)
        for name in ('verus','rust_verify','z3'): (tools_dir/'verus-x86-linux'/name).chmod(0o755)
        verifier=str(tools_dir/'verus-x86-linux/verus'); env['VERUS_PATH']=verifier
        # Imported audit utilities use these pinned values, not PATH lookup.
        os.environ.update(VERUS_PATH=verifier,VERUS_SYSROOT=sysroot)
        import shard_host_core_proof as host
        import upper_protocol_proof as core
        host.audit_sources(); host.audit_controls(); core.audit()
        report['source_inputs']=source_inputs()
        run('sharding-source-audit',['python3','scripts/native_sharding_proof_sources.py'])
        # The same validation routines must reject dependency and result bypasses.
        for bad in ('srpc v0.1.0 []','mtree-sys v0.1.0 []','mako-sharding v0.1.0 [engine-host,protocol]'):
            try: audit_tree(bad)
            except RuntimeError: pass
            else: raise RuntimeError('dependency audit control accepted forbidden implementation')
        for bad in ('', 'verification results:: 0 verified, 0 errors', 'verification results:: 22 verified, 1 errors'):
            try: validate_proof(subprocess.CompletedProcess([],0,bad))
            except RuntimeError: pass
            else: raise RuntimeError('proof result audit accepted an empty/failing run')
        for bad in ('error[E0001]\nverification results:: 0 verified, 1 errors', 'Resource limit\nverification results:: 0 verified, 1 errors'):
            try: validate_proof(subprocess.CompletedProcess([],1,bad),reject=True)
            except RuntimeError: pass
            else: raise RuntimeError('control audit accepted a nonsemantic failure')
        manifests=[('workspace',['--manifest-path','crates/Cargo.toml',*PACKAGES]),
                   ('sharding',['--manifest-path','src/cluster/Cargo.toml','--no-default-features','--features','protocol'])]
        for name,selection in manifests:
            tree=run(name+'-dependencies',['cargo','tree','--locked',*selection,'-e','normal,build,dev','--prefix','none','--format','{p} [{f}]'],machine=True)
            report[name+'_dependencies']=audit_tree(tree.stdout)
            artifacts=run(name+'-compile',['cargo','test','--locked',*selection,'--no-run','--message-format=json'])
            tests=[]
            for line in artifacts.stdout.splitlines():
                try: item=json.loads(line)
                except json.JSONDecodeError: continue
                if item.get('reason')!='compiler-artifact': continue
                manifest=Path(item['manifest_path'])
                import tomllib
                package=tomllib.loads(manifest.read_text())['package']['name']
                if package not in ALLOWED: raise RuntimeError(f'forbidden compiled package: {package}')
                if package=='mako-sharding' and item['features']!=['protocol']: raise RuntimeError('native sharding feature compiled')
                if item['profile']['test'] and item.get('executable'): tests.append(item['executable'])
            if not tests: raise RuntimeError('no executable scenarios found')
            if not args.bootstrap:
                total=0
                for binary in tests:
                    result=run(name+'-'+Path(binary).name,[binary,'--test-threads=1'])
                    summaries=re.findall(r'test result: ok\. (\d+) passed; (\d+) failed; (\d+) ignored;',result.stdout)
                    if not summaries or any(int(s[1]) or int(s[2]) for s in summaries): raise RuntimeError('tests skipped or failed')
                    total+=sum(int(s[0]) for s in summaries)
                if total<({'workspace':21,'sharding':24}[name]): raise RuntimeError('scenario coverage decreased')
                report[name+'_tests']=total
        host.check_pins()
        if args.bootstrap:
            report['warmed']=True
            return

        def proof(name,entry,cwd=ROOT,reject=False,module=None,function=None,minimum=1):
            command=[verifier,'--sysroot',sysroot,'--crate-type=lib','--edition=2021','--no-cheating','--num-threads',str(workers),'--triggers-mode','silent',entry]
            if reject: command+=['--verify-only-module',module,'--verify-function',function]
            result=run(name,command,cwd=cwd,expected=None)
            validate_proof(result,reject,minimum)
            print(SUMMARY.search(result.stdout)[0],flush=True)
        proof('production-sharding','src/cluster/lib.rs',minimum=1495)
        proof('storage-host-core',host.ENTRY,minimum=40)
        proof('transaction-protocol-core',core.ENTRY,minimum=46)
        core.controls(proof)
        # Reuse existing semantic mutants, with full positives already checked.
        tree=ast.parse((ROOT/'tla/mako/scripts/verify_controls.py').read_text())
        native=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='NATIVE_CONTROLS' for t in n.targets))
        selected=[('sharding',native[i]) for i in (0,8)]+[('host',host.CONTROLS[i]) for i in (0,3)]
        for family,(name,filename,before,after,(module,function)) in selected:
            with tempfile.TemporaryDirectory(prefix='upper-existing-control-') as tmp:
                dest=Path(tmp)
                if family=='sharding':
                    for path in ('src/cluster','crates/shard-host-core/src','tla/mako/src'):
                        shutil.copytree(ROOT/path,dest/path)
                    entry='src/cluster/lib.rs'; path=dest/filename
                else:
                    shutil.copytree(host.CRATE,dest/'crates/shard-host-core')
                    entry=host.ENTRY; path=dest/'crates/shard-host-core'/filename
                source=path.read_text()
                if source.count(before)!=1: raise RuntimeError(f'{name}: mutation site changed')
                path.write_text(source.replace(before,after))
                proof(name,entry,cwd=dest,reject=True,module=module,function=function)
        if time.monotonic()-started>budget: raise TimeoutError('upper wall deadline exceeded')
        if source_inputs()!=report['source_inputs']: raise RuntimeError('checked inputs changed during verification')
        report['passed']=True
    except BaseException as error:
        report['error']=str(error)
        raise
    finally:
        report['seconds']=round(time.monotonic()-started,3)
        (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        if tools_dir: shutil.rmtree(tools_dir)
        print(f"upper: {'PASS' if report['passed'] else 'BOOTSTRAP' if report.get('warmed') else 'FAIL'} {report['seconds']}s; {output/'report.json'}",flush=True)

if __name__=='__main__': main()
