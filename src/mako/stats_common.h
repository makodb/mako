#pragma once

#include "counter.h"
#include "macros.h"
#include "fileutils.h"

enum class stats_command : uint8_t { GET_COUNTER_VALUE = 0x1 };

// @safe - POD struct
struct get_counter_value_t {
  uint64_t timestamp_us_; // usec
  counter_data d_;
};

class packet {
public:
  static const size_t MAX_DATA = 0xFFFF - 4;
  // @safe - simple initialization
  packet() : size_(0) {}
  // @safe - simple assignment
  inline void clear() { size_ = 0; }
  // @unsafe - uses NDB_MEMCPY
  inline void
  assign(const char *p, size_t n)
  {
    INVARIANT(n <= MAX_DATA);
    NDB_MEMCPY(&data_[0], p, n);
    size_ = n;
  }
  // @unsafe - calls @unsafe assign
  inline void
  assign(const std::string &s)
  {
    assign(s.data(), s.size());
  }
  // @unsafe - file I/O and C-style cast
  int
  sendpkt(int fd) const
  {
    // XXX: we don't care about endianness
    return fileutils::writeall(
        fd, (const char *) &size_, sizeof(size_) + size_);
  }
  // @unsafe - file I/O and C-style cast
  int
  recvpkt(int fd)
  {
    // XXX: we don't care about endianness
    int r;
    if ((r = fileutils::readall(fd, (char *) &size_, sizeof(size_)))) {
      clear();
      return r;
    }
    if (size_ > packet::MAX_DATA) {
      std::cerr << "bad packet read with excessive size" << std::endl;
      clear();
      return -1;
    }
    if ((r = fileutils::readall(fd, &data_[0], size_))) {
      clear();
      return r;
    }
    return 0;
  }
  // @safe - simple getter
  inline uint32_t size() const { return size_; }
  // @safe - simple getter returning pointer to member array
  inline const char * data() const { return &data_[0]; }
private:
  uint32_t size_;
  char data_[MAX_DATA];
};
