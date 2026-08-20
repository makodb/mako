

#include "server.h"
// #include "paxos_worker.h"
// removed `#include "exec.h"` —
// FpgaRaftExecutor class deleted.
#include "frame.h"
#include "coordinator.h"
#include "../classic/tpc_command.h"
#include "rrr/misc/serializable.hpp"  // wrap_serializable_aliased


namespace janus {

bool FpgaRaftServer::looping = false;

struct hb_loop_args_type {
	FpgaRaftCommo* commo;
	FpgaRaftServer* sch;
};

FpgaRaftServer::FpgaRaftServer(Frame * frame) {
  frame_ = frame ;
  setIsFPGALeader(frame_->site_info_->locale_id == 0) ;
  setIsLeader(frame_->site_info_->locale_id == 0) ;
  stop_ = false ;
  timer_ = new Timer() ;
}

void FpgaRaftServer::Setup() {
  SimpleRWCommand::SetZeroTime();
  Log_info("Raft svr {} SetZeroTime", loc_id_);
	if (heartbeat_ && !FpgaRaftServer::looping && IsLeader()) {
		Log_info("starting loop at server");
		FpgaRaftServer::looping = true;
		hb_loop_args_type* hb_loop_args = new hb_loop_args_type();
		hb_loop_args->commo = (FpgaRaftCommo*) commo();
		hb_loop_args->sch = this;
		verify(hb_loop_args->commo && hb_loop_args->sch);
		loop_th_ = rusty::Some(rusty::thread::spawn([hb_loop_args]() {
			FpgaRaftServer::HeartbeatLoop(hb_loop_args);
		}));
	}
}

void* FpgaRaftServer::HeartbeatLoop(void* args) {
	hb_loop_args_type* hb_loop_args = (hb_loop_args_type*) args;

	FpgaRaftServer::looping = true;
	while(FpgaRaftServer::looping) {
		usleep(100*1000);
		uint64_t prevLogIndex = hb_loop_args->sch->lastLogIndex;	
		
		auto instance = hb_loop_args->sch->GetFpgaRaftInstance(prevLogIndex);
		auto term = instance->term;
		auto prevTerm = instance->prevTerm;
		auto ballot = instance->ballot;
		auto slot = instance->slot_id;
		// SendAppendEntriesAgain now takes const Command&;
		// pass instance->log_ directly.
		const auto& cmd = instance->log_;


		parid_t partition_id = hb_loop_args->sch->partition_id_;
		hb_loop_args->commo->BroadcastHeartbeat(partition_id, prevLogIndex);

		auto matcheds = hb_loop_args->commo->matchedIndex;
		for (auto it = matcheds.begin(); it != matcheds.end(); it++) {
			if (prevLogIndex > it->second + 10000 && cmd.has_value()) {
				Log_info("leader_id: {} vs follower_id for {}: {}", prevLogIndex, it->first, it->second);
				//hb_loop_args->commo->SendHeartbeat(partition_id, it->first, prevLogIndex);
				hb_loop_args->commo->SendAppendEntriesAgain(it->first,
																				partition_id,
                                        slot,
                                        ballot,
                                        hb_loop_args->sch->IsLeader(),
                                        term,
                                        prevLogIndex,
                                        prevTerm,
                                        hb_loop_args->sch->commitIndex,
                                        cmd);
			}
		}
	}
	delete hb_loop_args;
	return nullptr;
}

FpgaRaftServer::~FpgaRaftServer() {
		if (heartbeat_ && FpgaRaftServer::looping) {
			FpgaRaftServer::looping = false;
			if (loop_th_.is_some()) {
				loop_th_.take().unwrap().join().unwrap();
			}
		}
    
		stop_ = true ;
    Log_info("site par {}, loc {}: prepare {}, accept {}, commit {}", partition_id_, loc_id_, n_prepare_, n_accept_, 
    n_commit_);
    // Log_info("site par {}, loc {}: client2follower 50pct: {:.2f} 90pct: {:.2f} 99pct: {:.2f}", partition_id_, loc_id_, client2follower_.pct50(), client2follower_.pct90(), client2follower_.pct99());
}

void FpgaRaftServer::RequestVote2FPGA() {

  // currently don't request vote if no log
  if(this->commo_ == NULL || lastLogIndex == 0 ) return ;

  parid_t par_id = this->frame_->site_info_->partition_id_ ;
  parid_t loc_id = this->frame_->site_info_->locale_id ;

  if(paused_) {
      resetTimer() ;
      Log_debug("fpga raft server {} request vote to fpga rejected due to paused", loc_id );
      // req_voting_ = false ;
      return ;
  }

  Log_debug("fpga raft server {} in request vote to fpga", loc_id );

  uint32_t lstoff = 0  ;
  slotid_t lst_idx = 0 ;
  ballot_t lst_term = 0 ;

  {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // TODO set fpga isleader false, recheck 
    setIsFPGALeader(false) ;
    currentTerm++ ;
    lstoff = lastLogIndex - snapidx_ ;
    auto log = GetFpgaRaftInstance(lstoff) ;
    lst_idx = lstoff + snapidx_ ;
    lst_term = log->term ;
  }
  
  auto sp_quorum = ((FpgaRaftCommo *)(this->commo_))->BroadcastVote2FPGA(par_id,lst_idx,lst_term,loc_id, currentTerm );
  sp_quorum->wait();
  std::lock_guard<std::recursive_mutex> lock1(mtx_);
  if (sp_quorum->yes()) {
    // become a leader
    setIsFPGALeader(true) ;
    Log_debug("vote accepted {} curterm {}", loc_id, currentTerm);
  } else if (sp_quorum->no()) {
    // become a follower
    Log_debug("vote rejected {}", loc_id);
    setIsFPGALeader(false) ;
    //reset cur term if new term is higher
    ballot_t new_term = sp_quorum->Term() ;
    currentTerm = new_term > currentTerm? new_term : currentTerm ;
  } else {
    // TODO process timeout.
    Log_debug("vote timeout {}", loc_id);
  }
  req_voting_ = false ;
}

void FpgaRaftServer::OnVote2FPGA(const slotid_t& lst_log_idx,
                            const ballot_t& lst_log_term,
                            const parid_t& can_id,
                            const ballot_t& can_term,
                            ballot_t *reply_term,
                            bool_t *vote_granted,
                            rusty::Function<void()> cb) {

  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("fpga raft receives vote from candidate: {:x}", can_id);

  uint64_t cur_term = currentTerm ;
  if( can_term < cur_term)
  {
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false, std::move(cb)) ;
    return ;
  }

  // has voted to a machine in the same term, vote no
  // TODO when to reset the vote_for_??
//  if( can_term == cur_term && vote_for_ != INVALID_PARID )
  if( can_term == cur_term)
  {
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false, std::move(cb)) ;
    return ;
  }

  // lstoff starts from 1
  uint32_t lstoff = lastLogIndex - snapidx_ ;

  ballot_t curlstterm = snapterm_ ;
  slotid_t curlstidx = lastLogIndex ;

  if(lstoff > 0 )
  {
    auto log = GetFpgaRaftInstance(lstoff) ;
    curlstterm = log->term ;
  }

  Log_debug("vote for lstoff {}, curlstterm {}, curlstidx {}", lstoff, curlstterm, curlstidx  );


  // TODO del only for test
  verify(lstoff == lastLogIndex ) ;

  if( lst_log_term > curlstterm || (lst_log_term == curlstterm && lst_log_idx >= curlstidx) )
  {
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, true, std::move(cb)) ;
    return ;
  }

  doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false, std::move(cb)) ;

}


bool FpgaRaftServer::RequestVote() {
  for(int i = 0; i < 1000; i++) Log_info("not calling the wrong method");

  // currently don't request vote if no log
  if(this->commo_ == NULL || lastLogIndex == 0 ) return false;

  parid_t par_id = this->frame_->site_info_->partition_id_ ;
  parid_t loc_id = this->frame_->site_info_->locale_id ;


  if(paused_) {
      Log_debug("fpga raft server {} request vote rejected due to paused", loc_id );
      resetTimer() ;
      // req_voting_ = false ;
      return false;
  }

  Log_debug("fpga raft server {} in request vote", loc_id );

  uint32_t lstoff = 0  ;
  slotid_t lst_idx = 0 ;
  ballot_t lst_term = 0 ;

  {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // TODO set fpga isleader false, recheck 
    setIsFPGALeader(false) ;
    currentTerm++ ;
    lstoff = lastLogIndex - snapidx_ ;
    auto log = GetFpgaRaftInstance(lstoff) ;
    lst_idx = lstoff + snapidx_ ;
    lst_term = log->term ;
  }
  
  auto sp_quorum = ((FpgaRaftCommo *)(this->commo_))->BroadcastVote(par_id,lst_idx,lst_term,loc_id, currentTerm );
  sp_quorum->wait();
  std::lock_guard<std::recursive_mutex> lock1(mtx_);
  if (sp_quorum->yes()) {
    // become a leader
    setIsLeader(true) ;

    this->rep_frame_ = this->frame_ ;

    auto co = ((TxLogServer *)(this))->CreateRepCoord(0);
    auto empty_cmd = rusty::Arc<TpcEmptyCommand>::make();
    // dropped tautological `kMarshallKind == static_kind()` verify
    // (the kMarshallKind constant retired with the L8 TypeList migration).
    // aliased wrap via Command::pack_aliased preserves
    // Arc identity through the proxy.
    ((CoordinatorFpgaRaft*)co)->Submit(
        janus::Command::pack_aliased<TpcEmptyCommand>(std::move(empty_cmd)));
    
    //RequestVote2FPGA() ;
    if(IsLeader())
    {
	  	//for(int i = 0; i < 100; i++) Log_info("wait wait wait");
      Log_debug("vote accepted {} curterm {}", loc_id, currentTerm);
  		req_voting_ = false ;
			return true;
    }
    else
    {
      Log_debug("fpga vote rejected {} curterm {}, do rollback", loc_id, currentTerm);
      setIsLeader(false) ;
    	return false;
		}
  } else if (sp_quorum->no()) {
    // become a follower
    Log_debug("vote rejected {}", loc_id);
    setIsLeader(false) ;
    //reset cur term if new term is higher
    ballot_t new_term = sp_quorum->Term() ;
    currentTerm = new_term > currentTerm? new_term : currentTerm ;
  	req_voting_ = false ;
		return false;
  } else {
    // TODO process timeout.
    Log_debug("vote timeout {}", loc_id);
  	req_voting_ = false ;
		return false;
  }
}

void FpgaRaftServer::OnVote(const slotid_t& lst_log_idx,
                            const ballot_t& lst_log_term,
                            const parid_t& can_id,
                            const ballot_t& can_term,
                            ballot_t *reply_term,
                            bool_t *vote_granted,
                            rusty::Function<void()> cb) {

  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("fpga raft receives vote from candidate: {:x}", can_id);

  setIsFPGALeader(false) ;

  // TODO wait all the log pushed to fpga host

  uint64_t cur_term = currentTerm ;
  if( can_term < cur_term)
  {
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false, std::move(cb)) ;
    return ;
  }

  // has voted to a machine in the same term, vote no
  // TODO when to reset the vote_for_??
//  if( can_term == cur_term && vote_for_ != INVALID_PARID )
  if( can_term == cur_term)
  {
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false, std::move(cb)) ;
    return ;
  }

  // lstoff starts from 1
  uint32_t lstoff = lastLogIndex - snapidx_ ;

  ballot_t curlstterm = snapterm_ ;
  slotid_t curlstidx = lastLogIndex ;

  if(lstoff > 0 )
  {
    auto log = GetFpgaRaftInstance(lstoff) ;
    curlstterm = log->term ;
  }

  Log_debug("vote for lstoff {}, curlstterm {}, curlstidx {}", lstoff, curlstterm, curlstidx  );


  // TODO del only for test
  verify(lstoff == lastLogIndex ) ;

  if( lst_log_term > curlstterm || (lst_log_term == curlstterm && lst_log_idx >= curlstidx) )
  {
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, true, std::move(cb)) ;
    return ;
  }

  doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false, std::move(cb)) ;

}

void FpgaRaftServer::StartTimer()
{
    if(!init_ ){
        resetTimer() ;
        Fiber::create_run([&]() {
            Log_debug("start timer for election") ;
            int32_t duration = randDuration() ;
            while(!stop_)
            {
                if ( !IsLeader() && timer_->elapsed() > duration) {
                    Log_info(" timer time out") ;
                    // ask to vote
                    // req_voting_ = true ;
                    RequestVote() ;
                    /*while(req_voting_)
                    {
                      auto sp_e1 = create_sp_timeout_event(wait_int_);
                      sp_e1->wait_timeout(wait_int_) ;
                      if(stop_) return ;
                    }*/
                    Log_debug("start a new timer") ;
                    resetTimer() ;
                    duration = randDuration() ;
                }
                auto sp_e2 = create_sp_timeout_event(wait_int_);
                sp_e2->wait() ;
            } 
        });
      init_ = true ;
  }
}

/* NOTE: same as ReceiveAppend */
/* NOTE: broadcast send to all of the host even to its own server 
 * should we exclude the execution of this function for leader? */
  void FpgaRaftServer::OnAppendEntries(const slotid_t slot_id,
                                     const ballot_t ballot,
                                     const uint64_t leaderCurrentTerm,
                                     const uint64_t leaderPrevLogIndex,
                                     const uint64_t leaderPrevLogTerm,
                                     const uint64_t leaderCommitIndex,
																		 const struct DepId dep_id,
                                     const janus::Command& cmd,
                                     uint64_t *followerAppendOK,
                                     uint64_t *followerCurrentTerm,
                                     uint64_t *followerLastLogIndex,
                                     rusty::Function<void()> cb) {
#ifdef LATENCY_LOG_DEBUG
        Log_info("Time of cmd <{}, {}> arrive svr {} OnAppendEntries: {:.2f}ms", SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second, loc_id_, SimpleRWCommand::GetMsTimeElaps());
#endif
        // Log_info("OnAppendEntries svr {}", loc_id_);
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        // StartTimer() ; xxx: need to uncomment
        // client2follower_.append(SimpleRWCommand::GetCommandMsTimeElaps(cmd));
        
        Log_debug("fpga-raft scheduler on append entries for "
                "slot_id: {:x}, loc: {}, PrevLogIndex: {}",
                slot_id, this->loc_id_, leaderPrevLogIndex);
        if ((leaderCurrentTerm >= this->currentTerm) &&
                (leaderPrevLogIndex <= this->lastLogIndex)
                /* TODO: log[leaderPrevLogidex].term == leaderPrevLogTerm */) {
            //resetTimer() ;
            if (leaderCurrentTerm > this->currentTerm) {
                currentTerm = leaderCurrentTerm;
                Log_debug("server {}, set to be follower", loc_id_ ) ;
                setIsLeader(false) ;
            }

						//this means that this is a retry of a previous one for a simulation
						/*if (slot_id == 100000000 || leaderPrevLogIndex + 1 < lastLogIndex) {
							for (int i = 0; i < 1000000; i++) Log_info("Dropping this AE message: {} {}", leaderPrevLogIndex, lastLogIndex);
							//verify(0);
							*followerAppendOK = 0;
							cb();
							return;
						}*/
            verify(this->lastLogIndex == leaderPrevLogIndex);
            this->lastLogIndex = leaderPrevLogIndex + 1 /* TODO:len(ents) */;
            uint64_t prevCommitIndex = this->commitIndex;
            this->commitIndex = std::max(leaderCommitIndex, this->commitIndex);
            /* TODO: Replace entries after s.log[prev] w/ ents */
            /* TODO: it should have for loop for multiple entries */
            auto instance = GetFpgaRaftInstance(lastLogIndex);
            instance->log_ = cmd;


            // Pass the content to a thread that is always running
            // Disk write event
            // Wait on the event
            instance->term = this->currentTerm;
            //app_next_(*instance->log_); 
            verify(lastLogIndex > commitIndex);

            *followerAppendOK = 1;
            *followerCurrentTerm = this->currentTerm;
            *followerLastLogIndex = this->lastLogIndex;
            
						if (cmd.kind_ == TpcCommitCommand::static_kind()){
              const auto p_cmd = marshallable_cast<TpcCommitCommand>(cmd);
              const auto vec_piece_data = marshallable_cast<VecPieceData>(p_cmd.unwrap()->cmd_);
              verify(vec_piece_data.is_some());
              auto sp_vec_piece = vec_piece_data.unwrap()->sp_vec_piece_data_;
              
							vector<struct KeyValue> kv_vector;
							int index = 0;
							for (auto it = sp_vec_piece->begin(); it != sp_vec_piece->end(); it++){
								auto cmd_input = (*it)->input.values_;
								for (auto it2 = cmd_input->begin(); it2 != cmd_input->end(); it2++) {
									struct KeyValue key_value = {it2->first, it2->second.get_i32()};
									kv_vector.push_back(key_value);
								}
							}

							struct KeyValue key_values[kv_vector.size()];
							std::copy(kv_vector.begin(), kv_vector.end(), key_values);

							// auto de = IO::write(filename, key_values, sizeof(struct KeyValue), kv_vector.size());
							// de->wait();
            } else {
							int value = -1;
							// auto de = IO::write(filename, &value, sizeof(int), 1);
              // de->wait();
            }
        }
        else {
            Log_debug("reject append loc: {}, leader term {} last idx {}, server term: {} last idx: {}",
                this->loc_id_, leaderCurrentTerm, leaderPrevLogIndex, currentTerm, lastLogIndex);          
            *followerAppendOK = 0;
        }

				/*if (rand() % 1000 == 0) {
					usleep(25*1000);
				}*/
        WAN_WAIT
        cb();
    }

// removed `FpgaRaftServer::OnForward`
// (~20 LOC) — only caller was the deleted
// `FpgaRaftServiceImpl::Forward` handler; the matching
// FpgaRaft::Forward RPC declaration is gone from rcc_rpc.rpc.

  void FpgaRaftServer::OnCommit(const slotid_t slot_id,
                              const ballot_t ballot,
                              const janus::Command& cmd) {
#ifdef LATENCY_LOG_DEBUG
    Log_info("Time of cmd <{}, {}> arrive svr {} OnCommit: {:.2f}ms", SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second, loc_id_, SimpleRWCommand::GetMsTimeElaps());
#endif
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // Log_info("OnCommit");
		struct timespec begin, end;
		//clock_gettime(CLOCK_MONOTONIC, &begin);

    // This prevents the log entry from being applied twice
    if (in_applying_logs_) {
      return;
    }
    in_applying_logs_ = true;
    
    for (slotid_t id = executeIndex + 1; id <= commitIndex; id++) {
        auto next_instance = GetFpgaRaftInstance(id);
        // next_instance->log_ is Command; unwrap at the
        // boundary for RuleWitnessGC + GetCmdID (still take
        // shared_ptr<Marshallable>).  app_next_ takes Command.
        if (next_instance->log_.has_value()) {
            Log_debug("fpga-raft par:{} loc:{} executed slot {:x} now", partition_id_, loc_id_, id);
            // WAN_WAIT
            RuleWitnessGC(next_instance->log_);
#ifdef LATENCY_LOG_DEBUG
            Log_info("Time of cmd <{}, {}> arrive svr {} app_next: {:.2f}ms", SimpleRWCommand::GetCmdID(next_instance->log_).first, SimpleRWCommand::GetCmdID(next_instance->log_).second, loc_id_, SimpleRWCommand::GetMsTimeElaps());
#endif
            app_next_(id, next_instance->log_);
            executeIndex++;
        } else {
            break;
        }
    }
    in_applying_logs_ = false;

    int i = min_active_slot_;
    while (i + 6000 < executeIndex) {
      removeCmd(i++);
    }
    min_active_slot_ = i;

		/*clock_gettime(CLOCK_MONOTONIC, &end);
		Log_info("time of decide on server: {}", (end.tv_sec - begin.tv_sec)*1000000000 + end.tv_nsec - begin.tv_nsec);*/
  }
  void FpgaRaftServer::SpCommit(const uint64_t cmt_idx) {
      verify(0) ; // TODO delete it
      std::lock_guard<std::recursive_mutex> lock(mtx_);
      Log_debug("fpga raft spcommit for index: {:x} for server {}", cmt_idx, loc_id_);
      verify(cmt_idx != 0 ) ;
      if (cmt_idx < commitIndex) {
          return ;
      }

      commitIndex = cmt_idx;

      for (slotid_t id = executeIndex + 1; id <= commitIndex; id++) {
          auto next_instance = GetFpgaRaftInstance(id);
          // same Command-boundary pattern as the apply
          // loop above.
          if (next_instance->log_.has_value()) {
              // WAN_WAIT
              RuleWitnessGC(next_instance->log_);
              app_next_(id, next_instance->log_);
              Log_debug("fpga-raft par:{} loc:{} executed slot {:x} now", partition_id_, loc_id_, id);
              executeIndex++;
          } else {
              break;
          }
      }
  }

  void FpgaRaftServer::removeCmd(slotid_t slot) {
    const auto cmd = marshallable_cast<TpcCommitCommand>(raft_logs_[slot]->log_);
    if (cmd.is_none())
      return;
    tx_sched_->DestroyTx(cmd.unwrap()->tx_id_);
    raft_logs_.erase(slot);
  }

#ifdef ZERO_OVERHEAD
  bool FpgaRaftServer::ConflictWithOriginalUnexecutedLog(const janus::Command& cmd) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    for (slotid_t id = executeIndex + 1; id <= maxIndex; id++) {
      auto next_instance = GetFpgaRaftInstance(id);
      // Conflict has Command overload now; both args
      // are Command so dispatch directly.
      if (next_instance->log_.has_value() &&
          SimpleRWCommand::Conflict(next_instance->log_, cmd))
        return true;
    }
    return false;
  }
#endif

} // namespace janus
