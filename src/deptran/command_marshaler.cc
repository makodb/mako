#include "__dep__.h"
#include "marshal-value.h"
#include "command.h"
#include "procedure.h"
#include "command_marshaler.h"
#include "procedure.h"

namespace janus {

Marshal& CmdData::ToMarshal(Marshal& m) const {
  m << id_;
  m << type_;
  m << inn_id_;
  m << root_id_;
  m << root_type_;
  m << client_id_;
  m << cmd_id_in_client_;
  m << rule_mode_on_and_is_original_path_only_command_;
  return m;
};

Marshal& CmdData::FromMarshal(Marshal& m) {
  m >> id_;
  m >> type_;
  m >> inn_id_;
  m >> root_id_;
  m >> root_type_;
  m >> client_id_;
  m >> cmd_id_in_client_;
  m >> rule_mode_on_and_is_original_path_only_command_;
  return m;
};

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


} // namespace janus
