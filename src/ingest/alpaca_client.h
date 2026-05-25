#pragma once

#include <memory>
#include <string>
#include <vector>

namespace ohlcv::ingest {

struct AlpacaConfig {
    std::string host = "stream.data.alpaca.markets";
    std::string port = "443";
    std::string path = "/v2/iex";  // free-tier IEX feed
    std::string key_id;
    std::string secret_key;
};

// Synchronous TLS WebSocket client for Alpaca's market data stream.
// v1 is intentionally blocking and single-threaded so the data path is
// trivial to reason about; the same shape ports to async/io_uring later.
class AlpacaClient {
public:
    explicit AlpacaClient(AlpacaConfig config);
    ~AlpacaClient();

    AlpacaClient(const AlpacaClient&) = delete;
    AlpacaClient& operator=(const AlpacaClient&) = delete;

    void connect();
    void authenticate();
    void subscribe(const std::vector<std::string>& trades,
                   const std::vector<std::string>& quotes,
                   const std::vector<std::string>& bars);

    // Blocking read of one frame. Returns the raw payload as a string.
    std::string read_frame();

    void close();

private:
    AlpacaConfig config_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ohlcv::ingest
