#!/usr/bin/env python3
"""Recompile only missing selected build objects after interrupted Ninja."""
import json
from pathlib import Path
import shlex
import subprocess
import sys

root = Path('/home/users/shuai/mako/.claude/worktrees/masstree-rocks')
build = root / 'build_c22'
sys.path.insert(0, str(root / 'scripts'))
import mako_local_fingerprint as verifier

database = verifier.read_compile_commands(build / 'compile_commands.json')
roots = verifier.select_compile_roots(database, root, build)
missing = {str((build / entry.output).resolve()) for entry in roots if not (build / entry.output).is_file()}
raw = json.loads((build / 'compile_commands.json').read_text())
selected = []
for entry in raw:
    args = entry.get('arguments') or shlex.split(entry['command'])
    output = Path(entry['directory']) / args[args.index('-o') + 1]
    if str(output.resolve()) in missing:
        selected.append((entry, args))
assert len(selected) == len(missing), (len(selected), missing)
print(json.dumps({'missing': sorted(missing), 'commands': [a for _, a in selected]}, indent=2), flush=True)
for entry, args in selected:
    subprocess.run(args, cwd=entry['directory'], check=True)
print('Missing objects rebuilt; original archive/fingerprint verification remains required.', flush=True)
