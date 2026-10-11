#include <std_compat.hpp>
#include <rusty/result.hpp>
#include "client_proxy.h"
#include <cstring>
#include <cerrno>

namespace mako {
// @unsafe - bounded envelope serialization into the existing SRPC archive.
srpc::FutureResult MakoClientProxy::async_Call(const MakoGatewayRequest& request,
                                              const srpc::FutureAttr& attr) {
    if (request.value_length > sizeof(request.value))
        return srpc::FutureResult::Err(static_cast<srpc::i32>(EINVAL));
    const std::string bytes(reinterpret_cast<const char*>(&request),
        offsetof(MakoGatewayRequest, value) + request.value_length);
    return client_->request(static_cast<srpc::i32>(request.kind), attr,
        [&](srpc::BinaryWriteArchive& archive) { srpc::Serialize_::serialize(bytes, archive); });
}
// @unsafe - transport errors do not report an application commit or create IDs.
srpc::i32 MakoClientProxy::Call(const MakoGatewayRequest& request,
                               MakoGatewayResponse* response, uint32_t timeout_ms) {
    if (!response || request.value_length > sizeof(request.value)) return EINVAL;
    auto result = async_Call(request);
    if (result.is_err()) return result.unwrap_err();
    auto future = result.unwrap();
    future->timed_wait(static_cast<double>(timeout_ms) / 1000.0);
    if (!future->ready() || future->timed_out()) return ETIMEDOUT;
    const auto error = future->get_error_code();
    if (error) return error;
    std::string bytes;
    srpc::deserialize_from(future->get_reply(), bytes);
    const size_t header = offsetof(MakoGatewayResponse, value);
    if (bytes.size() < header || bytes.size() > sizeof(*response)) return EPROTO;
    *response = {};
    std::memcpy(response, bytes.data(), bytes.size());
    if (response->client != request.client || response->sequence != request.sequence
        || response->value_length > sizeof(response->value)
        || bytes.size() != header + response->value_length) return EPROTO;
    return 0; // application status AND outcome remain in the actual response
}
} // namespace mako
