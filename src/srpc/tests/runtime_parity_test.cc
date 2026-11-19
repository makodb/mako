// @unsafe - Integration fixture: native threads and canonical SRPC runtime.
#include <std_compat.hpp>
#include <rusty/async.hpp>
#include <rusty/box.hpp>
#include "../srpc.hpp"

import srpc.serializable;
import std;

using namespace srpc;

namespace {
constexpr int32_t RPC_ID = 0x00e0'2001;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Echo : public Service {
 public:
    int32_t __reg_to__(Server& server, size_t index) override {
        return server.reg_fast_rpc(RPC_ID, index);
    }
    void __dispatch__(int32_t, rusty::Box<Request> request,
                      WeakServerConnection connection) const override {
        int64_t value = 0;
        BinaryReadArchive archive{.source_ = make_source_proxy_buffer(&request->src)};
        Deserialize_::deserialize(value, archive);
        auto sconn = connection.upgrade().unwrap();
        sconn->reply(*request, 0,
            ServerReplyFn{[value](BinaryWriteArchive& out) {
                Serialize_::serialize(value * 2, out);
            }});
    }
};

struct WakeGate {
    std::atomic<bool> ready{false};
    rusty::Waker waker;
    bool published = false;
};

struct GateAwaiter {
    WakeGate* gate;
    bool await_ready() const noexcept { return gate->ready.load(); }
    void await_suspend(std::coroutine_handle<>) const {
        auto* context = rusty::current_context();
        require(context && context->waker, "missing canonical task waker");
        gate->waker = *context->waker;
        gate->published = true;
    }
    void await_resume() const noexcept {}
};

rusty::Task<int64_t> wait_for_gate(WakeGate* gate) {
    while (!gate->ready.load()) co_await GateAwaiter{gate};
    co_return 7;
}

void run_fixture() {
    auto reactor = Reactor::get_reactor();
    std::vector<std::string> order;
    bool timer_done = false;
    bool deadline_respected = false;
    Fiber::create_run([&] {
        order.push_back("start");
        const auto started = std::chrono::steady_clock::now();
        this_fiber::sleep_ms(10);
        deadline_respected = std::chrono::steady_clock::now() - started >=
            std::chrono::milliseconds(10);
        order.push_back("resume");
        timer_done = true;
    });
    const bool timer_suspended = !timer_done;
    Fiber::create_run([&] { order.push_back("peer"); });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!timer_done && std::chrono::steady_clock::now() < deadline) {
        reactor->run_loop(false, true);
        std::this_thread::yield();
    }
    require(timer_done, "timer did not resume");
    require(order == std::vector<std::string>{"start", "peer", "resume"}, "timer order");
    require(timer_suspended && deadline_respected, "timer did not suspend for its deadline");

    const auto owner = std::this_thread::get_id();
    WakeGate gate;
    int64_t wake_value = 0;
    bool completion_on_owner = false;
    reactor_spawn_stackless_task_with_result<int64_t>(*reactor, wait_for_gate(&gate),
        [&](int64_t value) {
            wake_value = value;
            completion_on_owner = std::this_thread::get_id() == owner;
        });
    require(gate.published, "task did not publish a waker");
    const bool pending_before_wake = wake_value == 0;
    bool foreign_thread = false;
    std::thread foreign([&] {
        foreign_thread = std::this_thread::get_id() != owner;
        gate.ready.store(true);
        gate.waker.wake();
    });
    foreign.join();
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (wake_value == 0 && std::chrono::steady_clock::now() < deadline) {
        reactor->run_loop(false, true);
        std::this_thread::yield();
    }
    require(pending_before_wake && foreign_thread && completion_on_owner && wake_value == 7,
            "foreign wake did not complete on owner");

    auto poll = PollThread::create();
    auto server = Server::new_(rusty::Some(poll.clone()));
    server.reg_service_typed(rusty::make_box<Echo>());
    require(server.start(reinterpret_cast<const int8_t*>("127.0.0.1:0")) == 0, "bind failed");
    const auto address = "127.0.0.1:" + std::to_string(server.get_bound_port());
    auto client = Client::create(poll.clone());
    require(client->connect(reinterpret_cast<const int8_t*>(address.c_str()), true) == 0,
            "connect failed");
    auto success = client->request(RPC_ID, FutureAttr{}, [](BinaryWriteArchive& out) {
        Serialize_::serialize(int64_t{21}, out);
    }).unwrap();
    const auto success_error = success->get_error_code();
    int64_t reply = 0;
    deserialize_from(success->get_reply(), reply);
    auto failure = client->request(RPC_ID + 1, FutureAttr{}, [](BinaryWriteArchive&) {}).unwrap();
    const auto failure_error = failure->get_error_code();
    client->close();
    require(reply == 42 && success_error == 0 && failure_error == 2, "RPC result mismatch");

    std::cout << std::boolalpha << "SRPC_RUNTIME_PARITY {\"version\":1,\"timer_order\":[";
    for (size_t index = 0; index < order.size(); ++index) {
        if (index) std::cout << ',';
        std::cout << '"' << order[index] << '"';
    }
    std::cout << "],\"timer_suspended\":" << timer_suspended
        << ",\"deadline_respected\":" << deadline_respected
        << ",\"pending_before_wake\":" << pending_before_wake
        << ",\"foreign_thread\":" << foreign_thread
        << ",\"completion_on_owner\":" << completion_on_owner
        << ",\"wake_value\":" << wake_value
        << ",\"rpc_reply\":" << reply
        << ",\"rpc_success_error\":" << success_error
        << ",\"rpc_missing_error\":" << failure_error << "}\n";
}
} // namespace

int main() {
    try { run_fixture(); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
