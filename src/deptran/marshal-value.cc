#include "__dep__.h"
#include "marshal-value.h"

namespace rrr {

// Marshal-deprecation slice A: the Marshal-form mdb::Value serde is
// deleted; the archive form below owns the wire format.
// archive operators for mdb::Value. Wire
// format is byte-for-byte identical to the Marshal-based operators
// above: ver_ (i64 typically), then i32 tag (0=I32, 1=I64, 2=DOUBLE,
// 3=STR), then the payload.
void serialize(const mdb::Value &value, BinaryWriteArchive &ar) {
  rrr::Serialize_::serialize(value.ver_, ar);
  switch (value.get_kind()) {
    case mdb::Value::I32:
      rrr::Serialize_::serialize(static_cast<i32>(0), ar);
      rrr::Serialize_::serialize(value.get_i32(), ar);
      break;
    case mdb::Value::I64:
      rrr::Serialize_::serialize(static_cast<i32>(1), ar);
      rrr::Serialize_::serialize(value.get_i64(), ar);
      break;
    case mdb::Value::DOUBLE:
      rrr::Serialize_::serialize(static_cast<i32>(2), ar);
      rrr::Serialize_::serialize(value.get_double(), ar);
      break;
    case mdb::Value::STR:
      rrr::Serialize_::serialize(static_cast<i32>(3), ar);
      rrr::Serialize_::serialize(value.get_str(), ar);
      break;
    default:
      verify(0);
      break;
  }
}

BinaryWriteArchive &operator<<(BinaryWriteArchive &ar, const mdb::Value &value) { serialize(value, ar); return ar; }

void deserialize(mdb::Value &value, BinaryReadArchive &ar) {
  rrr::Deserialize_::deserialize(value.ver_, ar);
  i32 k;
  rrr::Deserialize_::deserialize(k, ar);
  switch (k) {
    case 0: {
      int32_t i32_v;
      rrr::Deserialize_::deserialize(i32_v, ar);
      value.set_i32(i32_v);
      break;
    }
    case 1: {
      int64_t i64_v;
      rrr::Deserialize_::deserialize(i64_v, ar);
      value.set_i64(i64_v);
      break;
    }
    case 2: {
      double d;
      rrr::Deserialize_::deserialize(d, ar);
      value.set_double(d);
      break;
    }
    case 3: {
      std::string str;
      rrr::Deserialize_::deserialize(str, ar);
      value.set_str(str);
      break;
    }
  }
}

BinaryReadArchive &operator>>(BinaryReadArchive &ar, mdb::Value &value) { deserialize(value, ar); return ar; }

} // namespace rrr

