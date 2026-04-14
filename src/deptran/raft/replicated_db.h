#pragma once
#include "__dep__.h"
#include "rrr/misc/marshal.hpp"
#include <string>
#include <vector>

namespace janus {

// @safe - Trivially copyable enum for operation type
enum class ReplicatedDBOp : uint8_t {
    PUT = 1,
    DELETE = 2,
    BATCH = 3
};

// @safe - Plain data struct for batch operations
struct KVOperation {
    ReplicatedDBOp op;
    std::string key;
    std::string value;  // empty for DELETE
};

// @unsafe - Inherits from non-borrow-checked Marshallable
class ReplicatedDBCommand : public Marshallable {
public:
    ReplicatedDBOp op_ = ReplicatedDBOp::PUT;
    std::string key_;
    std::string value_;
    std::vector<KVOperation> batch_ops_;  // Only used when op_ == BATCH

    // @unsafe - Calls Marshallable constructor (non-borrow-checked)
    ReplicatedDBCommand() : Marshallable(MarshallDeputy::CMD_REPLICATED_DB) {}

    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static shared_ptr<ReplicatedDBCommand> CreatePut(const std::string& key, const std::string& value);
    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static shared_ptr<ReplicatedDBCommand> CreateDelete(const std::string& key);
    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static shared_ptr<ReplicatedDBCommand> CreateBatch(const std::vector<KVOperation>& ops);

    // @unsafe - Marshallable interface (non-borrow-checked I/O)
    Marshal& to_marshal(Marshal& m) const override;
    // @unsafe - Marshallable interface (non-borrow-checked I/O)
    Marshal& from_marshal(Marshal& m) override;
};

} // namespace janus
