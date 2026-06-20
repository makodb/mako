#include <std_compat.hpp>  // textual STL before `import std` (abi_tag fix)
#include <math.h>
#include <unistd.h>

#include "benchmark_service.h"

using namespace benchmark;
using namespace rrr;

static Counter g_nop_counter = Counter::new_(0);
extern int rpc_bench_vector_size;

namespace {

inline i8 compute_prime(i32 n) {
    if (n <= 0) {
        return -1;
    }
    if (n <= 3) {
        return 1;
    }
    if (n % 2 == 0) {
        return 0;
    }
    int d = 3;
    int m = sqrt(n) + 1;  // +1 for sqrt float errors.
    while (d <= m) {
        if (n % d == 0) {
            return 0;
        }
        d++;
    }
    return 1;
}

inline double compute_dot_prod(const point3& p1, const point3& p2) {
    return p1.x * p2.x + p1.y * p2.y + p1.z * p2.z;
}

inline v32 compute_add(const v32& a, const v32& b) {
    v32 out;
    out.set(a.get() + b.get());
    return out;
}

}  // namespace

rusty::Result<BenchmarkService::RpcFastPrimeResponse, i32>
BenchmarkService::fast_prime(const RpcFastPrimeRequest& req) {
    RpcFastPrimeResponse resp{};
    resp.flag = compute_prime(req.n);
    return rusty::Result<RpcFastPrimeResponse, i32>::Ok(resp);
}

rusty::Result<BenchmarkService::RpcFastDotProdResponse, i32>
BenchmarkService::fast_dot_prod(const RpcFastDotProdRequest& req) {
    RpcFastDotProdResponse resp{};
    resp.v = compute_dot_prod(req.p1, req.p2);
    return rusty::Result<RpcFastDotProdResponse, i32>::Ok(resp);
}

rusty::Result<BenchmarkService::RpcFastAddResponse, i32>
BenchmarkService::fast_add(const RpcFastAddRequest& req) {
    RpcFastAddResponse resp{};
    resp.a_add_b = compute_add(req.a, req.b);
    return rusty::Result<RpcFastAddResponse, i32>::Ok(resp);
}

rusty::Result<BenchmarkService::RpcFastNopResponse, i32>
BenchmarkService::fast_nop(const RpcFastNopRequest& req) {
    (void)req;
    int cnt = g_nop_counter.next(1);
    if (cnt % 200000 == 0) {
        Log_info("%d nop requests", cnt);
    }
    return rusty::Result<RpcFastNopResponse, i32>::Ok(RpcFastNopResponse{});
}

rusty::Result<BenchmarkService::RpcFastVecResponse, i32>
BenchmarkService::fast_vec(const RpcFastVecRequest& req) {
    RpcFastVecResponse resp{};
    verify(req.n > 0);
    resp.v.insert(resp.v.begin(), req.n, 1);
    return rusty::Result<RpcFastVecResponse, i32>::Ok(resp);
}

rusty::Result<BenchmarkService::RpcPrimeResponse, i32>
BenchmarkService::prime(const RpcPrimeRequest& req) {
    RpcPrimeResponse resp{};
    resp.flag = compute_prime(req.n);
    return rusty::Result<RpcPrimeResponse, i32>::Ok(resp);
}

rusty::Result<BenchmarkService::RpcDotProdResponse, i32>
BenchmarkService::dot_prod(const RpcDotProdRequest& req) {
    RpcDotProdResponse resp{};
    resp.v = compute_dot_prod(req.p1, req.p2);
    return rusty::Result<RpcDotProdResponse, i32>::Ok(resp);
}

rusty::Result<BenchmarkService::RpcAddResponse, i32>
BenchmarkService::add(const RpcAddRequest& req) {
    RpcAddResponse resp{};
    resp.a_add_b = compute_add(req.a, req.b);
    return rusty::Result<RpcAddResponse, i32>::Ok(resp);
}

rusty::Result<BenchmarkService::RpcNopResponse, i32>
BenchmarkService::nop(const RpcNopRequest& req) {
    (void)req;
    int cnt = g_nop_counter.next(1);
    if (cnt % 200000 == 0) {
        Log_info("%d nop requests", cnt);
    }
    return rusty::Result<RpcNopResponse, i32>::Ok(RpcNopResponse{});
}

rusty::Task<rusty::Result<BenchmarkService::RpcAsyncNopResponse, i32>>
BenchmarkService::async_nop(const RpcAsyncNopRequest& req) {
    (void)req;
    int cnt = g_nop_counter.next(1);
    if (cnt % 200000 == 0) {
        Log_info("%d async_nop requests", cnt);
    }
    co_return rusty::Result<RpcAsyncNopResponse, i32>::Ok(RpcAsyncNopResponse{});
}

rusty::Result<BenchmarkService::RpcSleepResponse, i32>
BenchmarkService::sleep(const RpcSleepRequest& req) {
    int full_sec = static_cast<int>(req.sec);
    int usec = static_cast<int>((req.sec - full_sec) * 1000 * 1000);
    if (full_sec > 0) {
        ::sleep(full_sec);
    }
    if (usec > 0) {
        usleep(usec);
    }
    return rusty::Result<RpcSleepResponse, i32>::Ok(RpcSleepResponse{});
}

void BenchmarkService::deferred_echo(
    const RpcDeferredEchoRequest& req,
    RpcDeferredEchoResponse& resp,
    rrr::DeferredReply defer) {
    resp.result = req.val * 2;
    defer.reply();
}
