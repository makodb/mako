// Channel-mode inbound demux test for `ServerConnection`.
//
// Exercises `bind_channel(...)`'s on_frame callback wiring: a
// synthesized request frame fed through the stub's on_frame fires
// `ServerConnection::decode_request_and_dispatch` which routes to
// the registered service. We test:
//
//   * unhandled rpc_id → server replies ENOENT (reply frame
//     captured by the stub)
//   * heartbeat rpc_id → server replies 0 (kInternalHeartbeatRpcId)
//   * malformed frame (too short for xid+rpc_id) → server replies
//     EINVAL (after parsing xid)
//   * registered service → service's `__dispatch__` runs (we capture
//     the dispatch via a recording helper)

#include <stdint.h>

#include <gtest/gtest.h>


#include <rusty/arc.hpp>
#include <rusty/sync/weak.hpp>  // rusty::sync::downgrade
#include <rusty/box.hpp>
#include <rusty/refcell.hpp>

#include "../rrr.hpp"

import std;
import rusty;
import rrr.internal_protocol;

namespace rrr {
namespace {

// Stub that captures send_frame payloads AND lets the test deliver
// inbound frames via `deliver(payload, size)` (which fires the
// installed `on_frame` callback).
class StubChannel {
 public:
    ChannelError send_frame(const ChannelFrame& f) {
        if (closed_) return ChannelError::ConnectionReset;
        std::vector<std::uint8_t> bytes(f.payload, f.payload + f.size);
        captured_.push_back(std::move(bytes));
        return ChannelError::None;
    }
    void   flush()              {}
    void   close()              { closed_ = true; }
    bool   is_closed() const    { return closed_; }
    std::string peer_address() const { return "stub"; }
    void set_on_frame (OnFrameCallback cb) { on_frame_  = std::move(cb); }
    void set_on_closed(OnClosedCallback cb) { on_closed_ = std::move(cb); }
    void set_on_error (OnErrorCallback cb) { on_error_  = std::move(cb); }

    void deliver(const std::vector<std::uint8_t>& payload) {
        if (!on_frame_) return;
        ChannelFrame f{payload.data(), payload.size()};
        on_frame_(f);
    }

    const std::vector<std::vector<std::uint8_t>>& captured() const { return captured_; }
    std::size_t count() const { return captured_.size(); }

 private:
    OnFrameCallback  on_frame_;
    OnClosedCallback on_closed_;
    OnErrorCallback  on_error_;
    std::vector<std::vector<std::uint8_t>> captured_;
    bool closed_ = false;
};

class StubChannelAdapter : public ChannelConnectionBase {
 public:
    explicit StubChannelAdapter(std::shared_ptr<StubChannel> p)
        : stub_(std::move(p)) {}
    ChannelError send_frame(const ChannelFrame& f) override { return stub_->send_frame(f); }
    void   flush() override              { stub_->flush(); }
    void   close() override              { stub_->close(); }
    bool   is_closed() const override    { return stub_->is_closed(); }
    std::string peer_address() const override { return stub_->peer_address(); }
    void set_on_frame (OnFrameCallback  cb) override { stub_->set_on_frame (std::move(cb)); }
    void set_on_closed(OnClosedCallback cb) override { stub_->set_on_closed(std::move(cb)); }
    void set_on_error (OnErrorCallback  cb) override { stub_->set_on_error (std::move(cb)); }
 private:
    std::shared_ptr<StubChannel> stub_;
};

inline ChannelConnectionProxy make_stub_proxy(
        std::shared_ptr<StubChannel> stub) {
    return rusty::make_box<StubChannelAdapter>(std::move(stub));
}

// Build a wire frame body in the channel-mode format:
//   [xid:v64][rpc_id:i32][user-data...]
inline std::vector<std::uint8_t> build_request_frame(
        i64 xid, i32 rpc_id, const std::string& user = std::string()) {
    rrr::BufferSink sink;
    rrr::BinaryWriteArchive war(rrr::make_sink_proxy(&sink));
    rrr::Serialize_::serialize(v64(xid), war);
    rrr::Serialize_::serialize(rpc_id, war);
    if (!user.empty()) rrr::Serialize_::serialize(user, war);
    return std::vector<std::uint8_t>(
        sink.bytes.data(), sink.bytes.data() + sink.bytes.len());
}

// Tiny test service that records each dispatch and replies with
// the user payload echoed back.
class RecordingService {
 public:
    static constexpr i32 kEchoRpcId = 1234567;

    int __reg_to__(Server&, std::size_t) { return 0; }

    void __dispatch__(i32 rpc_id, rusty::Box<Request> req,
                      WeakServerConnection sconn) {
        std::lock_guard<std::mutex> lk(mu_);
        last_rpc_id_ = rpc_id;
        last_xid_    = req->xid;
        std::string echo;
        rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->src));
        rrr::Deserialize_::deserialize(echo, __req_ar__);
        last_payload_ = echo;
        ++dispatch_count_;
        // Reply back to the client, echoing the payload.
        auto sconn_opt = sconn.upgrade();
        if (sconn_opt.is_some()) {
            sconn_opt.unwrap()->reply(*req, /*err=*/0,
                [&](BinaryWriteArchive& out) { rrr::Serialize_::serialize(echo, out); });
        }
    }

    int    dispatch_count() {
        std::lock_guard<std::mutex> lk(mu_);
        return dispatch_count_;
    }
    i32    last_rpc_id() {
        std::lock_guard<std::mutex> lk(mu_);
        return last_rpc_id_;
    }
    i64    last_xid() {
        std::lock_guard<std::mutex> lk(mu_);
        return last_xid_;
    }
    std::string last_payload() {
        std::lock_guard<std::mutex> lk(mu_);
        return last_payload_;
    }

 private:
    std::mutex mu_;
    int dispatch_count_ = 0;
    i32 last_rpc_id_    = 0;
    i64 last_xid_       = 0;
    std::string last_payload_;
};
static_assert(ServiceLike<RecordingService>);

// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block with the C++ definition.
#if RUSTYCPP_RUST
const kFakeServerInstanceId: u64 = 0xfeedface00abcdef;
#endif
/*RUSTYCPP:GEN-BEGIN id=rpc_server_channel_recv_test.1 version=1 rust_sha256=4149f6423bcdaa0a6318589d370183f1eccbee366113fcf349ed9c1d0d22c1f8*/
constexpr uint64_t kFakeServerInstanceId = static_cast<uint64_t>(18369614217795587567);
/*RUSTYCPP:GEN-END id=rpc_server_channel_recv_test.1*/

class ServerChannelRecvTest : public ::testing::Test {
 protected:
    void SetUp() override {
        // No registered services; rpc_to_service_ is empty until the
        // individual test calls register_service().
        rusty::HashMap<i32, std::size_t> rpc_to_service;
        rusty::HashSet<i32> fast_rpc_ids;
        rusty::Vec<rusty::RefCell<ServiceProxy>> services;
        auto pending = rusty::Arc<ServerPendingRequestsAtomic>::make(0);
        auto drop = rusty::Arc<ServerDropHeartbeatRepliesAtomic>::make(false);
        ctx_ = rusty::Some(rusty::Arc<RpcServiceContext>::new_(
            RpcServiceContext::new_(
                std::move(rpc_to_service),
                std::move(fast_rpc_ids),
                std::move(services),
                std::string("0.0.0.0:0"),
                std::move(pending),
                std::move(drop),
                kFakeServerInstanceId)));
        sconn_ = rusty::Some(rusty::Arc<ServerConnection>::make(
            ctx_.as_ref().unwrap().clone(), /*socket=*/-1));
        // Wire `weak_self_` so the on_frame callback can upgrade.
        // Production goes through the listener accept path which
        // sets this; we set it manually here.
        const_cast<ServerConnection&>(*sconn_.as_ref().unwrap().get())
            .install_self_weak_for_testing(rusty::sync::downgrade(sconn_.as_ref().unwrap()));
    }

    void TearDown() override {
        sconn_ = rusty::None;
        ctx_ = rusty::None;
    }

    ServerConnection& mut_sconn() {
        return const_cast<ServerConnection&>(*sconn_.as_ref().unwrap().get());
    }

    rusty::Option<rusty::Arc<RpcServiceContext>> ctx_;
    rusty::Option<rusty::Arc<ServerConnection>>  sconn_;
};

// ---------------------------------------------------------------------------
// Unhandled rpc_id triggers an ENOENT reply.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelRecvTest, UnhandledRpcRepliesEnoent) {
    auto stub = std::make_shared<StubChannel>();
    mut_sconn().bind_channel(make_stub_proxy(stub));

    auto frame = build_request_frame(/*xid=*/77, /*rpc_id=*/9999, "ignored");
    stub->deliver(frame);

    ASSERT_EQ(stub->count(), 1u);
    rrr::BufferSource src(stub->captured().front().data(),
               stub->captured().front().size());
    rrr::BinaryReadArchive rar(rrr::make_source_proxy(&src));
    v64 v_xid;
    v32 v_err;
    v64 v_inst;
    rrr::Deserialize_::deserialize(v_xid, rar);
    rrr::Deserialize_::deserialize(v_err, rar);
    rrr::Deserialize_::deserialize(v_inst, rar);
    EXPECT_EQ(static_cast<i64>(v_xid.get()), 77);
    EXPECT_EQ(v_err.get(), ENOENT);
    EXPECT_EQ(static_cast<uint64_t>(v_inst.get()), kFakeServerInstanceId);
}

// ---------------------------------------------------------------------------
// Internal heartbeat rpc_id replies with error_code=0.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelRecvTest, HeartbeatRpcRepliesZero) {
    auto stub = std::make_shared<StubChannel>();
    mut_sconn().bind_channel(make_stub_proxy(stub));

    auto frame = build_request_frame(/*xid=*/3,
                                     /*rpc_id=*/static_cast<i32>(kInternalHeartbeatRpcId));
    stub->deliver(frame);

    ASSERT_EQ(stub->count(), 1u);
    rrr::BufferSource src(stub->captured().front().data(),
               stub->captured().front().size());
    rrr::BinaryReadArchive rar(rrr::make_source_proxy(&src));
    v64 v_xid;
    v32 v_err;
    v64 v_inst;
    rrr::Deserialize_::deserialize(v_xid, rar);
    rrr::Deserialize_::deserialize(v_err, rar);
    rrr::Deserialize_::deserialize(v_inst, rar);
    EXPECT_EQ(static_cast<i64>(v_xid.get()), 3);
    EXPECT_EQ(v_err.get(), 0);
    EXPECT_EQ(static_cast<uint64_t>(v_inst.get()), kFakeServerInstanceId);
}

// ---------------------------------------------------------------------------
// Malformed frame (xid only, no rpc_id) → EINVAL reply.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelRecvTest, MalformedFrameRepliesEinval) {
    auto stub = std::make_shared<StubChannel>();
    mut_sconn().bind_channel(make_stub_proxy(stub));

    rrr::BufferSink sink;
    rrr::BinaryWriteArchive war(rrr::make_sink_proxy(&sink));
    rrr::Serialize_::serialize(v64(/*xid=*/55), war);  // only xid, no rpc_id
    std::vector<std::uint8_t> bytes(
        sink.bytes.data(), sink.bytes.data() + sink.bytes.len());
    stub->deliver(bytes);

    ASSERT_EQ(stub->count(), 1u);
    rrr::BufferSource src(stub->captured().front().data(),
               stub->captured().front().size());
    rrr::BinaryReadArchive rar(rrr::make_source_proxy(&src));
    v64 v_xid;
    v32 v_err;
    rrr::Deserialize_::deserialize(v_xid, rar);
    rrr::Deserialize_::deserialize(v_err, rar);
    EXPECT_EQ(static_cast<i64>(v_xid.get()), 55);
    EXPECT_EQ(v_err.get(), EINVAL);
}

// ---------------------------------------------------------------------------
// Registered fast-rpc service: dispatch fires inline and the reply
// is captured.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelRecvTest, RegisteredFastRpcDispatches) {
    // Replace ctx_ with one containing a registered service.
    sconn_ = rusty::None;
    ctx_ = rusty::None;

    auto svc_box = rusty::make_box<RecordingService>();
    auto* svc_raw = svc_box.get();
    auto svc_proxy = make_service_proxy_from_typed_box<RecordingService>(
        std::move(svc_box));

    rusty::Vec<rusty::RefCell<ServiceProxy>> services;
    services.push(rusty::RefCell<ServiceProxy>(std::move(svc_proxy)));
    rusty::HashMap<i32, std::size_t> rpc_to_service;
    rpc_to_service.insert(RecordingService::kEchoRpcId, std::size_t{0});
    rusty::HashSet<i32> fast_rpc_ids;
    fast_rpc_ids.insert(RecordingService::kEchoRpcId);
    auto pending = rusty::Arc<ServerPendingRequestsAtomic>::make(0);
    auto drop = rusty::Arc<ServerDropHeartbeatRepliesAtomic>::make(false);
    ctx_ = rusty::Some(rusty::Arc<RpcServiceContext>::new_(
        RpcServiceContext::new_(
            std::move(rpc_to_service),
            std::move(fast_rpc_ids),
            std::move(services),
            std::string("0.0.0.0:0"),
            std::move(pending),
            std::move(drop),
            kFakeServerInstanceId)));
    sconn_ = rusty::Some(rusty::Arc<ServerConnection>::make(
        ctx_.as_ref().unwrap().clone(), /*socket=*/-1));
    const_cast<ServerConnection&>(*sconn_.as_ref().unwrap().get())
        .install_self_weak_for_testing(rusty::sync::downgrade(sconn_.as_ref().unwrap()));

    auto stub = std::make_shared<StubChannel>();
    mut_sconn().bind_channel(make_stub_proxy(stub));

    auto frame = build_request_frame(/*xid=*/100,
                                     RecordingService::kEchoRpcId,
                                     "ping");
    stub->deliver(frame);

    EXPECT_EQ(svc_raw->dispatch_count(), 1);
    EXPECT_EQ(svc_raw->last_rpc_id(), RecordingService::kEchoRpcId);
    EXPECT_EQ(svc_raw->last_xid(), 100);
    EXPECT_EQ(svc_raw->last_payload(), "ping");

    // The handler's reply was captured by the stub.
    ASSERT_EQ(stub->count(), 1u);
    rrr::BufferSource src(stub->captured().front().data(),
               stub->captured().front().size());
    rrr::BinaryReadArchive rar(rrr::make_source_proxy(&src));
    v64 v_xid;
    v32 v_err;
    v64 v_inst;
    std::string echo;
    rrr::Deserialize_::deserialize(v_xid, rar);
    rrr::Deserialize_::deserialize(v_err, rar);
    rrr::Deserialize_::deserialize(v_inst, rar);
    rrr::Deserialize_::deserialize(echo, rar);
    EXPECT_EQ(static_cast<i64>(v_xid.get()), 100);
    EXPECT_EQ(v_err.get(), 0);
    EXPECT_EQ(echo, "ping");
}

}  // namespace
}  // namespace rrr
