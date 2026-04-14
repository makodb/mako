#include "replicated_db.h"

using namespace janus;

// @unsafe - Static registration with Marshallable factory
static int volatile x_replicated_db =
    MarshallDeputy::reg_initializer(MarshallDeputy::CMD_REPLICATED_DB,
                                     []() -> Marshallable* {
                                       return new ReplicatedDBCommand();
                                     });

// @unsafe - Creates shared_ptr (non-borrow-checked ownership)
shared_ptr<ReplicatedDBCommand> ReplicatedDBCommand::CreatePut(
    const std::string& key, const std::string& value) {
  auto cmd = std::make_shared<ReplicatedDBCommand>();
  cmd->op_ = ReplicatedDBOp::PUT;
  cmd->key_ = key;
  cmd->value_ = value;
  return cmd;
}

// @unsafe - Creates shared_ptr (non-borrow-checked ownership)
shared_ptr<ReplicatedDBCommand> ReplicatedDBCommand::CreateDelete(
    const std::string& key) {
  auto cmd = std::make_shared<ReplicatedDBCommand>();
  cmd->op_ = ReplicatedDBOp::DELETE;
  cmd->key_ = key;
  cmd->value_ = "";
  return cmd;
}

// @unsafe - Creates shared_ptr (non-borrow-checked ownership)
shared_ptr<ReplicatedDBCommand> ReplicatedDBCommand::CreateBatch(
    const std::vector<KVOperation>& ops) {
  auto cmd = std::make_shared<ReplicatedDBCommand>();
  cmd->op_ = ReplicatedDBOp::BATCH;
  cmd->batch_ops_ = ops;
  return cmd;
}

// @unsafe - Marshal I/O is not borrow-checked
Marshal& ReplicatedDBCommand::to_marshal(Marshal& m) const {
  m << static_cast<uint8_t>(op_);
  m << key_;
  m << value_;
  if (op_ == ReplicatedDBOp::BATCH) {
    uint32_t count = static_cast<uint32_t>(batch_ops_.size());
    m << count;
    for (const auto& op : batch_ops_) {
      m << static_cast<uint8_t>(op.op);
      m << op.key;
      m << op.value;
    }
  }
  return m;
}

// @unsafe - Marshal I/O is not borrow-checked
Marshal& ReplicatedDBCommand::from_marshal(Marshal& m) {
  uint8_t op_val;
  m >> op_val;
  op_ = static_cast<ReplicatedDBOp>(op_val);
  m >> key_;
  m >> value_;
  if (op_ == ReplicatedDBOp::BATCH) {
    uint32_t count;
    m >> count;
    batch_ops_.resize(count);
    for (uint32_t i = 0; i < count; i++) {
      uint8_t sub_op_val;
      m >> sub_op_val;
      batch_ops_[i].op = static_cast<ReplicatedDBOp>(sub_op_val);
      m >> batch_ops_[i].key;
      m >> batch_ops_[i].value;
    }
  }
  return m;
}
