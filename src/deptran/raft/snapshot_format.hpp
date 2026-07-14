#pragma once

/**
 * Snapshot Binary Format for Raft/Paxos Consensus Protocols
 *
 * This header defines:
 * - SnapshotCompression: Compression type enum
 * - SnapshotChecksumType: Checksum algorithm enum
 * - SnapshotHeader: Binary header structure
 * - CRC32: Fast CRC32 checksum implementation
 * - SnapshotFormat: Serialization/deserialization utilities
 *
 * Binary Format:
 *   Magic (4B) | Version (4B) | Header Size (4B) | Data Size (8B) |
 *   Compression (1B) | Checksum Type (1B) | Last Index (8B) | Last Term (8B) |
 *   Timestamp (8B) | Header CRC (4B) | Data... | Data Checksum
 *
 * RustyCpp Compliance: Uses @safe/@unsafe annotations
 */

#include <cstdint>
#include <cstring>
#include <string>

#include "rrr/rrr.hpp"

namespace janus {
namespace raft {

/**
 * Compression type for snapshot data.
 * Currently only NONE is implemented; others reserved for future.
 */
// @safe - POD enum
enum class SnapshotCompression : uint8_t {
  NONE = 0,    // No compression
  SNAPPY = 1,  // Snappy compression (reserved)
  ZSTD = 2     // ZSTD compression (reserved)
};

/**
 * Checksum algorithm for snapshot verification.
 */
// @safe - POD enum
enum class SnapshotChecksumType : uint8_t {
  NONE = 0,    // No checksum
  CRC32 = 1,   // CRC32 (fast, 4 bytes)
  SHA256 = 2   // SHA256 (reserved, 32 bytes)
};

#pragma pack(push, 1)
#if RUSTYCPP_RUST
pub struct SnapshotHeader {
    magic: u32,
    version: u32,
    header_size: u32,
    data_size: u64,
    compression: u8,
    checksum_type: u8,
    last_index: u64,
    last_term: u64,
    timestamp_ms: u64,
    header_crc: u32,
    padding_0: u8,
    padding_1: u8,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=snapshot_format.1 version=1 rust_sha256=b74ec3683c4b1bf832130aa69425ce786773dbcdde4858cee0a9f00f32958a6e*/
struct SnapshotHeader;

struct SnapshotHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint64_t data_size;
    uint8_t compression;
    uint8_t checksum_type;
    uint64_t last_index;
    uint64_t last_term;
    uint64_t timestamp_ms;
    uint32_t header_crc;
    uint8_t padding_0;
    uint8_t padding_1;
};
/*RUSTYCPP:GEN-END id=snapshot_format.1*/
#pragma pack(pop)

static_assert(sizeof(SnapshotHeader) == 52, "SnapshotHeader must be 52 bytes");

#if RUSTYCPP_RUST
pub fn snapshot_header_defaults() -> SnapshotHeader {
    SnapshotHeader {
        magic: 0x504E4153,
        version: 1,
        header_size: 52,
        data_size: 0,
        compression: SnapshotCompression::NONE as u8,
        checksum_type: SnapshotChecksumType::NONE as u8,
        last_index: 0,
        last_term: 0,
        timestamp_ms: 0,
        header_crc: 0,
        padding_0: 0,
        padding_1: 0,
    }
}

pub fn snapshot_header_make(compression: SnapshotCompression,
                            checksum_type: SnapshotChecksumType,
                            data_size: u64,
                            last_index: u64,
                            last_term: u64,
                            timestamp_ms: u64) -> SnapshotHeader {
    SnapshotHeader {
        magic: 0x504E4153,
        version: 1,
        header_size: 52,
        data_size,
        compression: compression as u8,
        checksum_type: checksum_type as u8,
        last_index,
        last_term,
        timestamp_ms,
        header_crc: 0,
        padding_0: 0,
        padding_1: 0,
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=snapshot_format.2 version=1 rust_sha256=f76ce10c957962fbbcdb4719341799084e0a0c05ba27443b6966d585b065f3eb*/
inline SnapshotHeader snapshot_header_defaults() {
    return SnapshotHeader{.magic = 1347305811, .version = 1, .header_size = 52, .data_size = 0, .compression = static_cast<uint8_t>(SnapshotCompression::NONE), .checksum_type = static_cast<uint8_t>(SnapshotChecksumType::NONE), .last_index = 0, .last_term = 0, .timestamp_ms = 0, .header_crc = 0, .padding_0 = 0, .padding_1 = 0};
}

inline SnapshotHeader snapshot_header_make(SnapshotCompression compression, SnapshotChecksumType checksum_type, uint64_t data_size, uint64_t last_index, uint64_t last_term, uint64_t timestamp_ms) {
    return SnapshotHeader{.magic = 1347305811, .version = 1, .header_size = 52, .data_size = std::move(data_size), .compression = static_cast<uint8_t>(compression), .checksum_type = static_cast<uint8_t>(checksum_type), .last_index = std::move(last_index), .last_term = std::move(last_term), .timestamp_ms = std::move(timestamp_ms), .header_crc = 0, .padding_0 = 0, .padding_1 = 0};
}
/*RUSTYCPP:GEN-END id=snapshot_format.2*/

/**
 * CRC32 checksum calculator (IEEE 802.3 polynomial).
 * Table-driven implementation for speed.
 */
class CRC32 {
 public:
  // @safe - Default constructor
  CRC32() : crc_(0xFFFFFFFF) {}

  // @unsafe - Reads from raw pointer
  void Update(const char* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
      uint8_t byte = static_cast<uint8_t>(data[i]);
      crc_ = TABLE[(crc_ ^ byte) & 0xFF] ^ (crc_ >> 8);
    }
  }

  // @safe - Returns final CRC value
  uint32_t Finalize() const {
    return crc_ ^ 0xFFFFFFFF;
  }

  /**
   * Calculate CRC32 of a buffer in one call.
   */
  // @unsafe - Reads from raw pointer
  static uint32_t Calculate(const char* data, size_t size) {
    CRC32 crc;
    crc.Update(data, size);
    return crc.Finalize();
  }

  // DSL wrapper target; keep the table-driven implementation in Calculate().
  static uint32_t calculate(const char* data, size_t size) {
    return Calculate(data, size);
  }

  static uint32_t calculate(const uint8_t* data, size_t size) {
    return Calculate(reinterpret_cast<const char*>(data), size);
  }

 private:
  uint32_t crc_;

  // IEEE 802.3 polynomial: 0xEDB88320 (reversed)
  // @safe - Static lookup table
  static constexpr uint32_t TABLE[256] = {
      0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F,
      0xE963A535, 0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
      0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
      0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
      0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9,
      0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
      0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
      0xDBBBBBD6, 0xACBCCB40, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
      0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
      0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
      0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D, 0x76DC4190, 0x01DB7106,
      0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
      0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
      0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
      0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
      0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
      0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7,
      0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
      0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7A47, 0x5005713C, 0x270241AA,
      0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
      0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
      0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
      0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683, 0xE3630B12, 0x94643B84,
      0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
      0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB,
      0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
      0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
      0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
      0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55,
      0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
      0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28,
      0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
      0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
      0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
      0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242,
      0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
      0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69,
      0x616BFFD3, 0x166CCF45, 0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
      0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAE412ADA, 0xD946334C,
      0x4024D3F6, 0x37D3E760, 0xA9B3C6C3, 0xDEB4E655, 0x47B5D7EF, 0x30B2C779,
      0xDEED4E3C, 0xA9EA7EAA, 0x30E34910, 0x47E45986, 0xD980CC25, 0xAEDE87B3,
      0x37D7D609, 0x40D0E69F, 0xD0E67E0E, 0xA7E14E98, 0x3EE8F522, 0x49EFC5B4,
      0xD7A8D017, 0xA0AFE081, 0x39A6B13B, 0x4EA181AD};
};

#if RUSTYCPP_RUST
pub fn snapshot_crc32(data: *const u8, size: usize) -> u32 {
    CRC32::calculate(data, size)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=snapshot_format.3 version=1 rust_sha256=806411356a34b347f15210e8fe691574054596416297b55d4eb211c98f257729*/
inline uint32_t snapshot_crc32(const uint8_t* data, size_t size);

inline uint32_t snapshot_crc32(const uint8_t* data, size_t size) {
    return CRC32::calculate(data, std::move(size));
}
/*RUSTYCPP:GEN-END id=snapshot_format.3*/

inline uint32_t snapshot_crc32(const char* data, size_t size) {
  return snapshot_crc32(reinterpret_cast<const uint8_t*>(data), size);
}

/**
 * Snapshot format serialization and deserialization utilities.
 */
class SnapshotFormat {
 public:
  // Magic number: "SNAP" in little-endian
  static constexpr uint32_t MAGIC = 0x504E4153;
  // Format version
  static constexpr uint32_t VERSION = 1;

  /**
   * Serialize snapshot data to binary format.
   * @param last_index Last included log index
   * @param last_term Term of last included entry
   * @param data State machine data
   * @param size Size of data
   * @param output Output buffer (will be resized)
   * @param compression Compression type (NONE only for now)
   * @param checksum_type Checksum algorithm
   * @return true if serialization succeeded
   */
  // @unsafe - Reads from raw pointer, allocates output
  static bool Serialize(uint64_t last_index,
                        uint64_t last_term,
                        const char* data,
                        size_t size,
                        std::string* output,
                        SnapshotCompression compression = SnapshotCompression::NONE,
                        SnapshotChecksumType checksum_type = SnapshotChecksumType::CRC32) {
    if (!output) {
      Log_error("[SNAPSHOT-FORMAT] Serialize: null output");
      return false;
    }

    // Only NONE compression is supported
    if (compression != SnapshotCompression::NONE) {
      Log_error("[SNAPSHOT-FORMAT] Serialize: compression not supported");
      return false;
    }

    SnapshotHeader header = snapshot_header_make(
        compression,
        checksum_type,
        size,
        last_index,
        last_term,
        GetCurrentTimeMs());

    // Calculate header CRC (excluding header_crc field itself and padding)
    // CRC covers bytes 0..43 (before header_crc field at offset 44)
    header.header_crc = snapshot_crc32(
        reinterpret_cast<const char*>(&header), 44);

    // Calculate data checksum
    uint32_t data_crc = 0;
    if (checksum_type == SnapshotChecksumType::CRC32) {
      data_crc = snapshot_crc32(data, size);
    }

    // Calculate output size: header + data + checksum
    size_t checksum_size = (checksum_type == SnapshotChecksumType::CRC32) ? 4 : 0;
    size_t total_size = sizeof(SnapshotHeader) + size + checksum_size;

    // Resize output and copy data
    output->resize(total_size);
    char* ptr = output->data();

    // Copy header
    std::memcpy(ptr, &header, sizeof(SnapshotHeader));
    ptr += sizeof(SnapshotHeader);

    // Copy data
    if (size > 0 && data != nullptr) {
      std::memcpy(ptr, data, size);
      ptr += size;
    }

    // Copy checksum
    if (checksum_type == SnapshotChecksumType::CRC32) {
      std::memcpy(ptr, &data_crc, 4);
    }

    return true;
  }

  /**
   * Deserialize snapshot from binary format.
   * @param input Input buffer
   * @param input_size Size of input
   * @param last_index Output: last included index
   * @param last_term Output: last included term
   * @param data Output: state machine data
   * @return true if deserialization and verification succeeded
   */
  // @unsafe - Reads from raw pointer, writes to output params
  static bool Deserialize(const char* input,
                          size_t input_size,
                          uint64_t* last_index,
                          uint64_t* last_term,
                          std::string* data) {
    if (!input || !last_index || !last_term || !data) {
      Log_error("[SNAPSHOT-FORMAT] Deserialize: null parameters");
      return false;
    }

    // Check minimum size
    if (input_size < sizeof(SnapshotHeader)) {
      Log_error("[SNAPSHOT-FORMAT] Deserialize: input too small (%zu < %zu)",
                input_size, sizeof(SnapshotHeader));
      return false;
    }

    // Copy header
    SnapshotHeader header = snapshot_header_defaults();
    std::memcpy(&header, input, sizeof(SnapshotHeader));

    // Validate magic and version
    if (header.magic != MAGIC) {
      Log_error("[SNAPSHOT-FORMAT] Deserialize: invalid magic 0x%08X (expected 0x%08X)",
                header.magic, MAGIC);
      return false;
    }
    if (header.version != VERSION) {
      Log_error("[SNAPSHOT-FORMAT] Deserialize: unsupported version %u", header.version);
      return false;
    }

    // Verify header CRC
    uint32_t expected_header_crc = snapshot_crc32(input, 44);
    if (header.header_crc != expected_header_crc) {
      Log_error("[SNAPSHOT-FORMAT] Deserialize: header CRC mismatch (0x%08X != 0x%08X)",
                header.header_crc, expected_header_crc);
      return false;
    }

    // Check compression support
    if (static_cast<SnapshotCompression>(header.compression) != SnapshotCompression::NONE) {
      Log_error("[SNAPSHOT-FORMAT] Deserialize: compression not supported");
      return false;
    }

    // Calculate expected total size
    size_t checksum_size = 0;
    if (static_cast<SnapshotChecksumType>(header.checksum_type) == SnapshotChecksumType::CRC32) {
      checksum_size = 4;
    }
    size_t expected_size = sizeof(SnapshotHeader) + header.data_size + checksum_size;
    if (input_size < expected_size) {
      Log_error("[SNAPSHOT-FORMAT] Deserialize: input truncated (%zu < %zu)",
                input_size, expected_size);
      return false;
    }

    // Verify data checksum
    const char* data_ptr = input + sizeof(SnapshotHeader);
    if (static_cast<SnapshotChecksumType>(header.checksum_type) == SnapshotChecksumType::CRC32) {
      uint32_t expected_crc;
      std::memcpy(&expected_crc, data_ptr + header.data_size, 4);
      uint32_t actual_crc = snapshot_crc32(data_ptr, header.data_size);
      if (expected_crc != actual_crc) {
        Log_error("[SNAPSHOT-FORMAT] Deserialize: data CRC mismatch (0x%08X != 0x%08X)",
                  expected_crc, actual_crc);
        return false;
      }
    }

    // Extract fields
    *last_index = header.last_index;
    *last_term = header.last_term;
    data->assign(data_ptr, header.data_size);

    return true;
  }

  /**
   * Get the header from a snapshot buffer without full deserialization.
   * Useful for quick metadata inspection.
   */
  // @unsafe - Reads from raw pointer
  static bool GetHeader(const char* input, size_t input_size, SnapshotHeader* header) {
    if (!input || !header) {
      return false;
    }
    if (input_size < sizeof(SnapshotHeader)) {
      return false;
    }
    std::memcpy(header, input, sizeof(SnapshotHeader));
    if (header->magic != MAGIC || header->version != VERSION) {
      return false;
    }
    // Verify header CRC
    uint32_t expected_crc = snapshot_crc32(input, 44);
    return header->header_crc == expected_crc;
  }

 private:
  // @unsafe - Uses std::chrono
  static uint64_t GetCurrentTimeMs() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
  }
};

}  // namespace raft
}  // namespace janus
