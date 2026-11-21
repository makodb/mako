/* mrxdb_rocksdb_compat.h -- point rocksdb_* calls at the cache.
 *
 * Include this INSTEAD of <rocksdb/c.h> in a translation unit whose RocksDB
 * usage is confined to the subset below, and it compiles against the cache
 * unchanged.
 *
 * ===========================================================================
 * READ THIS BEFORE USING IT
 * ===========================================================================
 *
 * A rename cannot change a guarantee, and one guarantee genuinely differs:
 *
 *   rocksdb_put() RETURNS BEFORE THE WRITE IS DURABLE.
 *
 * Code that relied on RocksDB's WAL to make a write safe by the time put()
 * returned is NOT safe after this rename. It needs an explicit
 * rocksdb_flush() (i.e. mrxdb_flush) at each point where durability was
 * previously implicit.
 *
 * There is no way for a header to check that for you, which is why this file
 * is opt-in per translation unit rather than a project-wide alias. Renaming
 * an entire codebase with it and hoping is the failure mode it exists to make
 * deliberate.
 *
 * Also not preserved:
 *
 *   - rocksdb_write() IS NOT ATOMIC here. A reader can observe a partially
 *     applied batch. Any code depending on batch atomicity for correctness
 *     must not use this header.
 *   - rocksdb_flush() means "wait until acknowledged writes are durable",
 *     not "flush the memtable". Stronger, and slower.
 *   - There is no rocksdb_iter_prev(); direction is fixed at seek time.
 *   - Column families, snapshots, merge operators, compaction filters, and
 *     transactions are absent, not aliased. A translation unit using any of
 *     them will fail to compile, which is the intended outcome.
 *
 * The read options / write options / snapshot parameters are accepted and
 * ignored, so existing call sites keep compiling. Passing a non-default one
 * has no effect -- if that matters, do not use this header.
 */

#ifndef MRXDB_ROCKSDB_COMPAT_H
#define MRXDB_ROCKSDB_COMPAT_H

#include "mrxdb.h"

#include <stdio.h>  /* rocksdb_close reports a drain failure it cannot return */

#ifdef ROCKSDB_C_H
#error "mrxdb_rocksdb_compat.h and <rocksdb/c.h> in one translation unit: \
the rocksdb_* names would refer to two different databases."
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef mrxdb_t rocksdb_t;
typedef mrxdb_options_t rocksdb_options_t;
typedef mrxdb_writebatch_t rocksdb_writebatch_t;
typedef mrxdb_iterator_t rocksdb_iterator_t;

/* Read and write options are accepted and ignored. They are typedef'd to a
 * distinct empty struct rather than to void so that a `rocksdb_readoptions_t
 * *` variable still compiles. */
typedef struct mrxdb_compat_ignored_options_t {
  char unused;
} rocksdb_readoptions_t, rocksdb_writeoptions_t;

static inline rocksdb_readoptions_t *rocksdb_readoptions_create(void) {
  return (rocksdb_readoptions_t *)0;
}
static inline void rocksdb_readoptions_destroy(rocksdb_readoptions_t *o) {
  (void)o;
}
static inline rocksdb_writeoptions_t *rocksdb_writeoptions_create(void) {
  return (rocksdb_writeoptions_t *)0;
}
static inline void rocksdb_writeoptions_destroy(rocksdb_writeoptions_t *o) {
  (void)o;
}
/* Accepted and ignored: durability is a database-wide option here, set with
 * mrxdb_options_set_durability, not a per-write flag. */
static inline void rocksdb_writeoptions_set_sync(rocksdb_writeoptions_t *o,
                                                 unsigned char v) {
  (void)o;
  (void)v;
}

#define rocksdb_options_create mrxdb_options_create
#define rocksdb_options_destroy mrxdb_options_destroy
#define rocksdb_options_set_create_if_missing \
  mrxdb_options_set_create_if_missing

#define rocksdb_open mrxdb_open
#define rocksdb_free mrxdb_free

static inline void rocksdb_close(rocksdb_t *db) {
  /* RocksDB's close cannot fail and returns void. Here it can: the drain may
   * not complete. Losing that report is the price of the rename, so it is at
   * least said out loud rather than dropped. */
  char *err = (char *)0;
  mrxdb_close(db, &err);
  if (err) {
    fprintf(stderr, "rocksdb_close (mrxdb): %s\n", err);
    mrxdb_free(err);
  }
}

static inline char *rocksdb_get(rocksdb_t *db, const rocksdb_readoptions_t *o,
                                const char *key, size_t keylen,
                                size_t *vallen, char **errptr) {
  (void)o;
  return mrxdb_get(db, key, keylen, vallen, errptr);
}

static inline void rocksdb_put(rocksdb_t *db, const rocksdb_writeoptions_t *o,
                               const char *key, size_t keylen,
                               const char *val, size_t vallen,
                               char **errptr) {
  (void)o;
  mrxdb_put(db, key, keylen, val, vallen, errptr);
}

static inline void rocksdb_delete(rocksdb_t *db,
                                  const rocksdb_writeoptions_t *o,
                                  const char *key, size_t keylen,
                                  char **errptr) {
  (void)o;
  mrxdb_delete(db, key, keylen, errptr);
}

static inline void rocksdb_write(rocksdb_t *db,
                                 const rocksdb_writeoptions_t *o,
                                 rocksdb_writebatch_t *b, char **errptr) {
  (void)o;
  mrxdb_write(db, b, errptr);
}

static inline void rocksdb_flush(rocksdb_t *db, const rocksdb_options_t *o,
                                 char **errptr) {
  (void)o;
  mrxdb_flush(db, errptr);
}

#define rocksdb_writebatch_create mrxdb_writebatch_create
#define rocksdb_writebatch_destroy mrxdb_writebatch_destroy
#define rocksdb_writebatch_put mrxdb_writebatch_put
#define rocksdb_writebatch_delete mrxdb_writebatch_delete

static inline rocksdb_iterator_t *rocksdb_create_iterator(
    rocksdb_t *db, const rocksdb_readoptions_t *o) {
  (void)o;
  return mrxdb_create_iterator(db);
}

#define rocksdb_iter_destroy mrxdb_iter_destroy
#define rocksdb_iter_seek_to_first mrxdb_iter_seek_to_first
#define rocksdb_iter_seek mrxdb_iter_seek
#define rocksdb_iter_seek_for_prev mrxdb_iter_seek_for_prev
#define rocksdb_iter_valid mrxdb_iter_valid
#define rocksdb_iter_next mrxdb_iter_next
#define rocksdb_iter_key mrxdb_iter_key
#define rocksdb_iter_value mrxdb_iter_value
#define rocksdb_iter_get_error mrxdb_iter_get_error

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MRXDB_ROCKSDB_COMPAT_H */
