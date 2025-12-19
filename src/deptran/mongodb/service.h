#pragma once

#include "server.h"

namespace janus {

class MongodbServiceImpl: public MongodbService {
 public:
  MongodbServer* sched_;
  MongodbServiceImpl(TxLogServer* sched);

  void Commit(const MarshallDeputy& md_cmd,
              rrr::DeferredReply* defer) override;

};


} // namespace janus