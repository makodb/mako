#pragma once

#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <ctime>
#include <functional>

#include "utils.h"
#include "schema.h"
#include "locking.h"

#include "rrr/rrr.hpp"
#include <rusty/arc.hpp>

namespace mdb {

// forward declartion
class Schema;
class Table;


/**
 * Row - database table row with column data storage.
 *
 * Shared ownership uses `rusty::Arc<Row>`.  `Row` itself only carries
 * `NoCopy` semantics; the legacy `RefCounted` base + `ref_copy()` /
 * `release()` pattern is gone.
 */
class Row: public NoCopy {
  // fixed size part
  char *fixed_part_;
  int n_columns_ = 0;
  enum {
    DENSE,
    SPARSE,
  };

  int kind_;

  union {
    // for DENSE rows
    struct {
      // var size part
      char *dense_var_part_;

      // index table for var size part (marks the stop of a var segment)
      int *dense_var_idx_;
    };

    // for SPARSE rows
    std::string *sparse_var_;
  };

  Table *tbl_;

//protected:
 public:

  void update_fixed(const Schema::column_info *col, void *ptr, int len);

  bool rdonly_;
  const Schema *schema_;

  // hidden ctor, factory model
  Row() : fixed_part_(nullptr), kind_(DENSE),
          dense_var_part_(nullptr), dense_var_idx_(nullptr),
          tbl_(nullptr), rdonly_(false), schema_(nullptr) { }

  // Arc<Row> compatible - destructor must be public for Arc's control block
  virtual ~Row();

  // DEPRECATED: Compatibility shim for legacy ref_copy() calls.
  // New code should use Arc<Row>::clone() or avoid copying altogether.
  Row* ref_copy() { return this; }

  // DEPRECATED: Compatibility shim for legacy release() calls.
  // New code should use Arc<Row> with implicit drop.
  int release() { return 0; }

  // DEPRECATED: Compatibility shim for legacy ref_count() calls.
  // Always returns 1 (valid) since Row no longer uses refcounting.
  int ref_count() const { return 1; }

  void copy_into(Row *row) const;

  // generic row creation
  static Row *create(Row *raw_row,
                     const Schema *schema,
                     const std::vector<const Value *> &values);

  // helper function for row creation
  static void fill_values_ptr(const Schema *schema,
                              std::vector<const Value *> &values_ptr,
                              const Value &value,
                              size_t fill_counter) {
    values_ptr[fill_counter] = &value;
  }

  // helper function for row creation
  static void fill_values_ptr(const Schema *schema,
                              std::vector<const Value *> &values_ptr,
                              const std::pair<const std::string, Value> &pair,
                              size_t fill_counter) {
    int col_id = schema->get_column_id(pair.first);
    verify(col_id >= 0);
    values_ptr[col_id] = &pair.second;
  }

 public:

  virtual symbol_t rtti() const {
    return symbol_t::ROW_BASIC;
  }

  const Schema *schema() const {
    return schema_;
  }
  bool readonly() const {
    return rdonly_;
  }
  void make_readonly() {
    rdonly_ = true;
  }
  void make_sparse();
  void set_table(Table *tbl) {
    if (tbl != nullptr) {
      verify(tbl_ == nullptr);
    }
    tbl_ = tbl;
  }
  const Table *get_table() const {
    return tbl_;
  }

  Value get_column(int column_id) const;
  Value get_column(const std::string &col_name) const {
    return get_column(schema_->get_column_id(col_name));
  }
  virtual MultiBlob get_key() const;

  blob get_blob(int column_id) const;
  blob get_blob(const std::string &col_name) const {
    return get_blob(schema_->get_column_id(col_name));
  }

  virtual uint16_t Checksum() {
    auto n = schema_->columns_count();
    unsigned char ret = 0;
    for (auto i = 0; i < n; i++) {
      auto blob = get_blob(i);
      auto c = (const unsigned char*)blob.data;
      for (int j = 0; j < blob.len; j++) {
        ret ^= *c;
        c++;
      }
//      crc_basic<16>  crc_ccitt1( 0x1021, 0xFFFF, 0, false, false );
//      crc_ccitt1.process_bytes(blob.data, blob.len);
//      auto cs = crc_ccitt1.checksum();
//      ret ^= cs;
    }
    return ret;
  }

  void update(int column_id, i32 v) {
    const Schema::column_info *info = schema_->get_column_info(column_id);
    verify(info->type == Value::I32);
    update_fixed(info, &v, sizeof(v));
  }

  void update(int column_id, i64 v) {
    const Schema::column_info *info = schema_->get_column_info(column_id);
    verify(info->type == Value::I64);
    update_fixed(info, &v, sizeof(v));
  }
  void update(int column_id, double v) {
    const Schema::column_info *info = schema_->get_column_info(column_id);
    verify(info->type == Value::DOUBLE);
    update_fixed(info, &v, sizeof(v));
  }
  void update(int column_id, const std::string &str);
  void update(int column_id, const Value &v);

  void update(const std::string &col_name, i32 v) {
    this->update(schema_->get_column_id(col_name), v);
  }
  void update(const std::string &col_name, i64 v) {
    this->update(schema_->get_column_id(col_name), v);
  }
  void update(const std::string &col_name, double v) {
    this->update(schema_->get_column_id(col_name), v);
  }
  void update(const std::string &col_name, const std::string &v) {
    this->update(schema_->get_column_id(col_name), v);
  }
  void update(const std::string &col_name, const Value &v) {
    this->update(schema_->get_column_id(col_name), v);
  }

  // compare based on keys
  // must have same schema!
  int compare(const Row &another) const;

  bool operator==(const Row &o) const {
    return compare(o) == 0;
  }
  bool operator!=(const Row &o) const {
    return compare(o) != 0;
  }
  bool operator<(const Row &o) const {
    return compare(o) == -1;
  }
  bool operator>(const Row &o) const {
    return compare(o) == 1;
  }
  bool operator<=(const Row &o) const {
    return compare(o) != 1;
  }
  bool operator>=(const Row &o) const {
    return compare(o) != -1;
  }

  virtual Row *copy() const {
    Row *row = new Row();
    copy_into(row);
    return row;
  }

  template<class Container>
  static Row *create(const Schema *schema, const Container &values) {
    verify(values.size() == schema->columns_count());
    std::vector<const Value *> values_ptr(values.size(), nullptr);
    size_t fill_counter = 0;
    for (auto it = values.begin(); it != values.end(); ++it) {
      fill_values_ptr(schema, values_ptr, *it, fill_counter);
      fill_counter++;
    }
    return Row::create(new Row(), schema, values_ptr);
  }

  void to_string(std::string &str) {
    size_t s = str.size();
    int len = s;
    len += (sizeof(schema_->fixed_part_size_)
        + schema_->fixed_part_size_
        + sizeof(kind_));
    if (kind_ == DENSE && schema_->var_size_cols_ > 0) {
      len += schema_->var_size_cols_;
      len += dense_var_idx_[schema_->var_size_cols_ - 1];
      str.resize(len);
      int i = s;
      memcpy((void *) (str.data() + i),
             (void *) (&schema_->fixed_part_size_),
             sizeof(schema_->fixed_part_size_));
      i += sizeof(schema_->fixed_part_size_);
      memcpy((void *) (str.data() + i),
             (void *) (fixed_part_),
             schema_->fixed_part_size_);
      i += schema_->fixed_part_size_;
      memcpy((void *) (str.data() + i), (void *) (&kind_), sizeof(kind_));
      i += sizeof(kind_);
      memcpy((void *) (str.data() + i),
             (void *) dense_var_idx_,
             schema_->var_size_cols_);
      i += schema_->var_size_cols_;
      memcpy((void *) (str.data() + i),
             (void *) dense_var_part_,
             dense_var_idx_[schema_->var_size_cols_ - 1]);
      i += dense_var_idx_[schema_->var_size_cols_ - 1];
      verify(i == len);
    } else {
      str.resize(len);
      int i = s;
      memcpy((void *) (str.data() + i),
             (void *) (&schema_->fixed_part_size_),
             sizeof(schema_->fixed_part_size_));
      i += sizeof(schema_->fixed_part_size_);
      memcpy((void *) (str.data() + i),
             (void *) (fixed_part_),
             schema_->fixed_part_size_);
      i += schema_->fixed_part_size_;
      memcpy((void *) (str.data() + i), (void *) (&kind_), sizeof(kind_));
      i += sizeof(kind_);
      verify(i == len);
    }
  }
};


class CoarseLockedRow: public Row {
  RWLock lock_;

 protected:

  CoarseLockedRow() : Row(), lock_() {}
  // public dtor for Arc<Row> compatibility
 public:
  ~CoarseLockedRow() { }

  void copy_into(CoarseLockedRow *row) const {
    this->Row::copy_into((Row *) row);
    row->lock_ = lock_;
  }

 public:

  virtual symbol_t rtti() const {
    return symbol_t::ROW_COARSE;
  }

  bool rlock_row_by(lock_owner_t o) {
    return lock_.rlock_by(o);
  }
  bool wlock_row_by(lock_owner_t o) {
    return lock_.wlock_by(o);
  }
  bool unlock_row_by(lock_owner_t o) {
    return lock_.unlock_by(o);
  }

  virtual Row *copy() const {
    CoarseLockedRow *row = new CoarseLockedRow();
    copy_into(row);
    return row;
  }

  template<class Container>
  static CoarseLockedRow *create(const Schema *schema,
                                 const Container &values) {
    verify(values.size() == schema->columns_count());
    std::vector<const Value *> values_ptr(values.size(), nullptr);
    size_t fill_counter = 0;
    for (auto it = values.begin(); it != values.end(); ++it) {
      fill_values_ptr(schema, values_ptr, *it, fill_counter);
      fill_counter++;
    }
    return (CoarseLockedRow *) Row::create(new CoarseLockedRow(),
                                           schema,
                                           values_ptr);
  }
};

// inherit from CoarseLockedRow since we need locking on commit phase, when doing 2 phase commit
class VersionedRow: public CoarseLockedRow {
 public:
  std::vector<version_t> ver_{};
  void init_ver(int n_columns) {
    ver_.resize(n_columns, 0);
  }

  void copy_into(VersionedRow *row) const {
    this->CoarseLockedRow::copy_into((CoarseLockedRow *) row);
    int n_columns = schema_->columns_count();
    row->init_ver(n_columns);
    row->ver_ = this->ver_;
    verify(row->ver_.size() > 0);
  }

 public:

  virtual symbol_t rtti() const {
    return symbol_t::ROW_VERSIONED;
  }

  version_t get_column_ver(colid_t column_id) const {
    verify(ver_.size() > 0);
    verify(column_id < ver_.size());
    return ver_[column_id];
  }

  void set_column_ver(colid_t column_id, version_t ver) {
    ver_[column_id] = ver;
  }

  void incr_column_ver(colid_t column_id) {
    ver_[column_id] ++;
  }

  virtual Row *copy() const {
    VersionedRow *row = new VersionedRow();
    copy_into(row);
    return row;
  }


  Value get_column(int column_id) const {
    Value v = Row::get_column(column_id);
    v.ver_ = get_column_ver(column_id);
    return v;
  }

  template<class Container>
  static VersionedRow *create(const Schema *schema, const Container &values) {
    verify(values.size() == schema->columns_count());
    std::vector<const Value *> values_ptr(values.size(), nullptr);
    size_t fill_counter = 0;
    for (auto it = values.begin(); it != values.end(); ++it) {
      fill_values_ptr(schema, values_ptr, *it, fill_counter);
      fill_counter++;
    }
    VersionedRow *raw_row = new VersionedRow();
    raw_row->init_ver(schema->columns_count());
    return (VersionedRow *) Row::create(raw_row, schema, values_ptr);
  }
};

} // namespace mdb
