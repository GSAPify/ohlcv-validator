// Generates a deterministic binary replay dataset for the latency benchmark.
//
// Real frames would come from capturing the live Alpaca feed and re-encoding to
// the wire format; this generator stands in for that so the benchmark is
// reproducible and runnable in CI without market hours or network. A fixed seed
// makes every run byte-identical. A known fraction of records carry injected
// violations so the validator has something to catch and the benchmark can
// report nonzero violation counts.
//
// Usage: gen_dataset [output_path] [record_count]
//        defaults: data/replay.bin, 1,000,000 records

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "replay/binary_format.h"

namespace {

using ohlcv::model::WireBar;
using ohlcv::model::WireTrade;
using ohlcv::replay::RecordType;
using ohlcv::replay::WireRecord;

constexpr std::array<const char*, 8> kSymbols = {
    "AAPL", "MSFT", "NVDA", "AMZN", "GOOGL", "META", "TSLA", "AMD"};

void set_symbol(char (&dst)[ohlcv::model::kSymbolLen], const char* sym) {
    std::memset(dst, 0, sizeof(dst));
    std::strncpy(dst, sym, sizeof(dst));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path  = argc > 1 ? argv[1] : "data/replay.bin";
    const std::uint64_t n   = argc > 2 ? std::strtoull(argv[2], nullptr, 10)
                                       : 1'000'000ULL;

    std::mt19937_64 rng{0xC0FFEEULL};  // fixed seed → reproducible dataset
    std::uniform_real_distribution<double> jitter{-0.5, 0.5};
    std::uniform_int_distribution<int>     pick{0, 99};

    // Per-symbol running state so each symbol carries its own monotonic seq/ts.
    std::array<std::uint64_t, kSymbols.size()> seq{};
    std::array<std::uint64_t, kSymbols.size()> ts{};
    std::array<double, kSymbols.size()>        px{};
    for (std::size_t i = 0; i < kSymbols.size(); ++i) {
        seq[i] = 1;
        ts[i]  = 1'700'000'000'000'000'000ULL + i * 1'000'000ULL;
        px[i]  = 100.0 + static_cast<double>(i) * 10.0;
    }

    std::vector<WireRecord> records;
    records.reserve(n);

    for (std::uint64_t r = 0; r < n; ++r) {
        const std::size_t s = r % kSymbols.size();
        ts[s]  += 1'000'000ULL;          // +1ms
        seq[s] += 1;
        px[s]  += jitter(rng);
        if (px[s] < 1.0) px[s] = 1.0;

        const int roll = pick(rng);      // 0..99, drives violation injection

        // ~1 in 50 records is a bar; the rest are trades.
        if (r % 50 == 49) {
            WireRecord rec{};
            rec.type      = static_cast<std::uint8_t>(RecordType::Bar);
            WireBar& b    = rec.body.bar;
            set_symbol(b.symbol, kSymbols[s]);
            b.seq         = seq[s];
            b.start_ns    = ts[s];
            b.low         = px[s] - 0.5;
            b.high        = px[s] + 0.5;
            b.open        = px[s];
            b.close       = px[s] + jitter(rng) * 0.2;
            b.vwap        = px[s];
            b.volume      = 1000;
            b.trade_count = 20;

            if (roll < 2) b.low = b.high + 5.0;   // inverted band
            if (roll == 2) b.trade_count = 0;     // volume without trades
            records.push_back(rec);
            continue;
        }

        WireRecord rec{};
        rec.type        = static_cast<std::uint8_t>(RecordType::Trade);
        WireTrade& t    = rec.body.trade;
        set_symbol(t.symbol, kSymbols[s]);
        t.seq           = seq[s];
        t.ts_ns         = ts[s];
        t.trade_id      = r;
        t.price         = px[s];
        t.size          = 1 + static_cast<std::uint64_t>(roll % 50);
        t.exchange      = 'V';
        t.tape          = 'C';

        if (roll == 0) {                  // ~1% non-positive price
            t.price = -1.0;
        } else if (roll == 1) {           // ~1% sequence gap (drop 3 messages)
            seq[s] += 3;
            t.seq   = seq[s];
        } else if (roll == 2) {           // ~1% timestamp regression
            t.ts_ns -= 5'000'000ULL;
        }
        records.push_back(rec);
    }

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot open %s for writing\n", path.c_str());
        return 1;
    }

    const ohlcv::replay::FileHeader hdr{ohlcv::replay::kMagic,
                                        ohlcv::replay::kVersion,
                                        records.size()};
    std::fwrite(&hdr, sizeof(hdr), 1, f);
    std::fwrite(records.data(), sizeof(WireRecord), records.size(), f);
    std::fclose(f);

    std::printf("wrote %zu records (%zu bytes/record) to %s\n", records.size(),
                sizeof(WireRecord), path.c_str());
    return 0;
}
