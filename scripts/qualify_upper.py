#!/usr/bin/env python3
"""Measure five full checks, including representative incremental recompiles.

Touching a source changes only its mtime, so Cargo rebuilds its dependents while
the verified semantics remain fixed. Each run still checks every positive
obligation, semantic control and scenario. Run after ci/upper.sh --bootstrap.
"""
from pathlib import Path
import json
import os
import subprocess

ROOT=Path(__file__).resolve().parents[1]
BUILD=Path(os.environ.get('BUILD_DIR',ROOT/'build_docker')).resolve()
CASES=[('unchanged',None),('transaction-core','crates/mako-protocol-core/src/admission.rs'),
       ('protocol-client','crates/mako-protocol/src/gateway_client.rs'),
       ('migration','src/cluster/migration.rs'),('shared-contract','crates/mako-protocol/src/contracts.rs')]

def main():
    reports=[]
    (BUILD/'upper/qualification.json').write_text('[]\n')
    for name,relative in CASES:
        before=set((BUILD/'upper/runs').glob('*/report.json'))
        path=ROOT/relative if relative else None
        saved=path.stat() if path else None
        try:
            if path: path.touch()
            subprocess.run(['bash','ci/upper.sh'],cwd=ROOT,check=True)
        finally:
            if path: os.utime(path,ns=(saved.st_atime_ns,saved.st_mtime_ns))
        added=set((BUILD/'upper/runs').glob('*/report.json'))-before
        if len(added)!=1: raise RuntimeError('missing or ambiguous qualification result')
        report_path=added.pop(); report=json.loads(report_path.read_text())
        if not report['passed'] or report['seconds']>300: raise RuntimeError(f'{name} failed qualification')
        reports.append({'case':name,'seconds':report['seconds'],'report':str(report_path)})
        (BUILD/'upper/qualification.json').write_text(json.dumps(reports,indent=2)+'\n')
    print(json.dumps(reports,indent=2))

if __name__=='__main__': main()
