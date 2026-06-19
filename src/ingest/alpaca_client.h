#pragma once

#include <atomic>
#include <chrono>
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

    // Dead-peer detection: Beast sends keep-alive pings and fails the read with a
    // timeout if no pong arrives within this window. Without it (Beast's client
    // default is no idle timeout) a silently half-open peer blocks the read
    // forever. On a timeout/drop the client transparently reconnects.
    std::chrono::seconds idle_timeout{15};
    bool                 auto_reconnect = true;
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

    // Point the client at the caller's stop flag (e.g. one set by a signal
    // handler). When set, read_frame() returns empty instead of reconnecting on a
    // read failure — so a Ctrl+C on a quiet feed unblocks within idle_timeout and
    // the caller can shut down cleanly, rather than the reconnect loop spinning
    // forever (the infinite-hang observed live). The pointee must outlive the
    // client.
    void set_stop_flag(const std::atomic<bool>* flag) noexcept { stop_flag_ = flag; }

    // Blocking read of one frame. Returns the raw payload as a string. With
    // auto_reconnect on, a dropped/dead connection is re-established (connect →
    // auth → re-subscribe, with exponential backoff) transparently before the
    // read is retried — callers just see the reconnect's welcome/auth/sub ack
    // frames again, which the parser already handles.
    std::string read_frame();

    void close();

private:
    // Re-establish the connection and replay auth + the last subscription, with
    // exponential backoff. Loops until connected (auto_reconnect is best-effort).
    void establish();
    // Send the subscribe message for the given symbol sets (the wire write).
    void send_subscribe(const std::vector<std::string>& trades,
                        const std::vector<std::string>& quotes,
                        const std::vector<std::string>& bars);

    [[nodiscard]] bool stop_requested() const noexcept {
        return stop_flag_ != nullptr &&
               stop_flag_->load(std::memory_order_relaxed);
    }

    AlpacaConfig config_;
    struct Impl;
    std::unique_ptr<Impl> impl_;

    const std::atomic<bool>* stop_flag_ = nullptr;  // caller-owned; may be null

    // Last subscription, remembered so a reconnect can replay it.
    std::vector<std::string> sub_trades_, sub_quotes_, sub_bars_;
    bool                     subscribed_ = false;
};

}  // namespace ohlcv::ingest
