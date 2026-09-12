/* mrxdb.h -- the C ABI over the Rust masstree-over-RocksDB write-back cache.
 *
 * Shaped after rocksdb/c.h so a caller written against that API moves with a
 * rename rather than a rewrite. mrxdb_rocksdb_compat.h does the rename
 * mechanically.
 *
 * ===========================================================================
 * THE ONE DIFFERENCE THAT MATTERS
 * ===========================================================================
 *
 * A WRITE RETURNS BEFORE IT IS DURABLE. mrxdb_put returns once the value is
 * visible to readers; it reaches RocksDB later, in the background. A crash
 * between the two loses the write.
 *
 * That is the entire point of the cache, not a defect, and it is why these
 * functions are NOT named rocksdb_*: silently giving a caller weaker
 * durability under a familiar name is the worst outcome available.
 *
 * Callers needing RocksDB's guarantee call mrxdb_flush, which is a real
 * barrier -- it returns only once everything acknowledged before the call is
 * durable. mrxdb_close does the same on the way out.
 *
 * Two smaller differences follow from the design:
 *
 *  - MEMORY IS BOUNDED BY mrxdb_options_set_capacity_bytes, NOT BY A BLOCK
 *    CACHE. Every key is resident; only values are evicted. A workload with
 *    a billion tiny keys is bounded by the key set, and no capacity setting
 *    changes that.
 *
 *  - A DELETED KEY IS NEVER RECLAIMED FROM THE INDEX. A tombstone is
 *    published instead, because erasing the key would break the "an index
 *    miss means absent" invariant that makes reads cheap. mrxdb_len
 *    therefore counts deleted keys, and a delete-heavy workload grows the
 *    index until the database is reopened.
 *
 * ===========================================================================
 * CONVENTIONS
 * ===========================================================================
 *
 * ERRORS. Fallible calls take a `char **errptr`. On failure *errptr is set to
 * a NUL-terminated message the CALLER OWNS and frees with mrxdb_free; on
 * success it is left alone. Pass NULL to discard the detail. Initialise it to
 * NULL and check it after each call -- errors do not accumulate.
 *
 * BUFFERS. Anything returned as `char *` was allocated with malloc and is
 * freed with mrxdb_free (which is plain free). Keys and values are
 * length-delimited byte strings, never NUL-terminated: an embedded NUL is
 * ordinary data, and so is a zero-length value.
 *
 * ABSENT VERSUS FAILED. mrxdb_get returns NULL for both. They are told apart
 * by *errptr. A present-but-empty value returns non-NULL with *vallen == 0.
 *
 * THREADS. A mrxdb_t is safe to use from many threads at once. Iterators,
 * write batches, and options handles are not -- one owner each.
 *
 * THREAD COUNT IS CAPPED AND PERMANENT. The index allocates a core ID per
 * calling thread from a 512-entry space and NEVER RECYCLES IT. Use a fixed
 * pool of long-lived threads; a thread-per-request design fails once 512
 * threads have ever touched the database.
 */

#ifndef MRXDB_H
#define MRXDB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible change. Compare against mrxdb_abi_version() at
 * startup: a layout or semantic drift between this header and the linked
 * library corrupts silently otherwise. */
#define MRXDB_ABI_VERSION 1u

uint32_t mrxdb_abi_version(void);

/* --- handles ------------------------------------------------------------ */

typedef struct mrxdb_t mrxdb_t;
typedef struct mrxdb_options_t mrxdb_options_t;
typedef struct mrxdb_writebatch_t mrxdb_writebatch_t;
typedef struct mrxdb_iterator_t mrxdb_iterator_t;

/* --- memory ------------------------------------------------------------- */

/* Free a buffer or error string returned by this library. */
void mrxdb_free(void *p);

/* --- options ------------------------------------------------------------ */

mrxdb_options_t *mrxdb_options_create(void);
void mrxdb_options_destroy(mrxdb_options_t *o);

/* Accepted and ignored: the database is always created if missing. Present so
 * a ported RocksDB caller compiles unchanged. */
void mrxdb_options_set_create_if_missing(mrxdb_options_t *o, unsigned char v);

/* Byte ceiling for the EVICTABLE VALUE TIER; 0 disables eviction entirely.
 *
 * Compared against resident bytes minus the un-evictable floor (the entries
 * and record headers eviction never reclaims), so a value below that floor is
 * not "very small", it is unreachable. */
void mrxdb_options_set_capacity_bytes(mrxdb_options_t *o, uint64_t bytes);

/* Durability of each writeback batch:
 *   0  fsync every batch      -- the watermark means "on the platter"
 *   1  WAL, no fsync          -- survives a process crash, maybe not a
 *                                machine crash                     (default)
 *   2  no WAL                 -- fastest; only correct where the durable
 *                                store is itself a cache of something else */
void mrxdb_options_set_durability(mrxdb_options_t *o, int level);

/* --- open and close ----------------------------------------------------- */

/* Open, creating if needed. NULL on failure. `o` may be NULL for defaults.
 *
 * Interns every existing key before returning, so the cost is proportional to
 * the KEY COUNT (no values are read). Until that finishes an index miss
 * cannot be trusted as absence, which is why it is not deferred. */
mrxdb_t *mrxdb_open(const mrxdb_options_t *o, const char *name,
                    char **errptr);

/* Make everything acknowledged durable, then release the handle.
 *
 * SETS *errptr IF THE DRAIN FAILED, and frees the handle either way. Ignoring
 * that error means exiting having lost acknowledged writes. */
void mrxdb_close(mrxdb_t *db, char **errptr);

/* --- point operations --------------------------------------------------- */

/* Read a key. NULL means absent OR failed -- check *errptr. */
char *mrxdb_get(mrxdb_t *db, const char *key, size_t keylen, size_t *vallen,
                char **errptr);

/* Write. Returns once VISIBLE, not once durable. */
void mrxdb_put(mrxdb_t *db, const char *key, size_t keylen, const char *val,
               size_t vallen, char **errptr);

/* Delete. */
void mrxdb_delete(mrxdb_t *db, const char *key, size_t keylen, char **errptr);

/* Write only if absent. Returns 1 if written, 0 if the key was live.
 *
 * Not in rocksdb/c.h. It is here because the cache can answer it without a
 * read-modify-write: the index holds every key, so "is this key live" is a
 * local question. */
unsigned char mrxdb_insert(mrxdb_t *db, const char *key, size_t keylen,
                           const char *val, size_t vallen, char **errptr);

/* Block until everything acknowledged before this call is durable.
 *
 * THE REAL BARRIER. Not RocksDB's mrxdb_flush-the-memtable; this is the
 * function that turns "written" into "safe".
 *
 * Under sustained write overload this can block indefinitely, and that is
 * correct: the watermark is a LOW-water mark, so when writes arrive faster
 * than the durable store absorbs them, nothing becomes durable and there is
 * nothing honest to return. */
void mrxdb_flush(mrxdb_t *db, char **errptr);

/* Delete every key and make that durable. */
void mrxdb_clear(mrxdb_t *db, char **errptr);

/* --- write batches ------------------------------------------------------ */

/* NOT ATOMIC, unlike rocksdb_writebatch_t. Operations are replayed through
 * the ordinary write path and become visible as they land, so a reader can
 * observe a partially applied batch. The batch amortises call overhead and
 * fixes ordering within itself; it provides no isolation. */
mrxdb_writebatch_t *mrxdb_writebatch_create(void);
void mrxdb_writebatch_destroy(mrxdb_writebatch_t *b);
void mrxdb_writebatch_put(mrxdb_writebatch_t *b, const char *key,
                          size_t keylen, const char *val, size_t vallen);
void mrxdb_writebatch_delete(mrxdb_writebatch_t *b, const char *key,
                             size_t keylen);

/* Apply and empty the batch. The handle remains the caller's to destroy. */
void mrxdb_write(mrxdb_t *db, mrxdb_writebatch_t *b, char **errptr);

/* --- iteration ---------------------------------------------------------- */

/* A NEW ITERATOR IS NOT POSITIONED. Seek before reading, as with RocksDB.
 *
 * An iterator is a chunked scan, NOT a snapshot: it holds no lock and pins
 * nothing, so keys written after it starts may or may not appear, and a key
 * deleted mid-walk may still be yielded if it was already buffered. There is
 * no MVCC here to build a snapshot from. */
mrxdb_iterator_t *mrxdb_create_iterator(mrxdb_t *db);
void mrxdb_iter_destroy(mrxdb_iterator_t *it);

void mrxdb_iter_seek_to_first(mrxdb_iterator_t *it);
void mrxdb_iter_seek(mrxdb_iterator_t *it, const char *key, size_t keylen);
/* Position at the last key at or before `key` and walk BACKWARDS. */
void mrxdb_iter_seek_for_prev(mrxdb_iterator_t *it, const char *key,
                              size_t keylen);

unsigned char mrxdb_iter_valid(const mrxdb_iterator_t *it);

/* Step in the direction fixed at seek time.
 *
 * THERE IS NO mrxdb_iter_prev. Direction is chosen by which seek was used and
 * cannot change mid-walk: reversing would need a real cursor into the index
 * rather than a chunk buffer, and a `prev` that silently re-walked from the
 * start would be worse than its absence. */
void mrxdb_iter_next(mrxdb_iterator_t *it);

/* Valid until the next next/seek/destroy on this iterator. */
const char *mrxdb_iter_key(const mrxdb_iterator_t *it, size_t *klen);
const char *mrxdb_iter_value(const mrxdb_iterator_t *it, size_t *vlen);

/* Report an iteration failure.
 *
 * WORTH CHECKING: a value may have to be fetched back from the durable store
 * mid-walk, and that can fail. Without this the iterator merely looks like it
 * ended, turning a transient IO error into apparent data loss. */
void mrxdb_iter_get_error(const mrxdb_iterator_t *it, char **errptr);

/* --- introspection ------------------------------------------------------ */

/* The durability watermark: every version at or below it is durable. */
uint64_t mrxdb_watermark(const mrxdb_t *db);

/* Key count, INCLUDING deleted keys. See the header note. */
size_t mrxdb_len(const mrxdb_t *db);

/* 1 resident, 0 evicted, -1 unknown or deleted. For tests; nothing in a data
 * path should branch on it. */
int mrxdb_is_resident(const mrxdb_t *db, const char *key, size_t keylen);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MRXDB_H */
