#pragma once
#include <atomic>
#include <stdint.h>
#include <assert.h>
#include <atomic>
#include <iostream>
#include <stdexcept>

#include "config.h"
#include "runtime/thread_properties.hh"
#include "runtime/thread_registration.hh"
namespace mako { class ShardClient; }


// in Interface.hh, threadid_mask = type(0x1FF), range [0,511]
#ifndef MAX_THREADS
#define MAX_THREADS 460
#endif


class TThread {
    static __thread int nshards;  // shards
    static __thread int warehouses;  // warehouses per shard
    static __thread int shard_index;  // current shard_index
    static __thread int the_id; // thread-id
    static __thread int pid; // partition-id
    // MODE: 0 => default, 1 => no checking in_process
    static __thread int the_mode;
    // ROLE: 1 => disable multi-version, 0 => enable multi-version
    static __thread int the_role;
    // Bounded cadence counter for multi-version reclamation.
    static __thread uint32_t the_counter;
    // number of rpc servers
    static __thread int the_num_rpc_server;
    // if run micro-based benchmark
    static __thread int the_is_micro;

public:
    // number of transactions to skip before the next remote transactions
    static __thread int skipBeforeRemoteNewOrder;
    static __thread bool isHomeWarehouse;
    static __thread bool isRemoteShard;
    static __thread int skipBeforeRemotePayment;
    // for each worker thread, it has an rpc client to issue rpc requests
    static __thread mako::ShardClient* sclient;
    // for each worker thread, we set a global variable to ease programming
    // this structure is relatively slow, please avoid visiting it frequently
    static __thread HashWrapper* tprops;
    // indicate which shards the write-/write-set of the current transaction cover
    static __thread unsigned int readset_shard_bits;
    static __thread unsigned int writeset_shard_bits;
    // optimization: Work with abort_without_throw
    // for get: if found a key, but updated by other writes, then abort this transcation
    //   not through stack unwinding (try-exception). Instead using global variable to
    //   control this.
    // update back: can't miss any get operation in tpcc.cc
    static __thread bool transget_without_throw;
    static __thread bool transget_without_stable;
    static __thread unsigned int trans_nosend_abort;
    static __thread bool in_loading_phase;
    // Debugging bit
    static __thread int the_debug_bit;
    // it is just for the transaction counter
    static __thread int increment_id;
    static __thread bool is_worker_leader;

    static void set_mode(int mode) {
        the_mode = mode;
    }

    static int get_num_rpc_server() {
        if (the_num_rpc_server)
            return the_num_rpc_server;
        else{
            return 2;
        }
    }

    static bool isLeader(){
        return is_worker_leader;
    }

    static void set_num_rpc_server(int nn) { the_num_rpc_server = nn; }

    static void set_is_micro(int is_micro) { the_is_micro = is_micro; }

    static int get_is_micro() { return the_is_micro; }

    static int mode() {
        return the_mode;
    }

    static bool should_reclaim() {
        if (the_counter == 49) {
            the_counter = 0;
            return true;
        }
        ++the_counter;
        return false;
    }

    static void enable_multiverison(){
        the_role = 0;
    }

    static void disable_multiversion(){
        the_role = 1;
    }

    static bool is_multiversion(){
        return the_role == 0;
    }

    static void set_shard_index(int ss) {
        shard_index = ss;
    }

    static int get_shard_index() {
        return shard_index;
    }

    static void set_nshards(int ss) {
        nshards = ss;
    }

    static int get_nshards() {
        return nshards;
    }

    static void set_warehouses(int ss) {
        warehouses = ss;
    }

    static int get_warehouses() {
        return warehouses;
    }

    static int id() {
        return the_id;
    }

    // Assign one process-unique native runtime/RCU slot to this OS thread and retain it
    // across detach/reattach. Slots are deliberately not recycled across
    // threads because their deferred-RCU lists are process-lived.
    static int assign_stable_id() {
        const int assigned_stable_id = mako::silo::try_allocate_thread_id();
        if (assigned_stable_id < 0 || assigned_stable_id >= MAX_THREADS) {
            throw std::runtime_error("native worker thread-ID budget exhausted");
        }
        set_id(assigned_stable_id);
        return assigned_stable_id;
    }

    // Returns absolute partition ID for Paxos routing
    // In multi-shard mode, shard 0 uses partitions 0-(warehouses-1),
    // shard 1 uses partitions warehouses-(2*warehouses-1), etc.
    // Returns global partition ID across all shards (shard_index * warehouses + pid)
    // Use this for global uniqueness (e.g., shardClientAll keys, unique order IDs)
    static int getGlobalPartitionID(){
        return shard_index * warehouses + pid;
    }

    // Returns local partition ID within this shard (0 to warehouses-1)
    // Use this for Paxos workers and local storage operations (RocksDB persistence)
    static int getLocalPartitionID(){
        return pid;
    }

    static void set_id(int id) {
        assert(id >= 0 && id < MAX_THREADS);
        the_id = id;
    }

    static void set_pid(int id) {
        pid = id;
    }

    static void set_tprops(std::string k, int v) {
        tprops->set_tprops(k, v);
    }

    static int get_tprops(std::string k) {
        return tprops->get_tprops(k);
    }
};
