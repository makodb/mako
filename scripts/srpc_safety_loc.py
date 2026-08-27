#!/usr/bin/env python3
"""Tally @safe vs @unsafe vs unannotated LOC across srpc borrow-checked modules.

Walks each file line by line, tracking:
  - Per-class safety annotation (`// @safe class X {}` or `// @unsafe class X {}`).
  - Per-function safety annotation (preceding `// @safe -` / `// @unsafe -` comment).
  - Out-of-class method definitions (`ClassName::method_name(...) {`) inherit
    their containing class's annotation when they have no explicit one. Only
    matched at brace depth 0 (file scope) to avoid false-positive matches on
    function calls inside other function bodies.
  - Inner `// @unsafe { ... }` blocks override the surrounding function label
    for the lines inside the block.

Usage: `python3 scripts/srpc_safety_loc.py` from the worktree root.
"""
import re
from pathlib import Path

# Resolve worktree root relative to this script.
SCRIPT_DIR = Path(__file__).resolve().parent
WORKTREE = SCRIPT_DIR.parent

# Files in SRPC_BORROW_SRC (per src/srpc-cmake/CMakeLists.txt).
FILES = [
    "src/srpc/base/logging.cpp",
    "src/srpc/base/misc.cpp", "src/srpc/base/threading.cpp",
    "src/srpc/misc/any_message.cpp",
    "src/srpc/misc/serializable.cpp", "src/srpc/misc/serializable_envelope.cpp",
    "src/srpc/reactor/epoll_wrapper.cc", "src/srpc/reactor/fiber.cpp",
    "src/srpc/reactor/future.cpp", "src/srpc/reactor/reactor.cpp",
    "src/srpc/rpc/callbacks.cpp", "src/srpc/rpc/channel.cpp",
    "src/srpc/rpc/client.cpp",
    "src/srpc/rpc/fiber_channel.cpp", "src/srpc/rpc/idempotency.cpp",
    "src/srpc/rpc/inmemory_channel.cpp",
    "src/srpc/rpc/pollable_proxy.cpp",
    "src/srpc/rpc/server.cpp",
    "src/srpc/rpc/tcp_channel.cpp",
]

INLINE_UNSAFE_BLOCK = re.compile(r"^\s*//\s*@unsafe\s*\{")
FUNCTION_ANNOT = re.compile(r"^\s*//\s*@(safe|unsafe)\b(?!\s*\{)")
CLASS_DECL = re.compile(r"^\s*(?:export\s+)?(?:class|struct)\s+(\w+)")
# `namespace X {` or `namespace X::Y {` or `export namespace X {`. We track
# namespaces separately so they don't masquerade as function bodies and break
# the "are we at file scope?" check used for out-of-class method detection.
NAMESPACE_DECL = re.compile(r"^\s*(?:export\s+)?namespace\b")
# Out-of-class method definition. Must start the line with a return-type-looking
# token (not a control-flow keyword) and contain `ClassName::name(`. We further
# restrict matches to depth==0 below to avoid false positives on function calls.
OUT_OF_CLASS_METHOD = re.compile(
    r"^(?!\s*(?:if|while|for|switch|return|else|do|try|catch)\b)"
    r"\s*[\w\s<>:*&,\[\]]*?\b(\w+)::(?:~?\w+|operator\S+)\s*\("
)


def strip_line_comment(line):
    i = line.find("//")
    return line[:i] if i >= 0 else line


def count_braces(line):
    s = strip_line_comment(line)
    return s.count("{"), s.count("}")


def classify_file(path):
    bkts = {"safe_explicit": 0, "unsafe_explicit": 0, "unsafe_block": 0,
            "unannotated": 0, "other": 0}
    total = 0

    pending = None              # "safe" | "unsafe" | "unsafe_block" | None
    pending_for_class = None
    class_name_at_open = None   # name of class whose body opens on this line

    # Map class-name → "safe" | "unsafe" | None (declared but no annotation).
    class_annotations = {}

    # Active scope stacks.
    func_stack = []         # [(label, opening_depth)]
    class_stack = []        # [(class_name, opening_depth)]
    namespace_stack = []    # [(label, opening_depth)] — label is the
                            # `@safe`/`@unsafe` annotation that preceded
                            # this `namespace X {`, if any
    unsafe_block_stack = [] # [opening_depth]
    namespace_at_open = False  # marks that the next `{` opens a namespace

    # Pending out-of-class def name from a multi-line signature.
    pending_out_of_class = None

    depth = 0

    with open(path) as f:
        for line in f:
            total += 1

            # Classify this line first (using the active context).
            label = None
            if unsafe_block_stack:
                label = "unsafe_block"
            elif func_stack:
                label = func_stack[-1][0]

            if label is None:
                bkts["other"] += 1
            elif label == "safe":
                bkts["safe_explicit"] += 1
            elif label == "unsafe":
                bkts["unsafe_explicit"] += 1
            elif label == "unsafe_block":
                bkts["unsafe_block"] += 1
            elif label == "unannotated":
                bkts["unannotated"] += 1

            inline_block = bool(INLINE_UNSAFE_BLOCK.match(line))
            fn_match = FUNCTION_ANNOT.match(line) if not inline_block else None

            if inline_block:
                pending = "unsafe_block"
            elif fn_match:
                pending = fn_match.group(1)
                pending_for_class = fn_match.group(1)

            stripped = strip_line_comment(line)
            class_match = CLASS_DECL.search(stripped)
            namespace_match = NAMESPACE_DECL.search(stripped)
            class_name_at_open = None
            if class_match and "{" in stripped:
                class_name_at_open = class_match.group(1)
            if namespace_match and "{" in stripped and class_match is None:
                namespace_at_open = True

            # Out-of-class definition heuristic: only consider it a function
            # definition if we're at file or namespace scope (no class or
            # function on the active stacks). The depth itself can be > 0
            # because of enclosing namespaces.
            at_file_or_namespace_scope = (not class_stack) and (not func_stack)
            out_of_class_name = None
            if at_file_or_namespace_scope and pending is None:
                m_oc = OUT_OF_CLASS_METHOD.search(stripped)
                if m_oc:
                    cand = m_oc.group(1)
                    if cand in class_annotations:
                        if "{" in stripped:
                            out_of_class_name = cand
                        else:
                            pending_out_of_class = cand
            if pending_out_of_class is not None and "{" in stripped and pending is None:
                out_of_class_name = pending_out_of_class
                pending_out_of_class = None

            opens, closes = count_braces(line)

            for _ in range(opens):
                depth += 1
                if pending == "unsafe_block":
                    unsafe_block_stack.append(depth)
                    pending = None
                elif class_name_at_open is not None:
                    class_stack.append((class_name_at_open, depth))
                    class_annotations[class_name_at_open] = pending_for_class
                    pending_for_class = None
                    pending = None
                    class_name_at_open = None  # only first `{` opens the class
                elif namespace_at_open:
                    # Record namespace annotation if there's a pending one;
                    # later function/class bodies inside this namespace will
                    # inherit through namespace_stack when neither their own
                    # annotation nor an enclosing class annotation applies.
                    ns_label = pending if pending in ("safe", "unsafe") else None
                    namespace_stack.append((ns_label, depth))
                    namespace_at_open = False
                    pending = None
                    pending_for_class = None
                elif pending in ("safe", "unsafe"):
                    func_stack.append((pending, depth))
                    pending = None
                    pending_for_class = None
                elif out_of_class_name is not None:
                    inherited = class_annotations.get(out_of_class_name)
                    if inherited == "safe":
                        func_stack.append(("safe", depth))
                    elif inherited == "unsafe":
                        func_stack.append(("unsafe", depth))
                    else:
                        func_stack.append(("unannotated", depth))
                    out_of_class_name = None
                else:
                    # Inherit from innermost class or namespace with a
                    # recorded annotation. Classes take precedence over
                    # namespaces when both have one.
                    inherited = None
                    for cname, _ in reversed(class_stack):
                        ann = class_annotations.get(cname)
                        if ann is not None:
                            inherited = ann
                            break
                    if inherited is None:
                        for ns_label, _ in reversed(namespace_stack):
                            if ns_label is not None:
                                inherited = ns_label
                                break
                    if inherited == "safe":
                        func_stack.append(("safe", depth))
                    elif inherited == "unsafe":
                        func_stack.append(("unsafe", depth))
                    else:
                        func_stack.append(("unannotated", depth))

            for _ in range(closes):
                if unsafe_block_stack and unsafe_block_stack[-1] == depth:
                    unsafe_block_stack.pop()
                elif class_stack and class_stack[-1][1] == depth:
                    class_stack.pop()
                elif namespace_stack and namespace_stack[-1][1] == depth:
                    namespace_stack.pop()
                elif func_stack and func_stack[-1][1] == depth:
                    func_stack.pop()
                depth -= 1

            # Drop pending annotation if line had a `;` (in code, not in a
            # comment) and didn't open a body — that's a forward-decl, not a
            # definition. Strip comments before the `;` check or multi-line
            # annotation comments like `// overrides; the rest...` would
            # spuriously clear pending.
            if pending in ("safe", "unsafe") and ";" in stripped and opens == 0:
                pending = None
                pending_for_class = None
            if pending_out_of_class is not None and ";" in stripped and opens == 0:
                pending_out_of_class = None

    bkts["total"] = total
    return bkts


def main():
    grand = {k: 0 for k in ("total", "safe_explicit", "unsafe_explicit",
                             "unsafe_block", "unannotated", "other")}
    rows = []
    for rel in FILES:
        p = WORKTREE / rel
        if not p.exists():
            continue
        r = classify_file(p)
        rows.append((rel, r))
        for k in grand:
            grand[k] += r[k]

    rows.sort(key=lambda x: -x[1]["total"])
    print(f"{'file':40} {'total':>6} {'safe':>6} {'unsafe':>6} "
          f"{'block':>6} {'unann':>6} {'other':>6}")
    for rel, r in rows[:14]:
        print(f"{rel:40} {r['total']:>6} {r['safe_explicit']:>6} "
              f"{r['unsafe_explicit']:>6} {r['unsafe_block']:>6} "
              f"{r['unannotated']:>6} {r['other']:>6}")
    print()
    print(f"{'TOTAL':40} {grand['total']:>6} {grand['safe_explicit']:>6} "
          f"{grand['unsafe_explicit']:>6} {grand['unsafe_block']:>6} "
          f"{grand['unannotated']:>6} {grand['other']:>6}")
    print()
    tot = grand["total"]
    in_funcs = (grand["safe_explicit"] + grand["unsafe_explicit"]
                + grand["unsafe_block"] + grand["unannotated"])
    print(f"LOC inside function bodies: {in_funcs} of {tot} "
          f"({100.0 * in_funcs / tot:.1f}%)")
    print(f"LOC outside function bodies: {grand['other']} "
          f"({100.0 * grand['other'] / tot:.1f}%)")
    print()
    print("Of function-body LOC:")
    for k, label in (("safe_explicit", "@safe (function or inherited class @safe)"),
                     ("unsafe_explicit", "@unsafe (function or inherited class @unsafe)"),
                     ("unsafe_block", "inner @unsafe { ... } blocks"),
                     ("unannotated", "unannotated (default @unsafe per namespace)")):
        v = grand[k]
        pct = 100.0 * v / in_funcs if in_funcs else 0.0
        print(f"  {label:<60} {v:>5} ({pct:.1f}%)")


if __name__ == "__main__":
    main()
