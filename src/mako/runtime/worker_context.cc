// Shared application worker state; transaction state belongs to Rust.
#include "runtime/worker_context.hh"

__thread int TThread::the_id;
__thread int TThread::assigned_stable_id = -1;
std::atomic<int> TThread::next_stable_id{0};
__thread int TThread::nshards;
__thread int TThread::shard_index;
__thread int TThread::pid;
__thread int TThread::the_mode;
__thread int TThread::the_num_rpc_server;
__thread int TThread::the_is_micro;
__thread uint32_t TThread::the_counter;
__thread int TThread::the_role;
__thread int TThread::warehouses;
__thread int TThread::the_debug_bit;
__thread bool TThread::transget_without_throw;
__thread bool TThread::transget_without_stable;
__thread bool TThread::is_worker_leader;
__thread unsigned int TThread::trans_nosend_abort;
__thread bool TThread::in_loading_phase;
__thread int TThread::increment_id;
__thread int TThread::skipBeforeRemoteNewOrder;
__thread bool TThread::isHomeWarehouse;
__thread bool TThread::isRemoteShard;
__thread int TThread::skipBeforeRemotePayment;
__thread unsigned int TThread::readset_shard_bits;
__thread unsigned int TThread::writeset_shard_bits;
__thread mako::ShardClient *TThread::sclient = nullptr;
__thread HashWrapper *TThread::tprops = nullptr;
