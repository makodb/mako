#include "__dep__.h"
#include "marshal-value.h"
#include "command.h"
#include "procedure.h"
#include "command_marshaler.h"
#include "procedure.h"

namespace janus {

// removed `CmdData::to_marshal` /
// `CmdData::from_marshal` definitions. Both were declared as virtual
// overrides of `Marshallable::to_marshal` / `from_marshal` but had no
// production callers — `CmdData` is never registered with
// `MarshallDeputy::reg_initializer` and never instantiated directly,
// so the only way they could fire was via virtual dispatch on a
// subclass instance.  `TxData` removed its own override pair in
// 1; `SimpleCommand` uses the free `operator<<(Marshal&,
// const SimpleCommand&)` / `operator<<(BinaryWriteArchive&, ...)`
// overloads below, never the inherited virtuals.  Removing the
// overrides means accidental virtual-dispatch calls hit the base
// `verify(0)` defaults and abort, which surfaces silent partial-write
// bugs (the legacy `CmdData::to_marshal` only emitted the 8 base
// fields, dropping any subclass-specific tail) as hard failures.

// Marshal-deprecation slice A: the Marshal-form SimpleCommand serde is
// deleted (zero callers — every wire path is archive-form below).
// archive operators for SimpleCommand.
// Wire format byte-for-byte identical to the Marshal-based pair
// above: the 8 inherited CmdData fields (id, type, inn_id, root_id,
// root_type, client_id, cmd_id_in_client, rule_mode flag), followed
// by SimpleCommand's own fields (input, output, output_size,
// partition_id_, timestamp_, rank_). The map<int32_t, Value>'s Value
// elements use the Phase 4d-6 archive operators in marshal-value.cc,
// and the TxWorkspace input field uses the Phase 4d-6 archive
// operators in procedure.cc.
void serialize(const SimpleCommand &cmd, srpc::BinaryWriteArchive &ar) {
  verify(cmd.input.size() < 10000);
  srpc::Serialize_::serialize(cmd.id_, ar);
  srpc::Serialize_::serialize(cmd.type_, ar);
  srpc::Serialize_::serialize(cmd.inn_id_, ar);
  srpc::Serialize_::serialize(cmd.root_id_, ar);
  srpc::Serialize_::serialize(cmd.root_type_, ar);
  srpc::Serialize_::serialize(cmd.client_id_, ar);
  srpc::Serialize_::serialize(cmd.cmd_id_in_client_, ar);
  srpc::Serialize_::serialize(cmd.rule_mode_on_and_is_original_path_only_command_, ar);
  srpc::Serialize_::serialize(cmd.input, ar);
  srpc::Serialize_::serialize(cmd.output, ar);
  srpc::Serialize_::serialize(cmd.output_size, ar);
  srpc::Serialize_::serialize(cmd.partition_id_, ar);
  srpc::Serialize_::serialize(cmd.timestamp_, ar);
  srpc::Serialize_::serialize(cmd.rank_, ar);
}

srpc::BinaryWriteArchive &operator<<(srpc::BinaryWriteArchive &ar, const SimpleCommand &cmd) { serialize(cmd, ar); return ar; }

void deserialize(SimpleCommand &cmd, srpc::BinaryReadArchive &ar) {
  srpc::Deserialize_::deserialize(cmd.id_, ar);
  srpc::Deserialize_::deserialize(cmd.type_, ar);
  srpc::Deserialize_::deserialize(cmd.inn_id_, ar);
  srpc::Deserialize_::deserialize(cmd.root_id_, ar);
  srpc::Deserialize_::deserialize(cmd.root_type_, ar);
  srpc::Deserialize_::deserialize(cmd.client_id_, ar);
  srpc::Deserialize_::deserialize(cmd.cmd_id_in_client_, ar);
  srpc::Deserialize_::deserialize(cmd.rule_mode_on_and_is_original_path_only_command_, ar);
  srpc::Deserialize_::deserialize(cmd.input, ar);
  srpc::Deserialize_::deserialize(cmd.output, ar);
  srpc::Deserialize_::deserialize(cmd.output_size, ar);
  srpc::Deserialize_::deserialize(cmd.partition_id_, ar);
  srpc::Deserialize_::deserialize(cmd.timestamp_, ar);
  srpc::Deserialize_::deserialize(cmd.rank_, ar);
}

srpc::BinaryReadArchive &operator>>(srpc::BinaryReadArchive &ar, SimpleCommand &cmd) { deserialize(cmd, ar); return ar; }


} // namespace janus
