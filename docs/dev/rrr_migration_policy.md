# Migration policy questions

Step 1 is: `src/rrr` contains no hand-written C++ — every line is
inline-Rust DSL, C++ generated from it, or external C behind
`extern "C"`. Converting `frame_codec.cpp` (the first target) surfaced a
question that will recur across all 7,106 lines.

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

## Settled

- **Module scaffolding is exempt** (`module;`, includes,
  `export module`, namespace open/close, forward decls). The target is
  zero hand-written *logic*. Revisit only if whole files become
  generated.
