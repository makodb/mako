#include <stddef.h>


#include <stdio.h>
#include <stdlib.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>

#include "benchmark_service.h"

import std;

using namespace benchmark;
using namespace rrr;
using namespace std;

const char *svr_addr = "127.0.0.1:8848";
int byte_size = 10;
int epoll_instances = 2;
bool fast_requests = false;
bool await_mode = false;
int seconds = 10;
int outgoing_requests = 1000;
int client_threads = 8;
int worker_threads = 16;
int rpc_bench_vector_size = 0;

enum class BenchRpcMode {
    FAST,
    FIBER,
    DEFER,
    ASYNC,
    FAST_VEC,
};

BenchRpcMode rpc_mode = BenchRpcMode::FIBER;
bool rpc_mode_explicit = false;

static const char* rpc_mode_name(BenchRpcMode mode) {
    switch (mode) {
    case BenchRpcMode::FAST:
        return "fast";
    case BenchRpcMode::FIBER:
        return "fiber";
    case BenchRpcMode::DEFER:
        return "defer";
    case BenchRpcMode::ASYNC:
        return "async";
    case BenchRpcMode::FAST_VEC:
        return "fast_vec";
    default:
        return "fiber";
    }
}

static bool parse_rpc_mode(const std::string& mode_str, BenchRpcMode* mode_out) {
    if (mode_str == "fast") {
        *mode_out = BenchRpcMode::FAST;
        return true;
    }
    if (mode_str == "fiber") {
        *mode_out = BenchRpcMode::FIBER;
        return true;
    }
    if (mode_str == "defer") {
        *mode_out = BenchRpcMode::DEFER;
        return true;
    }
    if (mode_str == "async") {
        *mode_out = BenchRpcMode::ASYNC;
        return true;
    }
    if (mode_str == "fast_vec") {
        *mode_out = BenchRpcMode::FAST_VEC;
        return true;
    }
    return false;
}

static string request_str;

std::atomic<i64>* g_client_req_counters = nullptr;
int g_client_req_counter_count = 0;

bool should_stop = false;

pthread_mutex_t g_stop_mutex;
pthread_cond_t g_stop_cond;

static i64 total_req_count() {
    i64 total = 0;
    for (int i = 0; i < g_client_req_counter_count; ++i) {
        total += g_client_req_counters[i].load(std::memory_order_relaxed);
    }
    return total;
}

static void signal_handler(int sig) {
    Log_info("caught signal %d, stopping server now", sig);
    should_stop = true;
    Pthread_mutex_lock(&g_stop_mutex);
    Pthread_cond_signal(&g_stop_cond);
    Pthread_mutex_unlock(&g_stop_mutex);
}

static void* stat_proc(void*) {
    std::vector<int> summary;
    summary.reserve(seconds);
    i64 last_cnt = 0;
    for (int i = 0; i < seconds; i++) {
        i64 cnt = total_req_count();
        if (last_cnt != 0) {
            long int qps = cnt - last_cnt;
            Log_info("qps: %ld", cnt - last_cnt);
            summary.push_back(qps);
        }
        last_cnt = cnt;
        sleep(1);
    }
    should_stop = true;
    int sum = 0;
    for (size_t i=0; i<summary.size(); i++) {
        sum += summary[i];
    }
    if (summary.empty()) {
        Log_info("avg qps: 0.00");
    } else {
        Log_info("avg qps: %2.2f", ((float)sum)/summary.size());
    }
    return nullptr;
}

struct ClientThreadArg {
    int thread_idx;
};

static rusty::Task<void> await_worker(BenchmarkProxy* proxy, int thread_idx, i32 rpc_id) {
    BenchmarkProxy::RpcFastNopRequest fast_nop_req;
    fast_nop_req.in_0 = request_str;

    BenchmarkProxy::RpcAsyncNopRequest async_nop_req;
    async_nop_req.in_0 = request_str;

    BenchmarkProxy::RpcNopRequest nop_req;
    nop_req.in_0 = request_str;

    BenchmarkProxy::RpcDeferredEchoRequest deferred_echo_req;
    deferred_echo_req.val = 1;

    BenchmarkProxy::RpcFastVecRequest fast_vec_req;
    fast_vec_req.n = rpc_bench_vector_size;

    while (!should_stop) {
        bool ok = false;
        if (rpc_id == BenchmarkService::FAST_NOP) {
            auto resp = co_await proxy->await_fast_nop(fast_nop_req);
            ok = resp.is_ok();
        } else if (rpc_id == BenchmarkService::FAST_VEC) {
            auto resp = co_await proxy->await_fast_vec(fast_vec_req);
            ok = resp.is_ok();
        } else if (rpc_id == BenchmarkService::ASYNC_NOP) {
            auto resp = co_await proxy->await_async_nop(async_nop_req);
            ok = resp.is_ok();
        } else if (rpc_id == BenchmarkService::DEFERRED_ECHO) {
            auto resp = co_await proxy->await_deferred_echo(deferred_echo_req);
            ok = resp.is_ok();
        } else {
            auto resp = co_await proxy->await_nop(nop_req);
            ok = resp.is_ok();
        }

        if (ok) {
            g_client_req_counters[thread_idx].fetch_add(1, std::memory_order_relaxed);
        }
    }
    co_return;
}

// Self-referencing waker: when the awaited Future completes, re-poll
// the task to drive the coroutine forward.  Returns the heap-allocated
// waker so the caller can keep it alive for the lifetime of the task.
//
// Without this, the prior implementation installed a no-op waker —
// rusty-cpp's `Task::poll` sets `current_context_tls` to the passed
// context, so `TypedFutureResultAwaiter::await_suspend` registered the
// no-op as the Future's completion callback.  When the reply arrived,
// the no-op fired and the coroutine never resumed.
static std::shared_ptr<rusty::Waker> prime_task(rusty::Task<void>& task) {
    auto waker = std::make_shared<rusty::Waker>();
    rusty::Task<void>* task_ptr = &task;
    std::weak_ptr<rusty::Waker> waker_weak{waker};
    waker->wake_fn = [task_ptr, waker_weak]() {
        auto wk = waker_weak.lock();
        if (!wk) return;
        rusty::Context ctx{wk.get()};
        (void)task_ptr->poll(ctx);
    };
    rusty::Context ctx{waker.get()};
    (void)task.poll(ctx);
    return waker;
}

static void* client_proc(void* arg_ptr) {
    auto* arg = static_cast<ClientThreadArg*>(arg_ptr);
    int thread_idx = arg->thread_idx;
    auto poll_thread_worker = PollThread::create();
    auto cl = Client::create(poll_thread_worker);
    verify(cl->connect(reinterpret_cast<const int8_t*>(svr_addr), true) == 0);
    FutureAttr fu_attr;
    i32 rpc_id;

    switch (rpc_mode) {
    case BenchRpcMode::FAST:
        rpc_id = BenchmarkService::FAST_NOP;
        break;
    case BenchRpcMode::FIBER:
        rpc_id = BenchmarkService::NOP;
        break;
    case BenchRpcMode::DEFER:
        rpc_id = BenchmarkService::DEFERRED_ECHO;
        break;
    case BenchRpcMode::ASYNC:
        rpc_id = BenchmarkService::ASYNC_NOP;
        break;
    case BenchRpcMode::FAST_VEC:
        rpc_id = BenchmarkService::FAST_VEC;
        break;
    default:
        rpc_id = BenchmarkService::NOP;
        break;
    }

    BenchmarkProxy proxy(const_cast<Client*>(cl.get()));
    std::vector<rusty::Task<void>> await_tasks;

    std::vector<std::shared_ptr<rusty::Waker>> await_wakers;
    if (await_mode) {
        await_tasks.reserve(outgoing_requests);
        await_wakers.reserve(outgoing_requests);
        for (int i = 0; i < outgoing_requests; ++i) {
            await_tasks.emplace_back(await_worker(&proxy, thread_idx, rpc_id));
        }
        for (auto& task : await_tasks) {
            await_wakers.push_back(prime_task(task));
        }
    }

    // Slim async-callback path: bench callers don't inspect the
    // returned Future (no `fu->wait()`, no `fu->get_reply()`), so use
    // `request_async` to skip the Arc<Future> + HashMap allocations.
    // The callback re-fires `do_work` on each successful reply,
    // keeping the pipeline full at depth `outgoing_requests`.
    std::function<void()> do_work_holder;
    auto do_work = [cl, rpc_id, thread_idx, &do_work_holder] {
        if (should_stop) return;
        auto write_fn = [rpc_id](rrr::BinaryWriteArchive& m) {
            if (rpc_id == BenchmarkService::FAST_NOP ||
                rpc_id == BenchmarkService::NOP ||
                rpc_id == BenchmarkService::ASYNC_NOP) {
                rrr::Serialize_::serialize(request_str, m);
            } else if (rpc_id == BenchmarkService::FAST_VEC) {
                rrr::Serialize_::serialize(rpc_bench_vector_size, m);
            } else if (rpc_id == BenchmarkService::DEFERRED_ECHO) {
                rrr::Serialize_::serialize(static_cast<rrr::i32>(1), m);
            }
        };
        rrr::AsyncReplyCallback on_reply{
            [thread_idx, &do_work_holder](rrr::i32 err,
                                          const std::uint8_t*,
                                          std::size_t) {
                if (err != 0) return;
                do_work_holder();
            }};
        auto send_result =
            cl->request_async(rpc_id, write_fn, std::move(on_reply));
        if (send_result.is_err()) return;
        g_client_req_counters[thread_idx].fetch_add(
            1, std::memory_order_relaxed);
    };
    do_work_holder = do_work;
    if (!await_mode) {
        for (int i = 0; i < outgoing_requests; i++) {
            do_work();
        }
    }
    while (!should_stop) {
        sleep(1);
    }

    cl->close();  // shared_ptr handles cleanup
    poll_thread_worker->shutdown();
    return nullptr;
}

int main(int argc, char **argv) {
    Log::set_level(Log::INFO);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    bool is_client = false, is_server = false;

    if (argc < 2) {
        printf("usage: perftest OPTIONS\n");
        printf("                -c|-s ip:port\n");
        printf("                -b    byte_size\n");
        printf("                -e    epoll_instances\n");
        printf("                -f    fast_requests\n");
        printf("                -m    rpc mode: fast|fiber|defer|async|fast_vec\n");
        printf("                -a    await client mode\n");
        printf("                -n    seconds\n");
        printf("                -o    outgoing_requests\n");
        printf("                -t    client_threads\n");
        printf("                -w    worker_threads\n");
        printf("                -v    vector test\n");
        exit(1);
    }

    char ch = 0;
    while ((ch = getopt(argc, argv, "c:s:b:e:fan:o:t:w:v:m:"))!= -1) {
        switch (ch) {
        case 'c':
            is_client = true;
            svr_addr = optarg;
            break;
        case 's':
            is_server = true;
            svr_addr = optarg;
            break;
        case 'b':
            byte_size = atoi(optarg);
            break;
        case 'e':
            epoll_instances = atoi(optarg);
            break;
        case 'f':
            fast_requests = true;
            break;
        case 'm': {
            BenchRpcMode parsed_mode;
            if (!parse_rpc_mode(optarg, &parsed_mode)) {
                Log_error("invalid rpc mode '%s' (expected: fast|fiber|defer|async|fast_vec)", optarg);
                exit(1);
            }
            rpc_mode = parsed_mode;
            rpc_mode_explicit = true;
            break;
        }
        case 'a':
            await_mode = true;
            break;
        case 'n':
            seconds = atoi(optarg);
            break;
        case 'o':
            outgoing_requests = atoi(optarg);
            break;
        case 't':
            client_threads = atoi(optarg);
            break;
        case 'w':
            worker_threads = atoi(optarg);
            break;
        case 'v':
            rpc_bench_vector_size = atoi(optarg);
            break;
        default:
            break;
        }
    }

    if (!rpc_mode_explicit) {
        if (rpc_bench_vector_size > 0) {
            rpc_mode = BenchRpcMode::FAST_VEC;
        } else if (fast_requests) {
            rpc_mode = BenchRpcMode::FAST;
        } else {
            rpc_mode = BenchRpcMode::FIBER;
        }
    }

    if (rpc_bench_vector_size > 0 && rpc_mode != BenchRpcMode::FAST_VEC) {
        Log_error("vector benchmark requires mode fast_vec (use -m fast_vec)");
        exit(1);
    }

    verify(is_server || is_client);
    if (is_server) {
        Log_info("server will start at     %s", svr_addr);
    } else {
        Log_info("client will connect to   %s", svr_addr);
    }
    Log_info("packet byte size:        %d", byte_size);
    Log_info("epoll instances:         %d", epoll_instances);
    Log_info("fast reqeust:            %s", fast_requests ? "true" : "false");
    Log_info("rpc mode:                %s", rpc_mode_name(rpc_mode));
    Log_info("await mode:              %s", await_mode ? "true" : "false");
    Log_info("running seconds:         %d", seconds);
    Log_info("outgoing requests:       %d", outgoing_requests);
    Log_info("client threads:          %d", client_threads);
    Log_info("worker threads:          %d", worker_threads);
    Log_info("vector size:             %d", rpc_bench_vector_size);

    request_str = string(byte_size, 'x');
    if (is_server) {
        auto server_poll_thread = rusty::Some(PollThread::create());
        auto svr = Server::new_(std::move(server_poll_thread));  // Server takes Option<Arc<...>>
        svr.reg_service_typed(rusty::make_box<BenchmarkService>());
        verify(svr.start(reinterpret_cast<const int8_t*>(svr_addr)) == 0);

        Pthread_mutex_init(&g_stop_mutex, nullptr);
        Pthread_cond_init(&g_stop_cond, nullptr);

        signal(SIGPIPE, SIG_IGN);
        signal(SIGHUP, SIG_IGN);
        signal(SIGCHLD, SIG_IGN);

        signal(SIGALRM, signal_handler);
        signal(SIGINT, signal_handler);
        signal(SIGQUIT, signal_handler);
        signal(SIGTERM, signal_handler);

        Pthread_mutex_lock(&g_stop_mutex);
        while (should_stop == false) {
            Pthread_cond_wait(&g_stop_cond, &g_stop_mutex);
        }
        Pthread_mutex_unlock(&g_stop_mutex);

    } else {
        g_client_req_counter_count = client_threads;
        g_client_req_counters = new std::atomic<i64>[client_threads];
        for (int i = 0; i < client_threads; ++i) {
            g_client_req_counters[i].store(0, std::memory_order_relaxed);
        }

        pthread_t* client_th = new pthread_t[client_threads];
        ClientThreadArg* client_args = new ClientThreadArg[client_threads];
        for (int i = 0; i < client_threads; i++) {
            client_args[i].thread_idx = i;
            Pthread_create(&client_th[i], nullptr, client_proc, &client_args[i]);
        }
        pthread_t stat_th;
        Pthread_create(&stat_th, nullptr, stat_proc, nullptr);
        Pthread_join(stat_th, nullptr);
        for (int i = 0; i < client_threads; i++) {
            Pthread_join(client_th[i], nullptr);
        }
        delete[] g_client_req_counters;
        g_client_req_counters = nullptr;
        g_client_req_counter_count = 0;
        delete[] client_args;
        delete[] client_th;
    }

    return 0;
}
