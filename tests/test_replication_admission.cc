#include <std_compat.hpp>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <gtest/gtest.h>
#include <rusty/async.hpp>
#include <rusty/refcell.hpp>
#include <rusty/sync/atomic.hpp>
#include "deptran/paxos/replication_outbox.h"
#include "deptran/paxos/server.h"
#include "srpc/tests/reactor_watchdog.h"

import std;
import rusty;
import srpc.tcp_channel;

namespace {

// @safe - Future completion may arrive from the connection's poll thread.
struct AdmissionResult {
  rusty::sync::atomic::AtomicBool done{true};
  rusty::sync::atomic::AtomicI32 serializations{0};
  rusty::sync::atomic::AtomicI32 completions{0};
  rusty::sync::atomic::AtomicI32 callback_error{0};
  rusty::sync::atomic::AtomicI32 admission_error{0};
};

// @unsafe - A socketpair TcpConnection with no PollThread exercises the
// production TCP queue deterministically. On the Lion runtime such a connection
// has no reader or writer task and no write-through (srpc
// rpc/tcp_channel.rs, tcpconn_is_foreign_sender), so an accepted frame stays
// queued until this test calls flush(), the same substrate as srpc's own
// TcpConnectionTest. Before the Lion runtime the drain was handle_write().
class ReplicationAdmissionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    int sockets[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    peer_ = sockets[1];
    tcp_ = rusty::Some(rusty::Arc<srpc::TcpConnection>::new_(
        srpc::TcpConnection::new_(sockets[0], "admission-peer")));
    ASSERT_EQ(::fcntl(sockets[0], F_SETFL, O_NONBLOCK), 0);
    ASSERT_EQ(::fcntl(peer_, F_SETFL, O_NONBLOCK), 0);
    poll_ = rusty::Some(srpc::PollThread::create());
    client_ = rusty::Some(rusty::Arc<srpc::ClientConnection>::new_(
        srpc::ClientConnection::new_(poll_.as_ref().unwrap().clone())));
    auto& client = const_cast<srpc::ClientConnection&>(*client_.as_ref().unwrap());
    client.bind_channel(srpc::make_tcp_connection_channel_proxy(tcp_.as_ref().unwrap().clone()));
    client.force_connected_for_testing();
    // An empty queue accepts one complete frame, then rejects the next until
    // flush() drains it.
    tcp().set_outbound_high_water(1);
    const uint8_t byte = 0x71;
    ASSERT_EQ(tcp().send_frame({&byte, 1}), srpc::ChannelError::None);
  }

  // @unsafe - Resume every owned waiter before releasing the fixture's owners.
  void TearDown() override {
    if (client_.is_some()) {
      client_.as_ref().unwrap()->close();
      EXPECT_TRUE(finish());
      client_ = rusty::None;
    }
    if (poll_.is_some()) {
      poll_.as_ref().unwrap()->shutdown();
      poll_ = rusty::None;
    }
    tcp_ = rusty::None;
    if (peer_ >= 0)
      ::close(peer_);
  }

  // @unsafe - Mutable native channel facade; all accesses stay on this thread.
  srpc::TcpConnection& tcp() {
    return const_cast<srpc::TcpConnection&>(*tcp_.as_ref().unwrap());
  }

  // @unsafe - The fiber owns both the connection and observations across yield.
  void start(uint64_t timeout_us) {
    result_->done.store(false);
    srpc::Fiber::create_run([client = client_.as_ref().unwrap().clone(),
                            result = result_, timeout_us] {
      srpc::FutureAttr attr;
      attr.callback = srpc::FutureCallback::from_callable(
          [result](rusty::Arc<srpc::Future> future) {
            result->callback_error.store(future->get_error_code());
            result->completions.fetch_add(1);
          });
      auto future = client->request_with_admission(
          0x57, attr, timeout_us, [result](srpc::BinaryWriteArchive& archive) {
            result->serializations.fetch_add(1);
            srpc::Serialize_::serialize(int32_t{0x12345678}, archive);
          });
      if (future.is_err())
        result->admission_error.store(future.unwrap_err());
      result->done.store(true);
    });
  }

  // @unsafe - Drive the actual timeout-event scheduler with a bounded deadline.
  bool finish(bool need_callback = false) {
    const auto deadline = srpc::Time::now(true) + 2'000'000;
    while ((!result_->done.load() ||
            (need_callback && result_->completions.load() == 0)) &&
           srpc::Time::now(true) < deadline)
      srpc::Reactor::get_reactor()->run_loop(false, true);
    return result_->done.load() &&
        (!need_callback || result_->completions.load() != 0);
  }

  int peer_ = -1;
  rusty::Option<rusty::Arc<srpc::TcpConnection>> tcp_;
  rusty::Option<rusty::Arc<srpc::PollThread>> poll_;
  rusty::Option<rusty::Arc<srpc::ClientConnection>> client_;
  rusty::Arc<AdmissionResult> result_ = rusty::Arc<AdmissionResult>::make();
};

// @unsafe - Observe the actual framed byte stream after queue pressure clears.
TEST_F(ReplicationAdmissionTest, EncodesOnceAndAdmitsExactlyOneFrameAfterDrain) {
  start(100'000);
  EXPECT_FALSE(result_->done.load());
  tcp().flush();
  std::array<uint8_t, 64> wire{};
  ASSERT_EQ(::read(peer_, wire.data(), wire.size()), 5);
  ASSERT_TRUE(finish());
  EXPECT_EQ(result_->admission_error.load(), 0);
  EXPECT_EQ(result_->serializations.load(), 1);
  tcp().flush();
  const auto count = ::read(peer_, wire.data(), wire.size());
  ASSERT_GT(count, 4);
  int32_t body_length = 0;
  std::memcpy(&body_length, wire.data(), sizeof(body_length));
  ASSERT_EQ(count, body_length + 4);
  srpc::BufferSource source(wire.data() + 4, body_length);
  srpc::BinaryReadArchive archive(srpc::make_source_proxy_buffer(&source));
  srpc::v64 xid;
  int32_t rpc = 0;
  int32_t value = 0;
  srpc::Deserialize_::deserialize(xid, archive);
  srpc::Deserialize_::deserialize(rpc, archive);
  srpc::Deserialize_::deserialize(value, archive);
  EXPECT_EQ(rpc, 0x57);
  EXPECT_EQ(value, 0x12345678);
  EXPECT_EQ(::read(peer_, wire.data(), wire.size()), -1);
  EXPECT_EQ(errno, EAGAIN);
}

// @unsafe - A deadline must complete once without appending the rejected frame.
TEST_F(ReplicationAdmissionTest, DeadlineCompletesWithoutSendingRejectedFrame) {
  start(2'000);
  ASSERT_TRUE(finish(true));
  EXPECT_EQ(result_->serializations.load(), 1);
  EXPECT_EQ(result_->completions.load(), 1);
  EXPECT_EQ(result_->callback_error.load(), srpc::CLIENT_ERR_AGAIN);
  EXPECT_EQ(result_->admission_error.load(), 0);
  tcp().flush();
  std::array<uint8_t, 64> wire{};
  EXPECT_EQ(::read(peer_, wire.data(), wire.size()), 5);
  EXPECT_EQ(::read(peer_, wire.data(), wire.size()), -1);
  EXPECT_EQ(errno, EAGAIN);
  client_.as_ref().unwrap()->close();
  EXPECT_EQ(result_->completions.load(), 1);
}

// @unsafe - Closing owns future completion even while admission is suspended.
TEST_F(ReplicationAdmissionTest, CloseDuringAdmissionCompletesOnlyOnce) {
  start(100'000);
  EXPECT_FALSE(result_->done.load());
  client_.as_ref().unwrap()->close();
  ASSERT_TRUE(finish(true));
  EXPECT_EQ(result_->serializations.load(), 1);
  EXPECT_EQ(result_->completions.load(), 1);
  EXPECT_NE(result_->callback_error.load(), 0);
  EXPECT_EQ(result_->admission_error.load(), 0);
}

// @safe - Only atomics cross the poll-worker/test-thread boundary.
struct PollAdmissionResults {
  std::array<AdmissionResult, 3> requests;
  rusty::sync::atomic::AtomicBool stop{false};
  rusty::sync::atomic::AtomicBool pressure_started{false};
  rusty::sync::atomic::AtomicBool timer_started{false};
  rusty::sync::atomic::AtomicBool child_started{false};
  rusty::sync::atomic::AtomicBool child_ran_inline{false};
  rusty::sync::atomic::AtomicBool child_admitted{false};
  rusty::sync::atomic::AtomicBool child_waited_for_admission{false};
  rusty::sync::atomic::AtomicBool finished{false};
};

// @safe - These event slots are created and borrowed only by the owning worker.
struct ReadyBaton {
  std::array<rusty::RefCell<rusty::Option<rusty::Arc<srpc::IntEvent>>>, 2> events;
};

// Every poll-owned request carries this much padding, so one accepted frame
// alone takes the production outbound queue to its high-water mark. On the
// Lion runtime a connection's reader and writer tasks are started only for a
// connection srpc creates itself (TcpFactory::connect or the accept driver),
// and such a connection keeps the default mark: set_outbound_high_water needs
// the concrete TcpConnection, which the factory does not hand out. A sender on
// the poll thread never writes through, so the frame stays queued until the
// worker returns to its runtime and the writer task runs.
constexpr size_t kAdmissionPadBytes = srpc::kTcpConnectionOutboundHighWaterDefault;
// Request header (xid, rpc id, marker value) plus the padding's length prefix.
constexpr size_t kAdmissionFrameSlackBytes = 64;

// @unsafe - Real loopback TCP, owned by the production poll worker through the
// production connect path (srpc::Client::connect: TcpFactory connect, reader
// and writer tasks on the worker's Lion runtime, direct channel binding). This
// is the path MultiPaxosCommo's request_replication uses.
// The test thread is the independent peer/watchdog, never an owner-reactor
// driver. The hard watchdog also bounds setup and shutdown if cleanup regresses.
class PollOwnedAdmissionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener_, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(listener_, reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)), 0);
    ASSERT_EQ(::listen(listener_, 1), 0);
    socklen_t length = sizeof(address);
    ASSERT_EQ(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                            &length), 0);
    const std::string endpoint =
        "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    poll_ = rusty::Some(srpc::PollThread::create());
    rpc_client_ = rusty::Some(
        srpc::Client::create(poll_.as_ref().unwrap().clone()));
    auto rpc_client = rpc_client_.as_ref().unwrap().clone();
    // Blocks this thread until the loopback handshake completes; the listen
    // backlog holds the connection until accept() below.
    ASSERT_EQ(rpc_client->connect(
        reinterpret_cast<const int8_t*>(endpoint.c_str()), true), 0);
    ASSERT_TRUE(rpc_client->connected());
    client_ = rusty::Some(rpc_client->connection().unwrap());
    peer_ = ::accept(listener_, nullptr, nullptr);
    ASSERT_GE(peer_, 0);
    ASSERT_EQ(::fcntl(peer_, F_SETFL, O_NONBLOCK), 0);
  }

  // @unsafe - Stop the producers independently of the stalled poll worker, then
  // close on its owner. Even the old unbounded scheduler can now quiesce.
  void TearDown() override {
    results_->stop.store(true);
    if (client_.is_some() && poll_.is_some()) {
      auto closed = rusty::Arc<rusty::sync::atomic::AtomicBool>::make(false);
      auto job = rusty::Arc<srpc::OneTimeJob>::new_(srpc::OneTimeJob::new_(
          [client = client_.as_ref().unwrap().clone(), closed] {
            client->close();
            closed->store(true);
          }));
      poll_.as_ref().unwrap()->add(rusty::Arc<srpc::Job>(job));
      const auto deadline = srpc::Time::now(true) + 2'000'000;
      while (!closed->load() && srpc::Time::now(true) < deadline)
        ::usleep(1000);
      EXPECT_TRUE(closed->load()) << "owner cleanup did not run";
    }
    // Client's Drop posts its own close job (redundant after the one above);
    // release it while the poll worker still runs, as srpc's fixtures do.
    rpc_client_ = rusty::None;
    if (poll_.is_some())
      poll_.as_ref().unwrap()->shutdown();
    // Closing must not complete any already-completed future a second time.
    if (exercised_) {
      for (const auto& request : results_->requests)
        EXPECT_EQ(request.completions.load(), 1);
      std::array<uint8_t, 256> extra{};
      const auto count = ::recv(peer_, extra.data(), extra.size(), MSG_DONTWAIT);
      EXPECT_TRUE(count == 0 || (count == -1 && errno == EAGAIN))
          << "unexpected additional admitted frame bytes: " << count;
    }
    client_ = rusty::None;
    poll_ = rusty::None;
    if (peer_ >= 0) ::close(peer_);
    if (listener_ >= 0) ::close(listener_);
  }

  // @unsafe - Peer-side socket I/O only; all waits share one independent bound.
  bool transfer(void* bytes, size_t size, bool sending, uint64_t deadline) {
    auto* cursor = static_cast<uint8_t*>(bytes);
    while (size != 0 && srpc::Time::now(true) < deadline) {
      pollfd descriptor{peer_, static_cast<short>(sending ? POLLOUT : POLLIN), 0};
      if (::poll(&descriptor, 1, 10) < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      const auto count = sending
          ? ::send(peer_, cursor, size, MSG_NOSIGNAL)
          : ::recv(peer_, cursor, size, 0);
      if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
      if (count <= 0) return false;
      cursor += count;
      size -= static_cast<size_t>(count);
    }
    return size == 0;
  }

  // @unsafe - Production retained-admission and future ownership, on the owner.
  static void request(const rusty::Arc<srpc::ClientConnection>& client,
                      const rusty::Arc<PollAdmissionResults>& results, int index) {
    srpc::FutureAttr attr;
    attr.callback = srpc::FutureCallback::from_callable(
        [results, index](rusty::Arc<srpc::Future> future) {
          results->requests[index].callback_error.store(future->get_error_code());
          results->requests[index].completions.fetch_add(1);
        });
    auto future = client->request_with_admission(
        0x57, attr, 500'000, [results, index](srpc::BinaryWriteArchive& archive) {
          results->requests[index].serializations.fetch_add(1);
          srpc::Serialize_::serialize(
              index == 2 ? int32_t{0x7fffffff} : int32_t{index}, archive);
          // Admission pressure: see kAdmissionPadBytes.
          srpc::Serialize_::serialize(std::string(kAdmissionPadBytes, 'p'),
                                      archive);
        });
    if (future.is_err())
      results->requests[index].admission_error.store(future.unwrap_err());
  }

  // @unsafe - Two finite fibers pass a fresh event baton, guaranteeing a ready
  // next generation without timing luck. A third fiber supplies timed pressure.
  static void start_ready_pressure(const rusty::Arc<PollAdmissionResults>& results,
                                   const rusty::Arc<srpc::IntEvent>& gate) {
    auto baton = rusty::Arc<ReadyBaton>::make();
    for (int side = 0; side != 2; ++side)
      *baton->events[side].borrow_mut() = rusty::Some(srpc::create_sp_int_event(1));
    for (int side = 0; side != 2; ++side) {
      srpc::Fiber::create_run([baton, results, side] {
        while (true) {
          auto current = baton->events[side].borrow()->as_ref().unwrap().clone();
          current->wait();
          auto other = baton->events[1 - side].borrow()->as_ref().unwrap().clone();
          results->pressure_started.store(true);
          if (results->stop.load()) {
            other->set(1);
            return;
          }
          *baton->events[side].borrow_mut() = rusty::Some(srpc::create_sp_int_event(1));
          other->set(1);
        }
      });
    }
    srpc::Fiber::create_run([gate, results, baton] {
      gate->wait();
      baton->events[0].borrow()->as_ref().unwrap()->set(1);
      while (!results->stop.load()) {
        results->timer_started.store(true);
        srpc::Fiber::sleep(100);
      }
    });
  }

  // @unsafe - No local flush or manual reactor pump is permitted: only the
  // worker's own writer task may drain the queue.
  void exercise(bool stackless) {
    exercised_ = true;
    auto job = rusty::Arc<srpc::OneTimeJob>::new_(srpc::OneTimeJob::new_(
        [client = client_.as_ref().unwrap().clone(), results = results_,
         stackless] {
          // SetUp connected and bound the channel through srpc::Client.
          auto gate = srpc::create_sp_int_event(1);
          if (!stackless)
            start_ready_pressure(results, gate);
          request(client, results, 0);
          if (stackless) {
            auto reactor = srpc::Reactor::get_reactor();
            auto self_slot = rusty::Arc<rusty::Cell<size_t>>::make(0);
            const auto slot = reactor->register_stackless_poller(
                [results, self_slot](rusty::Context& context) {
                  results->pressure_started.store(true);
                  if (results->stop.load()) return true;
                  context.waker->wake();
                  // Exercise both wake ingress and the owner-side requeue API:
                  // bounding only run_loop must not leave this FIFO drain open.
                  srpc::Reactor::get_reactor()->enqueue_stackless_task(self_slot->get());
                  return false;
                });
            self_slot->set(slot);
            reactor->enqueue_stackless_task(slot);
          } else {
            gate->set(1);
          }
          auto child_done = srpc::create_sp_int_event(1);
          srpc::Fiber::create_run([client, results, child_done] {
            results->child_started.store(true);
            request(client, results, 1);
            results->child_admitted.store(true);
            child_done->set(1);
          });
          results->child_ran_inline.store(results->child_started.load());
          // Request 0 filled the queue and nothing has drained it yet (this
          // job has not yielded to the worker's runtime), so the child must
          // be suspended in admission, not finished.
          results->child_waited_for_admission.store(
              !results->child_admitted.load());
          child_done->wait();
          request(client, results, 2);  // Terminal marker, after child admission.
          results->finished.store(true);
        }));
    poll_.as_ref().unwrap()->add(rusty::Arc<srpc::Job>(job));

    const auto deadline = srpc::Time::now(true) + 2'000'000;
    std::array<int64_t, 3> xids{};
    int received = 0;
    for (; received != 3; ++received) {
      int32_t length = 0;
      if (!transfer(&length, sizeof(length), false, deadline)) break;
      if (length <= 0 || static_cast<size_t>(length) >
                             kAdmissionPadBytes + kAdmissionFrameSlackBytes) {
        ADD_FAILURE() << "invalid production frame length " << length;
        break;
      }
      std::vector<uint8_t> body(static_cast<size_t>(length));
      if (!transfer(body.data(), body.size(), false, deadline)) break;
      EXPECT_GT(body.size(), kAdmissionPadBytes) << "request lost its padding";
      // The header is read; the padding that follows it is not inspected.
      srpc::BufferSource source(body.data(), body.size());
      srpc::BinaryReadArchive archive(srpc::make_source_proxy_buffer(&source));
      srpc::v64 xid;
      int32_t rpc = 0;
      int32_t value = 0;
      srpc::Deserialize_::deserialize(xid, archive);
      srpc::Deserialize_::deserialize(rpc, archive);
      srpc::Deserialize_::deserialize(value, archive);
      EXPECT_EQ(rpc, 0x57);
      EXPECT_EQ(value, received == 2 ? 0x7fffffff : received);
      xids[received] = xid.get();
      for (int previous = 0; previous != received; ++previous)
        EXPECT_NE(xids[previous], xid.get()) << "frame xid sent twice";
      // A protocol reply acknowledges the received xid, not a mock transport
      // echo. Its completion must return through this same real poll worker.
      srpc::BufferSink reply;
      srpc::BinaryWriteArchive writer(srpc::make_sink_proxy_buffer(&reply));
      srpc::Serialize_::serialize(xid, writer);
      srpc::Serialize_::serialize(srpc::v32(0), writer);
      srpc::Serialize_::serialize(srpc::v64(1), writer);
      int32_t reply_length = reply.bytes.len();
      if (!transfer(&reply_length, sizeof(reply_length), true, deadline) ||
          !transfer(reply.bytes.data(), reply.bytes.len(), true, deadline))
        break;
    }
    while (srpc::Time::now(true) < deadline &&
           (!results_->finished.load() ||
            results_->requests[2].completions.load() == 0))
      ::usleep(1000);
    // Freeze the liveness outcome BEFORE releasing pressure for cleanup.
    const bool progressed = received == 3 && results_->finished.load() &&
        results_->requests[2].completions.load() == 1;
    results_->stop.store(true);
    EXPECT_TRUE(progressed) << "poll-owned TCP made no complete progress under ready pressure";
    EXPECT_EQ(received, 3) << "terminal marker did not arrive";
    EXPECT_TRUE(results_->pressure_started.load());
    if (!stackless) EXPECT_TRUE(results_->timer_started.load());
    EXPECT_TRUE(results_->child_ran_inline.load());
    EXPECT_TRUE(results_->child_waited_for_admission.load())
        << "no admission pressure: the second request was admitted at once";
    for (const auto& result : results_->requests) {
      EXPECT_EQ(result.serializations.load(), 1);
      EXPECT_EQ(result.completions.load(), 1);
      EXPECT_EQ(result.admission_error.load(), 0);
      EXPECT_EQ(result.callback_error.load(), 0) << "healthy draining expired admission";
    }
  }

  srpc_test::Watchdog watchdog_{"PollOwnedAdmissionTest"};
  int listener_ = -1;
  int peer_ = -1;
  bool exercised_ = false;
  rusty::Option<rusty::Arc<srpc::PollThread>> poll_;
  rusty::Option<rusty::Arc<srpc::Client>> rpc_client_;
  rusty::Option<rusty::Arc<srpc::ClientConnection>> client_;
  rusty::Arc<PollAdmissionResults> results_ = rusty::Arc<PollAdmissionResults>::make();
};

// @unsafe - Same-worker jobs and recursive child fibers must relinquish I/O.
TEST_F(PollOwnedAdmissionTest, TimedReadyPressurePreservesAdmissionAndTerminalOrder) {
  exercise(false);
}

// @unsafe - Self-wakes must not turn a finite poll-worker turn into an inner drain.
TEST_F(PollOwnedAdmissionTest, StacklessSelfWakePreservesAdmissionAndTerminalOrder) {
  exercise(true);
}

// @safe - Per-message outcomes, published from the owner poll thread.
struct OutboxMessageResult {
  rusty::sync::atomic::AtomicI32 serializations{0};
  rusty::sync::atomic::AtomicI32 completions{0};
  rusty::sync::atomic::AtomicI32 callback_error{0};
  rusty::sync::atomic::AtomicI32 refusals{0};
  rusty::sync::atomic::AtomicI32 refused_error{0};
};

// @safe - Only atomics cross the poll-worker/test-thread boundary.
struct OutboxProbe {
  static constexpr int kMessages = 8;
  std::array<OutboxMessageResult, kMessages> messages;
  rusty::sync::atomic::AtomicI32 admit_calls{0};
  rusty::sync::atomic::AtomicI32 in_admit{0};
  rusty::sync::atomic::AtomicI32 max_in_admit{0};
  rusty::sync::atomic::AtomicBool every_admit_without_deadline{true};
  rusty::sync::atomic::AtomicI32 stall_reports{0};
  rusty::sync::atomic::AtomicI32 stall_bad_site{0};
  rusty::sync::atomic::AtomicI32 stall_bad_rpc{0};
  rusty::sync::atomic::AtomicU64 stall_min_waited_us{UINT64_MAX};
  rusty::sync::atomic::AtomicU64 stall_max_waited_us{0};
  rusty::sync::atomic::AtomicU64 stall_max_queued{0};
};

// @safe - Owner-thread observations of one outbox, published to the test.
struct OutboxSnapshot {
  rusty::sync::atomic::AtomicI32 queued{-1};
  rusty::sync::atomic::AtomicBool pumping{false};
  rusty::sync::atomic::AtomicBool watching{false};
  rusty::sync::atomic::AtomicI32 in_admit{-1};
  rusty::sync::atomic::AtomicU64 pump_starts{0};
  rusty::sync::atomic::AtomicU64 watcher_starts{0};
  rusty::sync::atomic::AtomicU64 stall_reports{0};
};

constexpr siteid_t kOutboxSite = 7;
constexpr int32_t kOutboxRpcBase = 0x5800;

// @unsafe - janus::ReplicationOutbox, the Paxos per-destination outbox, over
// the same production poll-owned loopback connection: the admit function is
// request_with_admission on that connection, as in commo.cc, plus in-flight
// accounting. Every Send and every observation runs as a job on the owner
// poll worker; the test thread is the peer. Each request carries
// kAdmissionPadBytes, so one queued frame fills the channel until the worker
// returns to its runtime.
class ReplicationOutboxTest : public PollOwnedAdmissionTest {
 protected:
  // @unsafe - Run fn as a job on the owner poll thread and wait for it.
  template <typename Fn>
  bool on_owner(Fn fn) {
    auto done = rusty::Arc<rusty::sync::atomic::AtomicBool>::make(false);
    auto job = rusty::Arc<srpc::OneTimeJob>::new_(srpc::OneTimeJob::new_(
        [fn, done]() mutable {
          fn();
          done->store(true);
        }));
    poll_.as_ref().unwrap()->add(rusty::Arc<srpc::Job>(job));
    const auto deadline = srpc::Time::now(true) + 2'000'000;
    while (!done->load() && srpc::Time::now(true) < deadline)
      ::usleep(1000);
    return done->load();
  }

  // @unsafe - The outbox under test; its stall reports land in probe_.
  janus::ReplicationOutbox make_outbox(uint64_t stall_after_us) {
    auto admit = [client = client_.as_ref().unwrap().clone(), probe = probe_](
                     i32 rpc_id, const srpc::FutureAttr& attr,
                     uint64_t timeout_us, janus::ReplicationWrite& write) {
      probe->admit_calls.fetch_add(1);
      probe->max_in_admit.fetch_max(probe->in_admit.fetch_add(1) + 1);
      if (timeout_us != janus::ReplicationOutbox::kNoAdmissionDeadline)
        probe->every_admit_without_deadline.store(false);
      auto result = client->request_with_admission(
          rpc_id, attr, timeout_us,
          [&write](srpc::BinaryWriteArchive& archive) { write(archive); });
      probe->in_admit.fetch_sub(1);
      return result;
    };
    auto report = [probe = probe_](const janus::ReplicationStall& stall) {
      probe->stall_reports.fetch_add(1);
      if (stall.site_id != kOutboxSite)
        probe->stall_bad_site.fetch_add(1);
      if (stall.rpc_id < kOutboxRpcBase ||
          stall.rpc_id >= kOutboxRpcBase + OutboxProbe::kMessages)
        probe->stall_bad_rpc.fetch_add(1);
      probe->stall_min_waited_us.fetch_min(stall.waited_us);
      probe->stall_max_waited_us.fetch_max(stall.waited_us);
      probe->stall_max_queued.fetch_max(stall.queued);
      janus::LogReplicationStall(stall);
    };
    return janus::ReplicationOutbox(kOutboxSite, poll_.as_ref().unwrap().clone(),
                                    admit, stall_after_us, report);
  }

  // @unsafe - Request `index`: rpc kOutboxRpcBase + index, value index.
  janus::ReplicationMessage message(int index) const {
    janus::ReplicationMessage request;
    request.rpc_id = kOutboxRpcBase + index;
    auto probe = probe_;
    request.attr.callback = srpc::FutureCallback::from_callable(
        [probe, index](rusty::Arc<srpc::Future> future) {
          probe->messages[index].callback_error.store(future->get_error_code());
          probe->messages[index].completions.fetch_add(1);
        });
    request.write = [probe, index](srpc::BinaryWriteArchive& archive) {
      probe->messages[index].serializations.fetch_add(1);
      srpc::Serialize_::serialize(int32_t{index}, archive);
      srpc::Serialize_::serialize(std::string(kAdmissionPadBytes, 'p'), archive);
    };
    request.refused = [probe, index](i32 error) {
      probe->messages[index].refused_error.store(error);
      probe->messages[index].refusals.fetch_add(1);
    };
    return request;
  }

  // @unsafe - Owner-side state of outbox, read in a job.
  rusty::Arc<OutboxSnapshot> snapshot(const janus::ReplicationOutbox& outbox) {
    auto snap = rusty::Arc<OutboxSnapshot>::make();
    EXPECT_TRUE(on_owner([outbox = outbox.clone(), snap, probe = probe_] {
      store(outbox, *snap, *probe);
    }));
    return snap;
  }

  // @unsafe - Owner thread only.
  static void store(const janus::ReplicationOutbox& outbox,
                    const OutboxSnapshot& snap,
                    const OutboxProbe& probe) {
    snap.queued.store(static_cast<int32_t>(outbox.queued()));
    snap.pumping.store(outbox.pumping());
    snap.watching.store(outbox.watching());
    snap.in_admit.store(probe.in_admit.load());
    snap.pump_starts.store(outbox.pump_starts());
    snap.watcher_starts.store(outbox.watcher_starts());
    snap.stall_reports.store(outbox.stall_reports());
  }

  // @unsafe - Wait until the outbox has no pump, no watcher and no queue.
  bool wait_idle(const janus::ReplicationOutbox& outbox) {
    const auto deadline = srpc::Time::now(true) + 3'000'000;
    while (srpc::Time::now(true) < deadline) {
      auto snap = snapshot(outbox);
      if (!snap->pumping.load() && !snap->watching.load() &&
          snap->queued.load() == 0)
        return true;
      ::usleep(5000);
    }
    return false;
  }

  // @unsafe - Wait for every message in [first, last) to end exactly once.
  bool wait_ended(int first, int last) {
    const auto deadline = srpc::Time::now(true) + 3'000'000;
    while (srpc::Time::now(true) < deadline) {
      bool all = true;
      for (int i = first; i != last; ++i)
        all = all && probe_->messages[i].completions.load() +
                         probe_->messages[i].refusals.load() != 0;
      if (all) return true;
      ::usleep(1000);
    }
    return false;
  }

  // @unsafe - Peer side: one request frame's header.
  bool read_request(uint64_t deadline, int64_t* xid, int32_t* rpc,
                    int32_t* value) {
    int32_t length = 0;
    if (!transfer(&length, sizeof(length), false, deadline)) return false;
    if (length <= 0 || static_cast<size_t>(length) >
                           kAdmissionPadBytes + kAdmissionFrameSlackBytes) {
      ADD_FAILURE() << "invalid production frame length " << length;
      return false;
    }
    std::vector<uint8_t> body(static_cast<size_t>(length));
    if (!transfer(body.data(), body.size(), false, deadline)) return false;
    EXPECT_GT(body.size(), kAdmissionPadBytes) << "request lost its padding";
    srpc::BufferSource source(body.data(), body.size());
    srpc::BinaryReadArchive archive(srpc::make_source_proxy_buffer(&source));
    srpc::v64 wire_xid;
    srpc::Deserialize_::deserialize(wire_xid, archive);
    srpc::Deserialize_::deserialize(*rpc, archive);
    srpc::Deserialize_::deserialize(*value, archive);
    *xid = wire_xid.get();
    return true;
  }

  // @unsafe - Peer side: a successful protocol reply to xid.
  bool reply(int64_t xid, uint64_t deadline) {
    srpc::BufferSink sink;
    srpc::BinaryWriteArchive writer(srpc::make_sink_proxy_buffer(&sink));
    srpc::Serialize_::serialize(srpc::v64(xid), writer);
    srpc::Serialize_::serialize(srpc::v32(0), writer);
    srpc::Serialize_::serialize(srpc::v64(1), writer);
    int32_t reply_length = sink.bytes.len();
    return transfer(&reply_length, sizeof(reply_length), true, deadline) &&
           transfer(sink.bytes.data(), sink.bytes.len(), true, deadline);
  }

  // @unsafe - Read, check and answer requests first..first+count-1, which
  // must arrive in exactly that order, each once.
  void deliver_in_order(int first, int count) {
    const auto deadline = srpc::Time::now(true) + 5'000'000;
    for (int i = 0; i != count; ++i) {
      int64_t xid = 0;
      int32_t rpc = 0;
      int32_t value = -1;
      ASSERT_TRUE(read_request(deadline, &xid, &rpc, &value))
          << "request " << first + i << " did not arrive";
      EXPECT_EQ(value, first + i) << "outbox broke FIFO order";
      EXPECT_EQ(rpc, kOutboxRpcBase + value);
      for (const auto seen : xids_) EXPECT_NE(seen, xid) << "frame xid sent twice";
      xids_.push_back(xid);
      ASSERT_TRUE(reply(xid, deadline));
    }
  }

  // @unsafe - Nothing beyond the delivered frames reached the peer.
  void expect_no_more_frames() {
    std::array<uint8_t, 256> extra{};
    const auto count = ::recv(peer_, extra.data(), extra.size(), MSG_DONTWAIT);
    EXPECT_TRUE(count == -1 && errno == EAGAIN)
        << "unexpected additional admitted frame bytes: " << count;
  }

  // @unsafe - Each of [first, last) was admitted once, encoded once, and
  // completed once without error.
  void expect_delivered(int first, int last) {
    for (int i = first; i != last; ++i) {
      const auto& result = probe_->messages[i];
      EXPECT_EQ(result.serializations.load(), 1) << "message " << i;
      EXPECT_EQ(result.completions.load(), 1) << "message " << i;
      EXPECT_EQ(result.callback_error.load(), 0) << "message " << i;
      EXPECT_EQ(result.refusals.load(), 0) << "message " << i;
    }
  }

  rusty::Arc<OutboxProbe> probe_ = rusty::Arc<OutboxProbe>::make();
  std::vector<int64_t> xids_;
};

// @unsafe - A full channel queues behind one admission waiter, in FIFO order,
// and Send never suspends its caller.
TEST_F(ReplicationOutboxTest, FullChannelKeepsFifoOrderWithOneWaiter) {
  constexpr int kCount = 5;
  auto outbox = make_outbox(2'000'000);
  auto sent = rusty::Arc<OutboxSnapshot>::make();
  ASSERT_TRUE(on_owner([this, outbox = outbox.clone(), sent] {
    for (int i = 0; i != kCount; ++i)
      outbox.Send(message(i));
    // Every Send returned to this job, which has not yielded to the worker's
    // runtime. Request 0 met an empty channel, so its pump admitted it and
    // ended inside the first Send; the second Send started the pump that now
    // waits for space for request 1, and the rest are queued behind it.
    store(outbox, *sent, *probe_);
  }));
  EXPECT_EQ(sent->queued.load(), kCount - 2);
  EXPECT_TRUE(sent->pumping.load());
  EXPECT_TRUE(sent->watching.load());
  EXPECT_EQ(sent->in_admit.load(), 1);
  EXPECT_EQ(sent->pump_starts.load(), 2u);

  deliver_in_order(0, kCount);
  ASSERT_TRUE(wait_ended(0, kCount));
  ASSERT_TRUE(wait_idle(outbox));
  expect_delivered(0, kCount);
  EXPECT_EQ(probe_->admit_calls.load(), kCount);
  EXPECT_EQ(probe_->max_in_admit.load(), 1) << "more than one admission waiter";
  EXPECT_TRUE(probe_->every_admit_without_deadline.load());
  EXPECT_EQ(probe_->stall_reports.load(), 0);
  auto idle = snapshot(outbox);
  EXPECT_EQ(idle->pump_starts.load(), 2u)
      << "one pump admits every request queued behind a full channel";
  EXPECT_EQ(idle->watcher_starts.load(), 1u);
  expect_no_more_frames();
}

// @unsafe - The pump ends when the outbox empties and a later Send starts a
// new one; a pump that never waited starts no watcher.
TEST_F(ReplicationOutboxTest, PumpRestartsAfterOutboxEmpties) {
  auto outbox = make_outbox(2'000'000);
  for (int batch = 0; batch != 2; ++batch) {
    auto sent = rusty::Arc<OutboxSnapshot>::make();
    ASSERT_TRUE(on_owner([this, outbox = outbox.clone(), sent, batch] {
      outbox.Send(message(2 * batch));
      outbox.Send(message(2 * batch + 1));
      store(outbox, *sent, *probe_);
    }));
    // Per batch: the first request's pump ends inside its Send, the second
    // request's pump waits for channel space.
    EXPECT_TRUE(sent->pumping.load()) << "batch " << batch;
    EXPECT_EQ(sent->pump_starts.load(), static_cast<uint64_t>(2 * (batch + 1)));
    deliver_in_order(2 * batch, 2);
    ASSERT_TRUE(wait_ended(2 * batch, 2 * batch + 2));
    ASSERT_TRUE(wait_idle(outbox)) << "batch " << batch;
  }
  // An empty channel admits at once: the pump starts and ends inside Send.
  auto sent = rusty::Arc<OutboxSnapshot>::make();
  ASSERT_TRUE(on_owner([this, outbox = outbox.clone(), sent] {
    outbox.Send(message(4));
    store(outbox, *sent, *probe_);
  }));
  EXPECT_FALSE(sent->pumping.load());
  EXPECT_FALSE(sent->watching.load());
  EXPECT_EQ(sent->queued.load(), 0);
  EXPECT_EQ(sent->pump_starts.load(), 5u);
  deliver_in_order(4, 1);
  ASSERT_TRUE(wait_ended(4, 5));
  ASSERT_TRUE(wait_idle(outbox));
  expect_delivered(0, 5);
  EXPECT_EQ(probe_->max_in_admit.load(), 1);
  auto idle = snapshot(outbox);
  EXPECT_EQ(idle->pump_starts.load(), 5u);
  EXPECT_LE(idle->watcher_starts.load(), 2u);
  expect_no_more_frames();
}

// @unsafe - Closing the connection completes the admitted and the waiting
// request with an error and refuses every request behind them; a closed
// connection refuses at once. Each request ends exactly once.
TEST_F(ReplicationOutboxTest, ClosedConnectionEndsEveryRequestOnce) {
  auto outbox = make_outbox(2'000'000);
  auto sent = rusty::Arc<OutboxSnapshot>::make();
  ASSERT_TRUE(on_owner([this, outbox = outbox.clone(), sent,
                        client = client_.as_ref().unwrap().clone()] {
    for (int i = 0; i != 4; ++i)
      outbox.Send(message(i));
    store(outbox, *sent, *probe_);
    client->close();
  }));
  EXPECT_EQ(sent->queued.load(), 2);
  EXPECT_TRUE(sent->pumping.load());
  ASSERT_TRUE(wait_ended(0, 4));
  ASSERT_TRUE(wait_idle(outbox));
  for (int i = 0; i != 2; ++i) {
    EXPECT_EQ(probe_->messages[i].completions.load(), 1) << "message " << i;
    EXPECT_NE(probe_->messages[i].callback_error.load(), 0) << "message " << i;
    EXPECT_EQ(probe_->messages[i].refusals.load(), 0) << "message " << i;
  }
  for (int i = 2; i != 4; ++i) {
    EXPECT_EQ(probe_->messages[i].refusals.load(), 1) << "message " << i;
    EXPECT_EQ(probe_->messages[i].refused_error.load(),
              srpc::CLIENT_ERR_NOT_CONNECTED) << "message " << i;
    EXPECT_EQ(probe_->messages[i].completions.load(), 0) << "message " << i;
    EXPECT_EQ(probe_->messages[i].serializations.load(), 0) << "message " << i;
  }

  // Already closed: the refused hook runs inside Send's inline pump.
  auto refused = rusty::Arc<rusty::sync::atomic::AtomicI32>::make(-1);
  auto after = rusty::Arc<OutboxSnapshot>::make();
  ASSERT_TRUE(on_owner([this, outbox = outbox.clone(), after, refused] {
    outbox.Send(message(4));
    refused->store(probe_->messages[4].refusals.load());
    store(outbox, *after, *probe_);
  }));
  EXPECT_EQ(refused->load(), 1);
  EXPECT_FALSE(after->pumping.load());
  EXPECT_EQ(probe_->messages[4].refused_error.load(),
            srpc::CLIENT_ERR_NOT_CONNECTED);
  EXPECT_EQ(probe_->messages[4].completions.load(), 0);
  ASSERT_TRUE(wait_idle(outbox));
  for (int i = 0; i != 5; ++i)
    EXPECT_EQ(probe_->messages[i].completions.load() +
                  probe_->messages[i].refusals.load(), 1) << "message " << i;
  EXPECT_EQ(probe_->max_in_admit.load(), 1);
}

// @unsafe - An admission that keeps waiting is reported at the threshold and
// every threshold after it, and is still delivered once, unchanged, when the
// peer drains: there is no deadline that could drop it.
TEST_F(ReplicationOutboxTest, LongAdmissionWaitIsReportedAndNotDropped) {
  constexpr uint64_t kStallAfterUs = 50'000;
  // Far more padding than loopback socket buffers absorb, so some admission
  // stays blocked while the peer does not read.
  constexpr int kCount = 6;
  auto outbox = make_outbox(kStallAfterUs);
  const auto began = srpc::Time::now(true);
  ASSERT_TRUE(on_owner([this, outbox = outbox.clone()] {
    for (int i = 0; i != kCount; ++i)
      outbox.Send(message(i));
  }));
  ::usleep(600'000);
  // Nothing ended while the peer was not reading: no drop, no refusal.
  for (int i = 0; i != kCount; ++i) {
    EXPECT_EQ(probe_->messages[i].completions.load(), 0) << "message " << i;
    EXPECT_EQ(probe_->messages[i].refusals.load(), 0) << "message " << i;
  }
  const int stalled_reports = probe_->stall_reports.load();
  EXPECT_GE(stalled_reports, 2) << "a 600 ms wait must be reported repeatedly";

  deliver_in_order(0, kCount);
  ASSERT_TRUE(wait_ended(0, kCount));
  ASSERT_TRUE(wait_idle(outbox));
  const auto elapsed = srpc::Time::now(true) - began;
  expect_delivered(0, kCount);
  EXPECT_EQ(probe_->admit_calls.load(), kCount) << "a request was admitted twice";
  EXPECT_EQ(probe_->max_in_admit.load(), 1);
  EXPECT_TRUE(probe_->every_admit_without_deadline.load());
  // Rate limit: one waiter at a time, one report per threshold of its wait.
  EXPECT_LE(static_cast<uint64_t>(probe_->stall_reports.load()),
            elapsed / kStallAfterUs);
  EXPECT_EQ(probe_->stall_bad_site.load(), 0);
  EXPECT_EQ(probe_->stall_bad_rpc.load(), 0);
  EXPECT_GE(probe_->stall_min_waited_us.load(), kStallAfterUs);
  EXPECT_GE(probe_->stall_max_waited_us.load(), 2 * kStallAfterUs);
  EXPECT_LE(probe_->stall_max_queued.load(), static_cast<uint64_t>(kCount - 2));
  auto idle = snapshot(outbox);
  EXPECT_EQ(idle->stall_reports.load(),
            static_cast<uint64_t>(probe_->stall_reports.load()));
  EXPECT_EQ(idle->pump_starts.load(), 2u);
  EXPECT_EQ(idle->watcher_starts.load(), 1u);
  expect_no_more_frames();
}

// @unsafe - The outbox is unsynchronized; Send off its owner poll thread is a
// verify failure, before anything is queued.
TEST_F(ReplicationOutboxTest, SendOffTheOwnerThreadIsRejected) {
  auto outbox = make_outbox(2'000'000);
  EXPECT_THROW(outbox.Send(message(0)), std::exception);
  auto snap = snapshot(outbox);
  EXPECT_EQ(snap->queued.load(), 0);
  EXPECT_FALSE(snap->pumping.load());
  EXPECT_EQ(snap->pump_starts.load(), 0u);
  EXPECT_EQ(probe_->admit_calls.load(), 0);
  EXPECT_EQ(probe_->messages[0].completions.load() +
                probe_->messages[0].refusals.load(), 0);
}

// @unsafe - Exercise the actual learner scheduler, not a forwarding mock.
TEST(PaxosLearnerPrefixTest, GapsDuplicatesAndReentrantDeliveryPreservePrefix) {
  janus::PaxosServer server;
  rusty::Vec<int> applied;
  janus::PaxosPrepCmd payload;
  payload.slots.push_back(77);
  janus::Command command(rusty::Arc<janus::PaxosPrepCmd>::make(std::move(payload)));
  server.reg_learner_action([&](int slot, janus::Command received) {
    EXPECT_EQ(marshallable_cast<janus::PaxosPrepCmd>(received).unwrap()->slots[0], 77);
    applied.push(slot);
    if (slot == 1) {
      server.OnForwardToLearner(0, 3, 0, command, [] {});
      EXPECT_EQ(applied.size(), 1);
    }
    return 0;
  });
  server.OnForwardToLearner(0, 2, 0, command, [] {});
  EXPECT_TRUE(applied.is_empty());
  EXPECT_EQ(server.max_committed_slot_learner_, 0);
  server.OnForwardToLearner(0, 1, 0, command, [] {});
  ASSERT_EQ(applied.size(), 3);
  EXPECT_EQ(applied[0], 1);
  EXPECT_EQ(applied[1], 2);
  EXPECT_EQ(applied[2], 3);
  EXPECT_EQ(server.max_committed_slot_learner_, 3);
  server.OnForwardToLearner(0, 2, 0, command, [] {});
  EXPECT_EQ(applied.size(), 3);
}

}  // namespace
