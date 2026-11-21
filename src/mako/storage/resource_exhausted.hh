#ifndef MAKO_STORAGE_RESOURCE_EXHAUSTED_HH
#define MAKO_STORAGE_RESOURCE_EXHAUSTED_HH

#include <stdexcept>

// Capacity errors end the current attempt and require application policy.
// They must not enter the transaction-conflict retry loop.
class storage_resource_exhausted : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

#endif
