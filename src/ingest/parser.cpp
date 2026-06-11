#include "ingest/parser.h"

#include <stdexcept>
#include <string>
#include <utility>

#include <simdjson.h>

#include "model/timestamp.h"

namespace ohlcv::ingest {

struct Parser::Impl {
    simdjson::ondemand::parser parser;
    // Reused across calls so simdjson can keep its scratch buffers warm.
    simdjson::padded_string scratch{simdjson::padded_string(0)};
};

Parser::Parser() : impl_(std::make_unique<Impl>()) {}
Parser::~Parser() = default;

namespace {

// Pull a string field as std::string. Returns empty if missing/wrong type.
std::string get_string(simdjson::ondemand::object& obj, std::string_view key) {
    std::string_view sv;
    if (auto err = obj[key].get_string().get(sv); err) {
        return {};
    }
    return std::string{sv};
}

double get_double(simdjson::ondemand::object& obj, std::string_view key) {
    double v = 0.0;
    if (auto err = obj[key].get_double().get(v); err) {
        return 0.0;
    }
    return v;
}

std::uint64_t get_u64(simdjson::ondemand::object& obj, std::string_view key) {
    std::uint64_t v = 0;
    if (auto err = obj[key].get_uint64().get(v); err) {
        return 0;
    }
    return v;
}

void parse_trade(simdjson::ondemand::object& obj, ParsedFrame& out) {
    model::Trade t;
    t.symbol   = get_string(obj, "S");
    t.trade_id = get_u64(obj, "i");
    t.exchange = get_string(obj, "x");
    t.price    = get_double(obj, "p");
    t.size     = get_u64(obj, "s");
    t.tape     = get_string(obj, "z");

    std::string ts = get_string(obj, "t");
    if (!ts.empty()) {
        t.ts_ns = model::parse_rfc3339_nano(ts);
    }
    out.trades.push_back(std::move(t));
}

void parse_bar(simdjson::ondemand::object& obj, ParsedFrame& out) {
    model::Bar b;
    b.symbol      = get_string(obj, "S");
    b.open        = get_double(obj, "o");
    b.high        = get_double(obj, "h");
    b.low         = get_double(obj, "l");
    b.close       = get_double(obj, "c");
    b.volume      = get_u64(obj, "v");
    b.trade_count = get_u64(obj, "n");
    b.vwap        = get_double(obj, "vw");

    std::string ts = get_string(obj, "t");
    if (!ts.empty()) {
        b.start_ns = model::parse_rfc3339_nano(ts);
    }
    out.bars.push_back(std::move(b));
}

void parse_quote(simdjson::ondemand::object& obj, ParsedFrame& out) {
    model::Quote q;
    q.symbol       = get_string(obj, "S");
    q.bid_exchange = get_string(obj, "bx");
    q.bid_price    = get_double(obj, "bp");
    q.bid_size     = get_u64(obj, "bs");
    q.ask_exchange = get_string(obj, "ax");
    q.ask_price    = get_double(obj, "ap");
    q.ask_size     = get_u64(obj, "as");
    q.tape         = get_string(obj, "z");

    std::string ts = get_string(obj, "t");
    if (!ts.empty()) {
        q.ts_ns = model::parse_rfc3339_nano(ts);
    }
    out.quotes.push_back(std::move(q));
}

void parse_success(simdjson::ondemand::object& obj, ParsedFrame& out) {
    const auto msg = get_string(obj, "msg");
    if (msg == "connected") {
        out.has_connected_ack = true;
    } else if (msg == "authenticated") {
        out.has_authenticated_ack = true;
    }
}

void parse_error(simdjson::ondemand::object& obj, ParsedFrame& out) {
    out.errors.push_back(get_string(obj, "msg"));
}

}  // namespace

ParsedFrame Parser::parse(std::string_view frame) {
    impl_->scratch = simdjson::padded_string{frame};

    auto doc = impl_->parser.iterate(impl_->scratch);

    simdjson::ondemand::array array;
    if (doc.get_array().get(array)) {
        throw std::runtime_error("frame is not a JSON array");
    }

    ParsedFrame out;
    for (auto element : array) {
        simdjson::ondemand::object obj;
        if (element.get_object().get(obj)) {
            out.unknown_types.emplace_back("<not an object>");
            continue;
        }

        std::string_view type;
        if (obj["T"].get_string().get(type)) {
            out.unknown_types.emplace_back("<missing T>");
            continue;
        }

        if (type == "t") {
            parse_trade(obj, out);
        } else if (type == "q") {
            parse_quote(obj, out);
        } else if (type == "b") {
            parse_bar(obj, out);
        } else if (type == "success") {
            parse_success(obj, out);
        } else if (type == "subscription") {
            out.has_subscription_ack = true;
        } else if (type == "error") {
            parse_error(obj, out);
        } else {
            out.unknown_types.emplace_back(type);
        }
    }
    return out;
}

}  // namespace ohlcv::ingest
