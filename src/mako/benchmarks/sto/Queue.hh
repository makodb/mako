#pragma once

#include <list>
#include "TaggedLow.hh"
#include "Transaction.hh"
#include "TWrapped.hh"

template <typename T, unsigned BUF_SIZE = 1000000,
          template <typename> class W = TOpaqueWrapped>
class Queue: public TObject {
public:
    typedef typename W<T>::version_type version_type;

    Queue() : head_index_(0), tail_index_(0), tail_version_(0), head_version_(0) {}

    // Transaction item keys
    static constexpr int PUSH_KEY = -1;
    static constexpr int HEAD_LOCK_KEY = -2;
    
    // Transaction item flags
    static constexpr TransItem::flags_type delete_bit = TransItem::user0_bit;
    static constexpr TransItem::flags_type read_writes = TransItem::user0_bit<<1;
    static constexpr TransItem::flags_type list_bit = TransItem::user0_bit<<2;
    static constexpr TransItem::flags_type empty_bit = TransItem::user0_bit<<3;

    // NONTRANSACTIONAL PUSH/POP/EMPTY
    void nontrans_push(const T& value) {
        queue_slots_[tail_index_] = value;
        tail_index_ = (tail_index_ + 1) % BUF_SIZE;
        assert(head_index_ != tail_index_);
    }
    
    T nontrans_pop() {
        assert(head_index_ != tail_index_);
        T value = queue_slots_[head_index_];
        head_index_ = (head_index_ + 1) % BUF_SIZE;
        return value;
    }

    bool nontrans_empty() const {
        return head_index_ == tail_index_;
    }

    template <typename RandomGen>
    void nontrans_shuffle(RandomGen generator) {
        auto head_ptr = &queue_slots_[head_index_];
        auto tail_ptr = &queue_slots_[tail_index_];
        // don't support wrap-around shuffle
        assert(head_ptr < tail_ptr);
        std::shuffle(head_ptr, tail_ptr, generator);
    }

    void nontrans_clear() {
        while (!nontrans_empty())
            nontrans_pop();
    }

    // TRANSACTIONAL CALLS
    void transPush(const T& value) {
        auto push_item = Sto::item(this, PUSH_KEY);
        if (push_item.has_write()) {
            handle_additional_push(push_item, value);
        } else {
            push_item.add_write(value);
        }
    }
    
private:
    // Helper to handle additional pushes when there are already pending writes
    void handle_additional_push(TransProxy& push_item, const T& value) {
        if (!is_list(push_item)) {
            convert_single_write_to_list(push_item, value);
        } else {
            add_to_existing_write_list(push_item, value);
        }
    }
    
    // Helper to convert single write to list when adding second element
    void convert_single_write_to_list(TransProxy& push_item, const T& value) {
        auto& existing_value = push_item.template write_value<T>();
        std::list<T> write_list;
        
        if (!is_empty(push_item)) {
            write_list.push_back(existing_value);
            push_item.clear_flags(empty_bit);
        }
        write_list.push_back(value);
        
        push_item.clear_write();
        push_item.add_write(write_list);
        push_item.add_flags(list_bit);
    }
    
    // Helper to add value to existing write list
    void add_to_existing_write_list(TransProxy& push_item, const T& value) {
        auto& write_list = push_item.template write_value<std::list<T>>();
        write_list.push_back(value);
    }
    
public:

    bool transPop() {
        version_type head_version = head_version_;
        fence();
        unsigned current_index = head_index_;
        
        auto queue_item = find_next_available_item(current_index);
        if (!queue_item.first) {
            return try_pop_from_pending_pushes(head_version);
        }
        
        // Lock head version and mark item for deletion
        lock_head_version(head_version);
        queue_item.second.add_flags(delete_bit);
        queue_item.second.add_write(0);
        return true;
    }
    
private:
    // Helper to find the next available (non-deleted) queue item
    std::pair<bool, TransProxy> find_next_available_item(unsigned& current_index) {
        while (current_index != tail_index_) {
            auto item = Sto::item(this, current_index);
            if (!has_delete(item)) {
                return std::make_pair(true, item);
            }
            current_index = (current_index + 1) % BUF_SIZE;
        }
        return std::make_pair(false, TransProxy{});
    }
    
    // Helper to try popping from pending push operations when queue appears empty
    bool try_pop_from_pending_pushes(version_type head_version) {
        version_type tail_version = tail_version_;
        fence();
        
        // Double-check if queue is still empty after fence
        if (head_index_ != tail_index_) {
            return false; // Queue is no longer empty, retry
        }
        
        auto push_item = Sto::item(this, PUSH_KEY);
        if (!push_item.has_read()) {
            push_item.observe(tail_version);
        }
        
        return pop_from_write_list(push_item);
    }
    
    // Helper to pop from pending write list
    bool pop_from_write_list(TransProxy& push_item) {
        if (!push_item.has_write()) {
            return false; // No pending pushes
        }
        
        if (is_list(push_item)) {
            return pop_from_pending_list(push_item);
        } else {
            return pop_from_single_pending_write(push_item);
        }
    }
    
    // Helper to pop from a list of pending writes
    bool pop_from_pending_list(TransProxy& push_item) {
        auto& write_list = push_item.template write_value<std::list<T>>();
        if (!write_list.empty()) {
            write_list.pop_front();
            // Mark that we read from the write list
            auto dummy_item = Sto::item(this, head_index_);
            dummy_item.add_flags(read_writes);
            return true;
        }
        return false;
    }
    
    // Helper to pop from a single pending write
    bool pop_from_single_pending_write(TransProxy& push_item) {
        if (!is_empty(push_item)) {
            push_item.add_flags(empty_bit);
            return true;
        }
        return false;
    }
    
    // Helper to lock head version for consistency
    void lock_head_version(version_type head_version) {
        auto lock_item = Sto::item(this, HEAD_LOCK_KEY);
        if (!lock_item.has_read()) {
            lock_item.observe(head_version);
        }
        lock_item.add_write(0);
    }
    
public:

    bool transFront(T& result_value) {
        version_type head_version = head_version_;
        fence();
        unsigned current_index = head_index_;
        
        auto queue_item = find_next_available_item(current_index);
        if (!queue_item.first) {
            return try_read_from_pending_pushes(result_value, head_version);
        }
        
        // Lock head version and read from queue
        lock_head_version(head_version);
        result_value = queue_slots_[current_index];
        return true;
    }
    
private:
    // Helper to try reading front value from pending push operations
    bool try_read_from_pending_pushes(T& result_value, version_type head_version) {
        version_type tail_version = tail_version_;
        fence();
        
        // Double-check if queue is still empty after fence
        if (head_index_ != tail_index_) {
            return false; // Queue is no longer empty, retry
        }
        
        auto push_item = Sto::item(this, PUSH_KEY);
        if (!push_item.has_read()) {
            push_item.observe(tail_version);
        }
        
        return read_front_from_write_list(push_item, result_value);
    }
    
    // Helper to read front value from pending write list
    bool read_front_from_write_list(TransProxy& push_item, T& result_value) {
        if (!push_item.has_write()) {
            return false; // No pending pushes
        }
        
        if (is_list(push_item)) {
            return read_front_from_pending_list(push_item, result_value);
        } else {
            return read_front_from_single_pending_write(push_item, result_value);
        }
    }
    
    // Helper to read front value from a list of pending writes
    bool read_front_from_pending_list(TransProxy& push_item, T& result_value) {
        auto& write_list = push_item.template write_value<std::list<T>>();
        if (!write_list.empty()) {
            result_value = write_list.front();
            return true;
        }
        return false;
    }
    
    // Helper to read front value from a single pending write
    bool read_front_from_single_pending_write(TransProxy& push_item, T& result_value) {
        if (!is_empty(push_item)) {
            auto& value = push_item.template write_value<T>();
            result_value = value;
            return true;
        }
        return false;
    }
    
public:
    
    // Helper methods for transaction item flag checking
    bool has_delete(const TransItem& item) const {
        return item.flags() & delete_bit;
    }
    
    bool is_read_write(const TransItem& item) const {
        return item.flags() & read_writes;
    }
 
    bool is_list(const TransItem& item) const {
        return item.flags() & list_bit;
    }
 
    bool is_empty(const TransItem& item) const {
        return item.flags() & empty_bit;
    }

    bool lock(TransItem& item, Transaction& transaction) override {
        int item_key = item.key<int>();
        if (item_key == PUSH_KEY)
            return transaction.try_lock(item, tail_version_);
        else if (item_key == HEAD_LOCK_KEY)
            return transaction.try_lock(item, head_version_);
        else
            return true;
    }

    bool check(TransItem& item, Transaction& transaction) override {
        (void) transaction;
        int item_key = item.key<int>();
        
        // check if was a pop or front operation
        if (item_key == HEAD_LOCK_KEY)
            return item.check_version(head_version_);
        // check if we read off the write_list (and locked tail_version_)
        else if (item_key == PUSH_KEY)
            return item.check_version(tail_version_);
        
        // shouldn't reach this for queue operations
        assert(false);
        return false;
    }

    void install(TransItem& item, Transaction& transaction) override {
        int item_key = item.key<int>();
        
        // ignore head lock marker item
        if (item_key == HEAD_LOCK_KEY)
            return;
            
        // install pop operations
        if (has_delete(item)) {
            install_pop_operation(item, transaction);
        }
        // install push operations
        else if (item_key == PUSH_KEY) {
            install_push_operation(item, transaction);
        }
    }
    
private:
    // Helper to install pop operations
    void install_pop_operation(TransItem& item, Transaction& transaction) {
        // only increment head if item popped from actual queue (not from write list)
        if (!is_read_write(item)) {
            head_index_ = (head_index_ + 1) % BUF_SIZE;
        }
        head_version_.set_version(transaction.commit_tid());
    }
    
    // Helper to install push operations
    void install_push_operation(TransItem& item, Transaction& transaction) {
        unsigned saved_head_index = head_index_;
        
        if (is_list(item)) {
            install_push_list(item, saved_head_index);
        } else if (!is_empty(item)) {
            install_single_push(item);
        }
        
        tail_version_.set_version(transaction.commit_tid());
    }
    
    // Helper to install multiple pushes from a list
    void install_push_list(TransItem& item, unsigned saved_head_index) {
        auto& write_list = item.template write_value<std::list<T>>();
        while (!write_list.empty()) {
            // assert queue is not out of space
            assert(tail_index_ != (saved_head_index - 1) % BUF_SIZE);
            queue_slots_[tail_index_] = write_list.front();
            write_list.pop_front();
            tail_index_ = (tail_index_ + 1) % BUF_SIZE;
        }
    }
    
    // Helper to install a single push
    void install_single_push(TransItem& item) {
        auto& value = item.template write_value<T>();
        queue_slots_[tail_index_] = value;
        tail_index_ = (tail_index_ + 1) % BUF_SIZE;
    }
    
public:
    
    void unlock(TransItem& item) override {
        int item_key = item.key<int>();
        if (item_key == PUSH_KEY)
            tail_version_.unlock();
        else if (item_key == HEAD_LOCK_KEY)
            head_version_.unlock();
    }

private:
    T queue_slots_[BUF_SIZE];
    unsigned head_index_;
    unsigned tail_index_;
    version_type tail_version_;
    version_type head_version_;
};
