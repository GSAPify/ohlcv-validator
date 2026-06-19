#include "ingest/alpaca_client.h"

#include <chrono>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>

#include "util/backoff.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace asio      = boost::asio;
namespace ssl       = boost::asio::ssl;
using tcp           = asio::ip::tcp;
using json          = nlohmann::json;

namespace ohlcv::ingest {

struct AlpacaClient::Impl {
    asio::io_context ioc;
    ssl::context     ssl_ctx{ssl::context::tlsv12_client};
    std::optional<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws;

    Impl() {
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(ssl::verify_peer);
    }
};

AlpacaClient::AlpacaClient(AlpacaConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {}

AlpacaClient::~AlpacaClient() = default;

void AlpacaClient::connect() {
    auto& ws = impl_->ws.emplace(impl_->ioc, impl_->ssl_ctx);

    tcp::resolver       resolver{impl_->ioc};
    const auto          endpoints = resolver.resolve(config_.host, config_.port);
    beast::get_lowest_layer(ws).connect(endpoints);

    // SNI is mandatory on most modern TLS endpoints; without it the server
    // can't pick the right cert and the handshake fails opaquely.
    if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(),
                                  config_.host.c_str())) {
        throw std::runtime_error("SSL_set_tlsext_host_name failed");
    }

    ws.next_layer().handshake(ssl::stream_base::client);

    // Start from the suggested client timeouts, but give the read a finite
    // idle_timeout + keep-alive pings. Beast's client default is no idle timeout
    // (idle_timeout = none, keep_alive_pings = false), so a quiet or half-open
    // peer blocks read() forever — the exact infinite hang observed live. With
    // this, a dead peer surfaces as a timeout the read loop turns into a
    // reconnect; a quiet-but-alive peer pongs the pings and stays up.
    auto to = websocket::stream_base::timeout::suggested(beast::role_type::client);
    to.idle_timeout     = config_.idle_timeout;
    to.keep_alive_pings = true;
    ws.set_option(to);
    ws.set_option(
        websocket::stream_base::decorator([](websocket::request_type& req) {
            req.set(http::field::user_agent, "ohlcv-validator/0.0.1");
        }));

    const std::string host_header = config_.host + ":" + config_.port;
    ws.handshake(host_header, config_.path);

    spdlog::info("websocket connected to {}{}", config_.host, config_.path);
}

void AlpacaClient::authenticate() {
    const json msg = {
        {"action", "auth"},
        {"key",    config_.key_id},
        {"secret", config_.secret_key},
    };
    impl_->ws->write(asio::buffer(msg.dump()));
    spdlog::info("auth message sent (key_id={}...)",
                 config_.key_id.substr(0, 4));
}

void AlpacaClient::subscribe(const std::vector<std::string>& trades,
                             const std::vector<std::string>& quotes,
                             const std::vector<std::string>& bars) {
    // Remember the subscription so a reconnect can replay it.
    sub_trades_ = trades;
    sub_quotes_ = quotes;
    sub_bars_   = bars;
    subscribed_ = true;
    send_subscribe(trades, quotes, bars);
}

void AlpacaClient::send_subscribe(const std::vector<std::string>& trades,
                                  const std::vector<std::string>& quotes,
                                  const std::vector<std::string>& bars) {
    const json msg = {
        {"action", "subscribe"},
        {"trades", trades},
        {"quotes", quotes},
        {"bars",   bars},
    };
    impl_->ws->write(asio::buffer(msg.dump()));
    spdlog::info("subscribed: trades={}, quotes={}, bars={}",
                 trades.size(), quotes.size(), bars.size());
}

void AlpacaClient::establish() {
    // base/cap for the exponential backoff; jitter is applied per attempt.
    constexpr auto kBase = std::chrono::milliseconds{250};
    constexpr auto kCap  = std::chrono::milliseconds{30'000};
    static thread_local std::mt19937 rng{std::random_device{}()};

    for (unsigned attempt = 0;; ++attempt) {
        if (stop_requested()) return;  // shutting down — stop trying to reconnect
        try {
            connect();
            authenticate();
            if (subscribed_) send_subscribe(sub_trades_, sub_quotes_, sub_bars_);
            spdlog::info("reconnected after {} attempt(s)", attempt + 1);
            return;
        } catch (const std::exception& e) {
            const auto ceil = ohlcv::util::backoff_ceiling(attempt, kBase, kCap);
            // Equal jitter: wait in [ceil/2, ceil] so retries don't synchronise.
            std::uniform_int_distribution<long long> dist(ceil.count() / 2,
                                                          ceil.count());
            const std::chrono::milliseconds delay{dist(rng)};
            spdlog::warn("reconnect attempt {} failed ({}); retrying in {} ms",
                         attempt + 1, e.what(), delay.count());
            std::this_thread::sleep_for(delay);
        }
    }
}

std::string AlpacaClient::read_frame() {
    for (;;) {
        if (stop_requested()) return {};  // asked to stop before blocking again
        try {
            beast::flat_buffer buffer;
            impl_->ws->read(buffer);
            return beast::buffers_to_string(buffer.data());
        } catch (const beast::system_error& e) {
            // A pending stop turns the read failure — including the idle_timeout
            // that fires on a quiet feed — into a clean bail (empty frame) instead
            // of a reconnect. That's what lets Ctrl+C on an idle feed exit within
            // idle_timeout and shut down gracefully, rather than spinning the
            // reconnect loop forever (the infinite hang seen live).
            if (stop_requested()) return {};
            if (!config_.auto_reconnect) throw;
            // Dead/dropped peer (incl. idle_timeout). Re-establish and retry; the
            // reconnect's ack frames will be returned by subsequent reads.
            spdlog::warn("read failed ({}); reconnecting", e.code().message());
            establish();
        }
    }
}

void AlpacaClient::close() {
    if (!impl_->ws) return;
    beast::error_code ec;
    impl_->ws->close(websocket::close_code::normal, ec);
    if (ec && ec != beast::errc::not_connected) {
        spdlog::warn("websocket close error: {}", ec.message());
    }
}

}  // namespace ohlcv::ingest
