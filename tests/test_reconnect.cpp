// Integration test for the WS client's transparent reconnect (#13), against a
// local TLS websocket server — no Alpaca, no network, no market hours. This is
// the one path that was "asserted, not tested": a working connection drops mid-
// stream and read_frame() must re-establish (reconnect → re-auth → re-subscribe)
// and resume, with the caller none the wiser.
//
// The client is used UNMODIFIED. The only test-side trick is trusting the test
// server's self-signed cert via SSL_CERT_FILE (which the client's
// set_default_verify_paths already honours) — so no production code changes to
// make this testable, just environment setup before the client is constructed.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include "ingest/alpaca_client.h"

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace asio      = boost::asio;
namespace ssl       = boost::asio::ssl;
using tcp           = asio::ip::tcp;

namespace {

// Self-signed cert + key for the test server (CN=ohlcv-test, 10y). The client
// doesn't do hostname verification, so the chain just needs to validate against
// the cert we mark trusted via SSL_CERT_FILE.
constexpr const char* kCertPem = R"PEM(-----BEGIN CERTIFICATE-----
MIICpjCCAY4CCQCqddSH3POwEDANBgkqhkiG9w0BAQsFADAVMRMwEQYDVQQDDApv
aGxjdi10ZXN0MB4XDTI2MDYxOTIwNTYxOVoXDTM2MDYxNjIwNTYxOVowFTETMBEG
A1UEAwwKb2hsY3YtdGVzdDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEB
AMN05r1DgFeXGiZ37jePbsXe/s2rZXaOafE1pSo8U8jlVhrXVhlFp1V/KFIxupUA
sCdPSejvJ24rqqtoRQHe22708Jka3pDUb4CHshC8R53pypKiaHoRniDZzx5rFvun
uQD58AwfmwLwCwCVdJR2kOguKDRZX5e8gpoZrgZ1vlC040vj7Z7GcFlvwi9wI5r1
6wy+fObz++mlSopmLU9m79NdmyBGt10ylZA0yXAaZ5abJlQex1iJqK9saEwP76Ou
o8hJOm0E1AL6FmU/m8Le6ISEglWAWliIwEY/JRdIhM4z+VDsqvcGYrwr3dRiZeRl
IMP2q2kO3ir+KLHU3WBhNI0CAwEAATANBgkqhkiG9w0BAQsFAAOCAQEAvR95hAC+
7iA4hwlDmoY50qjEieTTEx82fFJi+4gbgc2A9CuGfKpZ+4GhCsif4q8//mDDCf0Y
CAdGCuvI2YIYbnSwoxc575Vwb4Av+TF4KFBxWhHgDhSzwW9P5lQTD/nmfLcNdjST
5u/WgsJrpn9nYuob56cQCEbuBekquJX55bz3zFZ2GozvqF6HuPTCBfHOahL83bBC
/OyClAi5QMw/UIHznwp24Jqyqr5Ag/1OA5CiqmSJ7Ewqf5YwjneKLjY47x5RhoV1
gq0QZw1oETgk4pz1R+JH6w/nhX9AICAhNYkZ/Man3uaUQvCY/a+T3MMO5/+IUQco
0f7PSYNWdujBww==
-----END CERTIFICATE-----
)PEM";

constexpr const char* kKeyPem = R"PEM(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDDdOa9Q4BXlxom
d+43j27F3v7Nq2V2jmnxNaUqPFPI5VYa11YZRadVfyhSMbqVALAnT0no7yduK6qr
aEUB3ttu9PCZGt6Q1G+Ah7IQvEed6cqSomh6EZ4g2c8eaxb7p7kA+fAMH5sC8AsA
lXSUdpDoLig0WV+XvIKaGa4Gdb5QtONL4+2exnBZb8IvcCOa9esMvnzm8/vppUqK
Zi1PZu/TXZsgRrddMpWQNMlwGmeWmyZUHsdYiaivbGhMD++jrqPISTptBNQC+hZl
P5vC3uiEhIJVgFpYiMBGPyUXSITOM/lQ7Kr3BmK8K93UYmXkZSDD9qtpDt4q/iix
1N1gYTSNAgMBAAECggEBALX7cxaG8ckb0+o7Qd4TOuUawg0GZzriUDuYYgaYEr56
4ReupOh01N8ivI0C5iDzeg+voDYz7XeDSq27MH0UXSTLA/TZcp5QXNzD+wPf+aJK
2iR6+GGnY55cjZ5ZwRVgTT1eeKUhDDfI/cV2YgwU9MhoqWBMUle52bPW8xPqrt7+
Wo0VDHw9GAc0Uc+MIOWyyHV52YNiH0rRvhh9ws8B8Y6A/evjCrBa+XhS3Tl+7KdR
ZqycO730vHBSRN+ULXpZONoMUHuryY0Mx3pkrsd//VI4Bip9ySQkUTYNHOYhch4o
AGpzV2iQGYlRKu3rJMYt0iRmrCYYbCElUHr4WMGJplUCgYEA861LKiU4+dJYttCe
8tDyibhV7J7sMac9DMoGe/tpMeThu6TM0g04GwydNOtgI0w+sKpCJM0f6z7IrUia
AZrS9sHPTXcL2BqcHULw1RQlpKhy2SK/QtJsC/3v2JI0EhuMdtSSdIY4FJSYH7ei
DjZnuXKgcbDGcDGYG3NnW9eP4WsCgYEAzVdVyRy8KTnWoMHO5veirwG5U0u8i/MI
tB6yGhnk5PFfSa+A7ujsdmFbKwcHNcTle11zfxJRnEqyjVazoJoRHcFEnL2I5OnO
Am89BTf1+1uZiUTco/lcyJQNayQijmGBkXP98HYnJI0WH1uCnu/6HxWXN/ElKosK
ZFKbweSAp+cCgYA6EuveAH8Cswnnj/LBxeB3yBHaUcnSz5uyJ5fCBpn8hSLzOISD
7xiXAbuZuBrybqJmMj2PTb+0rgLfoXTquv5aRrhkKuIMv9LC+ogxEBskkezFKQ3S
HBoaBYwa3kVAp4Yjb+fzk2VcKknTDU22+2pe/R2V2t6AMKGisS9J6SbmPwKBgQCg
gt1m+NRIsZKJVRZDy11aydExQGmhSBgMnFYCOy2GnPssYUk6984nd1DJoJPNPx1X
QqSOtyYeMvHBs/1z1Br/FF1q3GmO7wh/NK8RTj40/tRUzgRfFQSnMbwPfU6Z17Rt
m6rr9aABXVvmpSTE0rfE5p6vNwwjZk54P27LsK+1DwKBgD5Ubx9bbfguDSJvL+4R
bLcxRKASuRkMUK3tCNaxbv8iiw2LAXqhU4Ydyfs+PeESrQWDY8PrsoIf8mzXA0ht
h91obozvJt6ExyrNa54noWa7BaIU2Toq5T7lI71ts5EI4t2UdzQ7SH9vFgC8+Uzx
3+efg8ChkJUyU6ZaUNFnWziO
-----END PRIVATE KEY-----
)PEM";

// A throwaway local TLS websocket server. Handles exactly two connections on its
// own thread: the first it accepts, reads the client's auth + subscribe, then
// drops (simulating a mid-stream disconnect); the second (the reconnect) it
// accepts, reads the replayed auth + subscribe, and sends one frame so the
// client's read_frame() returns.
class TestServer {
public:
    TestServer() {
        std::promise<unsigned short> port_promise;
        auto port_future = port_promise.get_future();
        thread_ = std::thread([this, p = std::move(port_promise)]() mutable {
            run(std::move(p));
        });
        port_ = port_future.get();  // blocks until bound + listening
    }

    ~TestServer() {
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] unsigned short port() const noexcept { return port_; }
    [[nodiscard]] int connections() const noexcept {
        return connections_.load();
    }

private:
    void run(std::promise<unsigned short> port_promise) {
        try {
            asio::io_context ioc;
            ssl::context     ctx{ssl::context::tlsv12_server};
            ctx.use_certificate_chain(asio::buffer(std::string{kCertPem}));
            ctx.use_private_key(asio::buffer(std::string{kKeyPem}),
                                ssl::context::pem);

            tcp::acceptor acceptor{
                ioc, tcp::endpoint{asio::ip::make_address("127.0.0.1"), 0}};
            port_promise.set_value(acceptor.local_endpoint().port());

            for (int i = 0; i < 2; ++i) {
                tcp::socket sock{ioc};
                acceptor.accept(sock);
                ++connections_;

                websocket::stream<beast::ssl_stream<tcp::socket>> ws{
                    std::move(sock), ctx};
                ws.next_layer().handshake(ssl::stream_base::server);
                ws.accept();  // websocket upgrade

                beast::flat_buffer a, s;
                ws.read(a);  // client's auth message
                ws.read(s);  // client's subscribe message

                if (i == 0) {
                    // Drop: tear the TCP connection down so the client's next
                    // read fails and its reconnect kicks in.
                    beast::error_code ec;
                    ws.next_layer().next_layer().close(ec);
                } else {
                    // The reconnect succeeded — send a frame so read_frame returns.
                    ws.write(asio::buffer(std::string{R"({"T":"test","ok":1})"}));
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    beast::error_code ec;
                    ws.next_layer().next_layer().close(ec);
                }
            }
        } catch (const std::exception&) {
            // Best-effort test server; failures surface as the client test
            // assertions not being met.
        }
    }

    std::thread      thread_;
    unsigned short   port_        = 0;
    std::atomic<int> connections_ = 0;
};

}  // namespace

// The headline: a working connection drops, and the *unmodified* client
// transparently reconnects and resumes — proving #13's socket path, not asserting
// it.
TEST(Reconnect, ReestablishesAfterDrop) {
    // Trust the test server's self-signed cert by pointing SSL_CERT_FILE at it
    // BEFORE the client is constructed (its ctor reads the default verify paths).
    const std::string cert_path = "/tmp/ohlcv_reconnect_test_cert.pem";
    {
        std::ofstream f(cert_path);
        f << kCertPem;
    }
    ::setenv("SSL_CERT_FILE", cert_path.c_str(), 1);

    TestServer server;

    ohlcv::ingest::AlpacaConfig cfg;
    cfg.host           = "127.0.0.1";
    cfg.port           = std::to_string(server.port());
    cfg.key_id         = "test-key";
    cfg.secret_key     = "test-secret";
    cfg.idle_timeout   = std::chrono::seconds(5);
    cfg.auto_reconnect = true;

    ohlcv::ingest::AlpacaClient client{std::move(cfg)};
    client.connect();
    client.authenticate();
    client.subscribe({"AAPL"}, {}, {});

    // First connection has been dropped by the server; this read must trigger a
    // transparent reconnect and come back with the frame the *second* connection
    // sends — not throw, not hang.
    const std::string frame = client.read_frame();

    EXPECT_NE(frame.find("\"T\":\"test\""), std::string::npos)
        << "expected the post-reconnect frame, got: " << frame;
    EXPECT_GE(server.connections(), 2)
        << "client did not re-establish after the drop";

    client.close();
}
