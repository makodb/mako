#include "MassTrans.hh"

// Global recovery state flag - used by Masstree for crash recovery coordination
volatile bool recovering = false;

// Global epoch counter for Masstree RCU - incremented by Transaction::epoch_advance_callback
volatile uint64_t globalepoch = 1;
