#pragma once
// Adopted from stuffed_str
class simple_str {
public:
  // @safe - pure bitwise arithmetic
  static unsigned pad(unsigned v)
  {
    if (likely(v <= 512)) {
      return (v + 15) & ~15;
    }
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
#if UINT_MAX == UINT64_MAX
    v |= v >> 32;
#endif
    v++;
    return v;
  }

  // @safe - calls safe pad() and performs arithmetic
  static inline int size_for(int len) {
    int res = pad(len + sizeof(simple_str));
    return res;

  }

  // @safe - simple comparison
  bool needs_resize(int len) {
    return len > (int)capacity_;
  }

  // @safe - returns raw pointer (allowed in @safe)
  const char *data() {
    return buf_;
  }
  
  // @safe - simple field access
  int length() {
    return size_;
  }
  
  // @safe - simple field access
  int capacity() {
    return capacity_;
  }

  // @unsafe - uses malloc and memcpy
  inline simple_str& operator= (const std::string& v) {
    int len = v.length();
    size_ = len;
    if (unlikely(needs_resize(len))) {
      capacity_ = size_for(len) - sizeof(simple_str);
      buf_ = (char *) malloc(capacity_);
    }
    memcpy(buf_, v.data(), len);
    return *this;    
  }
  
  // @safe - creates std::string from data
  operator std::string() {
    return std::string(this->data(), this->length());  
  }

  // @unsafe - uses malloc and memcpy
  simple_str(const std::string& v) {
    int len = v.length();
    size_ = len;
    capacity_ = size_for(len) - sizeof(simple_str);
    buf_ = (char *) malloc(capacity_);
    memcpy(buf_, v.data(), len);
  }
private:
  uint32_t size_;
  uint32_t capacity_;
  char *buf_;
};

