# Migration policy

Step 1 is: `src/rrr` contains no hand-written C++ — every line is
inline-Rust DSL, C++ generated from it, or external C behind
`extern "C"`.

## THE DECISION RULE

When a piece of C++ resists becoming DSL, apply these **in order** and
take the first that fits:

### 1. Is it a TRANSLATION BUG? Fix the translator.

The DSL cannot express it, or expresses it wrongly, because the
transpiler is wrong. Then the transpiler is what changes — not the
code, and not the shape of the port.

This is first because every workaround has to be repeated by every
future consumer, and because a translator that deviates from authentic
Rust behaviour makes the whole campaign unsound: the C++ side stops
being a faithful image of the Rust.

Worked example: `ClientConnection::pause` was renamed `pause_` by a
libc-collision rule applied in member position, where no collision is
possible. That was a bug. It was fixed in the transpiler
(rusty-cpp `e781abe4`), not worked around by renaming the method across
deptran.

### 2. Not a bug, but avoidable by REWRITING THE CALL SITE to equivalent logic? Rewrite it.

The translation is legitimate; the existing C++ just happens to use a
shape the DSL renders differently. If an equivalent formulation exists
that the DSL handles cleanly, change the code.

The bar is *equivalent logic* — same behaviour, different spelling. Not
"weaken the API until it lowers".

### 3. Neither applies? Convert to EXTERNAL C.

A `.c` (or `.S`) file behind `extern "C"`, assembled/compiled once and
linked by both toolchains — the arrangement already proven by
`runtime/fiber/fiber_x86_64.S` and the libc thunks.

**Last resort, deliberately.** Anything moved to C is permanently not
Rust, and step 2 (compile the DSL under rustc) inherits it forever. C is
for what genuinely cannot be expressed: raw-pointer/allocator surgery,
syscalls, assembly, third-party APIs.

### Why this order

Rule 3 is the cheapest way to hit "hand-written C++ -> 0" and the worst
way to reach the actual goal. A burndown metric can be satisfied while
the Rust story gets worse. The ordering exists so that cannot happen by
default: C has to be *argued for*, after 1 and 2 have failed.

---

## Applying it to frame_codec.cpp

| function | rule | disposition |
|---|---|---|
| `frame_decode_status_to_string` (8) | **2** | `-> &'static str` lowering to `std::string_view` is correct, not a bug. Rewrite the 3 `EXPECT_STREQ` call sites as `EXPECT_EQ`. → **DSL Rust** |
| `frame_codec_write_header` (28) | **2** | Raw-pointer parameter is an old API, not a kernel. Take a slice; 4 test call sites pass `hdr.data()`. `wire/frame.rs` already does this safely. → **DSL Rust** |
| `frame_codec_encode_into` (74) | **2 (try), else 3** | Body is validate/resize/copy over a `vector&`. `wire/frame.rs::encode_into` expresses it over slices under `deny(unsafe_code)`, so rule 2 probably reaches. Fall to C only if it does not. |
| module scaffolding (~26) | exempt | See Settled. |

Note what changed: the first triage sent 102 of 136 lines to external C.
Under the rule, most or all of it is rule 2 — **DSL Rust**. That
difference is the whole point of having the rule.

---

## Original framing of the question (kept for the reasoning)

## When the DSL lowering is WORSE than the C++, what wins?

`frame_decode_status_to_string` is 8 lines:

```cpp
inline constexpr const char* frame_decode_status_to_string(FrameDecodeStatus s) {
    switch (s) {
        case FrameDecodeStatus::NeedMoreBytes: return "NeedMoreBytes";
        ...
    }
}
```

Written as DSL Rust (`-> &'static str` + `match`), the transpiler emits:

```cpp
std::string_view frame_decode_status_to_string(FrameDecodeStatus s) {
    return ({ auto&& _m = s; std::optional<std::string_view> _match_value;
              bool _m_matched = false;
              if (!_m_matched && (_m == ...)) { _match_value.emplace(...); _m_matched = true; }
              ... if (!_m_matched) { rusty::intrinsics::unreachable(); }
              std::move(_match_value).value(); });
}
```

Two costs: the return type changes `const char*` -> `std::string_view`,
breaking `EXPECT_STREQ` at 3 call sites in
`rpc_frame_codec_test.cc`; and a `constexpr` switch becomes a runtime
`optional` dance.

Both stringifiers in the tree (this one and `channel_error_to_string`)
are hand-written today. That now looks deliberate rather than
accidental.

### The options, and why this is not obviously answerable

1. **Accept the lowering.** Convert to DSL, take `std::string_view`,
   update the 3 test call sites (`EXPECT_STREQ` -> `EXPECT_EQ`). The
   function becomes real Rust, which is the campaign's point. Cost:
   worse generated code, and a public type change per conversion.
2. **Push it to external C.** `const char* frame_decode_status_to_string(int)`
   in a `.c` file. No API change, no test churn, and it satisfies "zero
   hand-written C++" literally. Cost: we converted C++ to C, not to
   Rust — and if that becomes the default for anything the DSL handles
   badly, step 2 (compile the DSL under rustc) inherits a large C
   surface that is permanently not Rust.
3. **Fix the transpiler** to lower `-> &'static str` whose arms are all
   literals to `const char*`, keeping `constexpr`. Most work, but it is
   the option that makes the DSL able to express the shape — and the
   standing instruction is to fix translator deviations rather than
   route around them.

The tension: (2) is the cheapest way to hit the step-1 metric and the
worst way to reach the step-2 goal. A migration measured by
"hand-written C++ -> 0" can be satisfied while making the Rust story
worse, and this file is where that first becomes visible.

**Recommendation: (3) where the shape is common, (1) where it is not.**
Stringifiers are common enough to be worth a lowering. But this is a
scope call, not a code call.

## Kernels are not leaves — "move it to C" cascades

`frame_codec_write_header` (28 lines) reads like kernel material: a raw
`std::uint8_t*` and a `memcpy`. It is not self-contained. It calls
`encode_response_size`, which is defined in **DSL** in another module,
and uses `kMaxFramePayloadSize`, which is generated.

C cannot call a function that lives in a C++ module. So moving this one
function to external C requires one of:

 - cascade `encode_response_size` into C as well (and whatever it calls);
 - change the signature to take the already-encoded value, pushing the
   logic to every caller;
 - duplicate the encoding in C, which is a divergence waiting to happen.

Budget for this when sizing the remaining 7,106 lines: the kernel
boundary is not where the raw pointers are, it is where the *call graph*
can be cut.

## The first triage was too pessimistic

`write_header` was classified as kernel from its signature. Its BODY is
validate -> encode -> store 4 bytes; the only raw-pointer act is a
4-byte store. `crates/srpc/src/wire/frame.rs` already expresses exactly
this safely over a slice, under `deny(unsafe_code)`.

Its callers are four test sites passing `hdr.data()` from a
`std::array`. Taking a slice instead costs a 4-line test change and
makes the function **Rust rather than C**.

So of frame_codec's 102 "kernel" lines, ~28 are probably DSL-able and
~74 (`encode_into`: vector resize plus a memcpy of an arbitrary payload)
are more genuinely kernel. Classify by what the body DOES, not by what
the signature takes — a raw-pointer parameter is often just an old API.

## The whole file is blocked on this, not just the stringifier

`encode_into` (the ~74 genuinely-kernel lines) calls
`frame_codec_write_header`, which calls `encode_response_size` — DSL in
another module. So it cascades exactly like write_header does.

There is no piece of frame_codec.cpp that can be converted without
answering the question, because every non-scaffolding line reaches
DSL-side logic through the call graph. Converting any of it to C
converts that logic to C too.

That is the answer to "can we start somewhere easy and decide later":
no. The first file is the decision.

## Settled

- **Module scaffolding is exempt** (`module;`, includes,
  `export module`, namespace open/close, forward decls). The target is
  zero hand-written *logic*. Revisit only if whole files become
  generated.
