#pragma once

#include <cstdint>
#include <type_traits>

#include "model/wire.h"

namespace ohlcv::replay {

// On-disk replay format. The file is a header followed by a flat array of
// fixed-stride WireRecords, so reading it back is mmap + reinterpret_cast — no
// parsing, no allocation, no copy. This is the binary-feed analogue of an
// exchange's fixed-layout message stream (ITCH/SBE-style), and the reason the
// replay benchmark measures validation cost rather than JSON-parse cost.

inline constexpr std::uint32_t kMagic   = 0x564C484F;  // 'OHLV' little-endian
inline constexpr std::uint32_t kVersion = 2;           // v2 adds quote records

struct FileHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint64_t record_count;
};

enum class RecordType : std::uint8_t { Trade = 0, Bar = 1, Quote = 2 };

struct WireRecord {
    std::uint8_t type;        // RecordType
    std::uint8_t _pad[7];
    union Body {
        model::WireTrade trade;  // valid iff type == Trade
        model::WireBar   bar;    // valid iff type == Bar
        model::WireQuote quote;  // valid iff type == Quote
    } body;
};

static_assert(std::is_trivially_copyable_v<WireRecord>,
              "WireRecord must be memcpy/mmap-safe");
static_assert(std::is_trivially_copyable_v<FileHeader>);

// The on-disk contract. These sizes ARE the file format: anything reading the
// replay file (the C++ mmap path, the Python ML reader in ml/) hardcodes a
// 16-byte header and an 88-byte record stride. Pin them so a field added to a
// wire struct fails the build here instead of silently shifting the stride and
// corrupting every downstream reader.
static_assert(sizeof(FileHeader) == 16, "replay header is 16 bytes on disk");
static_assert(sizeof(WireRecord) == 88, "replay record stride is 88 bytes on disk");

}  // namespace ohlcv::replay
