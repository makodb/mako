#include "__dep__.h"
#include "marshal-value.h"

namespace rrr {

rrr::Marshal &operator<<(rrr::Marshal &m, const mdb::Value &value) {
  rrr::Serialize_::serialize(value.ver_, m);
  switch (value.get_kind()) {
    case Value::I32:
      rrr::Serialize_::serialize((i32) 0, m);
      rrr::Serialize_::serialize(value.get_i32(), m);
      break;
    case Value::I64:
      rrr::Serialize_::serialize((i32) 1, m);
      rrr::Serialize_::serialize(value.get_i64(), m);
      break;
    case Value::DOUBLE:
      rrr::Serialize_::serialize((i32) 2, m);
      rrr::Serialize_::serialize(value.get_double(), m);
      break;
    case Value::STR:
      rrr::Serialize_::serialize((i32) 3, m);
      rrr::Serialize_::serialize(value.get_str(), m);
      break;
    default:
      verify(0);
      break;
  }
  return m;
}

rrr::Marshal &operator>>(rrr::Marshal &m, mdb::Value &value) {
  rrr::Deserialize_::deserialize(value.ver_, m);
  i32 k;
  rrr::Deserialize_::deserialize(k, m);
  switch (k) {
    case 0:
      int32_t i32;
      rrr::Deserialize_::deserialize(i32, m);
      value.set_i32(i32);
      break;
    case 1:
      int64_t i64;
      rrr::Deserialize_::deserialize(i64, m);
      value.set_i64(i64);
      break;
    case 2:
      double d;
      rrr::Deserialize_::deserialize(d, m);
      value.set_double(d);
      break;
    case 3:
      std::string str;
      rrr::Deserialize_::deserialize(str, m);
      value.set_str(str);
      break;
  }
  return m;
}

// archive operators for mdb::Value. Wire
// format is byte-for-byte identical to the Marshal-based operators
// above: ver_ (i64 typically), then i32 tag (0=I32, 1=I64, 2=DOUBLE,
// 3=STR), then the payload.
BinaryWriteArchive &operator<<(BinaryWriteArchive &ar, const mdb::Value &value) {
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
  return ar;
}

BinaryReadArchive &operator>>(BinaryReadArchive &ar, mdb::Value &value) {
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
  return ar;
}

} // namespace rrr

