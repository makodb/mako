#!/usr/bin/env python3
"""Generate the `extern "C"` export layer over RaftServerBase.

Reads the `pub fn` signatures from src/deptran/raft/src/server_h.rs for the
methods listed below and prints one `pub unsafe extern "C" fn raft_server_<snake>`
per method, forwarding to it. `&self` -> `s: *const RaftServerBase`, `&mut self`
-> `s: *mut RaftServerBase`, `&T` params -> `*const T` (dereferenced at the
call), `&mut T` returns -> `*mut T`, `&T` returns -> `*const T`. Non-trivial
C++ objects a method takes BY VALUE cross by pointer and are copied inside
(BY_VALUE_CARRIERS): an Arc handle through its Clone, everything else through
its clone kernel into a default-constructed slot, so no export passes a
non-trivial object by value. Output is spliced into server_cc.rs's export
block; re-run when a method's signature changes.
"""
import pathlib
import re, sys

RS = str(pathlib.Path(__file__).resolve().parent.parent /
         'src' / 'deptran' / 'raft' / 'src' / 'server_h.rs')
INTERFACE = ['set_site_identity', 'set_commo', 'reg_learner_action', 'EnsureSetup', 'WaitForStartup',
             'PrepareForShutdown', 'IsLeader', 'GetLeaderHint', 'SetPreferredLeader',
             'RegisterLeaderChangeCallback', 'IsRpcReady', 'SiteId', 'PartitionId',
             'CommitIndex', 'Start', 'ServeVote', 'ServeAppendEntries', 'ServeInstallSnapshot',
             'SetStateMachineSnapshotCallbacks']
KERNEL_CALLED = ['ApplyThreadLoop', 'BindReplicationWakeOwner', 'FailStop', 'InitializeSnapshotManagerLocked',
                 'InstallSnapshotReplyAccepted', 'OnInstallSnapshotLocked', 'SetupInternal', 'StartElectionTimer']


def snake(name):
    return re.sub(r'(?<!^)(?=[A-Z])', '_', name).lower()


def signature(rs, name):
    m = re.search(r'^\s+(?:pub )?fn ' + name + r'\((.*?)\)(\s*->\s*[^{]+)?\s*\{', rs, re.S | re.M)
    if not m:
        sys.exit(f'no signature for {name}')
    params = [p.strip() for p in re.sub(r'\s+', ' ', m.group(1)).split(',') if p.strip()]
    ret = (m.group(2) or '').strip().lstrip('->').strip()
    return params, ret


def export(rs, name):
    params, ret = signature(rs, name)
    recv = params[0]
    assert recv in ('&self', '&mut self'), (name, recv)
    s_ty = '*const RaftServerBase' if recv == '&self' else '*mut RaftServerBase'
    out_params = [f's: {s_ty}']
    args = []
    pre, post = [], []
    for p in params[1:]:
        pname, pty = [x.strip() for x in p.split(':', 1)]
        if pty.startswith('&mut '):
            # Copy-in / copy-out through a local: the emitter lowers `&mut *p`
            # to the pointer itself, but passes `&mut local` as the lvalue the
            # method's `T&` wants. Same value either way.
            out_params.append(f'{pname}: *mut {pty[5:]}'); args.append(f'&mut {pname}_slot')
            pre.append(f'    let mut {pname}_slot: {pty[5:]} = *{pname};')
            post.append(f'    *{pname} = {pname}_slot;')
        elif pty.startswith('&'):
            out_params.append(f'{pname}: *const {pty[1:]}'); args.append(f'&*{pname}')
        elif pty in BY_VALUE_CARRIERS:
            # A carrier crosses borrowed, by pointer, and is copied inside: the
            # caller keeps its own reference.
            out_params.append(f'{pname}: *const {pty}')
            kernel = BY_VALUE_CARRIERS[pty]
            if kernel is None:
                pre.append(f'    let {pname}_copy: {pty} = (*{pname}).clone();')
            else:
                pre.append(f'    let mut {pname}_copy: {pty} = Default::default();')
                pre.append(f'    {kernel}({pname}, &mut {pname}_copy as *mut {pty});')
            args.append(f'{pname}_copy')
        else:
            out_params.append(f'{pname}: {pty}'); args.append(pname)
    call = f'(*s).{name}({", ".join(args)})'
    if ret.startswith('&mut '):
        ret_c, call = f'*mut {ret[5:]}', f'{call} as *mut {ret[5:]}'
    elif ret.startswith('&'):
        ret_c, call = f'*const {ret[1:]}', f'{call} as *const {ret[1:]}'
    else:
        ret_c = ret
    ret_s = f' -> {ret_c}' if ret_c else ''
    head = f'pub unsafe extern "C" fn raft_server_{snake(name)}('
    sig = head + ', '.join(out_params) + ')' + ret_s + ' {'
    if len(sig) > 78:
        indent = ' ' * len(head)
        sig = head + (',\n' + indent).join(out_params) + ')' + ret_s + ' {'
    if post:
        body = '\n'.join(pre + ([f'    let result = {call};'] if ret_c else [f'    {call};']) + post + (['    result'] if ret_c else []))
    elif pre:
        body = '\n'.join(pre + [f'    {call}'])
    else:
        body = f'    {call}'
    return (f'/// # Safety\n/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.\n'
            f'#[no_mangle]\n{sig}\n{body}\n}}')


# Carriers a method takes by value, and how the export copies one. An Arc
# handle (None) has a Clone: the copy constructor under the transpiler, the
# facade's clone kernel under rustc. A Command has no Clone, so its copy is the
# kernel named here, into a default-constructed slot.
BY_VALUE_CARRIERS = {'rusty::RaftPollThreadPtr': None,
                     'rusty::RaftIntEventPtr': None,
                     'rusty::RaftCommand': 'raft_command_clone_into',
                     'rusty::RaftSnapshotManagerPtr': 'raft_snapshot_manager_ptr_clone_into'}
# The four std::function carriers are NOT here: a libc++ std::function is not
# bitwise-relocatable, so the setters take them by reference and copy in place.

SCALARS = {'u8': 'uint8_t', 'u16': 'uint16_t', 'u32': 'uint32_t', 'u64': 'uint64_t', 'i8': 'int8_t',
           'i16': 'int16_t', 'i32': 'int32_t', 'i64': 'int64_t', 'usize': 'size_t', 'bool': 'bool', '': 'void'}


def cpp_type(t):
    t = t.strip()
    if t.startswith('*mut '):
        return cpp_type(t[5:]) + '*'
    if t.startswith('*const '):
        return 'const ' + cpp_type(t[7:]) + '*'
    m = re.match(r'rusty::sync::Arc<(.*)>$', t)
    if m:
        return f'rusty::Arc<{cpp_type(m.group(1))}>'
    return SCALARS.get(t, t)


def prototype(rs, name):
    params, ret = signature(rs, name)
    recv = params[0]
    s_ty = 'const RaftServerBase*' if recv == '&self' else 'RaftServerBase*'
    cps = [f'{s_ty} s']
    for p in params[1:]:
        pname, pty = [x.strip() for x in p.split(':', 1)]
        if pty.startswith('&mut '):
            cps.append(f'{cpp_type(pty[5:])}* {pname}')
        elif pty.startswith('&') or pty in BY_VALUE_CARRIERS:
            cps.append(f'const {cpp_type(pty.lstrip("&"))}* {pname}')
        else:
            cps.append(f'{cpp_type(pty)} {pname}')
    if ret.startswith('&mut '):
        rc = cpp_type(ret[5:]) + '*'
    elif ret.startswith('&'):
        rc = 'const ' + cpp_type(ret[1:]) + '*'
    else:
        rc = cpp_type(ret)
    return f'{rc} raft_server_{snake(name)}({", ".join(cps)});'


# The two functions with no method behind them: the object's lifetime. Rust
# owns the allocation (Box), C++ holds the pointer; the shim's constructor and
# destructor are one call each.
LIFECYCLE_RS = '''// --- Lifetime. Rust allocates and frees: the struct is a Box the shim
// holds as a raw pointer between these two calls.
/// # Safety
/// The returned pointer is owned by the caller until raft_server_delete.
#[no_mangle]
pub unsafe extern "C" fn raft_server_new() -> *mut RaftServerBase {
    let s: *mut RaftServerBase = Box::into_raw(Box::new(RaftServerBase::new()));
    (*s).ConstructRuntime();
    s
}

/// # Safety
/// `s` came from raft_server_new and is not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn raft_server_delete(s: *mut RaftServerBase) {
    (*s).Shutdown();
    // Drops this server's row from the C++ commo table -- see set_commo, and
    // commo_of in server.cc. Here rather than in the shim's destructor so the
    // key is released in the same function that frees what it keys on, and
    // while the pointer is still live: Shutdown reaches no kernel that
    // resolves the communicator.
    raft_unbind_commo(s as *mut RaftServerHandle);
    drop(rusty::Box::from_raw(s));
}'''
LOOPS_RS = '''// --- The two fiber loops, entered from the spawn kernels.
/// # Safety
/// `s` is a live `RaftServerBase`; runs on the calling fiber until shutdown.
#[no_mangle]
pub unsafe extern "C" fn raft_server_heartbeat_loop(s: *mut RaftServerBase) {
    heartbeat_loop_body(s)
}

/// # Safety
/// `s` is a live `RaftServerBase`; runs on the calling fiber until shutdown.
#[no_mangle]
pub unsafe extern "C" fn raft_server_run_election_timer_loop(s: *mut RaftServerBase,
                                                              wait_int_us: u64) {
    let timer: ElectionTimerLoop = ElectionTimerLoop::new(s, wait_int_us);
    timer.run()
}'''
GATE_RS = '''// --- The wake job, entered from the reactor's OneTimeJob (raft_queue_wake_job).
/// # Safety
/// `token` is the Box<GateWakeJob> RaftServerBase::queue_wake_job made raw,
/// handed back exactly once.
#[no_mangle]
pub unsafe extern "C" fn raft_wake_job_run(token: *mut core::ffi::c_void) {
    let job: rusty::Box<GateWakeJob> = rusty::Box::from_raw(token as *mut GateWakeJob);
    job.run();
}'''
GATE_H = ['// --- The wake job, entered from the reactor\'s OneTimeJob (raft_queue_wake_job).',
          'void raft_wake_job_run(void* token);']
LOOPS_H = ['// --- The two fiber loops, entered from the spawn kernels.',
           'void raft_server_heartbeat_loop(RaftServerBase* s);',
           'void raft_server_run_election_timer_loop(RaftServerBase* s, uint64_t wait_int_us);']
LIFECYCLE_H = ['// --- Lifetime: Rust allocates and frees; the shim holds the pointer.',
               'RaftServerBase* raft_server_new();', 'void raft_server_delete(RaftServerBase* s);']

# Two groups, and only two. The lab harness reads the struct directly from
# Rust (src/deptran/raft/src/lab*.rs), so it needs no exports of its own.
GROUPS = (('The replication interface: TxLogServer and RaftSpecific.', INTERFACE),
          ('What the kernels in server.cc call back into.', KERNEL_CALLED))


# Relative to this file, not absolute: a worktree must regenerate from ITS
# own headers. The absolute paths that were here read the main checkout's
# server.h while rewriting the worktree's, which can only produce a
# byte-for-byte mismatch or, worse, a silent stale regeneration.
_REPO = pathlib.Path(__file__).resolve().parent.parent
H = str(_REPO / 'src' / 'deptran' / 'raft' / 'server.h')
SCH = str(_REPO / 'src' / 'deptran' / 'scheduler.h')


def cpp_decls(text, class_rx):
    """`(ret, name, params, const)` for every method declared in the class whose
    header matches class_rx, read from the emitted C++."""
    m = re.search(class_rx + r'.*?\n\};', text, re.S)
    body = m.group(0)
    out = {}
    for d in re.finditer(r'^\s+(?:virtual )?([A-Za-z0-9_:<>,\* &]+?) ([A-Za-z_][A-Za-z0-9_]*)\(([^)]*)\)( const)?(?: = 0)?;', body, re.M):
        out[d.group(2)] = (d.group(1).strip(), d.group(3).strip(), d.group(4) or '')
    return out


def forwarder(name, ret, params, const, rust_ret):
    args = []
    for prm in [x.strip() for x in params.split(',') if x.strip()]:
        pname = prm.split()[-1].lstrip('*&')
        ptype = prm[:prm.rfind(pname)].strip()
        # A reference, or a carrier the export takes by pointer (BY_VALUE_CARRIERS),
        # crosses as its address; the export copies inside.
        args.append('&' + pname if ptype.endswith('&') or ptype in BY_VALUE_CARRIERS else pname)
    call = f'raft_server_{snake(name)}(' + ', '.join(['impl_'] + args) + ')'
    if ret == 'void':
        body = f'{call};'
    elif rust_ret.startswith('&'):
        body = f'return *{call};'
    else:
        body = f'return {call};'
    return f'  {ret} {name}({params}){const} {{ {body} }}'


def rust_decl(rs, name):
    """`(ret, params, const)` in C++ spelling, derived from the Rust signature (
    there is no C++ struct declaration to read any more)."""
    params, ret = signature(rs, name)
    const = ' const' if params[0] == '&self' else ''
    def cpp_ref(t):
        t = t.strip()
        if t.startswith('&mut '): return cpp_type(t[5:]) + '&'
        if t.startswith('&'): return 'const ' + cpp_type(t[1:]) + '&'
        return cpp_type(t)
    cps = []
    for p in params[1:]:
        pname, pty = [x.strip() for x in p.split(':', 1)]
        cps.append(f'{cpp_ref(pty)} {pname}')
    return cpp_ref(ret) if ret else 'void', ', '.join(cps), const


def shim(rs):
    sch = open(SCH).read()
    iface = {**cpp_decls(sch, r'class TxLogServer \{'), **cpp_decls(sch, r'class RaftSpecific : public TxLogServer \{')}
    out = ['class RaftServer : public RaftSpecific {', ' public:',
           '  RaftServer() : impl_(raft_server_new()) {}',
           '  // @unsafe - thread join and timer cleanup require manual resource management',
           '  ~RaftServer() { raft_server_delete(impl_); }', '',
           '  // --- TxLogServer and RaftSpecific, forwarded to the C ABI.']
    for n in INTERFACE:
        ret, params, const = iface[n]
        out.append(forwarder(n, ret, params, const, signature(rs, n)[1]).replace(f'{const} {{', f'{const} override {{', 1))
    out += ['',
            '  // The Rust object itself, for the one caller that must hand it to',
            '  // Rust rather than forward a method: the Rust lane\'s transport binds',
            '  // to it (raft_lane_rust.cc). Not part of RaftSpecific.',
            '  RaftServerBase* impl() const { return impl_; }',
            '', ' private:', '  RaftServerBase* impl_;', '};']
    return '\n'.join(out)


def main():
    rs = open(RS).read()
    if '--shim' in sys.argv:
        print(shim(rs))
        return
    if '--header' in sys.argv:
        out = ['#pragma once',
               '// GENERATED by scripts/raft_gen_exports.py -- do not edit; re-run and diff.',
               '//',
               '// The C ABI over RaftServerBase. The definitions are Rust, compiled by',
               '// rustc into libraft.a. This header is the only thing the hand-written C++',
               '// -- the RaftServer shim and the kernels -- is meant to know about the',
               '// struct\'s behaviour. Included from server.h after every type it names is',
               '// declared.',
               '#include <cstddef>', '#include <cstdint>', '', 'namespace janus {', 'extern "C" {']
        out.extend(LIFECYCLE_H)
        out.extend(LOOPS_H)
        out.extend(GATE_H)
        for title, names in GROUPS:
            out.append(f'// --- {title}')
            out.extend(prototype(rs, n) for n in names)
        out += ['}  // extern "C"', '}  // namespace janus', '']
        print('\n'.join(out))
        return
    out = [LIFECYCLE_RS, LOOPS_RS, GATE_RS]
    for title, names in GROUPS:
        out.append(f'// --- {title}')
        out.extend(export(rs, n) for n in names)
    print('\n\n'.join(out))


if __name__ == '__main__':
    main()
