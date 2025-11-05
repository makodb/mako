#pragma once

#include <vector>
#include "TaggedLow.hh"
#include "Transaction.hh"
#include "versioned_value.hh"


template <typename T, bool Opacity = false>
class PriorityQueue: public TObject {
    typedef TransactionTid::type Version;
    typedef versioned_value_struct<T> versioned_value;
    
    // Transaction item flags
    static constexpr TransItem::flags_type insert_tag = TransItem::user0_bit;
    static constexpr TransItem::flags_type delete_tag = TransItem::user0_bit<<1;
    static constexpr TransItem::flags_type dirty_tag = TransItem::user0_bit<<2;

    // Version bits for tracking element state
    static constexpr Version insert_bit = TransactionTid::user_bit;
    static constexpr Version delete_bit = TransactionTid::user_bit<<1;
    static constexpr Version dirty_bit = TransactionTid::user_bit<<2;

    // Special transaction keys
    static constexpr int POP_KEY = -2;
    static constexpr int EMPTY_KEY = -3;
    static constexpr int TOP_KEY = -4;
    
    // Constants
    static constexpr int INVALID_THREAD_ID = -1;
    static constexpr T EMPTY_QUEUE_VALUE = -1;
public:
    PriorityQueue() : heap_() {
        heap_size_ = 0;
        pop_lock_ = 0;
        pop_version_ = 0;
        dirty_thread_id_ = INVALID_THREAD_ID;
        dirty_min_value_ = EMPTY_QUEUE_VALUE;
        dirty_operation_count_ = 0;
    }

    // Adds element to the priority queue
    void add(versioned_value* new_element) {
        int child_index = heap_size_;
        if (child_index >= heap_.size()) {
            heap_.push_back(new_element);
        } else {
            heap_[child_index] = new_element;
        }
        heap_size_++;

        bubble_up(child_index);
    }
    
private:
    // Helper to maintain heap property by bubbling element up
    void bubble_up(int child_index) {
        while (child_index > 0) {
            int parent_index = (child_index - 1) / 2;
            if (should_swap_with_parent(child_index, parent_index)) {
                swap(child_index, parent_index);
                child_index = parent_index;
            } else {
                return;
            }
        }
    }
    
    // Helper to determine if child should be swapped with parent
    bool should_swap_with_parent(int child_index, int parent_index) const {
        return heap_[child_index]->read_value() > heap_[parent_index]->read_value();
    }
    
public:
    
    // Removes the maximum element from the heap
    versioned_value* removeMax(versioned_value* expected_value = nullptr) {
        if (heap_size_ <= 0) {
            return nullptr;
        }
        
        versioned_value* max_element = heap_[0];
        
        if (expected_value != nullptr && max_element != expected_value) {
            unlock(&pop_lock_);
            Sto::abort();
            return nullptr;
        }
        
        heap_size_--;
        
        if (heap_size_ == 0) {
            return max_element;
        }
        
        // Move last element to root and restore heap property
        heap_[0] = heap_[heap_size_];
        bubble_down(0);
        
        return max_element;
    }
    
private:
    // Helper to maintain heap property by bubbling element down
    void bubble_down(int parent_index) {
        while (has_children(parent_index)) {
            int larger_child_index = find_larger_child(parent_index);
            if (should_swap_with_child(parent_index, larger_child_index)) {
                swap(parent_index, larger_child_index);
                parent_index = larger_child_index;
            } else {
                break;
            }
        }
    }
    
    // Helper to check if node has children
    bool has_children(int parent_index) const {
        return (2 * parent_index + 1) < heap_size_;
    }
    
    // Helper to find the larger child of a parent node
    int find_larger_child(int parent_index) const {
        int left_child = parent_index * 2 + 1;
        int right_child = parent_index * 2 + 2;
        
        if (right_child >= heap_size_) {
            return left_child; // Only left child exists
        }
        
        return compare_elements(left_child, right_child) > 0 ? left_child : right_child;
    }
    
    // Helper to compare two elements in the heap
    int compare_elements(int index1, int index2) const {
        T value1 = heap_[index1]->read_value();
        T value2 = heap_[index2]->read_value();
        if (value1 > value2) return 1;
        if (value1 < value2) return -1;
        return 0;
    }
    
    // Helper to determine if parent should be swapped with child
    bool should_swap_with_child(int parent_index, int child_index) const {
        return heap_[child_index]->read_value() > heap_[parent_index]->read_value();
    }
    
public:
    
    versioned_value* getMax() {
        assert(TransactionTid::is_locked_here(pop_lock_));
        if (heap_size_ == 0) {
            return nullptr;
        }
        
        while (true) {
            versioned_value* max_candidate = heap_[0];
            auto transaction_item = Sto::item(this, max_candidate);
            
            if (is_inserted(max_candidate->version())) {
                return handle_inserted_max_candidate(transaction_item, max_candidate);
            } else if (is_deleted(max_candidate->version())) {
                removeMax(max_candidate);
                if (heap_size_ == 0) return nullptr;
            } else {
                return max_candidate;
            }
        }
    }
    
private:
    // Helper to handle max candidate that was inserted by a transaction
    versioned_value* handle_inserted_max_candidate(TransProxy& transaction_item, versioned_value* max_candidate) {
        if (has_insert(transaction_item)) {
            // Current transaction inserted this element (push then pop)
            return max_candidate;
        } else {
            // Another transaction is inserting a high priority element
            unlock(&pop_lock_);
            Sto::abort();
            return nullptr;
        }
    }
    
public:
    
    void push_nontrans(T value) {
        lock(&pop_lock_);
        versioned_value* new_element = versioned_value::make(value, TransactionTid::increment_value + insert_bit);
        add(new_element);
        unlock(&pop_lock_);
    }
    
    void push(T value) {
        lock(&pop_lock_); // Performance note: locking improves performance, could use readers-writers lock
        
        if (conflicts_with_dirty_state(value)) {
            unlock(&pop_lock_);
            Sto::abort();
            return;
        }
        
        versioned_value* new_element = versioned_value::make(value, TransactionTid::increment_value + insert_bit);
        add(new_element);
        Sto::item(this, new_element).add_write(value).add_flags(insert_tag);
        unlock(&pop_lock_);
    }
    
private:
    // Helper to check if push conflicts with dirty queue state
    bool conflicts_with_dirty_state(T value) const {
        return dirty_thread_id_ != INVALID_THREAD_ID && 
               dirty_thread_id_ != TThread::id() && 
               value > dirty_min_value_;
    }
    
public:
    
    T pop() {
        auto previous_reads = get_previous_transaction_reads();
        
        if (heap_size_ == 0) {
            return handle_empty_queue_pop(previous_reads.previously_read_value);
        }
        
        lock(&pop_lock_);
        if (is_queue_dirty_by_other_transaction()) {
            unlock(&pop_lock_);
            Sto::abort();
            return EMPTY_QUEUE_VALUE;
        }
        
        versioned_value* max_element = getMax();
        if (!validate_pop_consistency(max_element, previous_reads)) {
            unlock(&pop_lock_);
            Sto::abort();
            return EMPTY_QUEUE_VALUE;
        }
        
        if (max_element == nullptr) {
            return handle_empty_queue_after_lock();
        }
        
        update_dirty_state(max_element);
        removeMax(max_element);
        unlock(&pop_lock_);
        
        mark_element_for_deletion(max_element);
        Sto::item(this, POP_KEY).add_write(0);
        return max_element->read_value();
    }
    
private:
    // Structure to hold previous transaction reads
    struct PreviousReads {
        versioned_value* previously_read_value;
        bool previously_read_empty;
        TransProxy* top_item;
    };
    
    // Helper to get previous transaction reads
    PreviousReads get_previous_transaction_reads() {
        PreviousReads reads;
        reads.top_item = Sto::check_item(this, TOP_KEY);
        reads.previously_read_value = nullptr;
        
        if (reads.top_item != nullptr && reads.top_item->has_read()) {
            reads.previously_read_value = reads.top_item->template read_value<versioned_value*>();
        }
        
        auto empty_item = Sto::check_item(this, EMPTY_KEY);
        reads.previously_read_empty = empty_item != nullptr && empty_item->has_read();
        
        return reads;
    }
    
    // Helper to handle pop from empty queue
    T handle_empty_queue_pop(versioned_value* previously_read_value) {
        if (previously_read_value != nullptr) {
            Sto::abort();
        }
        Sto::item(this, EMPTY_KEY).add_read(0);
        Sto::item(this, POP_KEY).add_read(TransactionTid::unlocked(pop_version_));
        return EMPTY_QUEUE_VALUE;
    }
    
    // Helper to check if queue is dirty by another transaction
    bool is_queue_dirty_by_other_transaction() const {
        return dirty_thread_id_ != INVALID_THREAD_ID && dirty_thread_id_ != TThread::id();
    }
    
    // Helper to validate pop consistency with previous reads
    bool validate_pop_consistency(versioned_value* max_element, const PreviousReads& previous_reads) {
        bool should_be_inserted = false;
        
        if (previous_reads.previously_read_empty && max_element != nullptr) {
            should_be_inserted = true;
        }
        
        if (previous_reads.previously_read_value != nullptr) {
            if (previous_reads.previously_read_value->read_value() == max_element->read_value()) {
                // Note: Comparing values rather than versioned_values for consistency
                previous_reads.top_item->remove_read();
            } else {
                should_be_inserted = true;
            }
        }
        
        if (should_be_inserted) {
            auto transaction_item = Sto::item(this, max_element);
            return has_insert(transaction_item);
        }
        
        return true;
    }
    
    // Helper to handle empty queue after acquiring lock
    T handle_empty_queue_after_lock() {
        Sto::item(this, EMPTY_KEY).add_read(0);
        Sto::item(this, POP_KEY).add_read(TransactionTid::unlocked(pop_version_));
        unlock(&pop_lock_);
        return EMPTY_QUEUE_VALUE;
    }
    
    // Helper to update dirty state when popping
    void update_dirty_state(versioned_value* max_element) {
        T element_value = max_element->read_value();
        if (dirty_thread_id_ == INVALID_THREAD_ID || element_value < dirty_min_value_) {
            dirty_min_value_ = element_value;
            fence();
        }
        dirty_thread_id_ = TThread::id();
    }
    
    // Helper to mark element for deletion
    void mark_element_for_deletion(versioned_value* element) {
        auto transaction_item = Sto::item(this, element);
        if (has_insert(transaction_item)) {
            transaction_item.add_flags(delete_tag);
        } else {
            transaction_item.add_write(0).add_flags(delete_tag);
            dirty_operation_count_++;
        }
    }
    
public:
    
    T top() {
        if (heap_size_ == 0) {
            Sto::item(this, EMPTY_KEY).add_read(0);
            return EMPTY_QUEUE_VALUE;
        }
        
        Sto::item(this, POP_KEY).add_read(TransactionTid::unlocked(pop_version_));
        acquire_fence();
        
        if (heap_size_ == 0) {
            Sto::item(this, EMPTY_KEY).add_read(0);
            return EMPTY_QUEUE_VALUE;
        }
        
        lock(&pop_lock_);
        if (is_queue_dirty_by_other_transaction()) {
            unlock(&pop_lock_);
            Sto::abort();
        }
        
        versioned_value* max_element = getMax();
        unlock(&pop_lock_);
        
        if (max_element == nullptr) {
            Sto::item(this, EMPTY_KEY).add_read(0);
            return EMPTY_QUEUE_VALUE;
        }
        
        T top_value = max_element->read_value();
        Sto::item(this, max_element).add_read(max_element->version());
        Sto::item(this, TOP_KEY).add_read(max_element);
        return top_value;
    }
    
    int unsafe_size() {
        return heap_size_; // Note: this is not transactional yet
    }
    
    void lock(versioned_value* element) {
        lock(&element->version());
    }
    
    void unlock(versioned_value* element) {
        unlock(&element->version());
    }
    
    bool lock(TransItem& item, Transaction& transaction) override {
        return item.key<int>() != POP_KEY
            || transaction.try_lock(item, pop_version_);
    }
    
    bool check(TransItem& item, Transaction&) override {
        if (item.key<int>() == top_key) { return true; }
        else if (item.key<int>() == empty_key) {
            // check that no other transaction  pushed items onto the queue
            for (int i = 0; i < size_; i++) {
                versioned_value* val = heap_[i];
                if (!is_inserted(val->version())
                    || TransactionTid::is_locked_elsewhere(val->version()))
                    return false;
            }
            
            if (dirtytid_ != -1 && dirtytid_ != TThread::id()) return false;
            return true;
        }
        else if (item.key<int>() == pop_key) {
            return TransactionTid::check_version(popversion_, item.template read_value<Version>());
        } else {
            // This is top case
            auto e = item.key<versioned_value*>();
            if (dirtytid_ != -1 && dirtytid_ != TThread::id() && dirtyval_ >= e->read_value()) return false;
            else if (has_delete(item)) return true;
            // check that e is not pushed down by other transactions
            int level = 1; // level that contains the root
            bool found = false;
            for (int i = 0; i < size_; i++) {
                versioned_value* val = heap_[i];
                if (val == e || val->read_value() == e->read_value()) found = true; 
                else if (val->read_value() > e->read_value()) {
                    auto it = Sto::check_item(this, val);
                    if (it != NULL && has_insert(*it)) {
                        level = findLevel(i) + 1;
                        continue;
                    } else {
                        return false;
                    }
                }
                if (i == endOfLevel(level)) break;
            }
            if (dirtytid_ != -1 && dirtytid_ != TThread::id() && dirtyval_ >= e->read_value()) return false;
            if (!found) return false;
            else return true;
        }
    }
    
    
    void install(TransItem& item, Transaction& transaction) override {
        if (item.key<int>() == POP_KEY){
            if (Opacity) {
                TransactionTid::set_version(pop_version_, transaction.commit_tid());
            } else {
                TransactionTid::inc_nonopaque_version(pop_version_);
            }
        } else {
            auto element = item.key<versioned_value*>();
            if (has_insert(item)) {
                erase_inserted(&element->version());
            }
        }
    }
    
    void unlock(TransItem& item) override {
        if (item.key<int>() == POP_KEY)
            unlock(&pop_version_);
    }

    void cleanup(TransItem& item, bool committed) override {
        if (committed && dirty_thread_id_ == TThread::id()) {
            dirty_thread_id_ = INVALID_THREAD_ID;
        }
        
        if (!committed) {
            handle_transaction_abort(item);
        }
    }
    
private:
    // Helper to handle transaction abort cleanup
    void handle_transaction_abort(TransItem& item) {
        if (has_insert(item) && has_delete(item)) {
            // Insert then delete in same transaction - no cleanup needed
            return;
        }
        
        if (has_insert(item)) {
            cleanup_aborted_insert(item);
        } else if (has_delete(item)) {
            cleanup_aborted_delete(item);
        }
    }
    
    // Helper to cleanup aborted insert operation
    void cleanup_aborted_insert(TransItem& item) {
        auto element = item.key<versioned_value*>();
        mark_deleted(&element->version());
        fence();
        erase_inserted(&element->version());
    }
    
    // Helper to cleanup aborted delete operation
    void cleanup_aborted_delete(TransItem& item) {
        auto element = item.key<versioned_value*>();
        auto element_value = element->read_value();
        versioned_value* restored_element = versioned_value::make(element_value, TransactionTid::increment_value);
        
        lock(&pop_lock_);
        add(restored_element);
        unlock(&pop_lock_);
        fence();
        
        dirty_operation_count_--;
        if (dirty_operation_count_ == 0) {
            assert(dirty_thread_id_ == TThread::id());
            dirty_thread_id_ = INVALID_THREAD_ID;
        }
    }
    
public:
    
    // Used for debugging
    void print() {
        for (int index = 0; index < heap_size_; index++) {
            versioned_value* element = heap_[index];
            bool is_normal_state = !is_inserted(element->version()) && !is_deleted(element->version());
            std::cout << element->read_value() << "[" << is_normal_state << "] ";
        }
        std::cout << std::endl;
    }
    
    
private:
    static void lock(Version *v) {
        TransactionTid::lock(*v);
    }
    
    static void unlock(Version *v) {
        TransactionTid::unlock(*v);
    }
    
    static bool has_insert(const TransItem& item) {
        return item.flags() & insert_tag;
    }
    static bool has_delete(const TransItem& item) {
        return item.flags() & delete_tag;
    }
    
    static bool has_dirty(const TransItem& item) {
        return item.flags() & dirty_tag;
    }
    
    static bool is_inserted(Version v) {
        return v & insert_bit;
    }
    
    static void erase_inserted(Version* v) {
        *v = *v & (~insert_bit);
    }
    
    static void mark_inserted(Version* v) {
        *v = *v | insert_bit;
    }
    
    static bool is_dirty(Version v) {
        return v & dirty_bit;
    }
    
    static void erase_dirty(Version* v) {
        assert(is_dirty(*v));
        *v = *v & (~dirty_bit);
    }
    
    static void mark_dirty(Version* v) {
        assert(!is_dirty(*v));
        *v = *v | dirty_bit;
    }
            
    static bool is_deleted(Version v) {
        return v & delete_bit;
    }
            
    static void erase_deleted(Version* v) {
        *v = *v & (~delete_bit);
    }
            
    static void mark_deleted(Version* v) {
        *v = *v | delete_bit;
    }
    
    static int findLevel(int i) {
        return ceil(log((double) (i+2)) / log(2.0));
    }
    
    static int endOfLevel(int l) {
        assert(l >= 1);
        return (1 << l) - 2;
    }


    void swap(int index1, int index2) {
        versioned_value* temp = heap_[index1];
        heap_[index1] = heap_[index2];
        heap_[index2] = temp;
    }
    
    std::vector<versioned_value*> heap_;
    Version pop_lock_;
    Version pop_version_;
    int heap_size_;
    int dirty_min_value_; // min value popped by a transaction that dirtied the queue
    int dirty_thread_id_; // thread id of the transaction that dirtied the queue
    int dirty_operation_count_; // number of pops by the transaction that dirtied the queue
    
    
};
