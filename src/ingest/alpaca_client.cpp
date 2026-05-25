#include "ingest/alpaca_client.h"

#include <optional>
#include <stdexcept>

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

    ws.set_option(
        websocket::stream_base::timeout::suggested(beast::role_type::client));
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

std::string AlpacaClient::read_frame() {
    beast::flat_buffer buffer;
    impl_->ws->read(buffer);
    return beast::buffers_to_string(buffer.data());
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
