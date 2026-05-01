#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <rusty/rusty.hpp>

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include <rusty/arc.hpp>
#include <rusty/fn.hpp>

#ifdef RR
#pragma push_macro("RR")
#undef RR
#define RRR_RESTORE_RR_MACRO 1
#endif
#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>
#ifdef RRR_RESTORE_RR_MACRO
#pragma pop_macro("RR")
#undef RRR_RESTORE_RR_MACRO
#endif

// External safety annotations for pure functions
// @external: {
//   std::min: [safe]
// }




#include "../base/all.hpp"

// Workstream N Phase 4d-prep: pull in `marshal_archive.hpp` so
// `SerializableConcept<T>` is visible at template definition time
// for `MarshallDeputy(shared_ptr<T>)` and `set_marshallable<T>`.
// This lets those templates dispatch transparently to the
// `wrap_typed_marshallable` bridge overload (declared below) for
// migrated Serializable types — call sites need no updates.
//
// `marshal_archive.hpp` forward-declares `class Marshal` rather than
// including this file, so this is acyclic. The archive's MarshalSink/
// MarshalSource use `Marshal*` only; method bodies live in the .cpp.
#include "marshal_archive.hpp"


namespace rrr {

// @safe - Wrapper for std::min (pure function, no side effects)
template<typename T>
inline T safe_min(const T& a, const T& b) {
  // @unsafe
  { return std::min(a, b); }
}

// Workstream N Phase 5b-11: removed the entire `RPC_STATISTICS` block
// and `stat_marshal_in` declaration. After Phase 5b-7/5b-8 deleted
// the marshal-out side, the marshal-in side became dead too once
// Phase 5b-11 confirmed `Marshal::read_from_fd` /
// `Marshal::chnk_read_from_fd` / `chunk::read_from_fd` had no
// production callers anywhere in the codebase. The receive path
// uses `FdSource` (`marshal_archive.hpp`) instead.

// not thread safe, for better performance
class Marshal;

class Marshallable {
 public:
  int32_t kind_{0};
  // Workstream N Phase 5b-3: removed `bypass_to_socket_` and
  // `written_to_socket` — they were a zero-copy fast path that no
  // production code ever enabled. The matching `Marshal::bypass_copying`,
  // `MarshallDeputy::entity_size` / `write_to_fd`, and chunk's
  // `shared_data` infrastructure went away in the same commit.
  Marshallable() = delete;
  explicit Marshallable(int32_t k): kind_(k) {};
  int32_t kind() const { return kind_; }
  virtual ~Marshallable() = default;
  // @safe
  // @lifetime: (&'a, &'b mut) -> &'b mut
  virtual Marshal& to_marshal(Marshal& m) const {
    verify(0);
    return m;
  }
  // @safe
  // @lifetime: (&'a mut, &'b mut) -> &'b mut
  virtual Marshal& from_marshal(Marshal& m) {
    verify(0);
    return m;
  }
  // Workstream N Phase 5b-6: removed `entity_size()` and
  // `write_to_fd()` virtual methods. They were the consumer end of
  // the dead bypass-to-socket fast path that Phase 5b-3 deleted; no
  // production code calls them, and the test fixtures that
  // overrode them did so for an invariant nobody checks.
};

// Workstream N Phase 5b-4: removed `MarshallableFacade`,
// `MarshallableProxy` typedef, `MarshallableSharedPtrAdapter`,
// `make_marshallable_proxy`, `marshallable_proxy_inner`, and the
// supporting `PRO_DEF_MEM_DISPATCH(MarshallableMem*, ...)` macros.
// After Phase 5b-3 removed `MarshallDeputy::data_proxy()` and the
// bypass-to-socket fast path, the only remaining users were six
// tests in `rpc_marshallable_proxy_test.cc` (`AdapterForwards*`,
// `ProxyIsMoveOnly`, `RoundTripThroughProxy`) that exercised the
// proxy facade machinery itself; those tests went away with the
// proxy. Production now serializes Marshallable values via direct
// virtual dispatch (`md.inner()->to_marshal(m)`) plus the
// SerializableMarshallableAdapter for migrated types.

// Workstream N Phase 5b-5: removed `TypedMarshallableAdapterTraits`,
// `kHasTypedMarshallableAdapter`, the `TypedMarshallableAdapter`
// class template, and the legacy `wrap_typed_marshallable<T>` /
// `marshallable_cast<T>` overloads gated on the trait. After
// Phase 4 fully migrated all production payload types to
// Serializable, the typed-adapter trait path was exercised only by
// the `TypedOnlyPayload` test fixture in
// `rpc_marshallable_proxy_test.cc`; that fixture went away in this
// commit too. Two paths remain for `wrap_typed_marshallable` /
// `marshallable_cast`: direct Marshallable subclasses (the C-style
// path) and SerializableConcept types (the bridge path).

template <typename T>
inline constexpr bool kAlwaysFalse = false;

// Forward declaration of the bridge `wrap_typed_marshallable<T>` for
// SerializableConcept T. The actual definition lives in
// `marshal_serializable_bridge.hpp` (included via the rrr.hpp
// umbrella in production code).
//
// The forward decl is needed here so that two-phase template lookup
// inside `MarshallDeputy::set_marshallable<T>` (and the matching
// constructor) finds this overload during Phase 1 unqualified lookup
// at template definition. ADL on `shared_ptr<T>` only adds `std`
// and T's namespace — not `rrr`.
template <typename T>
  requires (!std::is_base_of_v<Marshallable, T> &&
            SerializableConcept<T>)
std::shared_ptr<Marshallable> wrap_typed_marshallable(
    std::shared_ptr<T> typed);

// @safe - Type-erasing wrapper for polymorphic Marshallable objects
// NOTE: Stores a proxy facade over shared_ptr<Marshallable>:
//   1. Polymorphism requirement - Marshallable is abstract base with many derived types
//   2. Type erasure pattern - kind_ determines actual type at runtime
//   3. Shared ownership is still available through inner() for dynamic casts
class MarshallDeputy {
  public:
    // Workstream L L5i: `MarInitializerFn` migrated from
    // `std::function` to `rusty::Function` (move-only). Because
    // rusty::Function is move-only, the registry's per-thread
    // snapshot cache (which relied on HashMap clone) is gone, and
    // the lookup API changed from "return a copyable Factory" to
    // "invoke the factory under the registry lock and return its
    // result" — see `create_initializer` below.
    //
    // Workstream N Phase 5b-9: simplified — `MarInitializerFn` now
    // returns `std::shared_ptr<Marshallable>` directly. The previous
    // `MarInitializerState` struct (post-5b-2: just `marshallable` +
    // `kind`) was redundant since `kind` is derivable as
    // `m->kind()`. The `make_initializer_state` private helper +
    // `set_marshallable_state` private setter went away with it;
    // `set_marshallable(shared_ptr<Marshallable>)` is the single
    // entry point.
    typedef rusty::Function<std::shared_ptr<rrr::Marshallable>()>
        MarInitializerFn;
    typedef rusty::HashMap<int32_t, MarInitializerFn> MarContainer;
    // The factory registry is now a file-local
    // `SpinMutex<MarContainer>`-protected static inside marshal.cpp
    // — see md_registry_locked() in marshal.cpp.
    // No external code referenced the prior `get_initializers()`
    // accessor; removed to keep the SpinMutex contained.
    // @unsafe - Registers proxy-backed initializer metadata factory.
    static int reg_initializer(int32_t, MarInitializerFn);
    // @unsafe - Registers typed default-constructible Marshallable
    // subclass. Workstream N Phase 5b-5: dropped the
    // `kHasTypedMarshallableAdapter<T>` branch — only Marshallable
    // subclasses use this path now (Serializable types register via
    // `rrr::reg_serializable_in_deputy<T>` in
    // `marshal_serializable_bridge.hpp`).
    template <typename T>
    static int reg_initializer(int32_t cmd_type)
      requires (std::is_default_constructible_v<T> &&
                std::is_base_of_v<rrr::Marshallable, T>)
    {
      return reg_initializer(cmd_type, []() -> std::shared_ptr<rrr::Marshallable> {
        return std::make_shared<T>();
      });
    }
    // @unsafe - Looks up the factory for `cmd_type` under the
    // registry SpinMutex, invokes it, and returns its product.
    // The Factory is move-only (rusty::Function), so we invoke
    // under the lock rather than copy-out. Aborts via verify() if
    // the kind is not registered.
    static std::shared_ptr<rrr::Marshallable> create_initializer(int32_t cmd_type);

  public:
    // Workstream N Phase 5b-3: removed `bypass_to_socket_` (and the
    // commented-out `written_to_socket`) — the zero-copy fast path
    // that read this field is gone (`Marshal::bypass_copying` and
    // chunk's `shared_data` infrastructure deleted in the same
    // commit).
    // Workstream N Phase 3f-1: collapsed `sp_data_` (was
    // `shared_ptr<MarshallableProxy>`) and `inner_sp_data_` into a
    // single `shared_ptr<Marshallable>` storage. The MarshallableProxy
    // form is constructed on demand inside `data_proxy()`. Public API
    // (`inner()`, `data_proxy()`-using `entity_size()`/`write_to_fd`)
    // is preserved.
    std::shared_ptr<rrr::Marshallable> inner_sp_data_;
    int32_t kind_{0};
    // Workstream N Phase 3f-2 (added field) / Phase 3f-3 (made lazy):
    // SerializableProxy view of `inner_sp_data_`. Populated lazily on
    // first call to `serializable()` rather than eagerly inside
    // `set_marshallable`, so deputies that never go through the
    // proxy-shaped accessor pay no proxy-construction cost — and the
    // common SerializableMarshallableAdapter path (an inner that is
    // already a SerializableProxy view) does not pay for stacked
    // M→S→M→S adapter wrapping that nobody reads.
    //
    // For legacy Marshallable types `serializable()` wraps the
    // underlying via `as_serializable(inner_sp_data_)` (the
    // MarshallableSerializableAdapter — save-only). For
    // already-Serializable types (entered via `as_marshallable(proxy)`
    // → SerializableMarshallableAdapter), the same path applies; the
    // resulting proxy is a save-only stacked view. Wire format is
    // unchanged — `operator<<`/`operator>>` continue to drive
    // `inner_sp_data_->to_marshal` / `from_marshal` directly.
    //
    // Stored as `mutable std::shared_ptr<SerializableProxy>` to allow
    // const-qualified `serializable() const` to populate the cache.
    // The shared_ptr indirection keeps MarshallDeputy copyable; ~25
    // call sites do `req.cmd = md;` copy-assign that would otherwise
    // need to be retrofitted with std::move. Copy semantics share the
    // cache when both sides have already populated, or independently
    // populate on first read otherwise; either way both copies wrap
    // the same underlying `inner_sp_data_` so saves through the proxy
    // produce identical bytes. SBO inside the proxy plus shared_ptr
    // aliasing keeps the per-deputy overhead at one heap allocation,
    // paid only on first proxy access.
    mutable std::shared_ptr<rrr::SerializableProxy> serializable_;
    enum Kind {
      UNKNOWN=0,
      EMPTY_GRAPH=1,
      RCC_GRAPH=2,
      CONTAINER_CMD=3,
      CMD_TPC_PREPARE=4,
      CMD_TPC_COMMIT=5,
      CMD_VEC_PIECE=6,
      CMD_BLK_PXS=7,
      CMD_BLK_PREP_PXS=8,
      CMD_HRTBT_PXS=9,
      CMD_SYNCREQ_PXS=10,
      CMD_SYNCRESP_PXS=11,
      CMD_SYNCNOOP_PXS=12,
      CMD_PREP_PXS=13,
      CMD_TPC_EMPTY=14,
      CMD_NOOP=15,
      CMD_TPC_BATCH=16,
      CMD_MULTI_STRING=18,
      CMD_REC_VEC=19,
      CMD_VIEW_DATA=20,
      CMD_KV=21,
      CMD_KEY_CMD_BATCH=22,
      CMD_REPLICATED_DB=23
    };
    /**
     * This should be called by the rpc layer.
     */
    MarshallDeputy() : kind_(UNKNOWN){}
    /**
     * This should be called by inherited class as instructor.
     * @param kind
     */
    // @safe - Constructor accepts shared_ptr<Marshallable> with polymorphism support
    // SAFETY: Moves ownership, proper null checking in usage
    //
    // L6-A2 (2026-05-01): made non-`explicit` so user code (deptran/) can
    // hand a `std::shared_ptr<Marshallable>` to any function expecting a
    // `MarshallDeputy` and the rrr boundary will wrap it automatically —
    // matching the CLAUDE.md guidance "Convert at the edge; isolate the
    // conversion in one spot; annotate the boundary `@unsafe`".  The
    // asymmetry (shared_ptr→MarshallDeputy implicit, not the reverse)
    // is intentional and lets the signature migration ripple through
    // without requiring explicit wrapping at every call site.
    MarshallDeputy(std::shared_ptr<rrr::Marshallable> m) {
      set_marshallable(std::move(m));
    }

    // @unsafe - Template constructor for derived types
    // Uses raw pointer dereference through shared_ptr->member
    // L6-A2: also non-explicit (see above).
    template<typename T>
    MarshallDeputy(std::shared_ptr<T> sp_m)
      requires std::is_base_of_v<rrr::Marshallable, T>
    {
      set_marshallable(
          std::static_pointer_cast<rrr::Marshallable>(std::move(sp_m)));
    }

    // Workstream N Phase 5b-5: simplified — only SerializableConcept T
    // remains. The bridge `wrap_typed_marshallable<T>` forward-decl
    // above lets Phase 1 unqualified lookup find it during template
    // instantiation.
    // L6-A2: also non-explicit (see above).
    template<typename T>
    MarshallDeputy(std::shared_ptr<T> sp_t)
      requires (!std::is_base_of_v<rrr::Marshallable, T> &&
                SerializableConcept<T>)
    {
      set_marshallable(std::move(sp_t));
    }

    // Workstream N Phase 5b-15: removed a 4-line commented-out
    // `reset_write_offsets()` virtual that referenced two long-gone
    // fields (`written_to_socket` deleted in 5b-3 and `sp_data_`
    // deleted in 3f-1).  The method was tied to the bypass-to-socket
    // fast path that 5b-3 retired wholesale; nothing else used it.

    rrr::Marshal& create_actual_object_from(rrr::Marshal& m);

    // @unsafe - Stores a shared_ptr<Marshallable> as the deputy's
    // payload. Workstream N Phase 5b-9: inlined the previous
    // make_initializer_state / set_marshallable_state two-step;
    // there's no separate state struct anymore.
    //
    // Workstream N Phase 3f-2: body moved to marshal.cpp so the
    // bridge helper `as_serializable(...)` (declared in
    // marshal_serializable_bridge.hpp, which depends on this header)
    // is reachable when populating `serializable_`. The header
    // dependency graph is acyclic: marshal.hpp → marshal_archive.hpp;
    // marshal_serializable_bridge.hpp → marshal.hpp, and marshal.cpp
    // pulls in `../rrr.hpp` which exports the bridge.
    void set_marshallable(std::shared_ptr<rrr::Marshallable> m);

    // @unsafe - Template delegates to non-borrow-checked set_marshallable.
    // Workstream N Phase 5b-5: simplified — only SerializableConcept T
    // remains. The bridge `wrap_typed_marshallable<T>` forward-decl
    // above lets Phase 1 unqualified lookup find it during template
    // instantiation.
    template <typename T>
    void set_marshallable(std::shared_ptr<T> typed)
      requires (!std::is_base_of_v<rrr::Marshallable, T> &&
                SerializableConcept<T>)
    {
      set_marshallable(wrap_typed_marshallable(std::move(typed)));
    }

    bool has_marshallable() const { return inner_sp_data_ != nullptr; }

    // @safe - Accessor for underlying shared_ptr (keeps legacy
    // shared_ptr& API stable for call sites that pass `md.inner()` to
    // functions taking `std::shared_ptr<Marshallable>&`).
    std::shared_ptr<rrr::Marshallable>& inner() { return inner_sp_data_; }

    const std::shared_ptr<rrr::Marshallable>& inner() const {
      return inner_sp_data_;
    }

    // @unsafe - Lazily constructs and caches a SerializableProxy view
    // of the inner Marshallable on first call. Aborts if no
    // marshallable is set.
    //
    // Workstream N Phase 3f-3: defined out-of-line in marshal.cpp so
    // the bridge helper `as_serializable(...)` is reachable (same
    // header-cycle reasoning as `set_marshallable` — the bridge
    // header depends on this header; marshal.cpp pulls in the bridge
    // through `../rrr.hpp`).
    //
    // Returns by reference so callers can do
    // `md.serializable()->save(ar)` directly; the underlying proxy
    // is `mutable`-qualified inside SerializableProxy so the
    // const-qualified accessor can dispatch through the proxy's
    // non-const `operator->`.
    rrr::SerializableProxy& serializable() const;

    // Workstream N Phase 5b-3: removed `entity_size`, `write_to_fd`,
    // `track_write_2`, and the private `data_proxy()` helper. They
    // were the deputy-side end of the dead bypass-to-socket fast
    // path (`Marshal::bypass_copying` and chunk's `shared_data`
    // infrastructure deleted in the same commit).

    ~MarshallDeputy() = default;
    // Workstream N Phase 3f-2: explicitly default the copy/move
    // special members. Declaring the destructor (= default) suppresses
    // the implicit move ctor / move assignment per [class.copy.ctor]/8;
    // without these defaults, `MarshallDeputy b = std::move(a)` would
    // fall back to copy semantics — the shared_ptr fields would not be
    // nulled in `a` after the move, and the new `serializable_` field
    // breaks our move-leaves-source-empty test expectation. With
    // these explicit defaults, copy semantics still work (matches the
    // ~25 `req.cmd = md;` call sites) and move semantics work as the
    // shared_ptr fields' default move dictates.
    MarshallDeputy(const MarshallDeputy&) = default;
    MarshallDeputy(MarshallDeputy&&) noexcept = default;
    MarshallDeputy& operator=(const MarshallDeputy&) = default;
    MarshallDeputy& operator=(MarshallDeputy&&) noexcept = default;

    // Workstream N Phase 5b-9: removed private `make_initializer_state` /
    // `set_marshallable_state` helpers. The work is now inlined in the
    // public `set_marshallable(shared_ptr<Marshallable>)` setter.
};

// Centralized cast helpers for marshallable payload extraction.
// These isolate call sites from direct dynamic_pointer_cast usage on
// MarshallDeputy::inner().
//
// Workstream N Phase 5b-5: simplified — only Marshallable subclasses
// hit this overload. SerializableConcept T's are handled by the
// matching overload in `marshal_serializable_bridge.hpp`, which
// routes through `serializable_cast<T>` and synthesizes a
// `shared_ptr<T>` aliasing the underlying
// SerializableMarshallableAdapter. Call sites continue to use
// `marshallable_cast<T>(...)` regardless of T's migration state.
template <typename T>
  requires std::is_base_of_v<Marshallable, T>
inline std::shared_ptr<T> marshallable_cast(
    const std::shared_ptr<Marshallable>& value) {
  return std::dynamic_pointer_cast<T>(value);
}

inline std::shared_ptr<Marshallable> marshallable_ref_as_shared(
    Marshallable* value) {
  if (value == nullptr) {
    return nullptr;
  }
  return std::shared_ptr<Marshallable>(value, [](Marshallable*) {});
}

template <typename T>
inline std::shared_ptr<T> marshallable_cast(Marshallable& value) {
  return marshallable_cast<T>(marshallable_ref_as_shared(&value));
}

template <typename T>
inline std::shared_ptr<T> marshallable_cast(const Marshallable& value) {
  return marshallable_cast<T>(
      marshallable_ref_as_shared(const_cast<Marshallable*>(&value)));
}

template <typename T>
inline std::shared_ptr<T> marshallable_cast(Marshallable* value) {
  if (value == nullptr) {
    return nullptr;
  }
  return marshallable_cast<T>(*value);
}

template <typename T>
inline std::shared_ptr<T> marshallable_cast(const Marshallable* value) {
  if (value == nullptr) {
    return nullptr;
  }
  return marshallable_cast<T>(*value);
}

// L6-A2 (2026-05-01): a `marshallable_cast<T>(const MarshallDeputy&)`
// overload was already provided below — user code can write
// `marshallable_cast<T>(md)` directly without reaching for `md.inner()`.
template <typename T>
inline std::shared_ptr<T> marshallable_cast(const MarshallDeputy& deputy) {
  return marshallable_cast<T>(deputy.inner());
}

template <typename T>
inline std::shared_ptr<T> marshallable_cast(MarshallDeputy& deputy) {
  return marshallable_cast<T>(deputy.inner());
}

template <typename T>
inline std::shared_ptr<T> marshallable_cast(const MarshallDeputy* deputy) {
  if (deputy == nullptr) {
    return nullptr;
  }
  return marshallable_cast<T>(*deputy);
}

template <typename T>
inline std::shared_ptr<T> marshallable_cast(MarshallDeputy* deputy) {
  if (deputy == nullptr) {
    return nullptr;
  }
  return marshallable_cast<T>(*deputy);
}

class Marshal: public NoCopy {
private:
  // Migrated from RefCounted to std::shared_ptr for automatic reference counting
  // Workstream N Phase 5b-3: removed `marshallable_entity`,
  // `shared_data`, `written_to_socket` fields and the
  // `raw_bytes(MarshallDeputy, sz)` ctor — they backed the dead
  // bypass-to-socket fast path.
  struct raw_bytes {
    char *ptr = nullptr;
    size_t size = 0;
    static const size_t min_size;

    raw_bytes(size_t sz = min_size) {
      size = std::max(sz, min_size);
      ptr = new char[size];
    }
    raw_bytes(const void *p, size_t n) {
      size = std::max(n, min_size);
      ptr = new char[size];
      memcpy(ptr, p, n);
    }

    size_t resize_to(size_t new_sz){
      size = safe_min(size, new_sz);
      //char *x = new char[size];
      //memcpy(x, ptr, size);
      //delete[] ptr;
      //ptr = x;
      return size;
    }

    raw_bytes(const raw_bytes &) = delete;
    raw_bytes &operator=(const raw_bytes &) = delete;
    ~raw_bytes() { if(ptr)delete[] ptr; }
  };

  struct chunk: public NoCopy {
   private:

    // Private constructor for shared_copy - takes shared_ptr by value, copies it
    chunk(std::shared_ptr<raw_bytes> dt, size_t rd_idx, size_t wr_idx)
        : data(dt),  // Copy shared_ptr, increments refcount
          read_idx(rd_idx),
          write_idx(wr_idx), next(nullptr) {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
    }

   public:

    std::shared_ptr<raw_bytes> data;  // Migrated from raw_bytes* to shared_ptr
    size_t read_idx;
    size_t write_idx;
    chunk *next;

    // Updated constructors to use std::make_shared instead of new.
    // Workstream N Phase 5b-3: removed `chunk(MarshallDeputy, sz)`
    // ctor (backed dead bypass-to-socket fast path).
    chunk() : data(std::make_shared<raw_bytes>()),
              read_idx(0), write_idx(0), next(nullptr) { }

    chunk(size_t sz)
        : data(std::make_shared<raw_bytes>(sz)),
          read_idx(0), write_idx(0), next(nullptr) {}

    chunk(const void *p, size_t n)
        : data(std::make_shared<raw_bytes>(p, n)),
          read_idx(0), write_idx(n), next(nullptr) { }
    // Destructor is now default - shared_ptr handles cleanup automatically
    ~chunk() = default;

    // NOTE: This function is only intended for Marshal::read_from_marshal.
    // @unsafe - Creates a new chunk sharing the same data buffer
    chunk *shared_copy() const {
      //if(read_idx != 0 && write_idx != 0) Log_info("read_idx: %d and write_idx: %d", read_idx, write_idx);
      return new chunk(data, read_idx, write_idx);
    }

    size_t resize_to_current() {
      // Workstream N Phase 5b-3: removed
      // `verify(data->shared_data == false)` — `shared_data` no
      // longer exists on raw_bytes.
      size_t sz = data->resize_to(write_idx);
      verify(data->size == write_idx);
      return sz;
    }

    // @safe - Returns the content size
    size_t content_size() const {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      return write_idx - read_idx;
    }

    // @unsafe - Returns pointer to heap data, not reference to local
    // SAFETY: Returns pointer into data->ptr array which outlives this function
    char *set_bookmark() {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);

      char* result = &data->ptr[write_idx++];

      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      return result;
    }

    size_t write(const void *p, size_t n) {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);

      size_t n_write = safe_min(n, data->size - write_idx);
      if (n_write > 0) {
        memcpy(data->ptr + write_idx, p, n_write);
      }
      write_idx += n_write;

      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      return n_write;
    }

    // @safe - Reads data from chunk buffer
    // SAFETY: Internal @unsafe block handles raw pointer arithmetic and memcpy
    size_t read(void *p, size_t n) {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);

      size_t n_read = safe_min(n, write_idx - read_idx);
      // @unsafe - raw pointer arithmetic
      {
        if (n_read > 0) {
          memcpy(p, data->ptr + read_idx, n_read);
        }
      }
      read_idx += n_read;

      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      return n_read;
    }

    // Workstream N Phase 5b-3: removed `is_shared_data_chunk()` —
    // `data->shared_data` no longer exists.

    // @safe - Peeks at data in chunk buffer
    // SAFETY: Internal @unsafe block handles raw pointer arithmetic and memcpy
    size_t peek(void *p, size_t n) const {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      size_t n_peek = safe_min(n, write_idx - read_idx);
      // @unsafe - raw pointer arithmetic
      {
        if (n_peek > 0) {
          memcpy(p, data->ptr + read_idx, n_peek);
        }
      }

      return n_peek;
    }

    size_t discard(size_t n) {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);

      size_t n_discard = safe_min(n, write_idx - read_idx);
      read_idx += n_discard;

      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      return n_discard;
    }

    // Workstream N Phase 5b-7: removed `chunk::write_to_fd(int)` —
    // its only caller was `Marshal::write_to_fd(int)` which went
    // away in the same commit (no production callers).

    // Workstream N Phase 5b-11: removed `chunk::read_from_fd(int,
    // size_t)`. Its only callers were `Marshal::read_from_fd` and
    // `Marshal::chnk_read_from_fd` — both of which were unreferenced
    // by any production caller and went away in the same commit.
    // The receive path uses `FdSource` (`marshal_archive.hpp`) for
    // direct fd reads.

    // check if it is not possible to write to the chunk anymore.
    bool fully_written() const {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      return write_idx == data->size;
    }

    // check if it is not possible to read any data even if retry later
    bool fully_read() const {
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      //Log_info("fully read %d %d", read_idx, data->size);
      return read_idx == data->size;
    }

    void reset() {
      read_idx = write_idx = 0;
    }
  };

  chunk *head_;
  chunk *tail_;
  i32 write_cnt_;
  size_t content_size_;

  // for debugging purpose
  size_t content_size_slow() const;

 public:

	bool found_dep;
  bool valid_id;

	// @unsafe - Contains raw pointer for deferred writes
	struct bookmark {
    size_t size = 0;
    char **ptr = nullptr;

    // @safe - Default constructor
    bookmark() = default;

    // Non-copyable
    bookmark(const bookmark&) = delete;
    bookmark& operator=(const bookmark&) = delete;

    // @safe - Move constructor transfers ownership
    bookmark(bookmark&& other) noexcept : size(other.size), ptr(other.ptr) {
      other.size = 0;
      other.ptr = nullptr;
    }

    // @unsafe - Move assignment (uses delete[])
    bookmark& operator=(bookmark&& other) noexcept {
      if (this != &other) {
        delete[] ptr;
        size = other.size;
        ptr = other.ptr;
        other.size = 0;
        other.ptr = nullptr;
      }
      return *this;
    }

    // @unsafe - Destructor (uses delete[])
    ~bookmark() {
      delete[] ptr;
    }
  };

  Marshal()
      : head_(nullptr), tail_(nullptr), write_cnt_(0), content_size_(0) { }
  ~Marshal();

  void init_block_read(size_t block_size){
    head_ = tail_ = new chunk(block_size);
  }

  // @safe - Simple empty check
  bool empty() const {
    assert(content_size_ == content_size_slow());
    return content_size_ == 0;
  }
  // @safe - Returns cached content size
  size_t content_size() const {
    assert(content_size_ == content_size_slow());
    return content_size_;
  }

  // @unsafe - Writes data to marshal buffer (uses raw pointer members)
  size_t write(const void *p, size_t n);
  // @safe - Reads data from marshal buffer (raw pointer version, for internal use)
  // SAFETY: Internal @unsafe block handles raw pointer operations
  size_t read(void *p, size_t n);
  // @safe - Reads data into a reference (type-safe version)
  // SAFETY: Internal @unsafe block handles raw pointer operations
  template<typename T>
  size_t read(T& out, size_t n = sizeof(T)) {
    static_assert(std::is_trivially_copyable_v<T>, "read requires trivially copyable type");
    // @unsafe - reinterpret_cast for type-safe wrapper
    {
      return read(reinterpret_cast<void*>(&out), n);
    }
  }
  // @safe - Peeks at data without consuming
  // SAFETY: Internal @unsafe block handles raw pointer operations
  template<typename T>
  size_t peek(T& out, size_t n = sizeof(T)) const {
    static_assert(std::is_trivially_copyable_v<T>, "peek requires trivially copyable type");
    // @unsafe - raw pointer operations
    {
      assert(tail_ == nullptr || tail_->next == nullptr);
      assert(empty() || (head_ != nullptr && !head_->fully_read()));
      char* pc = reinterpret_cast<char*>(&out);
      size_t n_peek = 0;
      chunk* chnk = head_;
      while (chnk != nullptr && n - n_peek > 0) {
        size_t cnt = chnk->peek(pc + n_peek, n - n_peek);
        if (cnt == 0) {
          break;
        }
        n_peek += cnt;
        chnk = chnk->next;
      }
      assert(n_peek <= n);
      assert(tail_ == nullptr || tail_->next == nullptr);
      assert(empty() || (head_ != nullptr && !head_->fully_read()));
      return n_peek;
    }
  }

  // Workstream N Phase 5b-11: removed `read_from_fd(int)` and
  // `chnk_read_from_fd(int, size_t)`. Neither had any production
  // callers; the receive path uses `FdSource`
  // (`marshal_archive.hpp`) instead.

  // @unsafe - Reuses chunks from another marshal (uses raw pointer members)
  size_t read_reuse_chnk(Marshal& m, size_t nbytes);

  // @unsafe - Reads data into chunk (uses raw pointer members)
  size_t read_chnk(void* p, size_t n);

  // NOTE: This function is only used *internally* to chop a slice of marshal object.
  // Use case 1: In C++ server io thread, when a compelete packet is received, read it off
  //             into a Marshal object and hand over to worker threads.
  // Use case 2: In Python extension, buffer message in Marshal object, and send to network.
  // @safe - Transfers data between Marshal objects
  // SAFETY: Internal @unsafe block wraps raw pointer operations (head_, tail_, chunk*)
  size_t read_from_marshal(Marshal &m, size_t n);

  // Workstream N Phase 5b-7: removed `write_to_fd(int)`. It had no
  // callers; new code uses `FdSink` (marshal_archive.hpp) to write
  // archive bytes directly to a file descriptor.

  void reset(){
    head_->reset();
    content_size_ = 0;
    write_cnt_ = 0;
  }

  // @safe - Creates bookmark for deferred writes, returns by move
  // SAFETY: Internal @unsafe block handles raw pointer operations
  bookmark set_bookmark(size_t n);

  // @safe - Writes value to bookmark locations
  // SAFETY: Internal @unsafe block handles pointer operations
  template<typename T>
  void write_bookmark(bookmark& bm, const T& value) {
    // @unsafe
    {
      static_assert(sizeof(T) <= sizeof(size_t) * 8, "bookmark value too large");
      const char *pc = reinterpret_cast<const char*>(&value);
      assert(bm.ptr != nullptr);
      for (size_t i = 0; i < bm.size; i++) {
        *(bm.ptr[i]) = pc[i];
      }
    }
  }

  // @safe - Returns and resets write counter
  i32 get_and_reset_write_cnt() {
    i32 cnt = write_cnt_;
    write_cnt_ = 0;
    return cnt;
  }

  // Workstream N Phase 5b-3: removed `bypass_copying` — the dead
  // bypass-to-socket fast path that no production type ever
  // enabled (no caller set `bypass_to_socket_=true`).
};

// @unsafe
// @lifetime: (&'a, const i8&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::i8 &v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @unsafe
// @lifetime: (&'a, const i16&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::i16 &v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @unsafe
// @lifetime: (&'a, const i32&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::i32 &v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @unsafe
// @lifetime: (&'a, const i64&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::i64 &v) {
  //Log_info("The sizeof v is: %d", sizeof(v));
  //auto start = std::chrono::steady_clock::now();
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  //auto end = std::chrono::steady_clock::now();
  //auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end-start).count();
  //Log_info("Time of << for int64 is: %d", duration);
	
	if (m.found_dep) {
		if (v != -1) {
			//Log_info("valid id: %d and %d", m.found_dep, v);
			m.valid_id = true;
		} else {
			//Log_info("invalid id: %d and %d", m.found_dep, v);
		}
		m.found_dep = false;
	}

  return m;
}

// @safe - Writes v32 to marshal
// @lifetime: (&'a, const v32&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::v32 &v) {
  // @unsafe
  {
    char buf[5];
    size_t bsize = rrr::SparseInt::dump(v.get(), buf);
    verify(m.write(buf, bsize) == bsize);
    return m;
  }
}

// @safe - Writes v64 to marshal
// @lifetime: (&'a, const v64&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rrr::v64 &v) {
  // @unsafe
  {
    char buf[9];
    size_t bsize = rrr::SparseInt::dump(v.get(), buf);
    verify(m.write(buf, bsize) == bsize);
    return m;
  }
}

// @unsafe
// @lifetime: (&'a, const uint8_t&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const uint8_t &u) {
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @unsafe
// @lifetime: (&'a, const uint16_t&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const uint16_t &u) {
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @unsafe
// @lifetime: (&'a, const uint32_t&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const uint32_t &u) {
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @unsafe
// @lifetime: (&'a, const uint64_t&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const uint64_t &u) {
  //Log_info("The sizeof u is: %d", sizeof(u));
  //auto start = std::chrono::steady_clock::now();
  verify(m.write(&u, sizeof(u)) == sizeof(u));
  //auto end = std::chrono::steady_clock::now();
  //auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end-start).count();
  //Log_info("Time of << for uint64 is: %d", duration);
  
  return m;
}

// @unsafe
// @lifetime: (&'a, const double&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const double &v) {
  verify(m.write(&v, sizeof(v)) == sizeof(v));
  return m;
}

// SAFETY: Writes string data safely with bounds checking
// @unsafe
// @lifetime: (&'a, const std::string&) -> &'a
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::string &v) {
  v64 v_len = v.length();
  m << v_len;
  if (v_len.get() > 0) {
    verify(m.write(v.c_str(), v_len.get()) == (size_t) v_len.get());
  }

	if (v == "dep") {
		// Log_info("dep: %s", v.c_str());
		m.found_dep = true;
	} else if (v == "hb") { 
		m.valid_id = true;
	} else {
    m.valid_id = true;
		// Log_info("not dep: %s", v.c_str());
	}

  return m;
}

// @unsafe
// @lifetime: (&'a, const T1&, const T2&) -> &'a
template<class T1, class T2>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::pair<T1, T2> &v) {
  // @unsafe {
    m << v.first;
    m << v.second;
    return m;
  // }
}

// @unsafe
// @lifetime: (&'a, const rusty::Vec<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rusty::Vec<T> &v) {
  // @unsafe {
    v64 v_len = v.size();
    m << v_len;
    for (typename rusty::Vec<T>::const_iterator it = v.begin(); it != v.end();
         ++it) {
      m << *it;
    }
    return m;
  // }
}

// @unsafe
// @lifetime: (&'a, const std::vector<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::vector<T> &v) {
  // Keep std::vector support for non-rrr call sites while rrr internals move to rusty containers.
  v64 v_len = v.size();
  m << v_len;
  for (typename std::vector<T>::const_iterator it = v.begin(); it != v.end();
       ++it) {
    m << *it;
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, const std::list<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::list<T> &v) {
  // @unsafe {
    v64 v_len = v.size();
    m << v_len;
    for (typename std::list<T>::const_iterator it = v.begin(); it != v.end();
         ++it) {
      m << *it;
    }
    return m;
  // }
}

// @unsafe
// @lifetime: (&'a, const rusty::BTreeSet<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rusty::BTreeSet<T> &v) {
  // @unsafe {
    v64 v_len = v.size();
    m << v_len;
    for (typename rusty::BTreeSet<T>::const_iterator it = v.begin(); it != v.end();
         ++it) {
      m << *it;
    }
    return m;
  // }
}

// @unsafe
// @lifetime: (&'a, const std::set<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::set<T> &v) {
  v64 v_len = v.size();
  m << v_len;
  for (typename std::set<T>::const_iterator it = v.begin(); it != v.end();
       ++it) {
    m << *it;
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, const rusty::BTreeMap<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const rusty::BTreeMap<K, V> &v) {
  // @unsafe {
    v64 v_len = v.size();
    m << v_len;
    // L9: rusty::BTreeMap iter `operator*()` returns
    // `std::tuple<const K&, const V&>` (post-2026-04 API).
    for (typename rusty::BTreeMap<K, V>::const_iterator it = v.begin(); it != v.end();
         ++it) {
      auto kv = *it;
      m << std::get<0>(kv) << std::get<1>(kv);
    }
    return m;
  // }
}

// @unsafe
// @lifetime: (&'a, const std::map<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const std::map<K, V> &v) {
  v64 v_len = v.size();
  m << v_len;
  for (typename std::map<K, V>::const_iterator it = v.begin(); it != v.end();
       ++it) {
    m << it->first << it->second;
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, const rusty::HashSet<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m,
                                const rusty::HashSet<T> &v) {
  // @unsafe {
    v64 v_len = v.size();
    m << v_len;
    for (typename rusty::HashSet<T>::const_iterator it = v.begin();
         it != v.end(); ++it) {
      m << *it;
    }
    return m;
  // }
}

// @unsafe
// @lifetime: (&'a, const std::unordered_set<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator<<(rrr::Marshal &m,
                                const std::unordered_set<T> &v) {
  v64 v_len = v.size();
  m << v_len;
  for (typename std::unordered_set<T>::const_iterator it = v.begin();
       it != v.end(); ++it) {
    m << *it;
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, const rusty::HashMap<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator<<(rrr::Marshal &m,
                                const rusty::HashMap<K, V> &v) {
  // @unsafe {
    v64 v_len = v.size();
    m << v_len;
    // L9: rusty::HashMap iter `operator*()` returns
    // `std::tuple<const K&, const V&>` (post-2026-04 API).
    for (typename rusty::HashMap<K, V>::const_iterator it = v.begin();
         it != v.end(); ++it) {
      auto kv = *it;
      m << std::get<0>(kv) << std::get<1>(kv);
    }
    return m;
  // }
}

// @unsafe
// @lifetime: (&'a, const std::unordered_map<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator<<(rrr::Marshal &m,
                                const std::unordered_map<K, V> &v) {
  v64 v_len = v.size();
  m << v_len;
  for (typename std::unordered_map<K, V>::const_iterator it = v.begin();
       it != v.end(); ++it) {
    m << it->first << it->second;
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, i8&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::i8 &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @unsafe
// @lifetime: (&'a, i16&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::i16 &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @unsafe
// @lifetime: (&'a, i32&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::i32 &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @unsafe
// @lifetime: (&'a, i64&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::i64 &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
	/*if (m.found_dep) {
		if (v != -1) {
			Log_info("valid id: %d", v);
			m.valid_id = true;
		} else {
			Log_info("invalid id: %d", v);
			m.valid_id = false;
		}
		m.found_dep = false;
	}*/
  return m;
}

// @unsafe
// @lifetime: (&'a, v32&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::v32 &v) {
  char byte0;
  verify(m.peek(byte0, 1) == 1);
  size_t bsize = rrr::SparseInt::buf_size(byte0);
  char buf[5];
  verify(m.read(buf, bsize) == bsize);
  i32 val = rrr::SparseInt::load_i32(buf);
  v.set(val);
  return m;
}

// @unsafe
// @lifetime: (&'a, v64&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, rrr::v64 &v) {
  char byte0;
  //Log_info("peeking data of %d", m.peek(byte0, 1));
  verify(m.peek(byte0, 1) == 1);
  size_t bsize = rrr::SparseInt::buf_size(byte0);
  char buf[9];
  verify(m.read(buf, bsize) == bsize);
  i64 val = rrr::SparseInt::load_i64(buf);
  v.set(val);
  return m;
}

// @unsafe
// @lifetime: (&'a, uint8_t&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, uint8_t &u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @unsafe
// @lifetime: (&'a, uint16_t&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, uint16_t &u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @unsafe
// @lifetime: (&'a, uint32_t&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, uint32_t &u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @unsafe
// @lifetime: (&'a, uint64_t&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, uint64_t &u) {
  verify(m.read(&u, sizeof(u)) == sizeof(u));
  return m;
}

// @unsafe
// @lifetime: (&'a, double&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, double &v) {
  verify(m.read(&v, sizeof(v)) == sizeof(v));
  return m;
}

// @unsafe
// @lifetime: (&'a, std::string&) -> &'a
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::string &v) {
  v64 v_len;
  m >> v_len;
  v.resize(v_len.get());
  if (v_len.get() > 0) {
    verify(m.read(&v[0], v_len.get()) == (size_t) v_len.get());
  }
	/*if (v == "dep") {
		Log_info("dep: %s", v.c_str());
		m.found_dep = true;
	} else {
		Log_info("not dep: %s", v.c_str());
	}*/
  return m;
}

// @unsafe
// @lifetime: (&'a, std::pair<T1,T2>&) -> &'a
template<class T1, class T2>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::pair<T1, T2> &v) {
  m >> v.first;
  m >> v.second;
  return m;
}

// @unsafe
// @lifetime: (&'a, rusty::Vec<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator>>(rrr::Marshal &m, rusty::Vec<T> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  v.reserve(v_len.get());
  for (int i = 0; i < v_len.get(); i++) {
    T elem;
    m >> elem;
    v.push_back(elem);
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, std::vector<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::vector<T> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  v.reserve(v_len.get());
  for (int i = 0; i < v_len.get(); i++) {
    T elem;
    m >> elem;
    v.push_back(elem);
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, std::list<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::list<T> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    T elem;
    m >> elem;
    v.push_back(elem);
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, rusty::BTreeSet<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator>>(rrr::Marshal &m, rusty::BTreeSet<T> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    T elem;
    m >> elem;
    v.insert(elem);
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, std::set<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::set<T> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    T elem;
    m >> elem;
    v.insert(elem);
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, rusty::BTreeMap<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator>>(rrr::Marshal &m, rusty::BTreeMap<K, V> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    K key;
    V value;
    m >> key >> value;
    insert_into_map(v, key, value);
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, std::map<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::map<K, V> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    K key;
    V value;
    m >> key >> value;
    insert_into_map(v, key, value);
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, rusty::HashSet<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator>>(rrr::Marshal &m, rusty::HashSet<T> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    T elem;
    m >> elem;
    v.insert(elem);
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, std::unordered_set<T>&) -> &'a
template<class T>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::unordered_set<T> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    T elem;
    m >> elem;
    v.insert(elem);
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, rusty::HashMap<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator>>(rrr::Marshal &m, rusty::HashMap<K, V> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    K key;
    V value;
    m >> key >> value;
    insert_into_map(v, key, value);
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, std::unordered_map<K,V>&) -> &'a
template<class K, class V>
inline rrr::Marshal &operator>>(rrr::Marshal &m, std::unordered_map<K, V> &v) {
  v64 v_len;
  m >> v_len;
  v.clear();
  for (int i = 0; i < v_len.get(); i++) {
    K key;
    V value;
    m >> key >> value;
    insert_into_map(v, key, value);
  }
  return m;
}

// @unsafe
// @lifetime: (&'a, MarshallDeputy&) -> &'a
inline rrr::Marshal& operator>>(rrr::Marshal& m, rrr::MarshallDeputy& rhs) {
  m >> rhs.kind_;
  rhs.create_actual_object_from(m);
  return m;
}

// SAFETY: Proper null checking and virtual method call
// @unsafe
// @lifetime: (&'a, const MarshallDeputy&) -> &'a
inline rrr::Marshal& operator<<(rrr::Marshal& m,const rrr::MarshallDeputy& rhs) {
  verify(rhs.kind_ != rrr::MarshallDeputy::UNKNOWN);
  verify(rhs.has_marshallable());
  // Workstream N Phase 5b-3: removed the dead `bypass_to_socket_`
  // fast path that called `m.bypass_copying(rhs, rhs.entity_size())`.
  // No production type ever set bypass_to_socket_=true.
  m << rhs.kind_;
  rhs.inner()->to_marshal(m);
  return m;
}

} // namespace rrr
