#ifdef CONFIG_H
#include CONFIG_H
#endif
#include "masstree/config.h"

#include "MassTrans.hh"

volatile bool recovering = false;
volatile uint64_t globalepoch = 1;
