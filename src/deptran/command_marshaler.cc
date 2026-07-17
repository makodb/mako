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

rrr::Marshal &operator<<(rrr::Marshal &m, const SimpleCommand &cmd) {
  verify(cmd.input.size() < 10000);
  m << cmd.id_;
  m << cmd.type_;
  m << cmd.inn_id_;
  m << cmd.root_id_;
  m << cmd.root_type_;
  m << cmd.client_id_;
  m << cmd.cmd_id_in_client_;
  m << cmd.rule_mode_on_and_is_original_path_only_command_;
  m << cmd.input;
  m << cmd.output;
  m << cmd.output_size;
  m << cmd.partition_id_;
  m << cmd.timestamp_;
  m << cmd.rank_;
  return m;
}

rrr::Marshal &operator>>(rrr::Marshal &m, SimpleCommand &cmd) {
  m >> cmd.id_;
  m >> cmd.type_;
  m >> cmd.inn_id_;
  m >> cmd.root_id_;
  m >> cmd.root_type_;
  m >> cmd.client_id_;
  m >> cmd.cmd_id_in_client_;
  m >> cmd.rule_mode_on_and_is_original_path_only_command_;
  m >> cmd.input;
  m >> cmd.output;
  m >> cmd.output_size;
  m >> cmd.partition_id_;
  m >> cmd.timestamp_;
  m >> cmd.rank_;
  return m;
}

// archive operators for SimpleCommand.
// Wire format byte-for-byte identical to the Marshal-based pair
// above: the 8 inherited CmdData fields (id, type, inn_id, root_id,
// root_type, client_id, cmd_id_in_client, rule_mode flag), followed
// by SimpleCommand's own fields (input, output, output_size,
// partition_id_, timestamp_, rank_). The map<int32_t, Value>'s Value
// elements use the Phase 4d-6 archive operators in marshal-value.cc,
// and the TxWorkspace input field uses the Phase 4d-6 archive
// operators in procedure.cc.
rrr::BinaryWriteArchive &operator<<(rrr::BinaryWriteArchive &ar, const SimpleCommand &cmd) {
  verify(cmd.input.size() < 10000);
  rrr::Serialize_::serialize(cmd.id_, ar);
  rrr::Serialize_::serialize(cmd.type_, ar);
  rrr::Serialize_::serialize(cmd.inn_id_, ar);
  rrr::Serialize_::serialize(cmd.root_id_, ar);
  rrr::Serialize_::serialize(cmd.root_type_, ar);
  rrr::Serialize_::serialize(cmd.client_id_, ar);
  rrr::Serialize_::serialize(cmd.cmd_id_in_client_, ar);
  rrr::Serialize_::serialize(cmd.rule_mode_on_and_is_original_path_only_command_, ar);
  rrr::Serialize_::serialize(cmd.input, ar);
  rrr::Serialize_::serialize(cmd.output, ar);
  rrr::Serialize_::serialize(cmd.output_size, ar);
  rrr::Serialize_::serialize(cmd.partition_id_, ar);
  rrr::Serialize_::serialize(cmd.timestamp_, ar);
  rrr::Serialize_::serialize(cmd.rank_, ar);
  return ar;
}

rrr::BinaryReadArchive &operator>>(rrr::BinaryReadArchive &ar, SimpleCommand &cmd) {
  rrr::Deserialize_::deserialize(cmd.id_, ar);
  rrr::Deserialize_::deserialize(cmd.type_, ar);
  rrr::Deserialize_::deserialize(cmd.inn_id_, ar);
  rrr::Deserialize_::deserialize(cmd.root_id_, ar);
  rrr::Deserialize_::deserialize(cmd.root_type_, ar);
  rrr::Deserialize_::deserialize(cmd.client_id_, ar);
  rrr::Deserialize_::deserialize(cmd.cmd_id_in_client_, ar);
  rrr::Deserialize_::deserialize(cmd.rule_mode_on_and_is_original_path_only_command_, ar);
  rrr::Deserialize_::deserialize(cmd.input, ar);
  rrr::Deserialize_::deserialize(cmd.output, ar);
  rrr::Deserialize_::deserialize(cmd.output_size, ar);
  rrr::Deserialize_::deserialize(cmd.partition_id_, ar);
  rrr::Deserialize_::deserialize(cmd.timestamp_, ar);
  rrr::Deserialize_::deserialize(cmd.rank_, ar);
  return ar;
}


} // namespace janus
