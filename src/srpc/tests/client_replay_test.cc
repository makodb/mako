// Public reconnect coverage matching tests/client_replay_rust.rs. The service,
// channels, poll thread, archives, and future completion all use the real runtime.
#include <gtest/gtest.h>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/sync/weak.hpp>
#include <rusty/string.hpp>
#include "../srpc.hpp"
#include "reactor_watchdog.h"

import std;
import srpc.serializable;
import srpc.inmemory_channel;
import srpc.request_queue;
import srpc.connection_metrics;
import srpc.reconnect_policy;
import srpc.misc;
import srpc.connection_state;

using namespace srpc;

namespace {
constexpr i32 kReplayRpc = 0x00e00091;

class ReplayEcho final : public Service {
public:
    explicit ReplayEcho(std::shared_ptr<std::vector<i64>> calls) : calls_(std::move(calls)) {}
    i32 __reg_to__(Server& server, size_t index) override {
        return server.reg_fast_rpc(kReplayRpc, index);
    }
    void __dispatch__(i32 rpc, rusty::Box<Request> request,
                      WeakServerConnection connection) const override {
        EXPECT_EQ(rpc, kReplayRpc);
        i64 value = 0;
        {
            BinaryReadArchive input{.source_ = make_source_proxy_buffer(&request->src)};
            Deserialize_::deserialize(value, input);
        }
        calls_->push_back(value);
        auto owner = connection.upgrade();
        ASSERT_TRUE(owner.is_some());
        owner.unwrap()->reply(*request, 0, ServerReplyFn{[value](BinaryWriteArchive& output) {
            Serialize_::serialize(value * 2, output);
        }});
    }
private:
    std::shared_ptr<std::vector<i64>> calls_;
};

class ClientReplayTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_;
    rusty::Option<rusty::Arc<Client>> client_;
    std::unique_ptr<Server> server_;
    std::shared_ptr<std::vector<i64>> calls_ = std::make_shared<std::vector<i64>>();
    std::shared_ptr<std::atomic<unsigned>> writes_ = std::make_shared<std::atomic<unsigned>>(0);

    void connect_then_disconnect(BufferingConfig config) {
        poll_ = rusty::Some(PollThread::create());
        auto poll = poll_.as_ref().unwrap().clone();
        auto board = rusty::Arc<InMemorySwitchboard>::make(InMemorySwitchboard::new_());
        server_.reset(new Server(Server::new_(rusty::Some(poll.clone()))));
        server_->set_channel_factory(make_inmemory_factory_proxy(
            rusty::Arc<InMemoryFactory>::make(InMemoryFactory::new_(board.clone()))));
        server_->reg_service_typed(rusty::make_box<ReplayEcho>(calls_));
        constexpr auto address = "inmemory://client-replay";
        ASSERT_EQ(server_->start(reinterpret_cast<const int8_t*>(address)), 0);
        client_ = rusty::Some(Client::create(poll.clone()));
        auto client = client_.as_ref().unwrap().clone();
        client->set_channel_factory(make_inmemory_factory_proxy(
            rusty::Arc<InMemoryFactory>::make(InMemoryFactory::new_(board.clone()))));
        ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(address), true), 0);
        client->set_buffering_config(config);
        client->connection().unwrap()->close();
        ASSERT_FALSE(client->connected());
    }

    rusty::Arc<Future> queue(i64 value) {
        auto writes = writes_;
        auto result = client_.as_ref().unwrap()->request(kReplayRpc, FutureAttr{},
            [value, writes](BinaryWriteArchive& output) {
                writes->fetch_add(1);
                Serialize_::serialize(value, output);
            });
        EXPECT_TRUE(result.is_ok());
        return result.unwrap();
    }

    void TearDown() override {
        if (client_.is_some()) client_.as_ref().unwrap()->close();
        client_ = rusty::None;
        server_.reset();
        if (poll_.is_some()) poll_.as_ref().unwrap()->shutdown();
        poll_ = rusty::None;
    }
};

TEST_F(ClientReplayTest, ReconnectReplaysOwnedBodiesInOrderOnce) {
    ASSERT_NO_FATAL_FAILURE(connect_then_disconnect(BufferingConfig::defaults()));
    auto client = client_.as_ref().unwrap().clone();
    std::vector<rusty::Arc<Future>> futures;
    for (i64 value : {11, 22, 33}) futures.push_back(queue(value));
    EXPECT_EQ(writes_->load(), 3u);
    EXPECT_TRUE(calls_->empty());
    EXPECT_EQ(client->pending_request_count(), 3u);
    ASSERT_EQ(client->reconnect({}), 0);
    EXPECT_EQ(writes_->load(), 3u);
    EXPECT_EQ(*calls_, (std::vector<i64>{11, 22, 33}));
    i64 expected = 22;
    for (const auto& future : futures) {
        ASSERT_TRUE(future->ready());
        ASSERT_EQ(future->get_error_code(), 0);
        i64 value = 0;
        deserialize_from(future->get_reply(), value);
        EXPECT_EQ(value, expected);
        expected += 22;
    }
    EXPECT_EQ(client->pending_request_count(), 0u);
    auto connection = client->connection().unwrap();
    EXPECT_EQ(connection->replay_pending_requests(), 0u);
    EXPECT_EQ(connection->pending_future_count(), 0u);
    EXPECT_EQ(connection->metrics().in_flight_requests(), 0u);
    EXPECT_EQ(client->metrics().requests_sent(), 3u);
}

TEST_F(ClientReplayTest, InlineFutureAndCallbackRepliesBalanceAdmission) {
    ASSERT_NO_FATAL_FAILURE(connect_then_disconnect(BufferingConfig::defaults()));
    auto client = client_.as_ref().unwrap().clone();
    ASSERT_EQ(client->reconnect({}), 0);
    auto future = queue(21);
    ASSERT_TRUE(future->ready());
    EXPECT_EQ(future->get_error_code(), 0);
    EXPECT_EQ(client->metrics().requests_sent(), 1u);
    EXPECT_EQ(client->metrics().requests_completed(), 1u);
    EXPECT_EQ(client->metrics().in_flight_requests(), 0u);

    i64 callback_value = -1;
    auto sent = client->request_async(kReplayRpc,
        [](BinaryWriteArchive& output) { Serialize_::serialize(i64{7}, output); },
        AsyncReplyCallback{[&](i32 error, const uint8_t* bytes, size_t size) {
            EXPECT_EQ(error, 0);
            ASSERT_EQ(size, sizeof(i64));
            std::memcpy(&callback_value, bytes, sizeof(callback_value));
        }});
    ASSERT_TRUE(sent.is_ok());
    EXPECT_EQ(callback_value, 14);
    EXPECT_EQ(client->metrics().requests_sent(), 2u);
    EXPECT_EQ(client->metrics().requests_completed(), 2u);
    EXPECT_EQ(client->metrics().in_flight_requests(), 0u);

    auto missing = client->request(kReplayRpc + 1, FutureAttr{}, [](BinaryWriteArchive&) {});
    ASSERT_TRUE(missing.is_ok());
    auto failed = missing.unwrap();
    ASSERT_TRUE(failed->ready());
    EXPECT_EQ(failed->get_error_code(), 2);
    EXPECT_EQ(client->metrics().requests_sent(), 3u);
    EXPECT_EQ(client->metrics().requests_failed(), 1u);
    EXPECT_EQ(client->metrics().in_flight_requests(), 0u);
}

TEST_F(ClientReplayTest, ExpiredRequestNeverReachesService) {
    auto config = BufferingConfig::defaults();
    config.default_ttl_ms = 1;
    ASSERT_NO_FATAL_FAILURE(connect_then_disconnect(config));
    auto future = queue(7);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    ASSERT_EQ(client_.as_ref().unwrap()->reconnect({}), 0);
    ASSERT_TRUE(future->ready());
    EXPECT_EQ(future->get_error_code(), kRequestQueueExpiredError);
    EXPECT_TRUE(calls_->empty());
    EXPECT_EQ(writes_->load(), 1u);
}

TEST_F(ClientReplayTest, OverflowResolvesEvictedFutureAndReplaysSurvivor) {
    auto config = BufferingConfig::defaults();
    config.max_pending = 1;
    config.overflow = OverflowStrategy::DROP_OLDEST;
    ASSERT_NO_FATAL_FAILURE(connect_then_disconnect(config));
    auto first = queue(9);
    auto second = queue(10);
    ASSERT_TRUE(first->ready());
    EXPECT_EQ(first->get_error_code(), kRequestQueueRejectedError);
    EXPECT_FALSE(second->ready());
    ASSERT_EQ(client_.as_ref().unwrap()->reconnect({}), 0);
    ASSERT_TRUE(second->ready());
    EXPECT_EQ(second->get_error_code(), 0);
    EXPECT_EQ(*calls_, (std::vector<i64>{10}));
    EXPECT_EQ(writes_->load(), 2u);
}

// Controlled delivery of callbacks that a real transport can select before
// replacement and deliver afterward. Client state, serialization, request
// admission, and future completion still run through generated production code.
struct RetainedChannelState {
    std::atomic<bool> closed{false};
    std::atomic<bool> dropped{false};
    std::mutex mutex;
    OnFrameCallback on_frame;
    OnClosedCallback on_closed;
    std::function<void()> on_send;
    std::vector<std::vector<std::uint8_t>> sent_frames;

    OnFrameCallback frame_callback() {
        std::lock_guard<std::mutex> guard(mutex);
        return on_frame.clone();
    }
    OnClosedCallback close_callback() {
        std::lock_guard<std::mutex> guard(mutex);
        return on_closed.clone();
    }
};

class RetainedChannel final : public ChannelConnectionBase {
public:
    explicit RetainedChannel(std::shared_ptr<RetainedChannelState> state)
        : state_(std::move(state)) {}
    ~RetainedChannel() override { state_->dropped.store(true); }
    ChannelError send_frame(const ChannelFrame& frame) const override {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            state_->sent_frames.emplace_back(frame.payload, frame.payload + frame.size);
            callback = state_->on_send;
        }
        if (callback) callback();
        return ChannelError::None;
    }
    void flush() const override {}
    void close() const override {
        if (!state_->closed.exchange(true)) {
            auto callback = state_->close_callback();
            if (callback.has_value()) callback.callable()(ChannelError::None);
        }
    }
    bool is_closed() const override { return state_->closed.load(); }
    rusty::String peer_address() const override { return "retained-channel"; }
    void set_on_frame(OnFrameCallback callback) override {
        std::lock_guard<std::mutex> guard(state_->mutex);
        state_->on_frame = std::move(callback);
    }
    void set_on_closed(OnClosedCallback callback) override {
        std::lock_guard<std::mutex> guard(state_->mutex);
        state_->on_closed = std::move(callback);
    }
    void set_on_error(OnErrorCallback) override {}
private:
    std::shared_ptr<RetainedChannelState> state_;
};

struct RetainedChannels {
    std::mutex mutex;
    std::vector<std::shared_ptr<RetainedChannelState>> channels;

    std::shared_ptr<RetainedChannelState> at(std::size_t index) {
        std::lock_guard<std::mutex> guard(mutex);
        return channels.at(index);
    }
    std::shared_ptr<RetainedChannelState> last() {
        std::lock_guard<std::mutex> guard(mutex);
        return channels.back();
    }
    std::size_t size() {
        std::lock_guard<std::mutex> guard(mutex);
        return channels.size();
    }
};

class RetainedChannelFactory final : public ChannelFactoryBase {
public:
    explicit RetainedChannelFactory(std::shared_ptr<RetainedChannels> channels)
        : channels_(std::move(channels)) {}
    ConnectResult connect(std::string_view) override {
        auto state = std::make_shared<RetainedChannelState>();
        {
            std::lock_guard<std::mutex> guard(channels_->mutex);
            channels_->channels.push_back(state);
        }
        ChannelConnectionProxy channel = rusty::make_box<RetainedChannel>(std::move(state));
        return {.connection = rusty::Some(std::move(channel)), .error = ChannelError::None};
    }
    rusty::Option<ChannelListenerProxy> make_listener() override { return rusty::None; }
    rusty::String backend_name() const override { return "retained-callback-test"; }
private:
    std::shared_ptr<RetainedChannels> channels_;
};

class ClientBindingTest : public ClientReplayTest {
protected:
    std::shared_ptr<RetainedChannels> channels_ = std::make_shared<RetainedChannels>();

    void SetUp() override {
        poll_ = rusty::Some(PollThread::create());
        client_ = rusty::Some(Client::create(poll_.as_ref().unwrap().clone()));
        auto client = client_.as_ref().unwrap().clone();
        client->set_channel_factory(rusty::make_box<RetainedChannelFactory>(channels_));
        client->set_reconnect_policy(ReconnectPolicy::no_retry());
        constexpr auto address = "tracked://client-binding";
        ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(address), true), 0);
    }

    auto reply_to_last_request(
        const std::shared_ptr<RetainedChannelState>& channel, i64 server_id) {
        std::vector<std::uint8_t> request;
        {
            std::lock_guard<std::mutex> guard(channel->mutex);
            request = channel->sent_frames.back();
        }
        auto source = BufferSource::new_(request.data(), request.size());
        BinaryReadArchive reader{.source_ = make_source_proxy_buffer(&source)};
        auto xid = v64::new_(0);
        Deserialize_::deserialize(xid, reader);
        BufferSink sink{};
        {
            BinaryWriteArchive writer{.sink_ = make_sink_proxy_buffer(&sink)};
            Serialize_::serialize(xid, writer);
            Serialize_::serialize(v32::new_(0), writer);
            Serialize_::serialize(v64::new_(server_id), writer);
        }
        return std::move(sink.bytes);
    }
};

TEST_F(ClientBindingTest, RetiredCloseCallbackPreservesReplacementAndPendingFuture) {
    auto client = client_.as_ref().unwrap().clone();
    auto connection = client->connection().unwrap();
    auto old = channels_->at(0);
    auto delayed_close = old->close_callback();
    ASSERT_TRUE(delayed_close.has_value());

    // Match TCP's closed-before-callback ordering while delaying the callback.
    old->closed.store(true);
    connection->close();
    ASSERT_FALSE(client->connected());
    ASSERT_TRUE(old->dropped.load());
    ASSERT_EQ(client->reconnect({}), 0);
    ASSERT_TRUE(client->connected());
    auto current = channels_->at(1);
    auto future = queue(7);
    ASSERT_FALSE(future->ready());
    ASSERT_EQ(connection->pending_future_count(), 1u);

    delayed_close.callable()(ChannelError::None);
    EXPECT_TRUE(client->connected());
    EXPECT_FALSE(current->closed.load());
    EXPECT_FALSE(future->ready());
    EXPECT_EQ(connection->pending_future_count(), 1u);

    auto reply = reply_to_last_request(current, 81);
    ChannelFrame frame{reply.data(), reply.size()};
    current->frame_callback().callable()(frame);
    ASSERT_TRUE(future->ready());
    EXPECT_EQ(future->get_error_code(), 0);
    EXPECT_EQ(connection->pending_future_count(), 0u);
}

TEST_F(ClientBindingTest, RetiredFrameCannotCompleteReplacementRequestOrChangeIdentity) {
    auto client = client_.as_ref().unwrap().clone();
    auto connection = client->connection().unwrap();
    auto old = channels_->at(0);
    auto stale_frame = old->frame_callback();
    ASSERT_TRUE(stale_frame.has_value());
    connection->close();
    ASSERT_EQ(client->reconnect({}), 0);
    auto current = channels_->at(1);
    auto future = queue(7);
    ASSERT_FALSE(future->ready());
    auto reply = reply_to_last_request(current, 81);
    ChannelFrame frame{reply.data(), reply.size()};
    const auto received_before = connection->metrics().bytes_received();

    stale_frame.callable()(frame);
    EXPECT_TRUE(client->connected());
    EXPECT_FALSE(current->closed.load());
    EXPECT_FALSE(future->ready());
    EXPECT_EQ(connection->pending_future_count(), 1u);
    EXPECT_EQ(connection->server_instance_id(), 0u);
    EXPECT_EQ(connection->metrics().bytes_received(), received_before);

    current->frame_callback().callable()(frame);
    ASSERT_TRUE(future->ready());
    EXPECT_EQ(future->get_error_code(), 0);
    EXPECT_EQ(connection->pending_future_count(), 0u);
    EXPECT_EQ(connection->server_instance_id(), 81u);
}

TEST_F(ClientBindingTest, RetiredReplayHandsQueuedWorkToReplacementBinding) {
    srpc_test::Watchdog watchdog("RetiredReplayHandsQueuedWorkToReplacementBinding", 10);
    auto client = client_.as_ref().unwrap().clone();
    auto connection = client->connection().unwrap();
    connection->set_buffering_config(BufferingConfig::defaults());
    connection->close();
    auto old_future = queue(7);
    ASSERT_FALSE(old_future->ready());

    struct Observation {
        i32 nested_result = -1;
        rusty::Option<rusty::Arc<Future>> replacement_future;
    };
    auto observed = std::make_shared<Observation>();
    auto installed = std::make_shared<std::atomic<bool>>(false);
    auto channels = channels_;
    auto weak = rusty::sync::downgrade(connection);
    auto writes = writes_;
    client->add_on_connected(OnConnectedCallbackFn{[installed, channels, weak, writes, observed]() {
        if (installed->exchange(true)) return;
        auto channel = channels->last();
        std::lock_guard<std::mutex> guard(channel->mutex);
        channel->on_send = [weak, writes, observed]() {
            auto owner = weak.upgrade();
            ASSERT_TRUE(owner.is_some());
            auto reentering = owner.unwrap();
            reentering->close();
            auto queued = clientconn_request_via_channel(*reentering, kReplayRpc, FutureAttr{},
                [writes](BinaryWriteArchive& output) {
                    writes->fetch_add(1);
                    Serialize_::serialize(i64{19}, output);
                });
            ASSERT_TRUE(queued.is_ok());
            observed->replacement_future = rusty::Some(queued.unwrap());
            observed->nested_result = reentering->reconnect({});
        };
    }});

    const auto outer_result = connection->reconnect({});
    client->clear_connection_callbacks();
    EXPECT_EQ(observed->nested_result, 0);
    EXPECT_EQ(outer_result, CLIENT_ERR_CANCELED);
    ASSERT_TRUE(observed->replacement_future.is_some());
    auto future = observed->replacement_future.unwrap();
    EXPECT_TRUE(old_future->ready());
    EXPECT_EQ(old_future->get_error_code(), CLIENT_ERR_NOT_CONNECTED);
    EXPECT_TRUE(connection->connected());
    EXPECT_EQ(connection->pending_request_count(), 0u);
    EXPECT_EQ(connection->pending_future_count(), 1u);
    EXPECT_FALSE(future->ready());
    EXPECT_EQ(writes_->load(), 2u);
    ASSERT_EQ(channels_->size(), 3u);
    auto current = channels_->last();
    std::vector<std::uint8_t> request;
    {
        std::lock_guard<std::mutex> guard(current->mutex);
        ASSERT_EQ(current->sent_frames.size(), 1u);
        request = current->sent_frames.front();
    }
    auto source = BufferSource::new_(request.data(), request.size());
    BinaryReadArchive reader{.source_ = make_source_proxy_buffer(&source)};
    auto xid = v64::new_(0);
    i32 rpc = 0;
    i64 value = 0;
    Deserialize_::deserialize(xid, reader);
    Deserialize_::deserialize(rpc, reader);
    Deserialize_::deserialize(value, reader);
    EXPECT_EQ(rpc, kReplayRpc);
    EXPECT_EQ(value, 19);
    EXPECT_EQ(connection->replay_pending_requests(), 0u);

    auto reply = reply_to_last_request(current, 83);
    current->frame_callback().callable()(ChannelFrame{reply.data(), reply.size()});
    ASSERT_TRUE(future->ready());
    EXPECT_EQ(future->get_error_code(), 0);
    EXPECT_EQ(connection->pending_future_count(), 0u);
    connection->close();
}

// Pause the real owner thread before queuing public close jobs. Shutdown after
// release drains all accepted jobs, so these checks do not assume std::set jobs
// execute in insertion order.
class PausedPollOwner {
public:
    explicit PausedPollOwner(const rusty::Arc<PollThread>& poll)
        : release_(std::make_shared<std::promise<void>>()) {
        auto entered = std::make_shared<std::promise<void>>();
        entered_ = entered->get_future();
        auto released = release_->get_future().share();
        auto job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([entered, released]() {
            entered->set_value();
            released.wait();
        }));
        poll->add(job);
    }
    ~PausedPollOwner() { release(); }
    bool parked() { return entered_.wait_for(std::chrono::seconds(3)) == std::future_status::ready; }
    void release() {
        if (!released_) {
            released_ = true;
            release_->set_value();
        }
    }
private:
    std::shared_ptr<std::promise<void>> release_;
    std::future<void> entered_;
    bool released_ = false;
};

TEST_F(ClientBindingTest, DeferredPublicCloseCannotRetireReplacementBinding) {
    srpc_test::Watchdog watchdog("DeferredPublicCloseCannotRetireReplacementBinding", 10);
    auto client = client_.as_ref().unwrap().clone();
    auto connection = client->connection().unwrap();
    auto poll = poll_.as_ref().unwrap().clone();
    PausedPollOwner paused(poll);
    ASSERT_TRUE(paused.parked());
    client->close();
    connection->close();
    ASSERT_EQ(connection->reconnect({}), 0);
    auto future = queue(23);
    paused.release();
    poll->shutdown();
    EXPECT_TRUE(connection->connected());
    EXPECT_FALSE(channels_->last()->closed.load());
    EXPECT_FALSE(future->ready());
    EXPECT_EQ(connection->pending_future_count(), 1u);
    connection->close();
}

TEST_F(ClientBindingTest, RepeatedPublicCloseKeepsQueuedRetirementValid) {
    srpc_test::Watchdog watchdog("RepeatedPublicCloseKeepsQueuedRetirementValid", 10);
    auto client = client_.as_ref().unwrap().clone();
    auto connection = client->connection().unwrap();
    auto old = channels_->at(0);
    auto poll = poll_.as_ref().unwrap().clone();
    PausedPollOwner paused(poll);
    ASSERT_TRUE(paused.parked());
    client->close();
    client->close();
    paused.release();
    poll->shutdown();
    EXPECT_EQ(connection->connection_state(), ConnectionState::DISCONNECTED);
    EXPECT_TRUE(old->closed.load());
    connection->close();
}
} // namespace
