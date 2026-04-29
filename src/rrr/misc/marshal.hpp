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

#ifdef RPC_STATISTICS
void stat_marshal_in(int fd, const void* buf, size_t nbytes, ssize_t ret);
void stat_marshal_out(int fd, const void* buf, size_t nbytes, ssize_t ret);
#endif // RPC_STATISTICS

// not thread safe, for better performance
class Marshal;

class Marshallable {
 public:
  int32_t kind_{0};
  bool bypass_to_socket_ = false;
  size_t written_to_socket = 0;
//  int32_t __debug_{10};
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
  virtual size_t entity_size() const {
    verify(0);
    return 0;
  }
  // @unsafe
  virtual size_t write_to_fd(int fd, size_t written_to_socket) const {
    verify(0);
    return 0;
  }

  // virtual size_t need_to_write(){
  //   return entity_size() ; - written_to_socket;
  // }

  // virtual void reset_write_offsets(){
  //    written_to_socket = 0;
  // }
};

PRO_DEF_MEM_DISPATCH(MarshallableMemToMarshal, to_marshal);
PRO_DEF_MEM_DISPATCH(MarshallableMemFromMarshal, from_marshal);
PRO_DEF_MEM_DISPATCH(MarshallableMemEntitySize, entity_size);
PRO_DEF_MEM_DISPATCH(MarshallableMemWriteToFd, write_to_fd);
PRO_DEF_MEM_DISPATCH(MarshallableMemKind, kind);
PRO_DEF_MEM_DISPATCH(MarshallableMemInner, inner);

struct MarshallableFacade : pro::facade_builder
    ::add_convention<MarshallableMemToMarshal, Marshal&(Marshal&) const>
    ::add_convention<MarshallableMemFromMarshal, Marshal&(Marshal&)>
    ::add_convention<MarshallableMemEntitySize, size_t() const>
    ::add_convention<MarshallableMemWriteToFd, size_t(int, size_t) const>
    ::add_convention<MarshallableMemKind, int32_t() const>
    ::add_convention<MarshallableMemInner, std::shared_ptr<Marshallable>() const>
    ::build {};

using MarshallableProxy = pro::proxy<MarshallableFacade>;

class MarshallableSharedPtrAdapter {
 public:
  explicit MarshallableSharedPtrAdapter(std::shared_ptr<Marshallable> m)
      : m_(std::move(m)) {}

  Marshal& to_marshal(Marshal& out) const { return m_->to_marshal(out); }
  Marshal& from_marshal(Marshal& in) { return m_->from_marshal(in); }
  size_t entity_size() const { return m_->entity_size(); }
  size_t write_to_fd(int fd, size_t written) const {
    return m_->write_to_fd(fd, written);
  }
  int32_t kind() const { return m_->kind(); }

  std::shared_ptr<Marshallable> inner() const { return m_; }

 private:
  std::shared_ptr<Marshallable> m_;
};

inline MarshallableProxy make_marshallable_proxy(
    std::shared_ptr<Marshallable> m) {
  return pro::make_proxy<MarshallableFacade>(
      MarshallableSharedPtrAdapter(std::move(m)));
}

inline std::shared_ptr<Marshallable> marshallable_proxy_inner(
    const MarshallableProxy& proxy) {
  return proxy->inner();
}

template <typename T>
struct TypedMarshallableAdapterTraits {
  static constexpr bool kEnabled = false;
};

template <typename T>
inline constexpr bool kHasTypedMarshallableAdapter =
    TypedMarshallableAdapterTraits<T>::kEnabled;

template <typename T>
inline constexpr bool kAlwaysFalse = false;

// Adapter that makes a non-Marshallable payload type marshallable.
template <typename T, int32_t KindV>
class TypedMarshallableAdapter : public Marshallable {
 public:
  TypedMarshallableAdapter()
      : Marshallable(KindV), typed_(std::make_shared<T>()) {}

  explicit TypedMarshallableAdapter(std::shared_ptr<T> typed)
      : Marshallable(KindV), typed_(std::move(typed)) {
    verify(typed_ != nullptr);
  }

  std::shared_ptr<T> typed() const { return typed_; }

  Marshal& to_marshal(Marshal& out) const override {
    return typed_->to_marshal(out);
  }

  Marshal& from_marshal(Marshal& in) override {
    return typed_->from_marshal(in);
  }

 private:
  std::shared_ptr<T> typed_;
};

// Workstream N Phase 4a-prep: constrained to types with a
// TypedMarshallableAdapter trait. Types migrated to Serializable
// (no trait) hit the matching overload defined in
// `marshal_serializable_bridge.hpp`, which routes through
// `wrap_serializable`. This lets call sites continue to use
// `wrap_typed_marshallable(make_shared<T>())` regardless of T's
// migration state.
template <typename T>
  requires kHasTypedMarshallableAdapter<T>
inline std::shared_ptr<Marshallable> wrap_typed_marshallable(
    std::shared_ptr<T> typed) {
  verify(typed != nullptr);
  using Adapter = typename TypedMarshallableAdapterTraits<T>::Adapter;
  static_assert(std::is_base_of_v<Marshallable, Adapter>,
                "Typed adapter must inherit rrr::Marshallable");
  return std::static_pointer_cast<Marshallable>(
      std::make_shared<Adapter>(std::move(typed)));
}

// Workstream N Phase 4d-prep: forward declaration of the bridge
// overload of `wrap_typed_marshallable<T>` for SerializableConcept T.
// The actual definition lives in `marshal_serializable_bridge.hpp`
// (which is included via the rrr.hpp umbrella in production code).
//
// The forward decl is needed here so that two-phase template lookup
// inside `MarshallDeputy::set_marshallable<T>` (and the matching
// constructor) finds this overload during Phase 1 unqualified lookup
// at template definition. Without it, only the legacy
// (TypedMarshallableAdapter) overload would be visible, since ADL
// on `shared_ptr<T>` only adds `std` and T's namespace — not `rrr`.
template <typename T>
  requires (!std::is_base_of_v<Marshallable, T> &&
            !kHasTypedMarshallableAdapter<T> &&
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
    // Workstream N Phase 5b-2: dropped legacy `proxy` field.
    // Phase 3f-1 stopped consuming it inside `set_marshallable_state`,
    // and no remaining caller / test relies on it. The MarshallableProxy
    // is constructed on demand from `inner_sp_data_` via `data_proxy()`.
    struct MarInitializerState {
      std::shared_ptr<rrr::Marshallable> marshallable;
      int32_t kind{0};
    };
    typedef std::function<MarInitializerState()> MarInitializerFn;
    typedef rusty::HashMap<int32_t, MarInitializerFn> MarContainer;
    // The factory registry is now a file-local
    // `SpinMutex<MarContainer>`-protected static inside marshal.cpp
    // — see md_registry() / md_registry_locked() in marshal.cpp.
    // No external code referenced the prior `get_initializers()`
    // accessor; removed to keep the SpinMutex contained.
    // @unsafe - Registers proxy-backed initializer metadata factory.
    static int reg_initializer(int32_t, MarInitializerFn);
    // @unsafe - Registers typed default-constructible marshallable.
    template <typename T>
    static int reg_initializer(int32_t cmd_type)
      requires std::is_default_constructible_v<T>
    {
      if constexpr (std::is_base_of_v<rrr::Marshallable, T>) {
        return reg_initializer(cmd_type, []() {
          return make_initializer_state(std::make_shared<T>());
        });
      } else if constexpr (kHasTypedMarshallableAdapter<T>) {
        return reg_initializer(cmd_type, [cmd_type]() {
          auto typed = std::make_shared<T>();
          auto wrapped = wrap_typed_marshallable(std::move(typed));
          auto state = make_initializer_state(std::move(wrapped));
          verify(state.kind == cmd_type);
          return state;
        });
      } else {
        static_assert(
            kAlwaysFalse<T>,
            "reg_initializer<T>() requires T to be Marshallable or trait-enabled");
        return 0;
      }
    }
    static MarInitializerFn get_initializer(int32_t);

  public:
    bool bypass_to_socket_ = false;
    // size_t written_to_socket = 0;
    // Workstream N Phase 3f-1: collapsed `sp_data_` (was
    // `shared_ptr<MarshallableProxy>`) and `inner_sp_data_` into a
    // single `shared_ptr<Marshallable>` storage. The MarshallableProxy
    // form is constructed on demand inside `data_proxy()`. Public API
    // (`inner()`, `data_proxy()`-using `entity_size()`/`write_to_fd`)
    // is preserved.
    std::shared_ptr<rrr::Marshallable> inner_sp_data_;
    int32_t kind_{0};
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
    explicit MarshallDeputy(std::shared_ptr<rrr::Marshallable> m) {
      set_marshallable(std::move(m));
    }

    // @unsafe - Template constructor for derived types
    // Uses raw pointer dereference through shared_ptr->member
    template<typename T>
    explicit MarshallDeputy(std::shared_ptr<T> sp_m)
      requires std::is_base_of_v<rrr::Marshallable, T>
    {
      set_marshallable(
          std::static_pointer_cast<rrr::Marshallable>(std::move(sp_m)));
    }

    // Workstream N Phase 4d-prep: requires clause now also matches
    // `SerializableConcept<T>`. The forward decl above of the bridge
    // `wrap_typed_marshallable<T>` for Serializable T means this
    // template's body finds the right overload at instantiation
    // time, regardless of T's migration state. Call sites that do
    // `MarshallDeputy md(make_shared<T>())` keep working as T is
    // migrated.
    template<typename T>
    explicit MarshallDeputy(std::shared_ptr<T> sp_t)
      requires (!std::is_base_of_v<rrr::Marshallable, T> &&
                (kHasTypedMarshallableAdapter<T> ||
                 SerializableConcept<T>))
    {
      set_marshallable(std::move(sp_t));
    }

    // virtual void reset_write_offsets(){
    //   written_to_socket = 0;
    //   sp_data_->reset_write_offsets();
    // }

    rrr::Marshal& create_actual_object_from(rrr::Marshal& m);
    // @unsafe - Setter accepts shared_ptr<Marshallable> and wraps it in proxy storage.
    void set_marshallable(std::shared_ptr<rrr::Marshallable> m) {
      set_marshallable_state(make_initializer_state(std::move(m)));
    }

    // @unsafe - Template delegates to non-borrow-checked set_marshallable.
    // Workstream N Phase 4d-prep: requires clause also matches
    // `SerializableConcept<T>`. `wrap_typed_marshallable` is
    // forward-declared above for both legacy and Serializable T's;
    // the right overload is picked at instantiation time.
    template <typename T>
    void set_marshallable(std::shared_ptr<T> typed)
      requires (!std::is_base_of_v<rrr::Marshallable, T> &&
                (kHasTypedMarshallableAdapter<T> ||
                 SerializableConcept<T>))
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

    virtual size_t entity_size() const {
      verify(has_marshallable());
      return sizeof(int32_t) + data_proxy()->entity_size();
    }

    // @unsafe
    size_t track_write_2(int fd, const void* p, size_t len, size_t offset){
      const char* x = (const char*)p;
      // @unsafe {
      size_t sz = ::write(fd, x + offset, len - offset);
      // }
      if(sz > len - offset || sz <= 0){
         return 0;
      }
      return sz;
    }

    // virtual size_t need_to_write(){
    //   // for marshalldeputy we only write headers. The rest is handled by Marshallable
    //   return entity_size() - written_to_socket;
    // }

    // @unsafe
    virtual size_t write_to_fd(int fd, int written_to_socket) {
        size_t sz = 0, prev = written_to_socket;
        if(written_to_socket < sizeof(kind_)){
          sz = track_write_2(fd, &kind_, sizeof(kind_), written_to_socket);
          //Log_info("Writing the kind of MarshallDeputy %d %d", sz, written_to_socket);
          written_to_socket += sz;
          if(written_to_socket < sizeof(kind_))return sz;
        }
        //Log_info("Written bytes of ghost chunk 1 %d %d %d", sz, kind_, written_to_socket);
        // @unsafe {
        // Safety check: inner_sp_data_ must not be null when writing.
        if (!has_marshallable()) {
          Log_error("MarshallDeputy::write_to_fd called with null inner_sp_data_ (kind=%d)", kind_);
          return 0;
        }
        sz = data_proxy()->write_to_fd(fd, written_to_socket - sizeof(kind_));
        // }
	      //std::cout << sz << std::endl;
        //Log_info("Written bytes of ghost chunk 2 %d %d", sz, kind_);
        written_to_socket += sz;
        //Log_info("Written bytes of ghost chunk 3 %d %d %d", written_to_socket, kind_, entity_size());
        //Log_info("Written bytes of ghost chunk 2 %d %d", written_to_socket, kind_);
        return written_to_socket - prev;
    }

    ~MarshallDeputy() = default;

  private:
    // @unsafe - Constructs MarInitializerState from a shared_ptr<Marshallable>
    // Workstream N Phase 5b-2: no longer populates a legacy `proxy`
    // field — that field was removed alongside the dead consumer
    // path. `data_proxy()` constructs a fresh `MarshallableProxy` on
    // demand from `inner_sp_data_` for the few sites that still need
    // the proxy view.
    static MarInitializerState make_initializer_state(
        std::shared_ptr<rrr::Marshallable> m) {
      verify(m != nullptr);
      MarInitializerState state;
      state.kind = m->kind();
      state.marshallable = std::move(m);
      return state;
    }

    // @unsafe - Moves shared_ptr state into MarshallDeputy internal field
    void set_marshallable_state(MarInitializerState state) {
      verify(inner_sp_data_ == nullptr);
      verify(state.marshallable != nullptr);
      verify(state.kind != UNKNOWN);
      verify(state.kind == state.marshallable->kind());
      inner_sp_data_ = std::move(state.marshallable);
      kind_ = state.kind;
      bypass_to_socket_ = inner_sp_data_->bypass_to_socket_;
    }

    // @unsafe - Returns a fresh `MarshallableProxy` constructed from
    // `inner_sp_data_`. Callers use the result transiently, e.g.
    // `data_proxy()->entity_size()` — the temporary lives until the
    // end of the full-expression. Phase 3f-1 collapsed the previously
    // cached `sp_data_` field into this on-demand construction.
    rrr::MarshallableProxy data_proxy() const {
      verify(inner_sp_data_ != nullptr);
      return make_marshallable_proxy(inner_sp_data_);
    }
};

// Centralized cast helpers for marshallable payload extraction.
// These isolate call sites from direct dynamic_pointer_cast usage on
// MarshallDeputy::inner().
//
// Workstream N Phase 4a-prep: constrained to types reachable through
// the legacy Marshallable / TypedMarshallableAdapter machinery.
// Types migrated to Serializable (no Marshallable inheritance, no
// TypedMarshallableAdapter trait) hit the matching overload in
// `marshal_serializable_bridge.hpp`, which routes through
// `serializable_cast<T>` and synthesizes a `shared_ptr<T>` aliasing
// the underlying SerializableMarshallableAdapter. This lets call
// sites continue to use `marshallable_cast<T>(...)` regardless of T's
// migration state.
template <typename T>
  requires (std::is_base_of_v<Marshallable, T> ||
            kHasTypedMarshallableAdapter<T>)
inline std::shared_ptr<T> marshallable_cast(
    const std::shared_ptr<Marshallable>& value) {
  if constexpr (std::is_base_of_v<Marshallable, T>) {
    return std::dynamic_pointer_cast<T>(value);
  } else {  // kHasTypedMarshallableAdapter<T>
    using Adapter = typename TypedMarshallableAdapterTraits<T>::Adapter;
    auto adapter = std::dynamic_pointer_cast<Adapter>(value);
    if (adapter == nullptr) {
      return nullptr;
    }
    return adapter->typed();
  }
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
  struct raw_bytes {
    char *ptr = nullptr;
    size_t size = 0;
    static const size_t min_size;
    MarshallDeputy marshallable_entity;
    bool shared_data = false;
    size_t written_to_socket = 0;

    raw_bytes(size_t sz = min_size) {
      size = std::max(sz, min_size);
      ptr = new char[size];
    }
    raw_bytes(const void *p, size_t n) {
      size = std::max(n, min_size);
      ptr = new char[size];
      memcpy(ptr, p, n);
    }

    raw_bytes(MarshallDeputy md, size_t sz){
      marshallable_entity = md;
      size = sz;
      shared_data = true;
      //Log_info("Creating a ghost chunk here of size %d of kind %d", sz, md.kind_);
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

    // Updated constructors to use std::make_shared instead of new
    chunk() : data(std::make_shared<raw_bytes>()),
              read_idx(0), write_idx(0), next(nullptr) { }

    chunk(MarshallDeputy md, size_t sz)
        : data(std::make_shared<raw_bytes>(md, sz)),
          read_idx(0), write_idx(sz), next(nullptr) {}

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
      verify(data->shared_data == false);
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

    bool is_shared_data_chunk(){
      return data->shared_data;
    }

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

    // @safe - Writes to file descriptor (I/O system call)
    // SAFETY: Internal @unsafe block handles I/O system calls and raw pointer operations
    int write_to_fd(int fd) {
      // @unsafe
      {
        assert(write_idx <= data->size);
        struct timespec begin2, begin2_cpu, end2, end2_cpu;
        /*clock_gettime(CLOCK_MONOTONIC, &begin2);
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &begin2_cpu);*/
        int cnt;
        if(data->shared_data){
          // Safety check: marshallable_entity must have valid sp_data_
          if (!data->marshallable_entity.has_marshallable()) {
            Log_error("chunk::write_to_fd: shared_data=true but marshallable_entity has no Marshallable");
            return -1;
          }
          cnt = data->marshallable_entity.write_to_fd(fd, data->written_to_socket);
          data->written_to_socket += cnt;
          //Log_info("wrote %d bytes of ghost %d", cnt, fd);
        }
        else{
          cnt = ::write(fd, data->ptr + read_idx, write_idx - read_idx);
          //Log_info("wrote %d bytes of normal %d", cnt, fd);
        }
#ifdef RPC_STATISTICS
        if(!data->shared_data)stat_marshal_out(fd, data->ptr + write_idx, data->size - write_idx, cnt);
        else{
          Log_debug("Missed RPC stats, shared data used in raw_bytes");
        }
#endif // RPC_STATISTICS
        //if(cnt == -1)verify(0);
        if (cnt > 0) {
          /*clock_gettime(CLOCK_MONOTONIC, &end2);
          clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end2_cpu);
          long total_cpu2 = (end2_cpu.tv_sec - begin2_cpu.tv_sec)*1000000000 + (end2_cpu.tv_nsec - begin2_cpu.tv_nsec);
          long total_time2 = (end2.tv_sec - begin2.tv_sec)*1000000000 + (end2.tv_nsec - begin2.tv_nsec);
          double util2 = (double) total_cpu2/total_time2;
          Log_info("elapsed CPU time (fd write of %d): %f", write_idx - read_idx, util2);*/
          read_idx += cnt;
        }

        assert(write_idx <= data->size);
        return cnt;
      }
    }

    // @unsafe - Reads from file descriptor (I/O system call)
    int read_from_fd(int fd, size_t bytes = -1) {
      if(bytes == -1)bytes = data->size - write_idx;
      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);

      int cnt = 0;
      if (write_idx < data->size) {
        cnt = ::read(fd, data->ptr + write_idx, bytes);
        if (cnt<=0){
          return cnt;
        }
#ifdef RPC_STATISTICS
        stat_marshal_in(fd, data->ptr + write_idx, bytes, cnt);
#endif // RPC_STATISTICS

        if (cnt > 0) {
          write_idx += cnt;
        }
      }

      assert(write_idx <= data->size);
      assert(read_idx <= write_idx);
      return cnt;
    }

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

  // @safe - Reads from file descriptor (I/O system call)
  // SAFETY: Internal @unsafe block handles I/O and raw pointer operations
  size_t read_from_fd(int fd);

  // @unsafe - Reads from file descriptor into chunk (I/O system call)
  size_t chnk_read_from_fd(int fd, size_t bytes);

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

  // @safe - Writes to file descriptor (I/O system call)
  // SAFETY: Internal @unsafe block handles I/O and raw pointer operations
  size_t write_to_fd(int fd);

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

  // @safe - Bypasses copying by sharing chunk pointers
  size_t bypass_copying(rrr::MarshallDeputy, size_t);
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
    for (typename rusty::BTreeMap<K, V>::const_iterator it = v.begin(); it != v.end();
         ++it) {
      m << it->first << it->second;
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
    for (typename rusty::HashMap<K, V>::const_iterator it = v.begin();
         it != v.end(); ++it) {
      m << it->first << it->second;
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
  if(rhs.bypass_to_socket_){
    m.bypass_copying(rhs, rhs.entity_size());
  }else{
    //Log_info("size is %d", rhs.entity_size());
    m << rhs.kind_;
    verify(rhs.has_marshallable()); // must be non-empty
    rhs.inner()->to_marshal(m);
  }
  return m;
}

} // namespace rrr
