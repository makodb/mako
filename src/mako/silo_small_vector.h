#ifndef _SILO_SMALL_VECTOR_H_
#define _SILO_SMALL_VECTOR_H_

#include <algorithm>
#include <vector>
#include <type_traits>

#include "macros.h"
#include "masstree/compiler.hh"

/**
 * A small vector optimization container that stores elements inline for small sizes
 * and switches to heap allocation for larger sizes.
 * 
 * References are not guaranteed to be stable across mutation operations.
 * 
 * @tparam T The element type
 * @tparam SmallSize The number of elements to store inline before switching to heap allocation
 */
template <typename T, size_t SmallSize = SMALL_SIZE_VEC>
class silo_small_vector {
  typedef std::vector<T> large_vector_type;

  static const bool is_trivially_destructible =
    mass::is_trivially_destructible<T>::value;

  // std::is_trivially_copyable not supported in g++-4.7
  static const bool is_trivially_copyable = std::is_scalar<T>::value;

public:

  typedef T value_type;
  typedef T & reference;
  typedef const T & const_reference;
  typedef size_t size_type;

  silo_small_vector() : small_size_(0), large_elems(0) {}
  ~silo_small_vector()
  {
    clearDestructive();
  }

  silo_small_vector(const silo_small_vector &that)
    : small_size_(0), large_elems(0)
  {
    assignFrom(that);
  }

  // not efficient, don't use in performance critical parts
  silo_small_vector(std::initializer_list<T> init_list)
    : small_size_(0), large_elems(nullptr)
  {
    if (init_list.size() > SmallSize) {
      large_elems = new large_vector_type(init_list);
    } else {
      for (const auto &element : init_list)
        push_back(element);
    }
  }

  silo_small_vector &
  operator=(const silo_small_vector &that)
  {
    assignFrom(that);
    return *this;
  }

  inline size_t
  size() const
  {
    if (unlikely(large_elems))
      return large_elems->size();
    return small_size_;
  }

  inline bool
  empty() const
  {
    return size() == 0;
  }

  inline reference
  front()
  {
    if (unlikely(large_elems))
      return large_elems->front();
    INVARIANT(small_size_ > 0);
    INVARIANT(small_size_ <= SmallSize);
    return *buffer_ptr();
  }

  inline const_reference
  front() const
  {
    if (unlikely(large_elems))
      return large_elems->front();
    INVARIANT(small_size_ > 0);
    INVARIANT(small_size_ <= SmallSize);
    return *buffer_ptr();
  }

  inline reference
  back()
  {
    if (unlikely(large_elems))
      return large_elems->back();
    INVARIANT(small_size_ > 0);
    INVARIANT(small_size_ <= SmallSize);
    return buffer_ptr()[small_size_ - 1];
  }

  inline const_reference
  back() const
  {
    if (unlikely(large_elems))
      return large_elems->back();
    INVARIANT(small_size_ > 0);
    INVARIANT(small_size_ <= SmallSize);
    return buffer_ptr()[small_size_ - 1];
  }

  inline void
  pop_back()
  {
    if (unlikely(large_elems)) {
      large_elems->pop_back();
      return;
    }
    INVARIANT(small_size_ > 0);
    if (!is_trivially_destructible)
      buffer_ptr()[small_size_ - 1].~T();
    small_size_--;
  }

  inline void
  push_back(const T &obj)
  {
    emplace_back(obj);
  }

  inline void
  push_back(T &&obj)
  {
    emplace_back(std::move(obj));
  }

  // C++11 goodness- a strange syntax this is

  template <class... Args>
  inline void
  emplace_back(Args &&... args)
  {
    if (unlikely(large_elems)) {
      INVARIANT(!small_size_);
      large_elems->emplace_back(std::forward<Args>(args)...);
      return;
    }
    if (unlikely(small_size_ == SmallSize)) {
      large_elems = new large_vector_type(buffer_ptr(), buffer_ptr() + small_size_);
      large_elems->emplace_back(std::forward<Args>(args)...);
      small_size_ = 0;
      return;
    }
    INVARIANT(small_size_ < SmallSize);
    new (&(buffer_ptr()[small_size_++])) T(std::forward<Args>(args)...);
  }

  inline reference
  operator[](int index)
  {
    if (unlikely(large_elems))
      return large_elems->operator[](index);
    return buffer_ptr()[index];
  }

  inline const_reference
  operator[](int index) const
  {
    if (unlikely(large_elems))
      return large_elems->operator[](index);
    return buffer_ptr()[index];
  }

  void
  clear()
  {
    if (unlikely(large_elems)) {
      INVARIANT(!small_size_);
      large_elems->clear();
      return;
    }
    if (!is_trivially_destructible)
      for (size_t element_index = 0; element_index < small_size_; element_index++)
        buffer_ptr()[element_index].~T();
    small_size_ = 0;
  }

  inline void
  reserve(size_t capacity)
  {
    if (unlikely(large_elems)) {
      large_elems->reserve(capacity);
    } else if (capacity > SmallSize) {
      // Logic improvement: Pre-allocate large vector if requested capacity exceeds small size
      large_elems = new large_vector_type();
      large_elems->reserve(capacity);
      // Move existing small elements to large vector
      for (size_t element_index = 0; element_index < small_size_; element_index++) {
        large_elems->emplace_back(std::move(buffer_ptr()[element_index]));
        if (!is_trivially_destructible)
          buffer_ptr()[element_index].~T();
      }
      small_size_ = 0;
    }
    // If capacity <= SmallSize and we're using small storage, no action needed
  }

  // non-standard API
  inline bool is_using_small_buffer() const { return !large_elems; }

  template <typename Compare = std::less<T>>
  inline void
  sort(Compare comparator = Compare())
  {
    if (unlikely(large_elems))
      std::sort(large_elems->begin(), large_elems->end(), comparator);
    else
      std::sort(small_buffer_begin(), small_buffer_end(), comparator);
  }

private:

  void
  clearDestructive()
  {
    if (unlikely(large_elems)) {
      INVARIANT(!small_size_);
      delete large_elems;
      large_elems = nullptr;  // Use nullptr instead of NULL
      return;
    }
    if (!is_trivially_destructible)
      for (size_t element_index = 0; element_index < small_size_; element_index++)
        buffer_ptr()[element_index].~T();
    small_size_ = 0;
  }

  /**
   * Iterator for small vector elements stored in inline buffer
   */
  template <typename ElementType>
  class small_buffer_iterator : public std::iterator<std::bidirectional_iterator_tag, ElementType> {
    friend class silo_small_vector;
  public:
    inline small_buffer_iterator() : element_ptr(nullptr) {}

    template <typename OtherElementType>
    inline small_buffer_iterator(const small_buffer_iterator<OtherElementType> &other)
      : element_ptr(other.element_ptr)
    {}

    inline ElementType &
    operator*() const
    {
      return *element_ptr;
    }

    inline ElementType *
    operator->() const
    {
      return element_ptr;
    }

    inline bool
    operator==(const small_buffer_iterator &other) const
    {
      return element_ptr == other.element_ptr;
    }

    inline bool
    operator!=(const small_buffer_iterator &other) const
    {
      return !operator==(other);
    }

    inline bool
    operator<(const small_buffer_iterator &other) const
    {
      return element_ptr < other.element_ptr;
    }

    inline bool
    operator>=(const small_buffer_iterator &other) const
    {
      return !operator<(other);
    }

    inline bool
    operator>(const small_buffer_iterator &other) const
    {
      return element_ptr > other.element_ptr;
    }

    inline bool
    operator<=(const small_buffer_iterator &other) const
    {
      return !operator>(other);
    }

    inline small_buffer_iterator &
    operator+=(int offset)
    {
      element_ptr += offset;
      return *this;
    }

    inline small_buffer_iterator &
    operator-=(int offset)
    {
      element_ptr -= offset;
      return *this;
    }

    inline small_buffer_iterator
    operator+(int offset) const
    {
      small_buffer_iterator iterator_copy = *this;
      return iterator_copy += offset;
    }

    inline small_buffer_iterator
    operator-(int offset) const
    {
      small_buffer_iterator iterator_copy = *this;
      return iterator_copy -= offset;
    }

    inline intptr_t
    operator-(const small_buffer_iterator &other) const
    {
      return element_ptr - other.element_ptr;
    }

    inline small_buffer_iterator &
    operator++()
    {
      ++element_ptr;
      return *this;
    }

    inline small_buffer_iterator
    operator++(int)
    {
      small_buffer_iterator current_iterator = *this;
      ++(*this);
      return current_iterator;
    }

    inline small_buffer_iterator &
    operator--()
    {
      --element_ptr;
      return *this;
    }

    inline small_buffer_iterator
    operator--(int)
    {
      small_buffer_iterator current_iterator = *this;
      --(*this);
      return current_iterator;
    }

  protected:
    inline small_buffer_iterator(ElementType *element_ptr) : element_ptr(element_ptr) {}

  private:
    ElementType *element_ptr;
  };

  /**
   * Unified iterator that works with both small buffer and large vector storage
   */
  template <typename ElementType, typename SmallBufferIterator, typename LargeVectorIterator>
  class unified_iterator : public std::iterator<std::bidirectional_iterator_tag, ElementType> {
    friend class silo_small_vector;
  public:
    inline unified_iterator() : is_large_storage(false) {}

    template <typename OtherElementType, typename OtherSmallIterator, typename OtherLargeIterator>
    inline unified_iterator(const unified_iterator<OtherElementType, OtherSmallIterator, OtherLargeIterator> &other)
      : is_large_storage(other.is_large_storage),
        small_buffer_iter(other.small_buffer_iter),
        large_vector_iter(other.large_vector_iter)
    {}

    inline ElementType &
    operator*() const
    {
      if (unlikely(is_large_storage))
        return *large_vector_iter;
      return *small_buffer_iter;
    }

    inline ElementType *
    operator->() const
    {
      if (unlikely(is_large_storage))
        return &(*large_vector_iter);
      return &(*small_buffer_iter);
    }

    inline bool
    operator==(const unified_iterator &other) const
    {
      if (unlikely(is_large_storage))
        return large_vector_iter == other.large_vector_iter;
      return small_buffer_iter == other.small_buffer_iter;
    }

    inline bool
    operator!=(const unified_iterator &other) const
    {
      return !operator==(other);
    }

    inline bool
    operator<(const unified_iterator &other) const
    {
      if (unlikely(is_large_storage))
        return large_vector_iter < other.large_vector_iter;
      return small_buffer_iter < other.small_buffer_iter;
    }

    inline bool
    operator>=(const unified_iterator &other) const
    {
      return !operator<(other);
    }

    inline bool
    operator>(const unified_iterator &other) const
    {
      if (unlikely(is_large_storage))
        return large_vector_iter > other.large_vector_iter;
      return small_buffer_iter > other.small_buffer_iter;
    }

    inline bool
    operator<=(const unified_iterator &other) const
    {
      return !operator>(other);
    }

    inline unified_iterator &
    operator+=(int offset)
    {
      if (unlikely(is_large_storage))
        large_vector_iter += offset;
      else
        small_buffer_iter += offset;
      return *this;
    }

    inline unified_iterator &
    operator-=(int offset)
    {
      if (unlikely(is_large_storage))
        large_vector_iter -= offset;
      else
        small_buffer_iter -= offset;
      return *this;
    }

    inline unified_iterator
    operator+(int offset) const
    {
      unified_iterator iterator_copy = *this;
      return iterator_copy += offset;
    }

    inline unified_iterator
    operator-(int offset) const
    {
      unified_iterator iterator_copy = *this;
      return iterator_copy -= offset;
    }

    inline intptr_t
    operator-(const unified_iterator &other) const
    {
      if (unlikely(is_large_storage))
        return large_vector_iter - other.large_vector_iter;
      else
        return small_buffer_iter - other.small_buffer_iter;
    }

    inline unified_iterator &
    operator++()
    {
      if (unlikely(is_large_storage))
        ++large_vector_iter;
      else
        ++small_buffer_iter;
      return *this;
    }

    inline unified_iterator
    operator++(int)
    {
      unified_iterator current_iterator = *this;
      ++(*this);
      return current_iterator;
    }

    inline unified_iterator &
    operator--()
    {
      if (unlikely(is_large_storage))
        --large_vector_iter;
      else
        --small_buffer_iter;
      return *this;
    }

    inline unified_iterator
    operator--(int)
    {
      unified_iterator current_iterator = *this;
      --(*this);
      return current_iterator;
    }

  protected:
    unified_iterator(SmallBufferIterator small_buffer_iter)
      : is_large_storage(false), small_buffer_iter(small_buffer_iter), large_vector_iter() {}
    unified_iterator(LargeVectorIterator large_vector_iter)
      : is_large_storage(true), small_buffer_iter(), large_vector_iter(large_vector_iter) {}

  private:
    bool is_large_storage;
    SmallBufferIterator small_buffer_iter;
    LargeVectorIterator large_vector_iter;
  };

  typedef small_buffer_iterator<T> small_buffer_iter;
  typedef small_buffer_iterator<const T> const_small_buffer_iter;
  typedef typename large_vector_type::iterator large_vector_iter;
  typedef typename large_vector_type::const_iterator const_large_vector_iter;

  inline small_buffer_iter
  small_buffer_begin()
  {
    INVARIANT(!large_elems);
    return small_buffer_iter(buffer_ptr());
  }

  inline const_small_buffer_iter
  small_buffer_begin() const
  {
    INVARIANT(!large_elems);
    return const_small_buffer_iter(buffer_ptr());
  }

  inline small_buffer_iter
  small_buffer_end()
  {
    INVARIANT(!large_elems);
    return small_buffer_iter(buffer_ptr() + small_size_);
  }

  inline const_small_buffer_iter
  small_buffer_end() const
  {
    INVARIANT(!large_elems);
    return const_small_buffer_iter(buffer_ptr() + small_size_);
  }

public:

  typedef unified_iterator<T, small_buffer_iter, large_vector_iter> iterator;
  typedef unified_iterator<const T, const_small_buffer_iter, const_large_vector_iter> const_iterator;

  typedef std::reverse_iterator<iterator> reverse_iterator;
  typedef std::reverse_iterator<const_iterator> const_reverse_iterator;

  inline iterator
  begin()
  {
    if (unlikely(large_elems))
      return iterator(large_elems->begin());
    return iterator(small_buffer_begin());
  }

  inline const_iterator
  begin() const
  {
    if (unlikely(large_elems))
      return const_iterator(large_elems->begin());
    return const_iterator(small_buffer_begin());
  }

  inline iterator
  end()
  {
    if (unlikely(large_elems))
      return iterator(large_elems->end());
    return iterator(small_buffer_end());
  }

  inline const_iterator
  end() const
  {
    if (unlikely(large_elems))
      return const_iterator(large_elems->end());
    return const_iterator(small_buffer_end());
  }

  inline reverse_iterator
  rbegin()
  {
    return reverse_iterator(end());
  }

  inline const_reverse_iterator
  rbegin() const
  {
    return const_reverse_iterator(end());
  }

  inline reverse_iterator
  rend()
  {
    return reverse_iterator(begin());
  }

  inline const_reverse_iterator
  rend() const
  {
    return const_reverse_iterator(begin());
  }

private:
  void
  assignFrom(const silo_small_vector &source)
  {
    if (unlikely(this == &source))
      return;
    clearDestructive();
    if (unlikely(source.large_elems)) {
      large_elems = new large_vector_type(*source.large_elems);
    } else {
      INVARIANT(source.small_size_ <= SmallSize);
      if (is_trivially_copyable) {
        NDB_MEMCPY(buffer_ptr(), source.buffer_ptr(), source.small_size_ * sizeof(T));
      } else {
        for (size_t element_index = 0; element_index < source.small_size_; element_index++)
          new (&(buffer_ptr()[element_index])) T(source.buffer_ptr()[element_index]);
      }
      small_size_ = source.small_size_;
    }
  }

  inline ALWAYS_INLINE T *
  buffer_ptr()
  {
    return reinterpret_cast<T *>(&small_elems_buf[0]);
  }

  inline ALWAYS_INLINE const T *
  buffer_ptr() const
  {
    return reinterpret_cast<const T *>(&small_elems_buf[0]);
  }

  size_t small_size_;
  char small_elems_buf[sizeof(T) * SmallSize];
  large_vector_type *large_elems;
};

#endif /* _SILO_SMALL_VECTOR_H_ */
