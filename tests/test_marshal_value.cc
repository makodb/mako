
#include "../srpc.hpp"

#include "memdb/value.h"
#include "deptran/marshal-value.h"

import rusty;

using namespace base;
using namespace mdb;

TEST(marshal, value) {
    std::string s("Hello");
    Value v1((i32)1);
    Value v2((i64)2);
    Value v3((double)3.14);
    Value v4(s);
    srpc::BufferSink sink;
    srpc::BinaryWriteArchive war(srpc::make_sink_proxy_buffer(&sink));

    Value v5, v6, v7, v8;
    srpc::Serialize_::serialize(v1, war);
    srpc::Serialize_::serialize(v2, war);
    srpc::Serialize_::serialize(v3, war);
    srpc::Serialize_::serialize(v4, war);
    srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    srpc::Deserialize_::deserialize(v5, rar);
    srpc::Deserialize_::deserialize(v6, rar);
    srpc::Deserialize_::deserialize(v7, rar);
    srpc::Deserialize_::deserialize(v8, rar);

    EXPECT_EQ(v5.get_i32(), (int32_t)1);

    EXPECT_EQ(v6.get_i64(), (int64_t)2);

    EXPECT_EQ(v7.get_double(), (double)3.14);

    EXPECT_EQ(v8.get_str(), s);
}
