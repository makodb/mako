#ifndef _NDB_BENCH_INLINE_STR_H_
#define _NDB_BENCH_INLINE_STR_H_

#include <stdint.h>
#include <string.h>

#include <string>
#include <ostream>

#include "../macros.h"
#include "serializer.h"

// equivalent to VARCHAR(N)

template <typename IntSizeType, unsigned int N>
class inline_str_base {
  // XXX: argh...
  template <typename T, bool DoCompress> friend class serializer;
public:

  inline_str_base() : sz(0) {}

  inline_str_base(const char *s)
  {
    assign(s);
  }

  inline_str_base(const char *s, size_t n)
  {
    assign(s, n);
  }

  inline_str_base(const std::string &s)
  {
    assign(s);
  }

  inline_str_base(const inline_str_base &that)
    : sz(that.sz > static_cast<IntSizeType>(N) ? static_cast<IntSizeType>(N) : that.sz)
  {
    NDB_MEMCPY(&buf[0], &that.buf[0], sz);
  }

  inline_str_base &
  operator=(const inline_str_base &that)
  {
    if (this == &that)
      return *this;
    // Defense-in-depth clamp: an optimistic read can temporarily expose a
    // source struct whose length byte is inconsistent with its fixed buffer.
    // If the page is corrupted upstream (as we hit on 2026-04-25 — a 4-byte
    // heap overflow in client.cc's InvokeInstall produced a customer row
    // whose c_first.sz byte happened to read 106), an unchecked memcpy of
    // `that.sz` bytes will blast `that.sz` bytes into our `buf[N+1]` and
    // trash the surrounding stack frame. Clamp to `N` so a corrupt source
    // byte can never overflow the destination.
    const IntSizeType n = (that.sz > N) ? static_cast<IntSizeType>(N) : that.sz;
    sz = n;
    NDB_MEMCPY(&buf[0], &that.buf[0], n);
    return *this;
  }

  inline size_t
  max_size() const
  {
    return N;
  }

  inline const char *
  c_str() const
  {
    buf[sz] = 0;
    return &buf[0];
  }

  inline std::string
  str(bool zeropad = false) const
  {
		if (zeropad) {
			INVARIANT(N >= sz);
			std::string r(N, 0);
			NDB_MEMCPY((char *) r.data(), &buf[0], sz);
			return r;
		} else {
			return std::string(&buf[0], sz);
		}
  }

  inline ALWAYS_INLINE const char *
  data() const
  {
    return &buf[0];
  }

  inline ALWAYS_INLINE size_t
  size() const
  {
    return sz;
  }

  inline ALWAYS_INLINE void
  assign(const char *s)
  {
    assign(s, strlen(s));
  }

  inline void trim() { sz = sz>N ? N : sz; }

  inline void
  assign(const char *s, size_t n)
  {
    INVARIANT(n <= N);
    NDB_MEMCPY(&buf[0], s, n);
    sz = n;
    buf[sz] = 0;
  }

  inline ALWAYS_INLINE void
  assign(const std::string &s)
  {
    assign(s.data(), s.size());
  }

  inline void
  resize(size_t n, char c = 0)
  {
    INVARIANT(n <= N);
    if (n > sz)
      NDB_MEMSET(&buf[sz], c, n - sz);
    sz = n;
  }

  inline void
  resize_junk(size_t n)
  {
    INVARIANT(n <= N);
    sz = n;
    buf[sz] = 0;
  }

  inline bool
  operator==(const inline_str_base &other) const
  {
    return memcmp(buf, other.buf, sz) == 0;
  }

  inline bool
  operator!=(const inline_str_base &other) const
  {
    return !operator==(other);
  }

private:
  IntSizeType sz;
  mutable char buf[N + 1];
} PACKED;

template <typename IntSizeType, unsigned int N>
inline std::ostream &
operator<<(std::ostream &o, const inline_str_base<IntSizeType, N> &s)
{
  o << std::string(s.data(), s.size());
  return o;
}

template <unsigned int N>
class inline_str_8 : public inline_str_base<uint8_t, N> {
  typedef inline_str_base<uint8_t, N> super_type;
public:
  inline_str_8() : super_type() {}
  inline_str_8(const char *s) : super_type(s) {}
  inline_str_8(const char *s, size_t n) : super_type(s, n) {}
  inline_str_8(const std::string &s) : super_type(s) {}
} PACKED;

template <unsigned int N>
class inline_str_16 : public inline_str_base<uint16_t, N> {
  typedef inline_str_base<uint16_t, N> super_type;
public:
  inline_str_16() : super_type() {}
  inline_str_16(const char *s) : super_type(s) {}
  inline_str_16(const char *s, size_t n) : super_type(s, n) {}
  inline_str_16(const std::string &s) : super_type(s) {}
} PACKED;

// equiavlent to CHAR(N)
template <unsigned int N, char FillChar = ' '>
class inline_str_fixed {
  // XXX: argh...
  template <typename T, bool DoCompress> friend class serializer;
public:
  inline_str_fixed()
  {
    NDB_MEMSET(&buf[0], FillChar, N);
  }

  inline_str_fixed(const char *s)
  {
    assign(s, strlen(s));
  }

  inline_str_fixed(const char *s, size_t n)
  {
    assign(s, n);
  }

  inline_str_fixed(const std::string &s)
  {
    assign(s.data(), s.size());
  }

  inline_str_fixed(const inline_str_fixed &that)
  {
    NDB_MEMCPY(&buf[0], &that.buf[0], N);
  }

  inline_str_fixed &
  operator=(const inline_str_fixed &that)
  {
    if (this == &that)
      return *this;
    NDB_MEMCPY(&buf[0], &that.buf[0], N);
    return *this;
  }

  inline ALWAYS_INLINE std::string
  str() const
  {
    return std::string(&buf[0], N);
  }

  inline ALWAYS_INLINE const char *
  data() const
  {
    return &buf[0];
  }

  inline ALWAYS_INLINE size_t
  size() const
  {
    return N;
  }

  inline ALWAYS_INLINE void
  assign(const char *s)
  {
    assign(s, strlen(s));
  }

  inline void
  assign(const char *s, size_t n)
  {
    INVARIANT(n <= N);
    NDB_MEMCPY(&buf[0], s, n);
    if ((N - n) > 0) // to suppress compiler warning
      NDB_MEMSET(&buf[n], FillChar, N - n); // pad with spaces
  }

  inline ALWAYS_INLINE void
  assign(const std::string &s)
  {
    assign(s.data(), s.size());
  }

  inline bool
  operator==(const inline_str_fixed &other) const
  {
    return memcmp(buf, other.buf, N) == 0;
  }

  inline bool
  operator!=(const inline_str_fixed &other) const
  {
    return !operator==(other);
  }

private:
  char buf[N];
} PACKED;

template <unsigned int N, char FillChar>
inline std::ostream &
operator<<(std::ostream &o, const inline_str_fixed<N, FillChar> &s)
{
  o << std::string(s.data(), s.size());
  return o;
}

// The record format historically advances by sizeof(T) for the concrete
// inline-string wrappers, even though variable strings only populate their
// length prefix and live characters. Decode their logical fields explicitly:
// treating a byte stream as a nontrivial C++ object would violate object
// lifetime rules.
template <typename String, typename SizeType, unsigned int N>
struct fixed_layout_inline_str_serializer {
  typedef String obj_type;

  static_assert(alignof(obj_type) == 1,
                "inline string wire values must remain byte-aligned");
  static_assert(sizeof(obj_type) == sizeof(SizeType) + N + 1,
                "inline string layout changed");

  static inline uint8_t *
  write(uint8_t *buf, const obj_type &obj)
  {
    const size_t size = obj.size() > N ? N : obj.size();
    const SizeType wire_size = static_cast<SizeType>(size);
    NDB_MEMCPY(buf, &wire_size, sizeof(wire_size));
    NDB_MEMCPY(buf + sizeof(wire_size), obj.data(), size);
    return buf + sizeof(obj_type);
  }

  static inline const uint8_t *
  read(const uint8_t *buf, obj_type *obj)
  {
    SizeType wire_size;
    NDB_MEMCPY(&wire_size, buf, sizeof(wire_size));
    const size_t size = wire_size > N ? N : wire_size;
    obj->assign(reinterpret_cast<const char *>(buf + sizeof(wire_size)), size);
    return buf + sizeof(obj_type);
  }

  static inline const uint8_t *
  failsafe_read(const uint8_t *buf, size_t nbytes, obj_type *obj)
  {
    if (unlikely(nbytes < sizeof(obj_type)))
      return nullptr;
    SizeType wire_size;
    NDB_MEMCPY(&wire_size, buf, sizeof(wire_size));
    if (unlikely(wire_size > N))
      return nullptr;
    obj->assign(reinterpret_cast<const char *>(buf + sizeof(wire_size)),
                wire_size);
    return buf + sizeof(obj_type);
  }

  static inline size_t
  nbytes(const obj_type *)
  {
    return sizeof(obj_type);
  }

  static inline size_t
  skip(const uint8_t *stream, uint8_t *rawv)
  {
    if (rawv)
      NDB_MEMCPY(rawv, stream, sizeof(obj_type));
    return sizeof(obj_type);
  }

  static inline size_t
  failsafe_skip(const uint8_t *stream, size_t nbytes, uint8_t *rawv)
  {
    if (unlikely(nbytes < sizeof(obj_type)))
      return 0;
    return skip(stream, rawv);
  }

  static inline constexpr size_t
  max_nbytes()
  {
    return sizeof(obj_type);
  }
};

template <unsigned int N, bool Compress>
struct serializer<inline_str_8<N>, Compress>
  : fixed_layout_inline_str_serializer<inline_str_8<N>, uint8_t, N> {};

template <unsigned int N, bool Compress>
struct serializer<inline_str_16<N>, Compress>
  : fixed_layout_inline_str_serializer<inline_str_16<N>, uint16_t, N> {};

template <typename String, unsigned int N>
struct fixed_layout_char_serializer {
  typedef String obj_type;

  static_assert(alignof(obj_type) == 1,
                "fixed inline string wire values must remain byte-aligned");
  static_assert(sizeof(obj_type) == N, "fixed inline string layout changed");

  static inline uint8_t *
  write(uint8_t *buf, const obj_type &obj)
  {
    NDB_MEMCPY(buf, obj.data(), N);
    return buf + sizeof(obj_type);
  }

  static inline const uint8_t *
  read(const uint8_t *buf, obj_type *obj)
  {
    obj->assign(reinterpret_cast<const char *>(buf), N);
    return buf + sizeof(obj_type);
  }

  static inline const uint8_t *
  failsafe_read(const uint8_t *buf, size_t nbytes, obj_type *obj)
  {
    if (unlikely(nbytes < sizeof(obj_type)))
      return nullptr;
    return read(buf, obj);
  }

  static inline size_t
  nbytes(const obj_type *)
  {
    return sizeof(obj_type);
  }

  static inline size_t
  skip(const uint8_t *stream, uint8_t *rawv)
  {
    if (rawv)
      NDB_MEMCPY(rawv, stream, sizeof(obj_type));
    return sizeof(obj_type);
  }

  static inline size_t
  failsafe_skip(const uint8_t *stream, size_t nbytes, uint8_t *rawv)
  {
    if (unlikely(nbytes < sizeof(obj_type)))
      return 0;
    return skip(stream, rawv);
  }

  static inline constexpr size_t
  max_nbytes()
  {
    return sizeof(obj_type);
  }
};

template <unsigned int N, char FillChar, bool Compress>
struct serializer<inline_str_fixed<N, FillChar>, Compress>
  : fixed_layout_char_serializer<inline_str_fixed<N, FillChar>, N> {};

// serializer<T> specialization
template <typename IntSizeType, unsigned int N, bool Compress>
struct serializer< inline_str_base<IntSizeType, N>, Compress > {
  typedef inline_str_base<IntSizeType, N> obj_type;
  static inline uint8_t *
  write(uint8_t *buf, const obj_type &obj)
  {
    buf = serializer<IntSizeType, Compress>::write(buf, obj.sz);
    NDB_MEMCPY(buf, &obj.buf[0], obj.sz);
    return buf + obj.sz;
  }

  static const uint8_t *
  read(const uint8_t *buf, obj_type *obj)
  {
    buf = serializer<IntSizeType, Compress>::read(buf, &obj->sz);
    // Clamp to prevent stack buffer overflow when source bytes are transiently
    // inconsistent (optimistic read racing a concurrent write). The atomicRead
    // retry loop detects the version mismatch and discards the garbage result.
    if (unlikely(obj->sz > N))
      obj->sz = static_cast<IntSizeType>(N);
    NDB_MEMCPY(&obj->buf[0], buf, obj->sz);
    return buf + obj->sz;
  }

  static const uint8_t *
  failsafe_read(const uint8_t *buf, size_t nbytes, obj_type *obj)
  {
    IntSizeType wire_size;
    const uint8_t * const hdrbuf =
      serializer<IntSizeType, Compress>::failsafe_read(
          buf, nbytes, &wire_size);
    if (unlikely(!hdrbuf))
      return nullptr;
    nbytes -= (hdrbuf - buf);
    if (unlikely(wire_size > N || nbytes < wire_size))
      return nullptr;
    obj->sz = wire_size;
    buf = hdrbuf;
    NDB_MEMCPY(&obj->buf[0], buf, wire_size);
    return buf + wire_size;
  }

  static inline size_t
  nbytes(const obj_type *obj)
  {
    return serializer<IntSizeType, Compress>::nbytes(&obj->sz) + obj->sz;
  }

  static inline size_t
  skip(const uint8_t *stream, uint8_t *oldv)
  {
    IntSizeType sz = 0;
    const uint8_t * const body = serializer<IntSizeType, Compress>::read(stream, &sz);
    const size_t totalsz = (body - stream) + sz;
    if (oldv)
      NDB_MEMCPY(oldv, stream, totalsz);
    return totalsz;
  }

  static inline size_t
  failsafe_skip(const uint8_t *stream, size_t nbytes, uint8_t *oldv)
  {
    IntSizeType sz = 0;
    const uint8_t * const body =
      serializer<IntSizeType, Compress>::failsafe_read(stream, nbytes, &sz);
    if (unlikely(!body))
      return 0;
    nbytes -= (body - stream);
    if (unlikely(sz > N || nbytes < sz))
      return 0;
    const size_t totalsz = (body - stream) + sz;
    if (oldv)
      NDB_MEMCPY(oldv, stream, totalsz);
    return totalsz;
  }

  static inline constexpr size_t
  max_nbytes()
  {
    return serializer<IntSizeType, Compress>::max_nbytes() + N;
  }
};

#endif /* _NDB_BENCH_INLINE_STR_H_ */
