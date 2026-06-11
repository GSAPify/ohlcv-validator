#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "model/bar.h"

namespace ohlcv::ingest {

// Result of parsing one Alpaca frame. A single frame is a JSON array which
// may contain mixed message types (trades, bars, acks, errors).
struct ParsedFrame {
    std::vector<model::Trade> trades;
    std::vector<model::Bar>   bars;
    std::vector<model::Quote> quotes;
    std::vector<std::string>  errors;          // payloads of T="error" messages
    std::vector<std::string>  unknown_types;   // T values we don't handle yet
    bool has_connected_ack       = false;
    bool has_authenticated_ack   = false;
    bool has_subscription_ack    = false;
};

// Stateful so simdjson's internal buffers can be reused across calls.
class Parser {
public:
    Parser();
    ~Parser();

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    // Throws on malformed JSON. Individual unparseable messages within an
    // otherwise-valid frame are reported via the result instead.
    ParsedFrame parse(std::string_view frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ohlcv::ingest
