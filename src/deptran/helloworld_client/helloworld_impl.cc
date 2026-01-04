#include "helloworld_impl.h"
#include <chrono>

namespace helloworld_client {
    HelloworldClientServiceImpl::HelloworldClientServiceImpl() {  }

    void HelloworldClientServiceImpl::txn_read(const std::vector<rrr::i64>& _req, rrr::i32* val, rrr::DeferredReply defer) {
        // run in different coroutine, but the server always sequential for the same connection;
        if (_req.size()==1) {
            Coroutine::CreateRun([val, defer = std::move(defer), _req, this]() mutable {
                std::cout<<"[server]receive first request"<<std::endl;
                //Coroutine::CurrentCoroutine()->Yield();
                sleep(5);
                *val = _req.size();
                defer.reply();
                std::cout<<"receive " <<  _req.size() << " request - done"<<std::endl;
            });
        }
        else {
            std::cout<<"before coroutine for second"<<std::endl;
            Coroutine::CreateRun([val, defer = std::move(defer), _req, this]() mutable {
                std::cout<<"[server]receive second request"<<std::endl;
                *val = _req.size();
                defer.reply();
                std::cout<<"receive " <<  _req.size() << " request - done"<<std::endl;
            });
            //Reactor::get_reactor()->ContinueCoro(first_req); // continue the first request manually
        }
    }
}