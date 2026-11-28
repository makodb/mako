import subprocess,pathlib,concurrent.futures,json
r=pathlib.Path.cwd(); source=r/'build_sto_reference/source'; build=r/'build_sto_reference/objects';build.mkdir(exist_ok=True)
flags=['/usr/bin/clang++-22','-std=c++23','-stdlib=libc++','-O3','-DNDEBUG','-march=native','-ffunction-sections','-fdata-sections','-fPIC','-D_LIBCPP_NO_ABI_TAG','-DHASHTABLE=0','-DSTO_OPACITY=0','-DREAD_MY_WRITES=1','-DRUSTYCPP_DISABLE_ARC_LOG','-DUSE_JEMALLOC','-DDISABLE_DISK','-DCONFIG_H="'+str(source/'src/mako/config/config-perf.h')+'"','-include',str(r/'build_sto_perf/generated/masstree/config.h')]
for p in [source/'src',source/'src/mako',source/'src/masstree',source/'src/srpc',source/'third-party/yaml-cpp/include',r/'third-party/rusty-cpp/include',r/'build_sto_perf/generated/masstree',r/'build_sto_perf/generated']:
 flags+=['-I',str(p)]
modules=json.loads((r/'build_sto_reference/rusty/CMakeFiles/rusty.dir/CXXModules.json').read_text())
flags += ['-fmodule-file=rusty='+str(r/'build_sto_reference/rusty/CMakeFiles/rusty.dir/rusty.pcm')]
for name, info in modules['references'].items():
 flags += ['-fmodule-file='+name+'='+str(r/'build_sto_reference/rusty'/info['path'])]
files=['src/mako/benchmarks/sto_masstree_compare.cc','src/mako/sto/Transaction.cc','src/mako/sto/TRcu.cc','src/mako/sto/Packer.cc','src/mako/sto/MassTrans.cc','src/mako/benchmarks/sync_util_init.cc','src/mako/sto/MakoTimestampClock.cc','src/mako/lib/message.cc',str(r/'build_sto_reference/local_environment.cc')]
def compile(p):
 cmd=flags+['-c',str(source/p),'-o',str(build/(pathlib.Path(p).name+'.o'))]
 (build/(pathlib.Path(p).name+'.command.json')).write_text(json.dumps(cmd))
 with (build/(pathlib.Path(p).name+'.log')).open('w') as f: rc=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT).returncode
 return p,rc
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as executor:
 results=list(executor.map(compile,files))
print(results)
raise SystemExit(any(rc for p,rc in results))
