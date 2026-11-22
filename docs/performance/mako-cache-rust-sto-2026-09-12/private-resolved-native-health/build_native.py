#!/usr/bin/env python3
"""Build only the explicit private health-proof boundary, recording its inputs."""
import hashlib
import json
import pathlib
import shlex
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
BUILD = pathlib.Path('/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22')
SOURCE = BUILD.parent / 'src/mako/storage/mtree_abi.cc'
entry = next(x for x in json.loads((BUILD / 'compile_commands.json').read_text())
             if x['output'] == 'CMakeFiles/mako.dir/src/mako/storage/mtree_abi.cc.o')
original = shlex.split(entry['command'])
command = []
removed = []
index = 0
while index < len(original):
    arg = original[index]
    if arg in ('-o', '-c'):
        removed.extend(original[index:index + 2])
        index += 2
        continue
    if arg in ('-MD', '-MP') or arg.startswith('@'):
        removed.append(arg)
    else:
        command.append(arg)
    index += 1
obj = HERE / 'native_health_proof.o'
archive = HERE / 'libmtree_health_proof.a'
command += ['-DMAKO_NATIVE_HEALTH_PROOF=1', '-c', str(HERE / 'native_health_proof.cc'), '-o', str(obj)]
subprocess.run(command, cwd=BUILD, check=True)
ar = ['/home/users/shuai/.linuxbrew/opt/llvm@22/bin/llvm-ar', 'rcs', str(archive), str(obj)]
subprocess.run(ar, check=True)
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
report = {'scope': 'private test-only real native poison transition; not native exception injection',
          'original_command': original, 'removed_output_dependency_module_scan_args': removed,
          'compile_command': command, 'compile_cwd': str(BUILD), 'archive_command': ar,
          'source_sha256': digest(SOURCE), 'helper_sha256': digest(HERE / 'native_health_proof.cc'),
          'archive_sha256': digest(archive),
          'production_archives': {str(path): digest(path) for path in
                                  (BUILD / 'libmako.a', BUILD / 'src/masstree/libmasstree.a')}}
(HERE / 'native-build.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'archive': str(archive), 'sha256': digest(archive)}))
