#pragma once

namespace janus {

// Used by PaxosWorker

class BulkOperationsPaxos {
private:

    std::vector<MultiPaxosCoordinator*> accept{};
    SpinLock acc_;
    const unsigned int cnt = 100;
    pthread_t th_;
    bool stop_flag = false;
    PaxosWorker *pw;

public:

    void add_accept(MultiPaxosCoordinator* coord) {
        acc_.lock();
        accept.push_back(coord);
        acc_.unlock();
    }

    void start_accept_read() {
        while (!stop_flag) {

            acc_.lock();
            auto it = accept.begin();
            if ((int) accept.size() < cnt) {
                it = accept.end();
            } else {
                it += cnt;
            }
            std::vector<MultiPaxosCoordinator*> current(accept.begin(), it);
            accept.erase(accept.begin(), it);
            acc_.unlock();

            auto sp_job = std::make_shared<OneTimeJob>([&pw, current]() {
                worker->BulkSubmit(current);
            });

        }
    }

    BulkOperationsPaxos() {
        Pthread_create(&th_, nullptr, start_accept_read, nullptr)
    }

};

} // namespace janus