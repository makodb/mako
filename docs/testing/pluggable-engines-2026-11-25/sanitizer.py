import json, os, pathlib, subprocess, sys
root=pathlib.Path('/var/tmp/mako-shard-rust'); mode=sys.argv[1]
base=root/('build_native_san_'+mode+'_05')
manifest=dict(line.split('=',1) for line in (base/'rust-sto-sanitizer-manifest.txt').read_text().splitlines() if '=' in line)
env=os.environ.copy(); tool=root/'build_toolchains/rustup/toolchains/nightly-2026-08-12-x86_64-unknown-linux-gnu/bin'
env.update(PATH=str(tool)+':'+env['PATH'], CC=str(base/'sanitizer-cc'), CXX='/usr/bin/clang++-22',
 MAKO_BUILD_DIR=str(base), LIBCXX_DIR='/usr/lib/llvm-22/lib', MAKO_MTREE_NATIVE_INTEGRATION='1',
 MAKO_MTREE_NATIVE_LIB_DIRS=str(base)+':/usr/lib/llvm-22/lib',
 MAKO_MTREE_NATIVE_LIBS='static:-bundle=mako_native_storage,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread',
 CARGO_TARGET_DIR=str(base/'rust-workspace-target'), CARGO_BUILD_JOBS='16', CARGO_BUILD_TARGET=manifest['target'],
 CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER='/usr/bin/clang-22', RUSTFLAGS=manifest['rustflags'], RUSTDOCFLAGS=manifest['rustflags'])
env['LD_LIBRARY_PATH']='/usr/lib/llvm-22/lib:'+env.get('LD_LIBRARY_PATH','')
if mode=='address': env['ASAN_OPTIONS']=manifest['asan_options']
else:
 env['TSAN_OPTIONS']=manifest['tsan_options']; env['ROCKSDB_LIB_DIR']=manifest['rocksdb_library_dir']
 env['LD_LIBRARY_PATH']=env['ROCKSDB_LIB_DIR']+':'+env['LD_LIBRARY_PATH']
out=root/'build_upper_development/pluggable-qualification'/mode; out.mkdir(parents=True,exist_ok=True)
command=[str(tool/'cargo'),'test','--locked','--manifest-path',str(root/'crates/mako-runtime/Cargo.toml'),'--features','replication,rocksdb-engine','--target',manifest['target'],'--test','participant','--test','coordinator','--test','replication','--test','engine_checkpoint','--test','rocks_local','--test','migration','--lib','--no-run','--message-format=json']
built=subprocess.run(command,env=env,text=True,capture_output=True)
(out/'build.log').write_text(built.stdout+built.stderr)
if built.returncode: print(built.stderr[-10000:]); sys.exit(built.returncode)
cases={'prepared_worker_panic_fences_admission_and_preserves_decision_and_migration_pin','global_lock_barrier_rejects_point_absence_and_scan_write_skew','a_busy_discovery_health_snapshot_does_not_refuse_an_authoritative_completion','client_resolves_a_saved_commit_without_repeating_body_or_leaking_migration_pin','durable_native_prepare_restores_commit_abort_and_absent_tombstone_after_restart','prepared_work_survives_listener_teardown_and_recovers_saved_commit_without_abort','frozen_prepare_rejects_new_writes_terms_and_changed_intent_and_retains_migration_pin','contract::elected_replica_restores_prepared_intent_and_obeys_the_saved_decision'}


cases.update({
 'local_checkpoint_replays_suffix_and_tombstones_without_rocks_user_rows',
 'checkpoint_covers_saved_complete_before_affine_install',
 'corrupt_engine_checkpoint_is_rejected_before_admission',
 'rocks_local_checkpoint_reopens_atomic_data_and_metadata',
 'persistent_log_rejects_a_proposal_from_another_admission_term',
 'migration_copies_masstree_into_rocksdb',
 'migration_copies_rocksdb_into_masstree',
 'migration_copies_between_rocksdb_engines',
 'conversion::tests::conversion_refuses_live_source_existing_target_and_corrupt_checkpoint',
 'conversion::tests::legacy_trimmed_log_converts_both_engines_and_roundtrips_prepares_receipts_tombstones',
})

def invoke(command, timeout=180):
 attempts=[]
 for attempt in range(20):
  result=subprocess.run(command,env=env,capture_output=True,text=True,timeout=timeout)
  output=result.stdout+result.stderr
  startup_failure=(mode=='thread' and result.returncode!=0 and not result.stdout.strip()
     and 'ThreadSanitizer: encountered an incompatible memory layout' in result.stderr
     and 'WARNING: ThreadSanitizer' not in result.stderr)
  if not startup_failure: return result,attempts
  attempts.append(dict(exit_code=result.returncode,stderr=result.stderr))
 return result,attempts

report=[]
for line in built.stdout.splitlines():
 item=json.loads(line)
 if item.get('reason')!='compiler-artifact' or not item.get('executable') or not item['profile']['test']: continue
 exe=item['executable']
 listing, listing_retries=invoke([exe,'--list','--format','terse'])
 if listing.returncode: print(listing.stderr); sys.exit(listing.returncode)
 listed=listing.stdout
 for line in listed.splitlines():
  name=line.removesuffix(': test')
  if name not in cases: continue
  result,startup_retries=invoke([exe,name,'--exact','--test-threads=1'])
  output=result.stdout+result.stderr; (out/(name+'.log')).write_text(output)
  passed=result.returncode==0 and '1 passed; 0 failed; 0 ignored' in result.stdout
  report.append(dict(test=name,passed=passed,exit_code=result.returncode,startup_retries=startup_retries,listing_retries=listing_retries))
  print(mode,name,'PASS' if passed else 'FAIL',flush=True)
  if not passed: print(output[-10000:]); (out/'report.json').write_text(json.dumps(report,indent=2)); sys.exit(1)
assert len(report)==len(cases),(len(report),len(cases))
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
