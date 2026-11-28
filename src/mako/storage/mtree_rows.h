// Private storage ABI. Only the safe Rust handle module may call this API.
// All handles are native allocations, never IDs into a second row index.
#pragma once
#pragma GCC visibility push(hidden)
#include "mtree_abi.h"
#ifdef __cplusplus
extern "C" {
#endif
uint64_t mako_rows_abi(void) noexcept;
struct mt_rows;
struct mt_row_budget;
struct mt_row;
struct mt_value;
struct mt_write;
struct mt_row_view {
  const unsigned char *data;
  size_t size;
  uint64_t version;
  uint32_t time_term;
  uint32_t present;
};
// Leaf addresses are private observation handles. Row directories only insert:
// leaves are never removed, and the owning tree stays alive until every Rust
// row/predicate handle is gone. No RCU guard crosses a transaction/RPC wait.
struct mt_leaf_stamp {
  const void *leaf;
  uint64_t version;
};
enum { MT_ROW_NEW_LEAVES = 130 }; // 1024-byte key / 8-byte trie slices + split
struct mt_row_insertion {
  struct mt_leaf_stamp before, after;
  size_t new_count;
  struct mt_leaf_stamp new_leaves[MT_ROW_NEW_LEAVES];
};
// Private index operations, implemented beside the native worker/RCU ownership.
mt_status mako_row_index_find(mt_tree *, mt_thread *, const void *, size_t,
                              uint64_t *, struct mt_leaf_stamp *) noexcept;
mt_status mako_row_index_insert(mt_tree *, mt_thread *, const void *, size_t,
                                void *, mt_status (*)(void *, uint64_t *),
                                uint64_t *, struct mt_row_insertion *) noexcept;
mt_status mako_row_index_check(struct mt_leaf_stamp) noexcept;
mt_status mako_row_index_seal(mt_tree *) noexcept;
mt_status mako_row_index_scan(mt_tree *, mt_thread *, mt_scan_direction,
                              const struct mt_scan_bound *,
                              const struct mt_scan_bound *, uint64_t *, size_t,
                              size_t, struct mt_leaf_stamp *, size_t, size_t *,
                              size_t *, uint32_t *) noexcept;
struct mt_visibility {
  uint32_t term;
  uint32_t stable[10]; // UINT32_MAX denotes unknown.
  uint32_t failed;     // UINT32_MAX denotes no failure bound.
};
mt_status mako_row_budget_create(uint64_t, struct mt_row_budget **) noexcept;
void mako_row_budget_drop(struct mt_row_budget *) noexcept;
uint64_t mako_row_budget_used(const struct mt_row_budget *) noexcept;
mt_status mako_rows_create(struct mt_row_budget *, uint64_t max_rows,
                           uint64_t max_keys, struct mt_rows **) noexcept;
void mako_rows_bind(struct mt_rows *, mt_tree *) noexcept;
void mako_rows_destroy(struct mt_rows *) noexcept;
mt_status mako_rows_resolve(struct mt_rows *, mt_thread *, const void *, size_t,
                            struct mt_row **,
                            struct mt_row_insertion *) noexcept;
mt_status mako_rows_find(struct mt_rows *, mt_thread *, const void *, size_t,
                         struct mt_row **, struct mt_leaf_stamp *) noexcept;
uint64_t mako_rows_generation(const struct mt_rows *) noexcept;
uint64_t mako_rows_count(const struct mt_rows *) noexcept;
void mako_rows_floor(struct mt_rows *, uint32_t) noexcept;
void mako_row_key(const struct mt_row *, const unsigned char **,
                  size_t *) noexcept;
// The safe handle layer proves that each row belongs to the supplied live
// table before calling these private entry points. No row stores a backpointer.
mt_status mako_row_read(struct mt_rows *, struct mt_row *, uint32_t owner,
                        const struct mt_visibility *, uint32_t checkpoint,
                        struct mt_value **, struct mt_row_view *) noexcept;
// Copies a stable latest row using atomic embedded bytes or the pinning latch.
// On BUFFER_TOO_SMALL, view.size reports the required capacity; no bytes copy.
mt_status mako_row_copy(struct mt_rows *, struct mt_row *, unsigned char *, size_t,
                        struct mt_row_view *) noexcept;
mt_status mako_value_new(const void *, size_t, uint32_t present,
                         struct mt_value **, struct mt_row_view *) noexcept;
struct mt_value *mako_value_clone(const struct mt_value *) noexcept;
void mako_value_drop(struct mt_value *) noexcept;
mt_status mako_row_check(struct mt_rows *, struct mt_row *, uint64_t version,
                         uint32_t owner) noexcept;
mt_status mako_row_lock(struct mt_rows *, struct mt_row *, uint32_t owner) noexcept;
// owner=0 allocates private scratch before acquisition; otherwise require the
// retained owner. Neither case publishes. Installation always checks a guard.
mt_status mako_write_prepare(struct mt_rows *, struct mt_row *, uint32_t owner,
                             const struct mt_value *,
                             struct mt_write **) noexcept;
void mako_write_drop(struct mt_write *) noexcept;
mt_status mako_row_install(struct mt_rows *, struct mt_row *, uint32_t owner, struct mt_write *,
                           uint64_t version, uint32_t time_term) noexcept;
mt_status mako_row_unlock(struct mt_rows *, struct mt_row *, uint32_t owner,
                          uint32_t disposition) noexcept;
#ifdef __cplusplus
}
#endif

#pragma GCC visibility pop
