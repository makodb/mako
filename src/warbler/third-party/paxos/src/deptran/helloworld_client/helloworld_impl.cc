#include "helloworld_impl.h"
#include <chrono>

namespace helloworld_client {
    HelloworldClientServiceImpl::HelloworldClientServiceImpl() {  }

    void HelloworldClientServiceImpl::txn_read(const std::vector<rrr::i64>& _req, rrr::i32* val, rrr::DeferredReply* defer) {
        
        const auto& func = [val, defer, _req, this]() {
            if (_req.size()==1) {
                std::cout<<"[server]receive first request"<<std::endl;
                //Coroutine::CurrentCoroutine()->Yield();
                sleep(5);
            } else if (_req.size()==2) {
                std::cout<<"[server]receive second request"<<std::endl;
            }
            *val = _req.size();
            defer->reply();
            std::cout<<"receive " <<  _req.size() << " request - done"<<std::endl;
        };

        // run in different coroutine, but the server always sequential for the same connection;
        if (_req.size()==1) {
            auto first_req = Coroutine::CreateRun(func);
        }
        else {
            std::cout<<"before coroutine for second"<<std::endl;
            Coroutine::CreateRun(func);
            //Reactor::GetReactor()->ContinueCoro(first_req); // continue the first request manually
        }
            
    }
}