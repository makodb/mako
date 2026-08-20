
#include "../rrr.hpp"

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
    rrr::BufferSink sink;
    rrr::BinaryWriteArchive war(rrr::make_sink_proxy_buffer(&sink));

    Value v5, v6, v7, v8;
    rrr::Serialize_::serialize(v1, war);
    rrr::Serialize_::serialize(v2, war);
    rrr::Serialize_::serialize(v3, war);
    rrr::Serialize_::serialize(v4, war);
    rrr::BufferSource src(sink.bytes.data(), sink.bytes.len());
    rrr::BinaryReadArchive rar(rrr::make_source_proxy_buffer(&src));
    rrr::Deserialize_::deserialize(v5, rar);
    rrr::Deserialize_::deserialize(v6, rar);
    rrr::Deserialize_::deserialize(v7, rar);
    rrr::Deserialize_::deserialize(v8, rar);

    EXPECT_EQ(v5.get_i32(), (int32_t)1);

    EXPECT_EQ(v6.get_i64(), (int64_t)2);

    EXPECT_EQ(v7.get_double(), (double)3.14);

    EXPECT_EQ(v8.get_str(), s);
}
