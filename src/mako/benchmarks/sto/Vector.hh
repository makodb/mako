#pragma once

#include "config.h"
#include "compiler.hh"
#include <iostream>
#include <vector>
#include <map>
#include <string.h>
#include "Transaction.hh"
#include "TArrayProxy.hh"
#include "Box.hh"
#include "rwlock.hh"
#include <stdexcept>

// Configuration constants
static constexpr size_t DEFAULT_ITERATOR_SIZE = 10000;

// Helper function to calculate log2 ceiling
inline size_t calculate_log2_ceil(size_t size) {
  return static_cast<size_t>(ceil(log(static_cast<double>(size)) / log(2.0)));
}

template<typename ValueType, bool Opacity = false, typename ElementType = Box<ValueType>, bool SmartIterator = true> class Vector;
template<typename ValueType, bool Opacity = false, typename ElementType = Box<ValueType>, bool SmartIterator = true> class VecIterator;

template <typename ValueType, bool Opacity, typename ElementType, bool SmartIterator>
class Vector : public TObject {
public:
  typedef unsigned index_type;
  
private:
  friend class VecIterator<ValueType, Opacity, ElementType, SmartIterator>;
  typedef const Vector<ValueType, Opacity, ElementType, SmartIterator> const_vector_type;
  typedef Vector<ValueType, Opacity, ElementType, SmartIterator> vector_type;
  
  typedef TransactionTid::type Version;
  
  static constexpr int vector_key = -1;
  static constexpr int push_back_key = -2;
  static constexpr int size_key = -3;
  static constexpr int size_pred_key = -4;
  static constexpr TransItem::flags_type list_bit = TransItem::user0_bit;
  static constexpr TransItem::flags_type delete_bit = TransItem::user0_bit<<1;
  static constexpr int32_t value_shift = 1;
  static constexpr int32_t geq_mask = 1;
public:
  typedef int key_type;
  typedef ValueType value_type;
  typedef value_type get_type;
  
  typedef VecIterator<ValueType, Opacity, ElementType, SmartIterator> iterator;
  typedef const VecIterator<ValueType, Opacity, ElementType, SmartIterator> const_iterator;
  typedef TArrayProxy<Vector<ValueType, Opacity, ElementType, SmartIterator>> proxy_type;
  typedef TConstArrayProxy<Vector<ValueType, Opacity, ElementType, SmartIterator>> const_proxy_type;
  typedef int32_t size_type;
  
  
  Vector(): resize_lock_() {
    capacity_ = 0;
    size_ = 0;
    vecversion_ = 0;
    data_ = nullptr;
  }
  
  Vector(size_type initial_size): resize_lock_() {
    size_ = 0;
    capacity_ = 1 << static_cast<int>(calculate_log2_ceil(initial_size));
    vecversion_ = 0;
    
    data_ = new ElementType[capacity_];
  }
  
  void reserve(size_type new_capacity) {
    if (new_capacity <= capacity_)
      return;
    ElementType* new_data = new ElementType[new_capacity];
    
    resize_lock_.write_lock();
    for (size_type index = 0; index < capacity_; index++) {
      new_data[index] = data_[index];
    }
    capacity_ = new_capacity;
    if (data_ != nullptr)
      Transaction::rcu_delete_array(data_);
    data_ = new_data;
    resize_lock_.write_unlock();
  }
  
  void push_back(const value_type& value) {
    if (trans_size_offs() < 0) {
      handle_push_back_with_deleted_items(value);
      return;
    }
    
    handle_regular_push_back(value);
  }
  
private:
  // Helper for push_back when there are deleted items to overwrite
  void handle_push_back_with_deleted_items(const value_type& value) {
    auto item = Sto::item(this, size_ + trans_size_offs());
    if (!item.has_write() || !has_delete(item)) {
      // Another transaction has modified items, abort
      Sto::abort();
    } else {
      item.clear_write();
      item.clear_flags(delete_bit);
      item.add_write(value);
      add_trans_size_offs(1);
    }
  }
  
  // Helper for regular push_back operation
  void handle_regular_push_back(const value_type& value) {
    auto item = Sto::item(this, push_back_key);
    if (item.has_write()) {
      handle_push_back_with_existing_writes(item, value);
    } else {
      item.add_write(value);
      item.clear_flags(list_bit);
    }
    add_lock_vector_item();
    add_trans_size_offs(1);
  }
  
  // Helper for push_back when there are already pending writes
  void handle_push_back_with_existing_writes(TransProxy& item, const value_type& value) {
    if (!is_list(item)) {
      // Convert single value to list
      auto& existing_value = item.template write_value<ValueType>();
      std::vector<ValueType> write_list;
      write_list.push_back(existing_value);
      write_list.push_back(value);
      item.clear_write();
      item.add_write(write_list);
      item.add_flags(list_bit);
    } else {
      // Add to existing list
      auto& write_list = item.template write_value<std::vector<ValueType>>();
      write_list.push_back(value);
    }
  }
  
public:
  void nontrans_push_back(ValueType element) {
    assert(size_ < capacity_);
    data_[size_].write(std::move(element));
    ++size_;
  }
  
  void pop_back() {
    auto item = Sto::item(this, push_back_key);
    if (item.has_write()) {
      handle_pop_back_with_pending_writes(item);
      return;
    }
    
    handle_pop_back_from_existing_elements();
  }
  
private:
  // Helper for pop_back when there are pending writes
  void handle_pop_back_with_pending_writes(TransProxy& item) {
    if (!is_list(item)) {
      item.clear_write();
    } else {
      auto& write_list = item.template write_value<std::vector<ValueType>>();
      write_list.pop_back();
      if (write_list.empty()) {
        item.clear_write();
      }
    }
    add_trans_size_offs(-1);
  }
  
  // Helper for pop_back from existing elements
  void handle_pop_back_from_existing_elements() {
    auto vecitem = add_vector_version(TransactionTid::unlocked(vecversion_));
    acquire_fence();
    size_type current_size = size_;
    acquire_fence();
    
    if (current_size + trans_size_offs() == 0) {
      // Empty vector - undefined behavior, do nothing
      return;
    }
    
    // Mark last element for deletion
    vecitem.add_write(0);
    auto item = Sto::item(this, current_size + trans_size_offs() - 1)
                   .add_write(0)
                   .add_flags(delete_bit);
    
    if (item.flags() & ElementType::valid_only_bit) {
      item.clear_read();
      item.clear_flags(ElementType::valid_only_bit);
    }
    add_trans_size_offs(-1);
  }
  
public:
  
  iterator erase(iterator pos) {
    iterator end_it = end();
    for (iterator it = pos; it != (end_it - 1); ++it) {
      *(it) = (ValueType) *(it + 1);
    }
    pop_back();
    return pos;
  }
  
  iterator insert(iterator pos, const ValueType& value) {
    iterator end_it = end();
    push_back(*(end_it - 1));
    for (iterator it = (end_it - 1); it != pos; --it) {
      *(it) = (ValueType) *(it - 1);
    }
    
    *pos = value;
    return pos;
  }
  
  size_type size() {
    add_vector_version(TransactionTid::unlocked(vecversion_));
    acquire_fence();
    return size_ + trans_size_offs();
  }
  
  bool checkSize(size_type expected_size) {
    if (!SmartIterator) {
      return size() == expected_size;
    }
    
    return check_size_with_predicate(expected_size);
  }
  
private:
  // Helper for smart iterator size checking with predicates
  bool check_size_with_predicate(size_type expected_size) {
    size_type current_size = size_;
    int32_t size_offset = trans_size_offs();
    
    int32_t predicate = calculate_size_predicate(expected_size, current_size, size_offset);
    update_size_predicate(predicate);
    
    return current_size + size_offset == expected_size;
  }
  
  // Helper to calculate size predicate value
  int32_t calculate_size_predicate(size_type expected_size, size_type current_size, int32_t size_offset) {
    int32_t predicate = (expected_size - size_offset) << value_shift;
    
    if (current_size + size_offset == expected_size) {
      // Exact match - predicate is already correct
    } else if (current_size + size_offset > expected_size) {
      predicate |= geq_mask;
    } else {
      Sto::abort();
    }
    
    return predicate;
  }
  
  // Helper to update size predicate with conflict resolution
  void update_size_predicate(int32_t new_predicate) {
    auto item = Sto::item(this, size_pred_key);
    if (item.has_predicate()) {
      int32_t old_predicate = item.template predicate_value<int32_t>();
      int32_t merged_predicate = merge_size_predicates(old_predicate, new_predicate);
      item.set_predicate(merged_predicate);
    } else {
      item.set_predicate(new_predicate);
    }
  }
  
  // Helper to merge conflicting size predicates
  int32_t merge_size_predicates(int32_t old_pred, int32_t new_pred) {
    // Add the covering predicate of the old predicate and the new one
    if (new_pred == old_pred || (old_pred & new_pred & geq_mask)) {
      return new_pred >= old_pred ? new_pred : old_pred;
    } else if ((old_pred & geq_mask) && old_pred <= (new_pred | geq_mask)) {
      return new_pred; // OK - old predicate is compatible
    } else if ((new_pred & geq_mask) && new_pred <= (old_pred | geq_mask)) {
      return old_pred; // OK - new predicate is compatible
    } else {
      Sto::abort(); // Incompatible predicates
    }
  }
  
public:
  
  size_type nontrans_size() const {
    return size_;
  }
  ValueType nontrans_get(key_type index) const {
    assert(index < size_);
    return data_[index].unsafe_read();
  }
  
  proxy_type front() {
    if (size_ + trans_size_offs() == 0)
      Sto::abort();
    return proxy_type(this, 0);
  }
  
  proxy_type back() {
    size_type current_size = size();
    if (current_size == 0)
      Sto::abort();
    return proxy_type(this, current_size - 1);
  }
  
  const_proxy_type operator[](key_type index) const {
    validate_index_bounds_with_abort(index);
    return const_proxy_type(this, index);
  }
  
  proxy_type operator[](key_type index) {
    validate_index_bounds_with_abort(index);
    return proxy_type(this, index);
  }
  
  value_type transGet(const key_type& index) const {
    Version version = vecversion_;
    acquire_fence();
    size_type current_size = size_;
    acquire_fence();
    
    validate_index_bounds(index);
    
    if (is_existing_element(index)) {
      return data_[index].transRead(Sto::item(this, index));
    } else {
      return get_from_pushed_elements(index, version);
    }
  }
  
private:
  // Helper to get value from pushed (not yet committed) elements
  value_type get_from_pushed_elements(key_type index, Version version) const {
    int offset = get_pushed_element_offset(index);
    auto extra_items = Sto::item(this, push_back_key);
    
    // Register version to invalidate concurrent push_backs
    add_vector_version(TransactionTid::unlocked(version));
    
    if (extra_items.has_write()) {
      if (is_list(extra_items)) {
        auto& write_list = extra_items.template write_value<std::vector<ValueType>>();
        if (!write_list.empty()) {
          return write_list[offset];
        }
        assert(false);
      } else {
        // Single element case
        assert(offset == 0);
        return extra_items.template write_value<ValueType>();
      }
    }
    assert(false);
  }
  
public:
  
  void transUpdate(const key_type& index, value_type value) {
    Version version = vecversion_;
    acquire_fence();
    size_type current_size = size_;
    acquire_fence();
    
    validate_index_bounds(index);
    
    if (is_existing_element(index)) {
      data_[index].transWrite(Sto::item(this, index), std::move(value));
    } else {
      update_pushed_element(index, value, version);
    }
  }
  
private:
  // Helper to update pushed (not yet committed) elements
  void update_pushed_element(key_type index, value_type value, Version version) {
    int offset = get_pushed_element_offset(index);
    auto extra_items = Sto::item(this, push_back_key);
    
    // Register version to invalidate concurrent push_backs
    add_vector_version(TransactionTid::unlocked(version));
    
    if (extra_items.has_write()) {
      if (is_list(extra_items)) {
        auto& write_list = extra_items.template write_value<std::vector<ValueType>>();
        if (!write_list.empty()) {
          write_list[offset] = value;
        } else {
          assert(false);
        }
      } else {
        // Single element case
        assert(offset == 0);
        extra_items.clear_write();
        extra_items.add_write(value);
      }
    } else {
      assert(false);
    }
  }
  
public:
  void transPut(const key_type& index, value_type value) {
    transUpdate(index, value);
  }
  
  static bool is_list(const TransItem& item) {
    return item.flags() & list_bit;
  }
  
  static bool has_delete(const TransItem& item) {
    return item.flags() & delete_bit;
  }
  
  void lock_version(Version& version) {
    TransactionTid::lock(version);
  }
  
  void unlock_version(Version& version) {
    TransactionTid::unlock(version);
  }
  
  bool is_locked(Version& version) const {
    return TransactionTid::is_locked(version);
  }
  
  void lock(key_type index) {
    while (true) {
      resize_lock_.read_lock();
      bool locked = data_[index].try_lock();
      resize_lock_.read_unlock();
      if (locked) break;
    }
  }
  
  void unlock(key_type index) {
    resize_lock_.read_lock();
    data_[index].unlock();
    resize_lock_.read_unlock();
  }
  
  void add_trans_size_offs(int size_offset) {
    // Performance note: Could be optimized by storing directly in Transaction
    // since the key is fixed, avoiding transset search overhead
    auto item = Sto::item(this, size_key);
    item.set_stash(item.template stash_value<int>(0) + size_offset);
  }
  
  int trans_size_offs() const {
    return Sto::item(const_cast<Vector<ValueType, Opacity, ElementType, SmartIterator>*>(this), size_key).template stash_value<int>(0);
  }
  
  TransProxy vector_item() const {
    // can switch this to fresh_item to not read our writes
    return Sto::item(this, vector_key);
  }
  
  void add_lock_vector_item() {
    vector_item().add_write(0);
  }
  
  TransProxy add_vector_version(Version ver) const {
    return vector_item().add_read(ver);
  }
  
  bool lock(TransItem& item, Transaction&) override {
    if (item.key<int>() == vector_key) {
      // Performance optimization: Only lock version if size changes occurred
      if (should_lock_vector_version()) {
        lock_version(vecversion_);
      }
    } else if (item.key<int>() != push_back_key) {
      lock(item.key<key_type>());
    }
    return true;
  }
  
  bool check_predicate(TransItem& item, Transaction&, bool) override {
    assert(item.key<int>() == size_pred_key);
    
    Version local_version = vecversion_;
    if (is_locked(local_version))
      return false;
      
    vector_item().add_read(local_version);
    acquire_fence();
    
    size_type current_size = size_;
    int32_t predicate = item.template predicate_value<int32_t>();
    int32_t predicate_value = predicate >> value_shift;
    
    if (predicate & geq_mask)
      return current_size >= predicate_value;
    else
      return current_size == predicate_value;
  }
  
  bool check(TransItem& item, Transaction&) override {
    if (item.key<int>() == vector_key || item.key<int>() == push_back_key)
      return TransactionTid::check_version(vecversion_, item.template read_value<Version>());
    
    key_type element_index = item.key<key_type>();
    assert(element_index >= 0);
    
    if (item.flags() & ElementType::valid_only_bit) {
      return element_index < size_ + trans_size_offs()
      && !data_[element_index].is_locked_elsewhere();
    } else {
      return data_[element_index].check_version(item.template read_value<Version>());
    }
  }
  
  void install(TransItem& item, Transaction& transaction) override {
    if (item.key<int>() == vector_key)
      return;
      
    if (item.key<int>() == push_back_key) {
      install_pushed_elements(item, transaction);
    } else {
      install_element_modification(item, transaction);
    }
  }
  
private:
  // Helper to install pushed elements
  void install_pushed_elements(TransItem& item, Transaction& transaction) {
    if (is_list(item)) {
      install_element_list(item, transaction);
    } else {
      install_single_element(item, transaction);
    }
    update_vector_version(transaction);
  }
  
  // Helper to install a list of elements
  void install_element_list(TransItem& item, Transaction& transaction) {
    auto& write_list = item.template write_value<std::vector<ValueType>>();
    for (size_t list_index = 0; list_index < write_list.size(); list_index++) {
      ensure_capacity_for_growth();
      resize_lock_.read_lock();
      data_[size_].write(write_list[list_index]);
      acquire_fence();
      size_++;
      resize_lock_.read_unlock();
    }
  }
  
  // Helper to install a single element
  void install_single_element(TransItem& item, Transaction& transaction) {
    auto& value = item.template write_value<ValueType>();
    ensure_capacity_for_growth();
    resize_lock_.read_lock();
    data_[size_].write(value);
    acquire_fence();
    size_++;
    resize_lock_.read_unlock();
  }
  
  // Helper to install element modifications (updates/deletes)
  void install_element_modification(TransItem& item, Transaction& transaction) {
    auto index = item.key<key_type>();
    if (has_delete(item)) {
      if (index < size_) {
        size_ = index;
        update_vector_version(transaction);
      }
    } else if (index >= size_) {
      assert(false);
    }
    
    resize_lock_.read_lock();
    data_[index].install(item, transaction);
    resize_lock_.read_unlock();
  }
  
public:
  
  void unlock(TransItem& item) override {
    if (item.key<int>() == vector_key)
      unlock_version(vecversion_);
    else if (item.key<int>() != push_back_key)
      unlock(item.key<key_type>());
  }
  
  void print(std::ostream& output_stream, const TransItem& item) const override {
    print_vector_header(output_stream);
    print_item_details(output_stream, item);
    output_stream << "}";
  }
  
private:
  // Helper to print vector type header
  void print_vector_header(std::ostream& output_stream) const {
    output_stream << "{Vector<";
    const char* type_info = strstr(__PRETTY_FUNCTION__, "with T = ");
    if (type_info) {
      type_info += 9;
      const char* semicolon = strchr(type_info, ';');
      output_stream.write(type_info, semicolon - type_info);
    }
    output_stream << "> " << static_cast<const void*>(this);
  }
  
  // Helper to print transaction item details
  void print_item_details(std::ostream& output_stream, const TransItem& item) const {
    if (item.key<int>() == size_pred_key) {
      print_size_predicate(output_stream, item);
    } else {
      print_regular_item(output_stream, item);
    }
  }
  
  // Helper to print size predicate information
  void print_size_predicate(std::ostream& output_stream, const TransItem& item) const {
    int32_t predicate = item.template predicate_value<int32_t>();
    const char* comparison = (predicate & geq_mask) ? ">=" : "==";
    int32_t value = predicate >> value_shift;
    output_stream << ".size" << comparison << value;
  }
  
  // Helper to print regular item information
  void print_regular_item(std::ostream& output_stream, const TransItem& item) const {
    print_item_key(output_stream, item);
    print_item_operations(output_stream, item);
  }
  
  // Helper to print item key information
  void print_item_key(std::ostream& output_stream, const TransItem& item) const {
    int item_key = item.key<int>();
    if (item_key == size_key)
      output_stream << ".size";
    else if (item_key == push_back_key)
      output_stream << ".push_back";
    else if (item_key == vector_key)
      output_stream << ".vector";
    else {
      output_stream << "[" << item_key << "]";
      if (has_delete(item))
        output_stream << "XX";
    }
  }
  
  // Helper to print item read/write operations
  void print_item_operations(std::ostream& output_stream, const TransItem& item) const {
    if (item.has_read())
      output_stream << " ?" << item.template read_value<Version>();
    if (item.has_write())
      output_stream << " =" << item.template write_value<void*>();
  }
  
public:
  
  iterator begin() {
    return iterator(this, 0, false);
  }
  iterator end() {
    //vector_item().add_read(vecversion_);// to invalidate size changes after this call.
    //acquire_fence();
    return iterator(this, 0, true);
  }
  
  // This is not-transactional and only used for debugging purposes
  void print() {
    for (int i = 0; i < nontrans_size(); i++) {
      std::cout << nontrans_get(i) << " ";
    }
    std::cout << std::endl;
  }
  
private:
  // Helper functions for common operations
  
  // Bounds checking helper with abort (for operator[])
  void validate_index_bounds_with_abort(key_type index) const {
    size_type current_size = size_ + trans_size_offs();
    if (index >= current_size)
      Sto::abort();
  }
  
  // Bounds checking helper with exception (for transGet/transUpdate)
  void validate_index_bounds(key_type index) const {
    size_type current_size = size_ + trans_size_offs();
    if (index >= current_size) {
      Sto::check_opacity();
      throw std::out_of_range("Vector index out of bounds");
    }
  }
  
  // Helper to check if index is within existing elements (not pushed elements)
  bool is_existing_element(key_type index) const {
    return index < size_;
  }
  
  // Helper to get the offset within pushed elements
  int get_pushed_element_offset(key_type index) const {
    return index - size_;
  }
  
  // Helper to handle capacity expansion
  void ensure_capacity_for_growth() {
    if (size_ >= capacity_) {
      int new_capacity = (capacity_ == 0) ? 1 : capacity_ << 1;
      reserve(new_capacity);
    }
  }
  
  // Helper to update vector version based on opacity setting
  void update_vector_version(Transaction& transaction) {
    if (Opacity) {
      TransactionTid::set_version(vecversion_, transaction.commit_tid());
    } else {
      TransactionTid::inc_nonopaque_version(vecversion_);
    }
  }
  
  // Helper to determine if vector version locking is needed
  bool should_lock_vector_version() const {
    return trans_size_offs() != 0;
  }

  size_type size_;
  size_type capacity_;
  Version vecversion_; // for vector size
  rwlock resize_lock_; // to do concurrent resize
  ElementType* data_;
  };
  
  
  template<typename ValueType, bool Opacity, typename ElementType, bool SmartIterator>
  class VecIterator : public std::iterator<std::random_access_iterator_tag, ValueType> {
  public:
    typedef ValueType value_type;
    typedef Vector<ValueType, Opacity, ElementType, SmartIterator> vector_type;
    typedef typename vector_type::proxy_type proxy_type;
    typedef VecIterator<ValueType, Opacity, ElementType, SmartIterator> iterator;
    VecIterator() = default;
    VecIterator(Vector<ValueType, Opacity, ElementType, SmartIterator>* vector_ptr, int position, bool is_end) 
      : vector_array_(vector_ptr), position_(position), is_end_(is_end) { }
    VecIterator(const VecIterator& other) : vector_array_(other.vector_array_), position_(other.position_), is_end_(other.is_end_) {}
    
    VecIterator& operator= (const VecIterator& other) {
      vector_array_ = other.vector_array_;
      position_ = other.position_;
      is_end_ = other.is_end_;
      return *this;
    }
    
    bool operator==(iterator other) const {
      if (vector_array_ != other.vector_array_)
        return false;
      else if (is_end_ == other.is_end_)
        return position_ == other.position_;
      else {
        size_t size_difference = is_end_ ? other.position_ - position_ : position_ - other.position_;
        return vector_array_->checkSize(size_difference);
      }
    }
    
    bool operator!=(iterator other) const {
      return !(operator==(other));
    }
    
    proxy_type operator*() {
      return proxy_type(vector_array_, is_end_ ? vector_array_->size() + position_ : position_);
    }
    
    proxy_type operator[](int offset) {
      return proxy_type(vector_array_, (is_end_ ? vector_array_->size() + position_ : position_) + offset);
    }
    
    /* This is the prefix case */
    iterator& operator++() { ++position_; return *this; }
    
    /* This is the postfix case */
    iterator operator++(int) {
      VecIterator<ValueType, Opacity, ElementType, SmartIterator> clone(*this);
      ++position_;
      return clone;
    }
    
    iterator& operator--() { --position_; return *this; }
    
    iterator operator--(int) {
      VecIterator<ValueType, Opacity, ElementType, SmartIterator> clone(*this);
      --position_;
      return clone;
    }
    
    iterator operator+(int offset) {
      VecIterator<ValueType, Opacity, ElementType, SmartIterator> clone(*this);
      clone.position_ += offset;
      return clone;
    }
    
    iterator operator-(int offset) {
      VecIterator<ValueType, Opacity, ElementType, SmartIterator> clone(*this);
      clone.position_ -= offset;
      return clone;
    }
    
    int operator-(const iterator& rhs) {
      assert(rhs.vector_array_ == vector_array_);
      if (is_end_ == rhs.is_end_)
        return (position_ - rhs.position_);
      else {
        size_t current_size = vector_array_->size();
        if (is_end_)
          return current_size + position_ - rhs.position_;
        else
          return position_ - rhs.position_ - current_size;
      }
    }
    
  private:
    Vector<ValueType, Opacity, ElementType, SmartIterator>* vector_array_;
    size_t position_;
    bool is_end_;
  };
