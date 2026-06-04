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

}  // namespace ohlcv::replay
