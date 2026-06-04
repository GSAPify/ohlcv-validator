// Generates a deterministic binary replay dataset for the latency benchmark.
//
// Real frames would come from capturing the live Alpaca feed and re-encoding to
// the wire format; this generator stands in for that so the benchmark is
// reproducible and runnable in CI without market hours or network. A fixed seed
// makes every run byte-identical. A known fraction of records carry injected
// violations so the validator has something to catch and the benchmark can
// report nonzero violation counts.
//
// Usage: gen_dataset [output_path] [record_count] [symbol_count]
//        defaults: data/replay.bin, 1,000,000 records, 8 symbols
//
// symbol_count drives how many distinct symbols the stream carries. It caps the
// parallelism of a shard-by-symbol consumer: you can't spread 8 symbols across
// 14 cores. Bump it (e.g. 256) to exercise multicore validation.

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
using ohlcv::model::WireQuote;
using ohlcv::model::WireTrade;
using ohlcv::replay::RecordType;
using ohlcv::replay::WireRecord;

// The 8 "real" tickers for small datasets; beyond that we synthesize tickers
// ("S00008", "S00009", …). All that matters for sharding is that every symbol's
// 8-byte key is distinct.
constexpr std::array<const char*, 8> kRealSymbols = {
    "AAPL", "MSFT", "NVDA", "AMZN", "GOOGL", "META", "TSLA", "AMD"};

std::vector<std::string> make_symbols(std::size_t count) {
    std::vector<std::string> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (i < kRealSymbols.size()) {
            out.emplace_back(kRealSymbols[i]);
        } else {
            char buf[ohlcv::model::kSymbolLen + 1];
            std::snprintf(buf, sizeof(buf), "S%05zu", i);  // <= 8 chars
            out.emplace_back(buf);
        }
    }
    return out;
}

void set_symbol(char (&dst)[ohlcv::model::kSymbolLen], const std::string& sym) {
    std::memset(dst, 0, sizeof(dst));
    std::strncpy(dst, sym.c_str(), sizeof(dst));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path  = argc > 1 ? argv[1] : "data/replay.bin";
    const std::uint64_t n   = argc > 2 ? std::strtoull(argv[2], nullptr, 10)
                                       : 1'000'000ULL;
    std::size_t sym_count   = argc > 3
        ? static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10)) : 8;
    if (sym_count < 1) sym_count = 1;

    const std::vector<std::string> symbols = make_symbols(sym_count);

    std::mt19937_64 rng{0xC0FFEEULL};  // fixed seed → reproducible dataset
    std::uniform_real_distribution<double> jitter{-0.5, 0.5};
    std::uniform_int_distribution<int>     pick{0, 99};

    // Per-symbol running state so each symbol carries its own monotonic seq/ts.
    std::vector<std::uint64_t> seq(sym_count, 1);
    std::vector<std::uint64_t> ts(sym_count);
    std::vector<double>        px(sym_count);
    for (std::size_t i = 0; i < sym_count; ++i) {
        ts[i] = 1'700'000'000'000'000'000ULL + i * 1'000'000ULL;
        px[i] = 100.0 + static_cast<double>(i % 50) * 10.0;  // keep prices sane
    }

    std::vector<WireRecord> records;
    records.reserve(n);

    std::uniform_int_distribution<int>           count_dist{8, 12};
    std::uniform_int_distribution<std::uint64_t> size_dist{1, 100};

    // One defect per bar-window, chosen up front, so injected violations are
    // isolated and the resulting counts are explainable. Per-trade defects hit
    // the window's first trade; reconstruction/band defects perturb the emitted
    // bar. A negative-price tick is excluded from the aggregate on BOTH sides
    // (here and in the validator), so the bar still reconstructs from the
    // remaining valid trades.
    enum Defect {
        kClean, kCorruptTrade, kSeqGap, kTsRegress,
        kReconVolume, kReconCount, kReconVwap, kReconOhlc, kBand,
        kQuoteCrossed, kQuoteLocked
    };

    // Each window emits a run of trades for one symbol, then the bar that closes
    // it — built from the trades' true aggregate. Round-robin over symbols.
    std::size_t s = sym_count - 1;  // so the first window uses symbol 0
    while (records.size() < n) {
        s = (s + 1) % sym_count;

        const int roll = pick(rng);       // ~1% each defect; ~92% clean
        Defect defect = kClean;
        switch (roll) {
            case 0: defect = kCorruptTrade; break;
            case 1: defect = kSeqGap;       break;
            case 2: defect = kTsRegress;    break;
            case 3: defect = kReconVolume;  break;
            case 4: defect = kReconCount;   break;
            case 5: defect = kReconVwap;    break;
            case 6: defect = kReconOhlc;    break;
            case 7: defect = kBand;         break;
            case 8: defect = kQuoteCrossed; break;
            case 9: defect = kQuoteLocked;  break;
            default: break;
        }

        // Accumulate the window's valid trades; the bar is built from this.
        std::uint64_t acc_vol = 0, acc_cnt = 0;
        double acc_pv = 0.0, acc_open = 0.0, acc_high = 0.0, acc_low = 0.0,
               acc_close = 0.0;
        bool have = false;

        const int n_trades = count_dist(rng);
        for (int k = 0; k < n_trades && records.size() < n; ++k) {
            ts[s]  += 1'000'000ULL;       // +1ms
            seq[s] += 1;
            px[s]  += jitter(rng);
            if (px[s] < 1.0) px[s] = 1.0;

            WireRecord rec{};
            rec.type     = static_cast<std::uint8_t>(RecordType::Trade);
            WireTrade& t = rec.body.trade;
            set_symbol(t.symbol, symbols[s]);
            t.seq      = seq[s];
            t.ts_ns    = ts[s];
            t.trade_id = records.size();
            t.price    = px[s];
            t.size     = size_dist(rng);
            t.exchange = 'V';
            t.tape     = 'C';

            bool valid = true;
            if (k == 0) {                 // per-trade defects land on the first
                if (defect == kCorruptTrade) {
                    t.price = -1.0;
                    valid   = false;
                } else if (defect == kSeqGap) {
                    seq[s] += 3;          // drop 3 messages
                    t.seq   = seq[s];
                } else if (defect == kTsRegress) {
                    t.ts_ns -= 5'000'000ULL;
                }
            }

            // Mirror the validator: only valid trades feed the aggregate.
            if (valid) {
                if (!have) {
                    acc_open = acc_high = acc_low = t.price;
                    have = true;
                } else {
                    if (t.price > acc_high) acc_high = t.price;
                    if (t.price < acc_low)  acc_low  = t.price;
                }
                acc_close = t.price;
                acc_cnt  += 1;
                acc_vol  += t.size;
                acc_pv   += t.price * static_cast<double>(t.size);
            }
            records.push_back(rec);
        }

        // Emit a couple of quotes for this symbol. They're validated on their own
        // (they don't feed bar reconstruction). A quote defect makes one crossed
        // (bid > ask) or locked (bid == ask).
        for (int qk = 0; qk < 2 && records.size() < n; ++qk) {
            ts[s]  += 1'000'000ULL;
            seq[s] += 1;
            WireRecord rec{};
            rec.type     = static_cast<std::uint8_t>(RecordType::Quote);
            WireQuote& q = rec.body.quote;
            set_symbol(q.symbol, symbols[s]);
            q.seq          = seq[s];
            q.ts_ns        = ts[s];
            q.bid_price    = px[s] - 0.05;
            q.ask_price    = px[s] + 0.05;
            q.bid_size     = 1 + size_dist(rng) % 20;
            q.ask_size     = 1 + size_dist(rng) % 20;
            q.bid_exchange = 'V';
            q.ask_exchange = 'V';
            q.tape         = 'C';
            if (qk == 0 && defect == kQuoteCrossed) {
                q.bid_price = px[s] + 0.05;  // bid above ask
                q.ask_price = px[s] - 0.05;
            } else if (qk == 0 && defect == kQuoteLocked) {
                q.bid_price = q.ask_price = px[s];
            }
            records.push_back(rec);
        }

        if (records.size() >= n) break;

        // Emit the bar from the accumulated aggregate.
        ts[s]  += 1'000'000ULL;
        seq[s] += 1;
        WireRecord rec{};
        rec.type   = static_cast<std::uint8_t>(RecordType::Bar);
        WireBar& b = rec.body.bar;
        set_symbol(b.symbol, symbols[s]);
        b.seq         = seq[s];
        b.start_ns    = ts[s];
        b.volume      = acc_vol;
        b.trade_count = acc_cnt;
        b.open        = acc_open;
        b.high        = acc_high;
        b.low         = acc_low;
        b.close       = acc_close;
        b.vwap        = acc_vol != 0 ? acc_pv / static_cast<double>(acc_vol)
                                     : acc_open;

        switch (defect) {                 // bar-level perturbations
            case kReconVolume: b.volume      += 100;     break;
            case kReconCount:  b.trade_count += 1;        break;
            case kReconVwap:   b.vwap        *= 1.001;    break;  // >> kPriceRelTol
            case kReconOhlc:   b.high        += 5.0;      break;  // trades never hit it
            case kBand:        b.low = b.high + 5.0;      break;  // inverted band
            default: break;
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

    std::printf("wrote %zu records (%zu bytes/record, %zu symbols) to %s\n",
                records.size(), sizeof(WireRecord), sym_count, path.c_str());
    return 0;
}
