#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include "validate/validator.h"

// Proves the headline claim instead of asserting it: counts every global heap
// allocation while a stream runs through the validator, and requires the count
// to be exactly zero. operator new is overridden process-wide for this test
// binary but only tallies while g_counting is set, so it doesn't perturb other
// tests (or gtest's own allocations).

namespace {
std::atomic<std::size_t> g_alloc_count{0};
std::atomic<bool>        g_counting{false};
}  // namespace

void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using ohlcv::model::WireBar;
using ohlcv::model::WireTrade;
using ohlcv::validate::Validator;

TEST(AllocGuard, HotPathDoesNotAllocate) {
    // Build everything the loop touches BEFORE counting: the validator (its
    // per-symbol table is an in-place std::array, no heap) and the records.
    Validator v;

    constexpr int kSymbols = 8;
    const char* names[kSymbols] = {"AAPL", "MSFT", "NVDA", "AMZN",
                                   "GOOGL", "META", "TSLA", "AMD"};
    WireTrade trades[kSymbols]{};
    WireBar   bars[kSymbols]{};
    for (int i = 0; i < kSymbols; ++i) {
        const double p = 100.0 + i;
        std::strncpy(trades[i].symbol, names[i], sizeof(trades[i].symbol));
        trades[i].price = p;
        trades[i].size  = 10;
        // Each bar is built to exactly reconstruct the single trade that
        // precedes it in the loop below, so the stream stays clean (zero
        // violations) and the test asserts both zero-alloc AND zero flags.
        std::strncpy(bars[i].symbol, names[i], sizeof(bars[i].symbol));
        bars[i].open = bars[i].high = bars[i].low = bars[i].close = p;
        bars[i].vwap        = p;
        bars[i].volume      = trades[i].size;  // one trade of this size
        bars[i].trade_count = 1;
    }

    // Touch the validator once per symbol so any first-use table growth (there
    // is none by design) would happen before the counted region.
    for (int i = 0; i < kSymbols; ++i) {
        (void)v.check(trades[i]);
        (void)v.check(bars[i]);
    }

    g_alloc_count.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);

    std::uint64_t sink = 0;
    for (std::uint64_t n = 0; n < 100'000; ++n) {
        const int i = static_cast<int>(n % kSymbols);
        trades[i].seq += 1;
        trades[i].ts_ns += 1000;
        bars[i].seq += 1;
        bars[i].start_ns += 1000;
        sink += v.check(trades[i]).flags;
        sink += v.check(bars[i]).flags;
    }

    g_counting.store(false, std::memory_order_relaxed);

    EXPECT_EQ(g_alloc_count.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(sink, 0U) << "clean stream should produce no violations";
}
