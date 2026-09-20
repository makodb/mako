#ifndef _NDB_BENCH_SERIALIZER_H_
#define _NDB_BENCH_SERIALIZER_H_

#include <new>
#include <stdint.h>
#include <type_traits>
#include "../macros.h"
#include "../varint.h"

typedef uint8_t *(*generic_write_fn)(uint8_t *, const uint8_t *);
typedef const uint8_t *(*generic_read_fn)(const uint8_t *, uint8_t *);
typedef const uint8_t *(*generic_failsafe_read_fn)(const uint8_t *, size_t, uint8_t *);
typedef size_t (*generic_nbytes_fn)(const uint8_t *);
typedef size_t (*generic_skip_fn)(const uint8_t *, uint8_t *);
typedef size_t (*generic_failsafe_skip_fn)(const uint8_t *, size_t, uint8_t *);

// wraps a real serializer, exposing generic functions
template <typename Serializer>
struct generic_serializer {
  typedef typename Serializer::obj_type obj_type;
  static_assert(std::is_trivially_copyable_v<obj_type> ||
                  alignof(obj_type) == 1,
                "nontrivial generic serializers require byte alignment");

  static inline const obj_type *
  aligned_object(const uint8_t *source, uint8_t *storage)
  {
    NDB_MEMCPY(storage, source, sizeof(obj_type));
    return std::launder(reinterpret_cast<const obj_type *>(storage));
  }

  static inline obj_type *
  aligned_object(uint8_t *source, uint8_t *storage)
  {
    NDB_MEMCPY(storage, source, sizeof(obj_type));
    return std::launder(reinterpret_cast<obj_type *>(storage));
  }

  static inline uint8_t *
  write(uint8_t *buf, const uint8_t *obj)
  {
    if constexpr (std::is_trivially_copyable_v<obj_type>) {
      alignas(obj_type) uint8_t storage[sizeof(obj_type)];
      return Serializer::write(buf, *aligned_object(obj, storage));
    } else {
      return Serializer::write(buf, *reinterpret_cast<const obj_type *>(obj));
    }
  }

  static inline const uint8_t *
  read(const uint8_t *buf, uint8_t *obj)
  {
    if constexpr (std::is_trivially_copyable_v<obj_type>) {
      alignas(obj_type) uint8_t storage[sizeof(obj_type)];
      obj_type * const aligned_obj = aligned_object(obj, storage);
      const uint8_t * const next = Serializer::read(buf, aligned_obj);
      NDB_MEMCPY(obj, aligned_obj, sizeof(obj_type));
      return next;
    } else {
      return Serializer::read(buf, reinterpret_cast<obj_type *>(obj));
    }
  }

  // returns nullptr on failure
  static inline const uint8_t *
  failsafe_read(const uint8_t *buf, size_t nbytes, uint8_t *obj)
  {
    if constexpr (std::is_trivially_copyable_v<obj_type>) {
      alignas(obj_type) uint8_t storage[sizeof(obj_type)];
      obj_type * const aligned_obj = aligned_object(obj, storage);
      const uint8_t * const next =
        Serializer::failsafe_read(buf, nbytes, aligned_obj);
      if (next)
        NDB_MEMCPY(obj, aligned_obj, sizeof(obj_type));
      return next;
    } else {
      return Serializer::failsafe_read(
          buf, nbytes, reinterpret_cast<obj_type *>(obj));
    }
  }

  static inline size_t
  nbytes(const uint8_t *obj)
  {
    if constexpr (std::is_trivially_copyable_v<obj_type>) {
      alignas(obj_type) uint8_t storage[sizeof(obj_type)];
      return Serializer::nbytes(aligned_object(obj, storage));
    } else {
      return Serializer::nbytes(reinterpret_cast<const obj_type *>(obj));
    }
  }

  static inline size_t
  skip(const uint8_t *stream, uint8_t *rawv)
  {
    return Serializer::skip(stream, rawv);
  }

  // returns 0 on failure
  static inline size_t
  failsafe_skip(const uint8_t *stream, size_t nbytes, uint8_t *rawv)
  {
    return Serializer::failsafe_skip(stream, nbytes, rawv);
  }

  static inline constexpr size_t
  max_nbytes()
  {
    return Serializer::max_nbytes();
  }
};

template <typename T, bool Compress>
struct serializer {
  static_assert(std::is_trivially_copyable_v<T>,
                "nontrivial types require an explicit serializer");

  typedef T obj_type;
  typedef std::conditional_t<std::is_copy_constructible_v<T>, T,
                             const T &> write_arg_type;

  static inline uint8_t *
  write(uint8_t *buf, write_arg_type obj)
  {
    NDB_MEMCPY(buf, &obj, sizeof(T));
    return buf + sizeof(T);
  }

  static inline const uint8_t *
  read(const uint8_t *buf, T *obj)
  {
    NDB_MEMCPY(reinterpret_cast<uint8_t *>(obj), buf, sizeof(T));
    return buf + sizeof(T);
  }

  static inline const uint8_t *
  failsafe_read(const uint8_t *buf, size_t nbytes, T *obj)
  {
    if (unlikely(nbytes < sizeof(T)))
      return nullptr;
    return read(buf, obj);
  }

  static inline size_t
  nbytes(const T *obj)
  {
    (void)obj;
    return sizeof(T);
  }

  static inline size_t
  skip(const uint8_t *stream, uint8_t *rawv)
  {
    if (rawv)
      NDB_MEMCPY(rawv, stream, sizeof(T));
    return sizeof(T);
  }

  static inline size_t
  failsafe_skip(const uint8_t *stream, size_t nbytes, uint8_t *rawv)
  {
    if (unlikely(nbytes < sizeof(T)))
      return 0;
    if (rawv)
      NDB_MEMCPY(rawv, stream, sizeof(T));
    return sizeof(T);
  }

  static inline constexpr size_t
  max_nbytes()
  {
    return sizeof(T);
  }
};

// serializer<T, True> specializations
template <>
struct serializer<uint32_t, true> {
  typedef uint32_t obj_type;

  static inline uint8_t *
  write(uint8_t *buf, uint32_t obj)
  {
    return write_uvint32(buf, obj);
  }

  static inline const uint8_t *
  read(const uint8_t *buf, uint32_t *obj)
  {
    uint32_t aligned_obj;
    const uint8_t * const next = read_uvint32(buf, &aligned_obj);
    NDB_MEMCPY(reinterpret_cast<uint8_t *>(obj), &aligned_obj,
               sizeof(aligned_obj));
    return next;
  }

  static inline const uint8_t *
  failsafe_read(const uint8_t *buf, size_t nbytes, uint32_t *obj)
  {
    uint32_t aligned_obj;
    const uint8_t * const next =
      failsafe_read_uvint32(buf, nbytes, &aligned_obj);
    if (next)
      NDB_MEMCPY(reinterpret_cast<uint8_t *>(obj), &aligned_obj,
                 sizeof(aligned_obj));
    return next;
  }

  static inline size_t
  nbytes(const uint32_t *obj)
  {
    uint32_t aligned_obj;
    NDB_MEMCPY(&aligned_obj, reinterpret_cast<const uint8_t *>(obj),
               sizeof(aligned_obj));
    return size_uvint32(aligned_obj);
  }

  static inline size_t
  skip(const uint8_t *stream, uint8_t *rawv)
  {
    return skip_uvint32(stream, rawv);
  }

  static inline size_t
  failsafe_skip(const uint8_t *stream, size_t nbytes, uint8_t *rawv)
  {
    return failsafe_skip_uvint32(stream, nbytes, rawv);
  }

  static inline constexpr size_t
  max_nbytes()
  {
    return 5;
  }
};

template <>
struct serializer<int32_t, true> {
  typedef int32_t obj_type;

  static inline uint8_t *
  write(uint8_t *buf, int32_t obj)
  {
    const uint32_t v = encode(obj);
    return serializer<uint32_t, true>::write(buf, v);
  }

  static inline const uint8_t *
  read(const uint8_t *buf, int32_t *obj)
  {
    uint32_t v;
    buf = serializer<uint32_t, true>::read(buf, &v);
    const int32_t decoded = decode(v);
    NDB_MEMCPY(reinterpret_cast<uint8_t *>(obj), &decoded, sizeof(decoded));
    return buf;
  }

  static inline const uint8_t *
  failsafe_read(const uint8_t *buf, size_t nbytes, int32_t *obj)
  {
    uint32_t v;
    buf = serializer<uint32_t, true>::failsafe_read(buf, nbytes, &v);
    if (unlikely(!buf))
      return 0;
    const int32_t decoded = decode(v);
    NDB_MEMCPY(reinterpret_cast<uint8_t *>(obj), &decoded, sizeof(decoded));
    return buf;
  }

  static inline size_t
  nbytes(const int32_t *obj)
  {
    int32_t aligned_obj;
    NDB_MEMCPY(&aligned_obj, reinterpret_cast<const uint8_t *>(obj),
               sizeof(aligned_obj));
    const uint32_t v = encode(aligned_obj);
    return serializer<uint32_t, true>::nbytes(&v);
  }

  static inline size_t
  skip(const uint8_t *stream, uint8_t *rawv)
  {
    return skip_uvint32(stream, rawv);
  }

  static inline size_t
  failsafe_skip(const uint8_t *stream, size_t nbytes, uint8_t *rawv)
  {
    return failsafe_skip_uvint32(stream, nbytes, rawv);
  }

  static inline constexpr size_t
  max_nbytes()
  {
    return 5;
  }

private:
  // zig-zag encoding from:
  // http://code.google.com/p/protobuf/source/browse/trunk/src/google/protobuf/wire_format_lite.h

  static inline ALWAYS_INLINE constexpr uint32_t
  encode(int32_t value)
  {
    return (static_cast<uint32_t>(value) << 1) ^
      static_cast<uint32_t>(-(value < 0));
  }

  static inline ALWAYS_INLINE constexpr int32_t
  decode(uint32_t value)
  {
    return (value >> 1) ^ -static_cast<int32_t>(value & 1);
  }
};

#endif /* _NDB_BENCH_SERIALIZER_H_ */
