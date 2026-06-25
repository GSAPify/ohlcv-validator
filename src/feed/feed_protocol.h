#pragma once

#include <cstdint>
#include <type_traits>

#include "replay/binary_format.h"

namespace ohlcv::feed {

// Which redundant line carried a copy of a message. An exchange publishes one
// sequence-numbered stream on two independent lines; the receiver arbitrates them.
enum class Line : std::uint8_t { A = 0, B = 1 };

// One sequenced message as it travels on the wire.
//
// The exchange assigns a monotonic per-session sequence number (ITCH/PITCH
// style); the identical numbered stream is published redundantly on lines A and
// B. The payload is the existing 88-byte WireRecord union (trade / quote / bar) --
// so a feed packet is just "a WireRecord + a sequence number + which line carried
// it", reusing the replay/validator record type rather than inventing a parallel
// one. Trivially copyable so it can be read straight off a socket by cast (the
// UDP transport in feed_handler does exactly that).
struct FeedPacket {
    std::uint64_t      seq;       // monotonic session sequence number
    std::uint8_t       line;      // Line: which redundant feed delivered this copy
    std::uint8_t       _pad[7];
    replay::WireRecord record;    // the trade / quote / bar payload
};

static_assert(std::is_trivially_copyable_v<FeedPacket>,
              "FeedPacket must be memcpy/socket-safe");
static_assert(sizeof(FeedPacket) == 104,
              "FeedPacket = 8 (seq) + 8 (line+pad) + 88 (record)");

}  // namespace ohlcv::feed
