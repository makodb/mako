# Is RaftServer convertible? Verified, not estimated

Produced by a five-way assessment with adversarial verification of every
claimed blocker: 25 agents, each blocker handed to a separate agent whose job
was to refute it with a transpiler probe.

**Result: 11 REFUTED, 9 OVERSTATED, 0 CONFIRMED.** No claimed blocker
survived. This supersedes the three progressively-less-restrictive answers
given earlier in the session, each of which was too pessimistic.

## What was refuted

| claimed blocker | verdict |
|---|---|
| Data members cannot all be DSL fields | REFUTED -- all 50 direct members typecheck as fields of a DSL `pub struct`, verified by compiling against the real build flags, 0 errors, and on the rustc side via the raft crate |
| No default constructor | REFUTED -- `#[cpp_ctor]` emits a real C++ constructor instead of a `static new_()` factory (predicates.rs:663-676) |
| Nested types (`LabAccess`) unexpressible | REFUTED -- an associated type in an inherent impl emits a real C++ member typedef |
| `try`/`catch` unexpressible | REFUTED -- every Raft use maps to `catch_unwind` or a Result; no kernel needed |
| Non-movable members block the generated ctors | REFUTED -- the all-fields and move constructors are emitted CONDITIONALLY (emit_items.rs:2902), and no existing raft DSL struct gets them |
| The apply `std::thread` needs a kernel | REFUTED -- `rusty::thread::spawn` plus a `Mutex<Option<JoinHandle>>` field, a shape that already ships elsewhere in the tree |
| `mtx_` guarding siblings by convention blocks it | REFUTED -- a DSL struct can hold the existing `RaftCheckedMutex` as a field and keep today's discipline. `rusty::Mutex<T>` is an improvement, not a precondition |

## What is real but smaller than claimed

- **Orphan-impl.** Confirmed as a mechanism and reproduced, but narrower than
  described: an `impl` is honored when its `pub struct` is in the SAME
  `#if RUSTYCPP_RUST` block. Sibling blocks in the same file are stubbed too.
  It constrains where an impl may sit, not where logic may live -- free
  functions taking `&mut RaftServer` can be added in later blocks, which is
  how incrementality comes back after the struct itself lands.
- **No access control.** Every DSL field emits public regardless of `pub`.
  RaftServer's 45 private members become public. Encapsulation is lost;
  nothing breaks.
- **No default arguments.** `Start(cmd, index, term, slot_id = -1,
  ballot = 1)` loses its defaults; call sites pass them explicitly.

## The actual risk: three SILENT mis-emissions

None of these block the conversion. All three produce wrong code with exit 0
and no diagnostic, which makes them worse than a blocker.

1. **`#[cfg(...)]` inside a DSL block is dropped.** The guarded code is
   emitted UNCONDITIONALLY. Relevant because `RAFT_TEST_CORO` changes
   RaftServer's behaviour.
2. **`#[derive(Default)]` is mis-lowered** when a field's own `Default` is
   non-zero: the emitter writes `return {};`, value-initialising and thus
   zeroing that field.
3. **The gate missed thirteen emitter markers.** `scripts/raft_dsl.sh` matched
   the literal `// TODO:` only, while the emitter also writes
   `// TODO orphan impl:` and thirteen `// TODO(interface_traits): ...`
   variants -- skipped by-value-self methods, skipped generic methods,
   skipped adapters, unsupported associated constants. Each silently drops a
   method from an adapter.

   FIXED in this commit: the scan now matches `// TODO` in any form. A GEN
   region is entirely generated, so any TODO inside one is an emitter marker
   by construction.

## Conclusion

The conversion is not capability-limited. What it needs is a careful
single landing of the struct declaration, and a gate that catches silent
emitter degradation -- one third of which was missing until now.
