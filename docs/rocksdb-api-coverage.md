# RocksDB C API coverage

What `mrxdb_rocksdb_compat.h` covers of `rocksdb/c.h`, and what it does
not. Generated against the header this build actually links
(`/usr/include/rocksdb/c.h`); regenerate with
`scripts/rocksdb_api_coverage.py` if the RocksDB version changes.

## Read this before treating the gap as a to-do list

Most of what is missing is missing **on purpose**, and a good deal of it
could not be added without changing what this thing is.

**The cache is not a RocksDB replacement.** The interface it has to
satisfy is mako's `OrderedIndex`
(`src/mako/storage/abstract_ordered_index.h`): `get`, `put`, `insert`,
`remove`, `scan`, `rscan`, `size`, `clear`, `get_table_id`,
`get_is_remote`. That is fully implemented and differential-tested
against the C++ cache. `mrxdb_*` exists so a caller already written
against `rocksdb/c.h` can move with a rename; it was never an attempt at
the whole API.

**Three groups are structurally impossible, not merely absent:**

* **Transactions (65 functions).** RocksDB builds `TransactionDB` on an
  atomic `WriteBatch`. Ours is not atomic — it replays through the
  ordinary write path, so a reader can observe a partially applied
  batch. There is also no snapshot to read from and no conflict
  detection. Supporting these means building a second commit protocol
  over the one the cache already has.

  This matters less than it looks: **mako has never used RocksDB
  transactions.** There is no reference to `rocksdb_transaction`,
  `TransactionDB`, or `OptimisticTransactionDB` anywhere outside
  `third-party/`. Concurrency control lives above storage, in
  `src/deptran` (OCC, 2PL, Janus, RCC). If transactional access at this
  layer is ever wanted, the seam is mako's own `TxnOrderedIndex`
  (`tx_get`/`tx_put`, taking a mako transaction handle), not RocksDB's.

* **Snapshots (1) and the indexed write batch (31).** Both need MVCC.
  The cache stores exactly one version per key; there is nothing to
  build a consistent read view out of.

* **Column families (6).** The cache is one keyspace over one masstree.
  Multiple families means multiple trees and a per-family flusher and
  watermark — a different design, not a missing call.

**Two groups are absent but would be straightforward:**

* **Options (150 + 41).** Almost all of these tune RocksDB itself, which
  still exists underneath — they belong on the durable store's options,
  not on the cache's. `mrxdb_options_set_capacity_bytes` and
  `set_durability` are the two that are genuinely ours.

* **Bulk load, backup, statistics, environment (77).** Nothing about the
  design prevents these; they simply have no caller yet. `multi_get`,
  `delete_range`, `property_value` and `compact_range` are the ones most
  likely to be asked for first.

**One group is a real gap worth noting:** `rocksdb_iter_prev` and the
bounded-iterator options. Direction here is fixed at seek time, because
a chunked scan has no cursor to reverse. Code that walks backwards
mid-iteration cannot be ported by renaming.

The full list follows, grouped.

<!-- GENERATED: everything below is rebuilt by scripts/rocksdb_api_coverage.py -->

| | count |
|---|---|
| RocksDB C functions | **567** |
| Supported here | **30** |
| Not supported | **537** |

## Supported

- `rocksdb_close`
- `rocksdb_create_iterator`
- `rocksdb_delete`
- `rocksdb_flush`
- `rocksdb_free`
- `rocksdb_get`
- `rocksdb_iter_destroy`
- `rocksdb_iter_get_error`
- `rocksdb_iter_key`
- `rocksdb_iter_next`
- `rocksdb_iter_seek`
- `rocksdb_iter_seek_for_prev`
- `rocksdb_iter_seek_to_first`
- `rocksdb_iter_valid`
- `rocksdb_iter_value`
- `rocksdb_open`
- `rocksdb_options_create`
- `rocksdb_options_destroy`
- `rocksdb_options_set_create_if_missing`
- `rocksdb_put`
- `rocksdb_readoptions_create`
- `rocksdb_readoptions_destroy`
- `rocksdb_write`
- `rocksdb_writebatch_create`
- `rocksdb_writebatch_delete`
- `rocksdb_writebatch_destroy`
- `rocksdb_writebatch_put`
- `rocksdb_writeoptions_create`
- `rocksdb_writeoptions_destroy`
- `rocksdb_writeoptions_set_sync`

## Not supported: DB options — 149

- `rocksdb_fifo_compaction_options_destroy`
- `rocksdb_options_compaction_readahead_size`
- `rocksdb_options_create_copy`
- `rocksdb_options_enable_statistics`
- `rocksdb_options_get_allow_mmap_reads`
- `rocksdb_options_get_allow_mmap_writes`
- `rocksdb_options_get_atomic_flush`
- `rocksdb_options_get_blob_compression_type`
- `rocksdb_options_get_blob_file_starting_level`
- `rocksdb_options_get_blob_gc_age_cutoff`
- `rocksdb_options_get_blob_gc_force_threshold`
- `rocksdb_options_get_bottommost_compression`
- `rocksdb_options_get_compaction_pri`
- `rocksdb_options_get_compaction_style`
- `rocksdb_options_get_compression`
- `rocksdb_options_get_create_if_missing`
- `rocksdb_options_get_enable_blob_files`
- `rocksdb_options_get_enable_blob_gc`
- `rocksdb_options_get_error_if_exists`
- `rocksdb_options_get_info_log`
- `rocksdb_options_get_info_log_level`
- `rocksdb_options_get_level0_stop_writes_trigger`
- `rocksdb_options_get_manual_wal_flush`
- `rocksdb_options_get_max_background_compactions`
- `rocksdb_options_get_max_background_flushes`
- `rocksdb_options_get_max_background_jobs`
- `rocksdb_options_get_max_file_opening_threads`
- `rocksdb_options_get_max_open_files`
- `rocksdb_options_get_max_write_buffer_number`
- `rocksdb_options_get_num_levels`
- `rocksdb_options_get_paranoid_checks`
- `rocksdb_options_get_prepopulate_blob_cache`
- `rocksdb_options_get_report_bg_io_stats`
- `rocksdb_options_get_statistics_level`
- `rocksdb_options_get_table_cache_numshardbits`
- `rocksdb_options_get_target_file_size_multiplier`
- `rocksdb_options_get_ttl`
- `rocksdb_options_get_unordered_write`
- `rocksdb_options_get_use_adaptive_mutex`
- `rocksdb_options_get_use_direct_reads`
- `rocksdb_options_get_use_fsync`
- `rocksdb_options_get_wal_compression`
- `rocksdb_options_get_wal_recovery_mode`
- `rocksdb_options_increase_parallelism`
- `rocksdb_options_optimize_for_point_lookup`
- `rocksdb_options_optimize_level_style_compaction`
- `rocksdb_options_prepare_for_bulk_load`
- `rocksdb_options_set_advise_random_on_open`
- `rocksdb_options_set_allow_ingest_behind`
- `rocksdb_options_set_allow_mmap_reads`
- `rocksdb_options_set_allow_mmap_writes`
- `rocksdb_options_set_arena_block_size`
- `rocksdb_options_set_atomic_flush`
- `rocksdb_options_set_blob_cache`
- `rocksdb_options_set_blob_compression_type`
- `rocksdb_options_set_blob_file_size`
- `rocksdb_options_set_blob_file_starting_level`
- `rocksdb_options_set_blob_gc_age_cutoff`
- `rocksdb_options_set_blob_gc_force_threshold`
- `rocksdb_options_set_block_based_table_factory`
- `rocksdb_options_set_bloom_locality`
- `rocksdb_options_set_bottommost_compression`
- `rocksdb_options_set_bytes_per_sync`
- `rocksdb_options_set_cf_paths`
- `rocksdb_options_set_compaction_filter`
- `rocksdb_options_set_compaction_filter_factory`
- `rocksdb_options_set_compaction_pri`
- `rocksdb_options_set_compaction_style`
- `rocksdb_options_set_comparator`
- `rocksdb_options_set_compression`
- `rocksdb_options_set_compression_options`
- `rocksdb_options_set_compression_per_level`
- `rocksdb_options_set_cuckoo_table_factory`
- `rocksdb_options_set_db_log_dir`
- `rocksdb_options_set_db_paths`
- `rocksdb_options_set_db_write_buffer_size`
- `rocksdb_options_set_disable_auto_compactions`
- `rocksdb_options_set_dump_malloc_stats`
- `rocksdb_options_set_enable_blob_files`
- `rocksdb_options_set_enable_blob_gc`
- `rocksdb_options_set_enable_pipelined_write`
- `rocksdb_options_set_env`
- `rocksdb_options_set_error_if_exists`
- `rocksdb_options_set_fifo_compaction_options`
- `rocksdb_options_set_hash_link_list_rep`
- `rocksdb_options_set_hash_skip_list_rep`
- `rocksdb_options_set_info_log`
- `rocksdb_options_set_info_log_level`
- `rocksdb_options_set_inplace_update_num_locks`
- `rocksdb_options_set_inplace_update_support`
- `rocksdb_options_set_is_fd_close_on_exec`
- `rocksdb_options_set_keep_log_file_num`
- `rocksdb_options_set_level0_stop_writes_trigger`
- `rocksdb_options_set_log_file_time_to_roll`
- `rocksdb_options_set_manifest_preallocation_size`
- `rocksdb_options_set_manual_wal_flush`
- `rocksdb_options_set_max_background_compactions`
- `rocksdb_options_set_max_background_flushes`
- `rocksdb_options_set_max_background_jobs`
- `rocksdb_options_set_max_bytes_for_level_base`
- `rocksdb_options_set_max_compaction_bytes`
- `rocksdb_options_set_max_file_opening_threads`
- `rocksdb_options_set_max_log_file_size`
- `rocksdb_options_set_max_manifest_file_size`
- `rocksdb_options_set_max_open_files`
- `rocksdb_options_set_max_subcompactions`
- `rocksdb_options_set_max_successive_merges`
- `rocksdb_options_set_max_total_wal_size`
- `rocksdb_options_set_max_write_buffer_number`
- `rocksdb_options_set_memtable_huge_page_size`
- `rocksdb_options_set_memtable_vector_rep`
- `rocksdb_options_set_merge_operator`
- `rocksdb_options_set_min_blob_size`
- `rocksdb_options_set_min_level_to_compress`
- `rocksdb_options_set_num_levels`
- `rocksdb_options_set_optimize_filters_for_hits`
- `rocksdb_options_set_paranoid_checks`
- `rocksdb_options_set_periodic_compaction_seconds`
- `rocksdb_options_set_plain_table_factory`
- `rocksdb_options_set_prefix_extractor`
- `rocksdb_options_set_prepopulate_blob_cache`
- `rocksdb_options_set_ratelimiter`
- `rocksdb_options_set_recycle_log_file_num`
- `rocksdb_options_set_report_bg_io_stats`
- `rocksdb_options_set_row_cache`
- `rocksdb_options_set_statistics_level`
- `rocksdb_options_set_stats_dump_period_sec`
- `rocksdb_options_set_stats_persist_period_sec`
- `rocksdb_options_set_table_cache_numshardbits`
- `rocksdb_options_set_target_file_size_base`
- `rocksdb_options_set_target_file_size_multiplier`
- `rocksdb_options_set_ttl`
- `rocksdb_options_set_uint64add_merge_operator`
- `rocksdb_options_set_unordered_write`
- `rocksdb_options_set_use_adaptive_mutex`
- `rocksdb_options_set_use_direct_reads`
- `rocksdb_options_set_use_fsync`
- `rocksdb_options_set_wal_bytes_per_sync`
- `rocksdb_options_set_wal_compression`
- `rocksdb_options_set_wal_dir`
- `rocksdb_options_set_wal_recovery_mode`
- `rocksdb_options_set_write_buffer_manager`
- `rocksdb_options_set_write_buffer_size`
- `rocksdb_options_set_write_dbid_to_manifest`
- `rocksdb_options_set_write_identity_file`
- `rocksdb_options_statistics_get_histogram_data`
- `rocksdb_options_statistics_get_string`
- `rocksdb_options_statistics_get_ticker_count`
- `rocksdb_universal_compaction_options_destroy`

## Not supported: core DB operations — 102

- `rocksdb_approximate_memory_usage_destroy`
- `rocksdb_approximate_sizes`
- `rocksdb_approximate_sizes_cf`
- `rocksdb_approximate_sizes_cf_with_flags`
- `rocksdb_batched_multi_get_cf`
- `rocksdb_cancel_all_background_work`
- `rocksdb_compact_range`
- `rocksdb_compact_range_cf`
- `rocksdb_compact_range_cf_opt`
- `rocksdb_compact_range_opt`
- `rocksdb_create_column_families_destroy`
- `rocksdb_create_default_env`
- `rocksdb_create_dir_if_missing`
- `rocksdb_create_iterator_cf`
- `rocksdb_create_iterators`
- `rocksdb_create_mem_env`
- `rocksdb_create_snapshot`
- `rocksdb_dbpath_create`
- `rocksdb_dbpath_destroy`
- `rocksdb_delete_cf`
- `rocksdb_delete_cf_with_ts`
- `rocksdb_delete_file`
- `rocksdb_delete_file_in_range`
- `rocksdb_delete_file_in_range_cf`
- `rocksdb_delete_range_cf`
- `rocksdb_delete_with_ts`
- `rocksdb_destroy_db`
- `rocksdb_disable_file_deletions`
- `rocksdb_disable_manual_compaction`
- `rocksdb_drop_column_family`
- `rocksdb_enable_file_deletions`
- `rocksdb_enable_manual_compaction`
- `rocksdb_envoptions_create`
- `rocksdb_envoptions_destroy`
- `rocksdb_flush_cf`
- `rocksdb_flush_cfs`
- `rocksdb_flush_wal`
- `rocksdb_get_cf`
- `rocksdb_get_cf_with_ts`
- `rocksdb_get_db_identity`
- `rocksdb_get_full_history_ts_low`
- `rocksdb_get_options_from_string`
- `rocksdb_get_pinned`
- `rocksdb_get_pinned_cf`
- `rocksdb_get_updates_since`
- `rocksdb_get_with_ts`
- `rocksdb_increase_full_history_ts_low`
- `rocksdb_ingest_external_file`
- `rocksdb_ingest_external_file_cf`
- `rocksdb_key_may_exist`
- `rocksdb_key_may_exist_cf`
- `rocksdb_level_metadata_destroy`
- `rocksdb_level_metadata_get_level`
- `rocksdb_list_column_families`
- `rocksdb_list_column_families_destroy`
- `rocksdb_livefiles`
- `rocksdb_load_latest_options`
- `rocksdb_load_latest_options_destroy`
- `rocksdb_merge`
- `rocksdb_merge_cf`
- `rocksdb_multi_get`
- `rocksdb_multi_get_cf`
- `rocksdb_multi_get_cf_with_ts`
- `rocksdb_multi_get_with_ts`
- `rocksdb_open_and_trim_history`
- `rocksdb_open_as_secondary`
- `rocksdb_open_as_secondary_column_families`
- `rocksdb_open_column_families`
- `rocksdb_open_column_families_with_ttl`
- `rocksdb_open_for_read_only`
- `rocksdb_open_with_ttl`
- `rocksdb_pinnableslice_destroy`
- `rocksdb_pinnableslice_value`
- `rocksdb_property_int`
- `rocksdb_property_int_cf`
- `rocksdb_property_value`
- `rocksdb_property_value_cf`
- `rocksdb_put_cf`
- `rocksdb_put_cf_with_ts`
- `rocksdb_put_with_ts`
- `rocksdb_release_snapshot`
- `rocksdb_repair_db`
- `rocksdb_set_options`
- `rocksdb_set_options_cf`
- `rocksdb_set_perf_level`
- `rocksdb_singledelete`
- `rocksdb_singledelete_cf`
- `rocksdb_singledelete_cf_with_ts`
- `rocksdb_singledelete_with_ts`
- `rocksdb_sst_file_metadata_destroy`
- `rocksdb_sst_file_metadata_get_directory`
- `rocksdb_sst_file_metadata_get_largestkey`
- `rocksdb_sst_file_metadata_get_smallestkey`
- `rocksdb_suggest_compact_range`
- `rocksdb_suggest_compact_range_cf`
- `rocksdb_try_catch_up_with_primary`
- `rocksdb_wait_for_compact`
- `rocksdb_wait_for_compact_options_destroy`
- `rocksdb_wait_for_compact_options_set_close_db`
- `rocksdb_wait_for_compact_options_set_flush`
- `rocksdb_wait_for_compact_options_set_timeout`
- `rocksdb_write_writebatch_wi`

## Not supported: transactions — 63

- `rocksdb_optimistictransaction_options_destroy`
- `rocksdb_optimistictransactiondb_close`
- `rocksdb_optimistictransactiondb_close_base_db`
- `rocksdb_optimistictransactiondb_property_int`
- `rocksdb_optimistictransactiondb_property_value`
- `rocksdb_optimistictransactiondb_write`
- `rocksdb_transaction_begin`
- `rocksdb_transaction_commit`
- `rocksdb_transaction_delete`
- `rocksdb_transaction_delete_cf`
- `rocksdb_transaction_destroy`
- `rocksdb_transaction_get`
- `rocksdb_transaction_get_cf`
- `rocksdb_transaction_get_for_update`
- `rocksdb_transaction_get_for_update_cf`
- `rocksdb_transaction_get_name`
- `rocksdb_transaction_merge`
- `rocksdb_transaction_merge_cf`
- `rocksdb_transaction_multi_get`
- `rocksdb_transaction_multi_get_cf`
- `rocksdb_transaction_multi_get_for_update`
- `rocksdb_transaction_multi_get_for_update_cf`
- `rocksdb_transaction_options_destroy`
- `rocksdb_transaction_options_set_deadlock_detect`
- `rocksdb_transaction_options_set_expiration`
- `rocksdb_transaction_options_set_lock_timeout`
- `rocksdb_transaction_options_set_set_snapshot`
- `rocksdb_transaction_options_set_skip_prepare`
- `rocksdb_transaction_prepare`
- `rocksdb_transaction_put`
- `rocksdb_transaction_put_cf`
- `rocksdb_transaction_rebuild_from_writebatch`
- `rocksdb_transaction_rebuild_from_writebatch_wi`
- `rocksdb_transaction_rollback`
- `rocksdb_transaction_rollback_to_savepoint`
- `rocksdb_transaction_set_commit_timestamp`
- `rocksdb_transaction_set_name`
- `rocksdb_transaction_set_savepoint`
- `rocksdb_transactiondb_close`
- `rocksdb_transactiondb_close_base_db`
- `rocksdb_transactiondb_delete`
- `rocksdb_transactiondb_delete_cf`
- `rocksdb_transactiondb_flush`
- `rocksdb_transactiondb_flush_cf`
- `rocksdb_transactiondb_flush_cfs`
- `rocksdb_transactiondb_flush_wal`
- `rocksdb_transactiondb_get`
- `rocksdb_transactiondb_get_base_db`
- `rocksdb_transactiondb_get_cf`
- `rocksdb_transactiondb_merge`
- `rocksdb_transactiondb_merge_cf`
- `rocksdb_transactiondb_multi_get`
- `rocksdb_transactiondb_multi_get_cf`
- `rocksdb_transactiondb_open`
- `rocksdb_transactiondb_options_destroy`
- `rocksdb_transactiondb_options_set_max_num_locks`
- `rocksdb_transactiondb_options_set_num_stripes`
- `rocksdb_transactiondb_property_int`
- `rocksdb_transactiondb_property_value`
- `rocksdb_transactiondb_put`
- `rocksdb_transactiondb_put_cf`
- `rocksdb_transactiondb_release_snapshot`
- `rocksdb_transactiondb_write`

## Not supported: read/write/flush options — 38

- `rocksdb_compactoptions_destroy`
- `rocksdb_compactoptions_get_target_level`
- `rocksdb_compactoptions_set_change_level`
- `rocksdb_compactoptions_set_full_history_ts_low`
- `rocksdb_compactoptions_set_target_level`
- `rocksdb_flushoptions_create`
- `rocksdb_flushoptions_destroy`
- `rocksdb_flushoptions_get_wait`
- `rocksdb_flushoptions_set_wait`
- `rocksdb_readoptions_get_async_io`
- `rocksdb_readoptions_get_fill_cache`
- `rocksdb_readoptions_get_pin_data`
- `rocksdb_readoptions_get_read_tier`
- `rocksdb_readoptions_get_tailing`
- `rocksdb_readoptions_set_async_io`
- `rocksdb_readoptions_set_auto_readahead_size`
- `rocksdb_readoptions_set_deadline`
- `rocksdb_readoptions_set_fill_cache`
- `rocksdb_readoptions_set_ignore_range_deletions`
- `rocksdb_readoptions_set_io_timeout`
- `rocksdb_readoptions_set_iter_start_ts`
- `rocksdb_readoptions_set_iterate_lower_bound`
- `rocksdb_readoptions_set_iterate_upper_bound`
- `rocksdb_readoptions_set_managed`
- `rocksdb_readoptions_set_pin_data`
- `rocksdb_readoptions_set_prefix_same_as_start`
- `rocksdb_readoptions_set_read_tier`
- `rocksdb_readoptions_set_readahead_size`
- `rocksdb_readoptions_set_snapshot`
- `rocksdb_readoptions_set_tailing`
- `rocksdb_readoptions_set_timestamp`
- `rocksdb_readoptions_set_total_order_seek`
- `rocksdb_readoptions_set_verify_checksums`
- `rocksdb_writeoptions_get_low_pri`
- `rocksdb_writeoptions_get_no_slowdown`
- `rocksdb_writeoptions_get_sync`
- `rocksdb_writeoptions_set_low_pri`
- `rocksdb_writeoptions_set_no_slowdown`

## Not supported: write batch (indexed) — 31

- `rocksdb_writebatch_wi_clear`
- `rocksdb_writebatch_wi_count`
- `rocksdb_writebatch_wi_data`
- `rocksdb_writebatch_wi_delete`
- `rocksdb_writebatch_wi_delete_cf`
- `rocksdb_writebatch_wi_delete_range`
- `rocksdb_writebatch_wi_delete_range_cf`
- `rocksdb_writebatch_wi_delete_rangev`
- `rocksdb_writebatch_wi_delete_rangev_cf`
- `rocksdb_writebatch_wi_deletev`
- `rocksdb_writebatch_wi_deletev_cf`
- `rocksdb_writebatch_wi_destroy`
- `rocksdb_writebatch_wi_get_from_batch`
- `rocksdb_writebatch_wi_get_from_batch_and_db`
- `rocksdb_writebatch_wi_get_from_batch_and_db_cf`
- `rocksdb_writebatch_wi_get_from_batch_cf`
- `rocksdb_writebatch_wi_iterate`
- `rocksdb_writebatch_wi_merge`
- `rocksdb_writebatch_wi_merge_cf`
- `rocksdb_writebatch_wi_mergev`
- `rocksdb_writebatch_wi_mergev_cf`
- `rocksdb_writebatch_wi_put`
- `rocksdb_writebatch_wi_put_cf`
- `rocksdb_writebatch_wi_put_log_data`
- `rocksdb_writebatch_wi_putv`
- `rocksdb_writebatch_wi_putv_cf`
- `rocksdb_writebatch_wi_rollback_to_save_point`
- `rocksdb_writebatch_wi_set_save_point`
- `rocksdb_writebatch_wi_singledelete`
- `rocksdb_writebatch_wi_singledelete_cf`
- `rocksdb_writebatch_wi_update_timestamps`

## Not supported: table & cache tuning — 30

- `rocksdb_block_based_options_destroy`
- `rocksdb_block_based_options_set_block_cache`
- `rocksdb_block_based_options_set_block_size`
- `rocksdb_block_based_options_set_checksum`
- `rocksdb_block_based_options_set_filter_policy`
- `rocksdb_block_based_options_set_format_version`
- `rocksdb_block_based_options_set_index_type`
- `rocksdb_block_based_options_set_no_block_cache`
- `rocksdb_cache_create_hyper_clock`
- `rocksdb_cache_create_lru`
- `rocksdb_cache_create_lru_opts`
- `rocksdb_cache_destroy`
- `rocksdb_cache_disown_data`
- `rocksdb_cache_set_capacity`
- `rocksdb_cuckoo_options_destroy`
- `rocksdb_cuckoo_options_set_cuckoo_block_size`
- `rocksdb_cuckoo_options_set_hash_ratio`
- `rocksdb_cuckoo_options_set_max_search_depth`
- `rocksdb_cuckoo_options_set_use_module_hash`
- `rocksdb_hyper_clock_cache_options_destroy`
- `rocksdb_hyper_clock_cache_options_set_capacity`
- `rocksdb_lru_cache_options_destroy`
- `rocksdb_lru_cache_options_set_capacity`
- `rocksdb_lru_cache_options_set_memory_allocator`
- `rocksdb_lru_cache_options_set_num_shard_bits`
- `rocksdb_write_buffer_manager_cost_to_cache`
- `rocksdb_write_buffer_manager_destroy`
- `rocksdb_write_buffer_manager_enabled`
- `rocksdb_write_buffer_manager_set_allow_stall`
- `rocksdb_write_buffer_manager_set_buffer_size`

## Not supported: write batch (plain) — 30

- `rocksdb_writebatch_clear`
- `rocksdb_writebatch_count`
- `rocksdb_writebatch_create_from`
- `rocksdb_writebatch_data`
- `rocksdb_writebatch_delete_cf`
- `rocksdb_writebatch_delete_cf_with_ts`
- `rocksdb_writebatch_delete_range`
- `rocksdb_writebatch_delete_range_cf`
- `rocksdb_writebatch_delete_rangev`
- `rocksdb_writebatch_delete_rangev_cf`
- `rocksdb_writebatch_deletev`
- `rocksdb_writebatch_deletev_cf`
- `rocksdb_writebatch_iterate`
- `rocksdb_writebatch_iterate_cf`
- `rocksdb_writebatch_merge`
- `rocksdb_writebatch_merge_cf`
- `rocksdb_writebatch_mergev`
- `rocksdb_writebatch_mergev_cf`
- `rocksdb_writebatch_pop_save_point`
- `rocksdb_writebatch_put_cf`
- `rocksdb_writebatch_put_cf_with_ts`
- `rocksdb_writebatch_put_log_data`
- `rocksdb_writebatch_putv`
- `rocksdb_writebatch_putv_cf`
- `rocksdb_writebatch_rollback_to_save_point`
- `rocksdb_writebatch_set_save_point`
- `rocksdb_writebatch_singledelete`
- `rocksdb_writebatch_singledelete_cf`
- `rocksdb_writebatch_singledelete_cf_with_ts`
- `rocksdb_writebatch_update_timestamps`

## Not supported: backup / restore / checkpoint — 22

- `rocksdb_backup_engine_close`
- `rocksdb_backup_engine_create_new_backup`
- `rocksdb_backup_engine_create_new_backup_flush`
- `rocksdb_backup_engine_info_backup_id`
- `rocksdb_backup_engine_info_count`
- `rocksdb_backup_engine_info_destroy`
- `rocksdb_backup_engine_info_number_files`
- `rocksdb_backup_engine_info_size`
- `rocksdb_backup_engine_info_timestamp`
- `rocksdb_backup_engine_open`
- `rocksdb_backup_engine_options_destroy`
- `rocksdb_backup_engine_options_get_sync`
- `rocksdb_backup_engine_options_set_backup_dir`
- `rocksdb_backup_engine_options_set_env`
- `rocksdb_backup_engine_options_set_sync`
- `rocksdb_backup_engine_purge_old_backups`
- `rocksdb_backup_engine_restore_db_from_backup`
- `rocksdb_backup_engine_verify_backup`
- `rocksdb_checkpoint_create`
- `rocksdb_checkpoint_object_destroy`
- `rocksdb_restore_options_destroy`
- `rocksdb_restore_options_set_keep_log_files`

## Not supported: bulk load / files — 19

- `rocksdb_ingestexternalfileoptions_destroy`
- `rocksdb_livefiles_column_family_name`
- `rocksdb_livefiles_count`
- `rocksdb_livefiles_destroy`
- `rocksdb_livefiles_largestkey`
- `rocksdb_livefiles_level`
- `rocksdb_livefiles_name`
- `rocksdb_livefiles_smallestkey`
- `rocksdb_sstfilewriter_add`
- `rocksdb_sstfilewriter_delete`
- `rocksdb_sstfilewriter_delete_range`
- `rocksdb_sstfilewriter_delete_with_ts`
- `rocksdb_sstfilewriter_destroy`
- `rocksdb_sstfilewriter_file_size`
- `rocksdb_sstfilewriter_finish`
- `rocksdb_sstfilewriter_merge`
- `rocksdb_sstfilewriter_open`
- `rocksdb_sstfilewriter_put`
- `rocksdb_sstfilewriter_put_with_ts`

## Not supported: stats & introspection — 18

- `rocksdb_memory_allocator_destroy`
- `rocksdb_memory_consumers_add_cache`
- `rocksdb_memory_consumers_add_db`
- `rocksdb_memory_consumers_destroy`
- `rocksdb_perfcontext_create`
- `rocksdb_perfcontext_destroy`
- `rocksdb_perfcontext_report`
- `rocksdb_perfcontext_reset`
- `rocksdb_statistics_histogram_data_destroy`
- `rocksdb_statistics_histogram_data_get_average`
- `rocksdb_statistics_histogram_data_get_count`
- `rocksdb_statistics_histogram_data_get_max`
- `rocksdb_statistics_histogram_data_get_median`
- `rocksdb_statistics_histogram_data_get_min`
- `rocksdb_statistics_histogram_data_get_p95`
- `rocksdb_statistics_histogram_data_get_p99`
- `rocksdb_statistics_histogram_data_get_std_dev`
- `rocksdb_statistics_histogram_data_get_sum`

## Not supported: environment / IO — 12

- `rocksdb_env_destroy`
- `rocksdb_env_get_background_threads`
- `rocksdb_env_get_high_priority_background_threads`
- `rocksdb_env_get_low_priority_background_threads`
- `rocksdb_env_join_all_threads`
- `rocksdb_env_lower_thread_pool_cpu_priority`
- `rocksdb_env_lower_thread_pool_io_priority`
- `rocksdb_env_set_background_threads`
- `rocksdb_env_set_low_priority_background_threads`
- `rocksdb_logger_destroy`
- `rocksdb_ratelimiter_create`
- `rocksdb_ratelimiter_destroy`

## Not supported: pluggable behaviour — 8

- `rocksdb_compactionfilter_destroy`
- `rocksdb_compactionfilter_set_ignore_snapshots`
- `rocksdb_compactionfilterfactory_destroy`
- `rocksdb_comparator_create`
- `rocksdb_comparator_destroy`
- `rocksdb_filterpolicy_destroy`
- `rocksdb_mergeoperator_destroy`
- `rocksdb_slicetransform_destroy`

## Not supported: column families — 6

- `rocksdb_column_family_handle_destroy`
- `rocksdb_column_family_handle_get_name`
- `rocksdb_column_family_metadata_destroy`
- `rocksdb_column_family_metadata_get_file_count`
- `rocksdb_column_family_metadata_get_name`
- `rocksdb_column_family_metadata_get_size`

## Not supported: WAL / replication — 5

- `rocksdb_wal_iter_destroy`
- `rocksdb_wal_iter_get_batch`
- `rocksdb_wal_iter_next`
- `rocksdb_wal_iter_status`
- `rocksdb_wal_iter_valid`

## Not supported: iterators — 4

- `rocksdb_iter_prev`
- `rocksdb_iter_refresh`
- `rocksdb_iter_seek_to_last`
- `rocksdb_iter_timestamp`
