#include "scheduler.h"

// TxLogServer::~TxLogServer() used to be defined here. The interface is now a
// DSL `pub trait`, and the emitter gives it an inline virtual destructor in the
// header, so an out-of-line definition would be a second one.
