#pragma once

// Core Masstree includes - only what we actually use
#include "masstree.hh"
#include "kvthread.hh"  // for threadinfo
#include "string.hh"    // for Masstree::Str

// STO-specific includes
#include "StringWrapper.hh"
#include "versioned_value.hh"
#include "stuffed_str.hh"

typedef stuffed_str<uint64_t> versioned_str;

struct versioned_str_struct : public versioned_str {
  typedef Masstree::Str value_type;
  typedef versioned_str::stuff_type version_type;

  bool needsResize(const value_type& new_value) {
    return needs_resize(new_value.length());
  }
  bool needsResize(const std::string& new_value) {
    return needs_resize(new_value.length());
  }

  versioned_str_struct* resizeIfNeeded(const value_type& new_value) {
    // Safe cast: versioned_str_struct has no instance variables or virtual methods
    return static_cast<versioned_str_struct*>(this->reserve(versioned_str::size_for(new_value.length())));
  }
  versioned_str_struct* resizeIfNeeded(const std::string& new_value) {
    // Safe cast: versioned_str_struct has no instance variables or virtual methods  
    return static_cast<versioned_str_struct*>(this->reserve(versioned_str::size_for(new_value.length())));
  }

  template <typename StringType>
  inline void set_value(const StringType& new_value) {
    auto *result = this->replace(new_value.data(), new_value.length());
    // We should already be the proper size at this point
    (void)result;
    assert(result == this);
  }
  
  // Responsibility is on the caller to ensure this read is atomic
  value_type read_value() {
    return Masstree::Str(this->data(), this->length());
  }
  
  inline version_type& version() {
    return stuff();
  }

  inline void deallocate_rcu(threadinfo& thread_info) {
    thread_info.deallocate_rcu(this, this->capacity() + sizeof(versioned_str_struct), memtag_value);
  }
};