import subprocess,json,pathlib
p=pathlib.Path.cwd();b=p/'build_sto_reference'
cmd=json.loads((b/'objects/sto_masstree_compare.cc.command.json').read_text())
cmd[cmd.index('-c')+1]=str(b/'string_compare.cc');cmd[-1]=str(b/'string_compare.o')
(b/'string_compile.command.json').write_text(json.dumps(cmd,indent=2)+'\n')
subprocess.run(cmd,check=True)
# Preserve the recorded order independently of parallel compilation timing.
objs=[str(b/'objects'/name) for name in ['TRcu.cc.o', 'Packer.cc.o', 'message.cc.o', 'MakoTimestampClock.cc.o', 'local_environment.cc.o', 'MassTrans.cc.o', 'sync_util_init.cc.o', 'Transaction.cc.o']]
cmd=['/usr/bin/clang++-22','-stdlib=libc++','-Wl,--gc-sections',str(b/'string_compare.o'),*objs,'-Wl,--start-group',*[str(b/'rusty'/name) for name in ['lib__cmake_cxx_std_23.a', 'libstring_port.a', 'libcell_port.a', 'libstd_port_hashbrown.a', 'libstd_port.a', 'libvec_port.a', 'librusty_async.a', 'libvec_deque_port.a', 'librc_port.a', 'liblinked_list_port.a', 'libbinary_heap_port.a', 'libarc_port.a', 'libbtree_port.a', 'librusty.a']],str(p/'build_sto_perf/libmako_native_storage.a'),'-Wl,--end-group','-L/usr/lib/llvm-22/lib','-Wl,-rpath,/usr/lib/llvm-22/lib','-lc++abi','-ljemalloc','-lnuma','-lpthread','-o',str(b/'cpp_string_clean')]
(b/'string_link.command.json').write_text(json.dumps(cmd,indent=2)+'\n');subprocess.run(cmd,check=True)
