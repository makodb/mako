#pragma once

/**
 * mako/buffered_iterator.hh - Buffered RocksDB-compatible Iterator
 *
 * Implements IIterator on top of Mako's callback-based Scan/ReverseScan.
 * On Seek/SeekToFirst/SeekToLast the full result set (up to max_buffer_size
 * entries) is buffered into a vector so that Next/Prev/key/value can be
 * answered without touching the storage layer again.
 *
 * Trade-offs:
 *   - Memory: O(N) where N is the number of buffered entries
 *   - Consistency: buffer is a snapshot captured at Seek time; subsequent
 *     writes are NOT visible through the same iterator
 *   - Large scans: use max_buffer_size to cap memory consumption
 */

#include "idb.hh"
#include "status.hh"
#include <string>
#include <utility>
#include <vector>
#include <cstdint>

namespace mako {

// @safe - Stateful buffered iterator; all mutation is through named methods
class BufferedIterator : public IIterator {
public:
    // @safe - Constructor borrows table and txn; does NOT take ownership
    explicit BufferedIterator(ITable* table, void* txn, size_t max_buffer_size = 10000)
        : table_(table), txn_(txn), max_buffer_size_(max_buffer_size),
          cursor_(-1), status_(Status::OK()) {}

    // @safe - Position at first key >= target; repopulates buffer
    void Seek(const std::string& target) override {
        populate_forward(target);
    }

    // @safe - Position at first key (empty string precedes all keys in Masstree)
    void SeekToFirst() override {
        populate_forward("");
    }

    // @safe - Position at last key using ReverseScan from sentinel high key
    void SeekToLast() override {
        entries_.clear();
        cursor_ = -1;
        status_ = Status::OK();

        // "\xff\xff\xff\xff\xff" is a sentinel that sorts after all reasonable keys
        const std::string high_sentinel(5, '\xff');
        size_t count = 0;
        status_ = table_->ReverseScan(txn_, high_sentinel, nullptr,
            [&](const std::string& k, const std::string& v) -> bool {
                entries_.emplace_back(k, v);
                ++count;
                if (max_buffer_size_ > 0 && count >= max_buffer_size_) return false;
                return true;
            });

        if (!status_.ok()) {
            entries_.clear();
            cursor_ = -1;
            return;
        }

        // ReverseScan gives descending order; reverse to get ascending buffer
        // then position cursor at the last element (which is the largest key).
        // Alternatively keep descending and let Prev/Next be aware — but the
        // simpler approach is to store ascending and place cursor at end.
        // We reverse so that the buffer is ascending, then set cursor to last entry.
        std::reverse(entries_.begin(), entries_.end());
        cursor_ = entries_.empty() ? -1 : static_cast<int64_t>(entries_.size()) - 1;
    }

    // @safe - Advance cursor; invalidate if past end
    void Next() override {
        if (cursor_ < 0) return;
        ++cursor_;
        if (cursor_ >= static_cast<int64_t>(entries_.size())) {
            cursor_ = -1;
        }
    }

    // @safe - Retreat cursor; invalidate if before start
    void Prev() override {
        if (cursor_ < 0) return;
        --cursor_;
        // cursor_ is int64_t; going below 0 is the invalid sentinel
    }

    // @safe - True iff cursor is within [0, size)
    bool Valid() const override {
        return cursor_ >= 0 && cursor_ < static_cast<int64_t>(entries_.size());
    }

    // @safe - Key at current cursor (caller must ensure Valid())
    std::string key() const override {
        return entries_[static_cast<size_t>(cursor_)].first;
    }

    // @safe - Value at current cursor (caller must ensure Valid())
    std::string value() const override {
        return entries_[static_cast<size_t>(cursor_)].second;
    }

    // @safe - Status of last scan operation
    Status status() const override {
        return status_;
    }

private:
    ITable* table_;          // Borrowed pointer (not owned)
    void* txn_;              // Borrowed transaction handle
    size_t max_buffer_size_; // 0 = unlimited

    std::vector<std::pair<std::string, std::string>> entries_;
    int64_t cursor_;         // -1 means invalid/exhausted
    Status status_;

    // @safe - Populate buffer via forward scan starting at start_key
    void populate_forward(const std::string& start_key) {
        entries_.clear();
        cursor_ = -1;
        status_ = Status::OK();

        size_t count = 0;
        status_ = table_->Scan(txn_, start_key, nullptr,
            [&](const std::string& k, const std::string& v) -> bool {
                entries_.emplace_back(k, v);
                ++count;
                if (max_buffer_size_ > 0 && count >= max_buffer_size_) return false;
                return true;
            });

        if (!status_.ok()) {
            entries_.clear();
            cursor_ = -1;
            return;
        }

        cursor_ = entries_.empty() ? -1 : 0;
    }
};

}  // namespace mako
