# Marshal → serde-shape migration plan

## Background

`Marshal` (in `src/rrr/misc/marshal.cpp`) bundles three responsibilities that
serde keeps separate:

| serde concept           | Marshal today              |
|-------------------------|----------------------------|
| `Vec<u8>` (storage)     | `Marshal::buf_`            |
| `Cursor<Vec<u8>>::pos`  | `Marshal::read_pos_`       |
| `io::Write` impl        | `Marshal::write(p, n)`     |
| `io::Read` impl         | `Marshal::read(p, n)`      |
| (no equivalent)         | `Marshal::set_bookmark` / `write_bookmark` |
| (no equivalent)         | `Marshal::read_from_marshal` (splice)      |

Because Marshal occupies both the storage slot and the cursor slot at once,
the codebase carries auxiliary wrapper structs `MarshalSink` / `MarshalSource`
solely to project Marshal onto the `SinkBase` / `SourceBase` virtual
interfaces. The wrappers hold raw `Marshal*` pointers, which is the
proximate `@unsafe` surface that motivated this migration.

In a serde-shaped design:

- `Vec<u8>` itself is the byte container; `impl Write for Vec<u8>` is the
  sink trait impl.
- The **read cursor lives in the consumer** (the `Deserializer` / archive),
  not in the storage. `Cursor<Vec<u8>>` owns both the Vec and the position,
  and `impl Read for Cursor` provides the source trait impl. The
  Deserializer owns the Cursor.

## Target shape

After the migration:

```
Vec<u8>                  ← byte storage (rusty::Vec<uint8_t>, no rrr wrapper)
   ↑ borrowed by
ByteCursor               ← read cursor: { Span<const u8> view, size_t pos }
   ↑ wrapped by
SourceProxy              ← Box<SourceBase>; held by BinaryReadArchive
                           (no MarshalSource, no MarshalSourceAdapter)

Marshal (write-only)     ← buf_: Vec<u8>; bookmarks; impl SinkBase
   ↑ wrapped by
SinkProxy                ← Box<SinkBase>; held by BinaryWriteArchive
                           (no MarshalSink, no MarshalSinkAdapter)
```

## Phased migration

### Phase 1 — drop the wrapper structs

**Goal**: eliminate `MarshalSink` and `MarshalSource` as standalone types.
Adapters wrap `Marshal*` directly.

**Blast radius**: ~6 hand-written construction sites, rpcgen template,
regenerated test stubs (`benchmark_service.h` and similar).

**Outcome**: same bidirectional Marshal; one fewer pointer-holding API
type; no behavioral change.

### Phase 2 — Marshal directly implements SinkBase + SourceBase

**Goal**: Marshal *is* the sink and *is* the source via multi-inheritance
(or two thin virtual mixins). `MarshalSinkAdapter` and
`MarshalSourceAdapter` disappear; `make_sink_proxy(Marshal&)` boxes the
Marshal itself.

**Caveats**: Marshal becomes polymorphic — gains a vtable, which costs a
pointer-word per instance. Consumers that take Marshal by value still
work (multi-inheritance with virtual bases is supported); slicing concerns
are nil because Marshal is `NoCopy` already.

**Outcome**: zero raw-pointer adapter layer between Marshal and the
serializer/deserializer archives.

### Phase 3 — extract read cursor into ByteCursor

**Goal**: move `read_pos_`, `read()`, `peek()` out of Marshal into a new
`ByteCursor` type. Marshal becomes write-only.

**ByteCursor shape**:
```cpp
class ByteCursor : public SourceBase {
  std::span<const uint8_t> view_;   // borrowed
  size_t pos_{0};
public:
  explicit ByteCursor(std::span<const uint8_t> view) : view_(view) {}
  size_t read_bytes(uint8_t* p, size_t n) override;
  // + peek<T>, remaining(), eof()
};
```

**Call-site migration**:
- `MarshalSource src(&m);` (Phase 1 form: `make_source_proxy(m)`) becomes
  `ByteCursor c(m.view()); make_source_proxy(c);`.
- The "write into a Marshal then read from it" round-trip becomes:
  write, call `m.view()` to get a `span<const uint8_t>`, hand to ByteCursor.
- Auto-reclamation (current Marshal behavior: `clear() + reset()` on full
  drain) belongs to the writer reset / explicit `Marshal::reset()`, not
  to the cursor.

**`read_from_marshal` splice**: becomes a free function
`splice_bytes(Marshal& dst, ByteCursor& src, size_t n)` or similar.

**Concurrent write+read** (request queue pattern): `ByteCursor` is a view
over a borrowed slice, so the producer can grow the Marshal while the
consumer reads from a snapshot — but if the Marshal reallocates its
`buf_` mid-read, the cursor's view dangles. The migration needs to
audit each shared-buffer site. Options:
  - Take a snapshot (copy bytes into the cursor's own Vec).
  - Hold a lock across the read.
  - Use index-based access (cursor holds `Marshal*` + position; reads pull
    fresh `buf_.data() + pos_` each call — effectively the current
    behavior but extracted into a separate type).

The third option preserves today's semantics and is the safe default for
the first cut. Pure-`span` borrowing is the long-term target.

**Outcome**: read cursor lives outside Marshal, matching serde layering.
Marshal is purely a writer (Vec<u8> + bookmarks).

### Phase 4 — rename Marshal → ByteWriter (optional)

Mechanical 56-file rename. Documentation/clarity only; does not change
safety. Defer until explicitly requested.

## Order of execution

Phases are independent and incremental:
1. Phase 1 is small, low-risk, immediate safety win.
2. Phase 2 is medium — adds vtable, removes adapters.
3. Phase 3 is the architecturally significant phase — splits responsibilities
   per serde layering. Multi-commit.
4. Phase 4 is cosmetic.

**Pause point**: re-check with user before starting Phase 3. The
concurrent-write-and-read sites need explicit decisions about snapshot vs
borrow semantics.
