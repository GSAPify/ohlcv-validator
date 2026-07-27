#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "replay/binary_format.h"
#include "replay/capture_writer.h"
#include "replay/mapped_file.h"

namespace rp = ohlcv::replay;

namespace {

rp::WireRecord make_trade_rec(const char* sym, std::uint64_t seq, double price) {
    rp::WireRecord r{};
    r.type = static_cast<std::uint8_t>(rp::RecordType::Trade);
    std::strncpy(r.body.trade.symbol, sym, sizeof(r.body.trade.symbol));
    r.body.trade.seq   = seq;
    r.body.trade.ts_ns = 1'700'000'000'000'000'000ULL + seq;
    r.body.trade.price = price;
    r.body.trade.size  = 10;
    return r;
}

rp::WireRecord make_quote_rec(const char* sym, std::uint64_t seq) {
    rp::WireRecord r{};
    r.type = static_cast<std::uint8_t>(rp::RecordType::Quote);
    std::strncpy(r.body.quote.symbol, sym, sizeof(r.body.quote.symbol));
    r.body.quote.seq       = seq;
    r.body.quote.bid_price = 100.0;
    r.body.quote.ask_price = 100.1;
    return r;
}

// Capturing then reading back through the SAME mmap path the replay tools use
// must yield byte-identical records and a correct header count.
TEST(CaptureWriter, RoundTripsThroughReplayFormat) {
    const std::string path = "/tmp/ohlcv_capture_roundtrip.bin";

    std::vector<rp::WireRecord> written;
    written.push_back(make_trade_rec("AAPL", 1, 144.5));
    written.push_back(make_quote_rec("AAPL", 2));
    written.push_back(make_trade_rec("MSFT", 1, 410.25));

    {
        rp::CaptureWriter w(path);
        ASSERT_TRUE(w.ok());
        for (const auto& rec : written) w.write(rec);
        EXPECT_EQ(w.count(), written.size());
    }  // destructor finalizes (patches the header count)

    rp::MappedFile m(path.c_str());
    ASSERT_NE(m.base(), nullptr);

    const auto* hdr = reinterpret_cast<const rp::FileHeader*>(m.base());
    EXPECT_EQ(hdr->magic, rp::kMagic);
    EXPECT_EQ(hdr->version, rp::kVersion);
    ASSERT_EQ(hdr->record_count, written.size());

    const auto* recs =
        reinterpret_cast<const rp::WireRecord*>(reinterpret_cast<const rp::FileHeader*>(m.base()) + 1);
    for (std::size_t i = 0; i < written.size(); ++i)
        EXPECT_EQ(std::memcmp(&recs[i], &written[i], sizeof(rp::WireRecord)), 0)
            << "record " << i << " differs after round-trip";
}

// An explicit finalize() before destruction must not double-write or corrupt.
TEST(CaptureWriter, FinalizeIsIdempotent) {
    const std::string path = "/tmp/ohlcv_capture_idempotent.bin";
    {
        rp::CaptureWriter w(path);
        ASSERT_TRUE(w.ok());
        w.write(make_trade_rec("AAPL", 1, 144.5));
        w.finalize();
        w.finalize();  // second call is a no-op
    }
    rp::MappedFile m(path.c_str());
    ASSERT_NE(m.base(), nullptr);
    const auto* hdr = reinterpret_cast<const rp::FileHeader*>(m.base());
    EXPECT_EQ(hdr->record_count, 1U);
}

// Writes a header with an arbitrary record_count followed by `body_records`
// actual records — something CaptureWriter cannot produce, since it patches the
// real count in finalize().
void write_lying_file(const std::string& path, std::uint64_t claimed_count,
                      std::size_t body_records) {
    const rp::FileHeader hdr{rp::kMagic, rp::kVersion, claimed_count};
    std::FILE*           f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fwrite(&hdr, sizeof(hdr), 1, f);
    const rp::WireRecord rec = make_trade_rec("AAPL", 1, 100.0);
    for (std::size_t i = 0; i < body_records; ++i)
        std::fwrite(&rec, sizeof(rec), 1, f);
    std::fclose(f);
}

// A well-formed file must pass the guard and expose every record.
TEST(ReplayBounds, AcceptsWellFormedFile) {
    const std::string path = "/tmp/ohlcv_bounds_ok.bin";
    write_lying_file(path, 3, 3);  // claimed == actual, so not lying

    rp::MappedFile m(path.c_str());
    ASSERT_NE(m.base(), nullptr);
    const auto view = rp::view_replay(m.base(), m.size());
    EXPECT_EQ(view.error, nullptr);
    EXPECT_EQ(view.count, 3U);
    ASSERT_NE(view.records, nullptr);
}

// The crash class: a header claiming more records than the mapping holds. The
// bench binaries used to walk straight off the end of the mmap (SIGSEGV).
TEST(ReplayBounds, RejectsCountBeyondFileSize) {
    const std::string path = "/tmp/ohlcv_bounds_truncated.bin";
    write_lying_file(path, 200000, 2);

    rp::MappedFile m(path.c_str());
    ASSERT_NE(m.base(), nullptr);
    const auto view = rp::view_replay(m.base(), m.size());
    EXPECT_NE(view.error, nullptr);
    EXPECT_EQ(view.records, nullptr);
}

// A hostile count near UINT64_MAX must not wrap the size arithmetic past the
// check — the guard divides instead of multiplying for exactly this case.
TEST(ReplayBounds, RejectsOverflowingRecordCount) {
    const std::string path = "/tmp/ohlcv_bounds_overflow.bin";
    write_lying_file(path, UINT64_MAX, 2);

    rp::MappedFile m(path.c_str());
    ASSERT_NE(m.base(), nullptr);
    const auto view = rp::view_replay(m.base(), m.size());
    EXPECT_NE(view.error, nullptr);
    EXPECT_EQ(view.records, nullptr);
}

// A file too small to even hold the 16-byte header must be rejected, not read.
TEST(ReplayBounds, RejectsTruncatedHeader) {
    const auto view = rp::view_replay(nullptr, 0);
    EXPECT_NE(view.error, nullptr);
    EXPECT_EQ(view.records, nullptr);
}

}  // namespace
