#pragma once

#include <cstddef>
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

// A bounds-checked view of a mapped replay file. `error` is non-null iff the
// mapping is unusable, in which case `records` is null and must not be read.
struct ReplayView {
    const WireRecord* records = nullptr;
    std::uint64_t     count   = 0;
    const char*       error   = nullptr;
};

// Validate a mapped replay file before a single record is dereferenced. The
// header's record_count is attacker- and truncation-controlled, so trusting it
// walks the reader off the end of the mapping (SIGSEGV). Same rule the Python
// reader in ml/replay_reader.py enforces: the size check is not optional.
[[nodiscard]] inline ReplayView view_replay(const std::byte* base,
                                            std::size_t      size) noexcept {
    ReplayView v;
    if (base == nullptr || size < sizeof(FileHeader)) {
        v.error = "truncated header (file smaller than 16 bytes)";
        return v;
    }
    const auto* hdr = reinterpret_cast<const FileHeader*>(base);
    if (hdr->magic != kMagic) {
        v.error = "bad magic: not a replay file";
        return v;
    }
    if (hdr->version != kVersion) {
        v.error = "unsupported format version (reader supports v2)";
        return v;
    }
    // Divide rather than multiply: count * sizeof(WireRecord) wraps for a
    // hostile count near UINT64_MAX and would slide straight past a <= check.
    if (hdr->record_count > (size - sizeof(FileHeader)) / sizeof(WireRecord)) {
        v.error = "record_count exceeds file size (truncated or corrupt)";
        return v;
    }
    v.records = reinterpret_cast<const WireRecord*>(base + sizeof(FileHeader));
    v.count   = hdr->record_count;
    return v;
}

}  // namespace ohlcv::replay
