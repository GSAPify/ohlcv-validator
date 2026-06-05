// Per-record latency DISTRIBUTION via rdtscp — the artifact the README has
// promised since day one (p50/p99/p999, not just throughput). It mmaps a binary
// dataset, times each decode+validate with the x86 cycle counter, subtracts the
// measured timer overhead, and bins the result into a percentile histogram.
//
// Build on an x86 Linux box (no external deps):
//   g++ -O3 -march=native -std=c++20 -I src \
//       src/bin/replay_bench_rdtsc.cpp src/validate/validator.cpp -o rdtsc_bench
// Run pinned to one core, away from CPU 0 (where the OS lives):
//   taskset -c 2 ./rdtsc_bench data/replay.bin 5
//
// Usage: replay_bench_rdtsc [dataset] [passes]   (defaults: data/replay.bin, 5)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "replay/binary_format.h"
#include "replay/mapped_file.h"
#include "util/histogram.h"
#include "util/tsc_timer.h"
#include "validate/validator.h"

namespace {

using ohlcv::replay::FileHeader;
using ohlcv::replay::RecordType;
using ohlcv::replay::WireRecord;

inline std::uint32_t validate_one(ohlcv::validate::Validator& v,
                                  const WireRecord& rec) noexcept {
    if (rec.type == static_cast<std::uint8_t>(RecordType::Trade))
        return v.check(rec.body.trade).flags;
    if (rec.type == static_cast<std::uint8_t>(RecordType::Bar))
        return v.check(rec.body.bar).flags;
    return v.check(rec.body.quote).flags;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string   path   = argc > 1 ? argv[1] : "data/replay.bin";
    const std::uint64_t passes = argc > 2 ? std::strtoull(argv[2], nullptr, 10)
                                          : 5ULL;

    ohlcv::replay::MappedFile m(path.c_str());
    if (m.base() == nullptr) {
        std::fprintf(stderr, "cannot mmap %s (run gen_dataset first)\n",
                     path.c_str());
        return 1;
    }
    const auto* hdr = reinterpret_cast<const FileHeader*>(m.base());
    if (hdr->magic != ohlcv::replay::kMagic) {
        std::fprintf(stderr, "bad magic: not a replay file\n");
        return 1;
    }
    const auto* records =
        reinterpret_cast<const WireRecord*>(m.base() + sizeof(FileHeader));
    const std::uint64_t count = hdr->record_count;

    if (!ohlcv::util::tsc_available()) {
        std::printf("WARNING: not an x86 build — coarse fallback timer, numbers "
                    "are not meaningful. Run this on the x86 box.\n");
    }

    const double tpns = ohlcv::util::calibrate_ticks_per_ns();  // ticks per ns

    // Timer self-overhead: back-to-back start/end with no work between. The
    // minimum is the irreducible rdtscp+lfence cost, which we subtract from
    // every sample so the histogram reflects the validate, not the clock.
    std::uint64_t timer_ovh = UINT64_MAX;
    for (int i = 0; i < 200000; ++i) {
        const std::uint64_t s = ohlcv::util::cycle_start();
        const std::uint64_t e = ohlcv::util::cycle_end();
        timer_ovh = std::min(timer_ovh, e - s);
    }

    // One untimed warm pass (fault pages, warm caches + branch predictor).
    {
        ohlcv::validate::Validator v;
        std::uint64_t sink = 0;
        for (std::uint64_t i = 0; i < count; ++i) sink += validate_one(v, records[i]);
        volatile std::uint64_t keep = sink;
        (void)keep;
    }

    // Timed: one rdtscp pair per record → a real per-event sample.
    ohlcv::util::LatencyHistogram hist(1'000'000);  // ns buckets up to 1 ms
    std::uint64_t checksum = 0;
    for (std::uint64_t p = 0; p < passes; ++p) {
        ohlcv::validate::Validator v;  // fresh per-symbol state each pass
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t s = ohlcv::util::cycle_start();
            checksum += validate_one(v, records[i]);
            const std::uint64_t e = ohlcv::util::cycle_end();
            std::uint64_t d = e - s;
            d = d > timer_ovh ? d - timer_ovh : 0;
            hist.record(static_cast<std::uint64_t>(
                static_cast<double>(d) / tpns + 0.5));
        }
    }

    const double ovh_ns = static_cast<double>(timer_ovh) / tpns;
    std::printf("\n");
    std::printf("per-record decode+validate latency — rdtscp (%s)\n",
                ohlcv::util::tsc_available() ? "x86" : "FALLBACK, not meaningful");
    std::printf("  dataset : %s\n", path.c_str());
    std::printf("  samples : %llu  (%llu records x %llu passes)\n",
                (unsigned long long)(count * passes),
                (unsigned long long)count, (unsigned long long)passes);
    std::printf("  TSC     : %.3f GHz   timer overhead %.2f ns (subtracted)\n",
                tpns, ovh_ns);
    std::printf("  ----------------------------------------\n");
    std::printf("  p50     : %llu ns\n", (unsigned long long)hist.percentile(0.50));
    std::printf("  p90     : %llu ns\n", (unsigned long long)hist.percentile(0.90));
    std::printf("  p99     : %llu ns\n", (unsigned long long)hist.percentile(0.99));
    std::printf("  p99.9   : %llu ns\n", (unsigned long long)hist.percentile(0.999));
    std::printf("  p99.99  : %llu ns\n", (unsigned long long)hist.percentile(0.9999));
    std::printf("  max     : %llu ns\n", (unsigned long long)hist.max());
    std::printf("  mean    : %.2f ns\n", hist.mean());
    std::printf("\n[checksum %llu]\n", (unsigned long long)checksum);
    return 0;
}
