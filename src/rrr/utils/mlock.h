module;

#include <pthread.h>

export module rrr:utils.mlock;

export void mlock_init();
export void m_lock(char* name);
export void m_unlock(char* name);
export pthread_mutex_t* m_getlock(char* name);
