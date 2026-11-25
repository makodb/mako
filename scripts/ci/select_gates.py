#!/usr/bin/env python3
"""Select correctness jobs by owned source. Schedules/manual runs select all.

Unknown source/build paths fail toward coverage; docs alone need no builds.
The upper suite is independent of component compilation. Shared contracts
trigger their adapter/component suites as well as the upper check.
"""
import argparse
import json
import os
import subprocess

NAMES = ('upper','components','replication','srpc','integration','extended','miri')
CONTRACTS = ('crates/mako-protocol/src/contracts.rs', 'crates/mako-protocol-core/',
             'crates/mako-engine-api/', 'crates/shard-host-core/',
             'src/cluster/storage.rs','src/cluster/transfer.rs','src/cluster/types.rs')

def select(paths, event='pull_request'):
    gates = {name:event in ('schedule','workflow_dispatch') for name in NAMES}
    for path in paths:
        if path.startswith(('docs/','.claude/')) or path.endswith('.md'):
            continue
        if path.startswith(CONTRACTS):
            for name in ('upper','components','replication','srpc','integration'): gates[name]=True
        elif path.startswith(('src/cluster/runtime', 'src/cluster/host', 'src/cluster/ffi',
                              'src/cluster/wire', 'src/cluster/metadata', 'src/cluster/rocks_metadata',
                              'src/cluster/gateway_ffi', 'src/cluster/lib.rs', 'src/cluster/Cargo.')):
            gates['upper']=gates['integration']=True
        elif path.startswith(('crates/mako-protocol/','src/cluster/','tla/mako/',
                              'scripts/upper_', 'ci/upper.sh')):
            gates['upper']=True
        elif path.startswith(('src/srpc/','third-party/rusty-cpp/','third-party/lion/')):
            gates['srpc']=gates['integration']=True
        elif path.startswith(('src/deptran/raft/','crates/raft-host/','crates/sto-rocksdb/',
                              'crates/rocksdb-raft-service/','ci/rocksdb_raft.sh')):
            gates['replication']=gates['integration']=True
        elif path.startswith(('crates/mako-runtime/','examples/','tests/')):
            gates['integration']=True
        elif path.startswith(('crates/sto-','crates/masstree/','crates/mtree-sys/',
                              'crates/mtx-sys/','crates/mrx','crates/mako-cache',
                              'src/masstree/','src/sto/')):
            gates['components']=gates['integration']=True
            if path.startswith(('crates/sto-core/','crates/sto-test-datatypes/')): gates['miri']=True
        else:
            for name in NAMES: gates[name]=True
    return gates

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--event',required=True)
    parser.add_argument('--base',default='')
    parser.add_argument('--head',default='HEAD')
    args=parser.parse_args()
    paths=[]
    event=args.event
    if event not in ('schedule','workflow_dispatch'):
        if not args.base or set(args.base)=={'0'}:
            event='workflow_dispatch'
        else:
            paths=subprocess.check_output(['git','diff','--name-only','--no-renames',args.base,args.head],text=True).splitlines()
    gates=select(paths,event)
    print(json.dumps({'paths':paths,'gates':gates},indent=2))
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'],'a') as output:
            for name,value in gates.items(): output.write(f'{name}={str(value).lower()}\n')
if __name__=='__main__': main()
