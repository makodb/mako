#include <gtest/gtest.h>
#include <type_traits>
#include "rrr.hpp"
#include "misc/marshallable_proxy.h"
#include "deptran/classic/tpc_command.h"
#include "deptran/procedure.h"

using namespace rrr;

namespace marshallable_proxy_test_types {

constexpr int32_t kTypedOnlyPayloadKind = 420043;

struct TypedOnlyPayload {
  int32_t value = 0;

  Marshal& to_marshal(Marshal& m) const {
    m << value;
    return m;
  }

  Marshal& from_marshal(Marshal& m) {
    m >> value;
    return m;
  }
};

using TypedOnlyPayloadAdapter =
    rrr::TypedMarshallableAdapter<TypedOnlyPayload, kTypedOnlyPayloadKind>;

}  // namespace marshallable_proxy_test_types

namespace rrr {

template <>
struct TypedMarshallableAdapterTraits<
    marshallable_proxy_test_types::TypedOnlyPayload> {
  static constexpr bool kEnabled = true;
  using Adapter = marshallable_proxy_test_types::TypedOnlyPayloadAdapter;
};

}  // namespace rrr

namespace {

constexpr int32_t kTestMarshallableKind = 420042;

static_assert(!std::is_base_of_v<Marshallable, janus::VecPieceData>);
static_assert(!std::is_base_of_v<Marshallable, janus::VecRecData>);
static_assert(!std::is_base_of_v<Marshallable, janus::ViewData>);
static_assert(!std::is_base_of_v<Marshallable, janus::KeyCmdBatchData>);
static_assert(!std::is_base_of_v<Marshallable, janus::TpcPrepareCommand>);
static_assert(!std::is_base_of_v<Marshallable, janus::TpcCommitCommand>);
static_assert(!std::is_base_of_v<Marshallable, janus::TpcEmptyCommand>);
static_assert(!std::is_base_of_v<Marshallable, janus::TpcNoopCommand>);
static_assert(!std::is_base_of_v<Marshallable, janus::TpcBatchCommand>);

class TestMarshallable : public Marshallable {
 public:
  int32_t value = 0;

  explicit TestMarshallable(int32_t v = 0)
      : Marshallable(kTestMarshallableKind), value(v) {}

  Marshal& to_marshal(Marshal& m) const override {
    m << kind_ << value;
    return m;
  }

  Marshal& from_marshal(Marshal& m) override {
    m >> kind_ >> value;
    return m;
  }

  size_t entity_size() const override {
    return sizeof(kind_) + sizeof(value);
  }

  size_t write_to_fd(int fd, size_t written) const override {
    return 0;
  }
};

void EnsureTestMarshallableInitializer() {
  static bool initialized = []() {
    MarshallDeputy::reg_initializer<TestMarshallable>(kTestMarshallableKind);
    return true;
  }();
  (void)initialized;
}

void EnsureTypedOnlyPayloadInitializer() {
  static bool initialized = []() {
    MarshallDeputy::reg_initializer<
        marshallable_proxy_test_types::TypedOnlyPayload>(
        marshallable_proxy_test_types::kTypedOnlyPayloadKind);
    return true;
  }();
  (void)initialized;
}

std::shared_ptr<janus::TpcCommitCommand> MakeTypedTpcCommitPayload(
    txnid_t tx_id,
    int ret,
    ballot_t term,
    bool_t recovery) {
  auto commit = std::make_shared<janus::TpcCommitCommand>();
  commit->tx_id_ = tx_id;
  commit->ret_ = ret;
  commit->term = term;

  auto vec_piece = std::make_shared<janus::VecPieceData>();
  vec_piece->sp_vec_piece_data_ =
      std::make_shared<std::vector<std::shared_ptr<janus::SimpleCommand>>>();
  vec_piece->is_recovery_command_ = recovery;
  commit->cmd_ = wrap_typed_marshallable(vec_piece);

  auto view_data = std::make_shared<janus::ViewData>();
  view_data->view_.n_ = 3;
  view_data->view_.view_id_ = 19;
  view_data->view_.timestamp_ = 777;
  view_data->view_.leaders_ = {0, 1, 2};
  view_data->partition_id_ = 11;
  commit->sp_view_data_ = view_data;

  return commit;
}

}  // namespace

TEST(MarshallableProxyFacadeTest, AdapterForwardsToMarshal) {
  auto sp = std::make_shared<TestMarshallable>(99);
  auto proxy = make_marshallable_proxy(sp);

  Marshal m;
  proxy->to_marshal(m);

  int32_t out_kind = 0, out_val = 0;
  m >> out_kind >> out_val;
  EXPECT_EQ(out_kind, kTestMarshallableKind);
  EXPECT_EQ(out_val, 99);
}

TEST(MarshallableProxyFacadeTest, AdapterForwardsFromMarshal) {
  auto sp = std::make_shared<TestMarshallable>();
  auto proxy = make_marshallable_proxy(sp);

  Marshal m;
  int32_t kind = 42, val = 77;
  kind = kTestMarshallableKind;
  m << kind << val;

  proxy->from_marshal(m);
  EXPECT_EQ(sp->kind_, kTestMarshallableKind);
  EXPECT_EQ(sp->value, 77);
}

TEST(MarshallableProxyFacadeTest, AdapterForwardsKind) {
  auto sp = std::make_shared<TestMarshallable>(0);
  auto proxy = make_marshallable_proxy(sp);

  EXPECT_EQ(proxy->kind(), kTestMarshallableKind);
}

TEST(MarshallableProxyFacadeTest, AdapterForwardsEntitySize) {
  auto sp = std::make_shared<TestMarshallable>(0);
  auto proxy = make_marshallable_proxy(sp);

  EXPECT_EQ(proxy->entity_size(), sizeof(int32_t) * 2);
}

TEST(MarshallableProxyFacadeTest, ProxyIsMoveOnly) {
  auto sp = std::make_shared<TestMarshallable>(5);
  auto proxy = make_marshallable_proxy(sp);

  auto moved = std::move(proxy);
  EXPECT_EQ(moved->kind(), kTestMarshallableKind);

  Marshal m;
  moved->to_marshal(m);
  int32_t k = 0, v = 0;
  m >> k >> v;
  EXPECT_EQ(v, 5);
}

TEST(MarshallableProxyFacadeTest, RoundTripThroughProxy) {
  auto src = std::make_shared<TestMarshallable>(123);
  auto src_proxy = make_marshallable_proxy(src);

  Marshal m;
  src_proxy->to_marshal(m);

  auto dst = std::make_shared<TestMarshallable>();
  auto dst_proxy = make_marshallable_proxy(dst);
  dst_proxy->from_marshal(m);

  EXPECT_EQ(dst->kind_, kTestMarshallableKind);
  EXPECT_EQ(dst->value, 123);
}

TEST(MarshallableProxyFacadeTest, DeputyDefaultsToNoMarshallable) {
  MarshallDeputy deputy;
  EXPECT_FALSE(deputy.has_marshallable());
  EXPECT_EQ(deputy.inner(), nullptr);
}

TEST(MarshallableProxyFacadeTest, DeputyStoresProxyAndPreservesInnerSharedPtr) {
  auto sp = std::make_shared<TestMarshallable>(88);
  MarshallDeputy deputy(sp);

  EXPECT_TRUE(deputy.has_marshallable());
  EXPECT_EQ(deputy.kind_, kTestMarshallableKind);
  EXPECT_EQ(marshallable_cast<TestMarshallable>(deputy), sp);
}

TEST(MarshallableProxyFacadeTest, DeputyRoundTripPreservesDerivedMarshallable) {
  EnsureTestMarshallableInitializer();

  auto src = std::make_shared<TestMarshallable>(321);
  MarshallDeputy outgoing(src);

  Marshal m;
  m << outgoing;

  MarshallDeputy incoming;
  m >> incoming;

  EXPECT_TRUE(incoming.has_marshallable());
  auto decoded = marshallable_cast<TestMarshallable>(incoming);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->kind_, kTestMarshallableKind);
  EXPECT_EQ(decoded->value, 321);
}

TEST(MarshallableProxyFacadeTest, InitializerReturnsProxyBackedMetadata) {
  EnsureTestMarshallableInitializer();

  auto initializer = MarshallDeputy::get_initializer(kTestMarshallableKind);
  auto state = initializer();
  ASSERT_NE(state.marshallable, nullptr);
  ASSERT_NE(state.proxy, nullptr);
  EXPECT_EQ(state.kind, kTestMarshallableKind);
  EXPECT_EQ(state.marshallable->kind(), kTestMarshallableKind);
  EXPECT_EQ((*state.proxy)->kind(), kTestMarshallableKind);
  EXPECT_EQ((*state.proxy)->inner(), state.marshallable);
}

TEST(MarshallableProxyFacadeTest, MarshallableCastFromSharedPtrKeepsType) {
  std::shared_ptr<Marshallable> base = std::make_shared<TestMarshallable>(17);
  auto typed = marshallable_cast<TestMarshallable>(base);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->value, 17);
}

TEST(MarshallableProxyFacadeTest, MarshallableCastFromNullDeputyPointerIsNull) {
  MarshallDeputy* deputy = nullptr;
  EXPECT_EQ(marshallable_cast<TestMarshallable>(deputy), nullptr);
}

TEST(MarshallableProxyFacadeTest, TypedPayloadRoundTripsViaDeputyAdapter) {
  EnsureTypedOnlyPayloadInitializer();
  auto payload = std::make_shared<marshallable_proxy_test_types::TypedOnlyPayload>();
  payload->value = 913;

  MarshallDeputy outgoing(payload);
  Marshal m;
  m << outgoing;

  MarshallDeputy incoming;
  m >> incoming;

  auto decoded =
      marshallable_cast<marshallable_proxy_test_types::TypedOnlyPayload>(
          incoming);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->value, 913);
}

TEST(MarshallableProxyFacadeTest, TypedPayloadInitializerStateContainsAdapter) {
  EnsureTypedOnlyPayloadInitializer();
  auto initializer = MarshallDeputy::get_initializer(
      marshallable_proxy_test_types::kTypedOnlyPayloadKind);
  auto state = initializer();

  EXPECT_EQ(state.kind, marshallable_proxy_test_types::kTypedOnlyPayloadKind);
  ASSERT_NE(state.marshallable, nullptr);
  ASSERT_NE(state.proxy, nullptr);

  auto decoded =
      marshallable_cast<marshallable_proxy_test_types::TypedOnlyPayload>(
          state.marshallable);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ((*state.proxy)->inner(), state.marshallable);
}

TEST(MarshallableProxyFacadeTest, DeptranVecPieceDataUsesTypedAdapterPath) {
  auto payload = std::make_shared<janus::VecPieceData>();
  payload->sp_vec_piece_data_ =
      std::make_shared<std::vector<std::shared_ptr<janus::SimpleCommand>>>();
  payload->time_sent_from_client_ = 42.5;
  payload->is_recovery_command_ = 1;

  auto wrapped = wrap_typed_marshallable(payload);
  ASSERT_NE(wrapped, nullptr);
  EXPECT_EQ(wrapped->kind(), MarshallDeputy::CMD_VEC_PIECE);

  MarshallDeputy deputy(payload);
  EXPECT_EQ(deputy.kind_, MarshallDeputy::CMD_VEC_PIECE);
  auto decoded = marshallable_cast<janus::VecPieceData>(deputy);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded, payload);
  EXPECT_DOUBLE_EQ(decoded->time_sent_from_client_, 42.5);
  EXPECT_EQ(decoded->is_recovery_command_, 1);
}

TEST(MarshallableProxyFacadeTest, DeptranViewDataMarshalRoundTrip) {
  janus::ViewData src;
  src.view_.n_ = 3;
  src.view_.view_id_ = 9;
  src.view_.timestamp_ = 12345;
  src.view_.leaders_ = {1, 2, 1};
  src.partition_id_ = 7;

  Marshal m;
  src.to_marshal(m);

  janus::ViewData dst;
  dst.from_marshal(m);

  EXPECT_EQ(dst.view_.n_, 3);
  EXPECT_EQ(dst.view_.view_id_, 9);
  EXPECT_EQ(dst.view_.timestamp_, 12345);
  ASSERT_EQ(dst.view_.leaders_.size(), 3u);
  EXPECT_EQ(dst.view_.leaders_[0], 1);
  EXPECT_EQ(dst.view_.leaders_[1], 2);
  EXPECT_EQ(dst.view_.leaders_[2], 1);
  EXPECT_EQ(dst.partition_id_, 7);
}

TEST(MarshallableProxyFacadeTest, DeptranVecRecAndBatchUseTypedAdapterPath) {
  auto vec_rec = std::make_shared<janus::VecRecData>();
  vec_rec->key_data_ = std::make_shared<std::vector<key_t>>();
  vec_rec->key_data_->push_back(11);
  vec_rec->key_data_->push_back(12);

  MarshallDeputy vec_rec_deputy(vec_rec);
  EXPECT_EQ(vec_rec_deputy.kind_, MarshallDeputy::CMD_REC_VEC);
  auto decoded_vec_rec = marshallable_cast<janus::VecRecData>(vec_rec_deputy);
  ASSERT_NE(decoded_vec_rec, nullptr);
  ASSERT_NE(decoded_vec_rec->key_data_, nullptr);
  ASSERT_EQ(decoded_vec_rec->key_data_->size(), 2u);
  EXPECT_EQ((*decoded_vec_rec->key_data_)[0], 11);
  EXPECT_EQ((*decoded_vec_rec->key_data_)[1], 12);

  auto batch = std::make_shared<janus::KeyCmdBatchData>();
  auto nested = std::make_shared<TestMarshallable>(77);
  batch->AddEntry(1001, nested);

  MarshallDeputy batch_deputy(batch);
  EXPECT_EQ(batch_deputy.kind_, MarshallDeputy::CMD_KEY_CMD_BATCH);
  auto decoded_batch = marshallable_cast<janus::KeyCmdBatchData>(batch_deputy);
  ASSERT_NE(decoded_batch, nullptr);
  ASSERT_EQ(decoded_batch->Size(), 1u);
  EXPECT_EQ(decoded_batch->GetKey(0), 1001);
  auto nested_decoded =
      marshallable_cast<TestMarshallable>(decoded_batch->GetCommand(0));
  ASSERT_NE(nested_decoded, nullptr);
  EXPECT_EQ(nested_decoded->value, 77);
}

TEST(MarshallableProxyFacadeTest, DeptranTpcCommitRoundTripUsesTypedAdapter) {
  auto src = MakeTypedTpcCommitPayload(/*tx_id=*/321, /*ret=*/9, /*term=*/17,
                                       /*recovery=*/1);

  MarshallDeputy outgoing(src);
  EXPECT_EQ(outgoing.kind_, MarshallDeputy::CMD_TPC_COMMIT);

  Marshal m;
  m << outgoing;

  MarshallDeputy incoming;
  m >> incoming;
  EXPECT_EQ(incoming.kind_, MarshallDeputy::CMD_TPC_COMMIT);

  auto decoded = marshallable_cast<janus::TpcCommitCommand>(incoming);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->tx_id_, 321);
  EXPECT_EQ(decoded->ret_, 9);
  EXPECT_EQ(decoded->term, 17);

  auto decoded_vec_piece = marshallable_cast<janus::VecPieceData>(decoded->cmd_);
  ASSERT_NE(decoded_vec_piece, nullptr);
  EXPECT_EQ(decoded_vec_piece->is_recovery_command_, 1);

  ASSERT_NE(decoded->sp_view_data_, nullptr);
  EXPECT_EQ(decoded->sp_view_data_->partition_id_, 11);
  EXPECT_EQ(decoded->sp_view_data_->view_.view_id_, 19);
}

TEST(MarshallableProxyFacadeTest, DeptranTpcBatchAndNoopEmptyUseTypedAdapter) {
  auto batch = std::make_shared<janus::TpcBatchCommand>();
  std::vector<std::shared_ptr<janus::TpcCommitCommand>> commits{
      MakeTypedTpcCommitPayload(/*tx_id=*/101, /*ret=*/1, /*term=*/3,
                                /*recovery=*/0),
      MakeTypedTpcCommitPayload(/*tx_id=*/202, /*ret=*/2, /*term=*/4,
                                /*recovery=*/1)};
  batch->AddCmds(commits);

  MarshallDeputy batch_outgoing(batch);
  EXPECT_EQ(batch_outgoing.kind_, MarshallDeputy::CMD_TPC_BATCH);
  Marshal batch_marshaled;
  batch_marshaled << batch_outgoing;

  MarshallDeputy batch_incoming;
  batch_marshaled >> batch_incoming;
  auto decoded_batch = marshallable_cast<janus::TpcBatchCommand>(batch_incoming);
  ASSERT_NE(decoded_batch, nullptr);
  ASSERT_EQ(decoded_batch->Size(), 2u);
  EXPECT_EQ(decoded_batch->cmds_.at(0)->tx_id_, 101);
  EXPECT_EQ(decoded_batch->cmds_.at(1)->tx_id_, 202);

  auto empty_cmd = std::make_shared<janus::TpcEmptyCommand>();
  MarshallDeputy empty_deputy(empty_cmd);
  EXPECT_EQ(empty_deputy.kind_, MarshallDeputy::CMD_TPC_EMPTY);
  ASSERT_NE(marshallable_cast<janus::TpcEmptyCommand>(empty_deputy), nullptr);

  auto noop_cmd = std::make_shared<janus::TpcNoopCommand>();
  MarshallDeputy noop_deputy(noop_cmd);
  EXPECT_EQ(noop_deputy.kind_, MarshallDeputy::CMD_NOOP);
  ASSERT_NE(marshallable_cast<janus::TpcNoopCommand>(noop_deputy), nullptr);
}
