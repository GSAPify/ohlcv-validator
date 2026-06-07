// Producer/consumer pipeline benchmark over the lock-free SPSC ring.
//
// This is the decoupling a real feed handler has but the single-thread benches
// don't: one thread hands records off, another consumes them. At IEX volume one
// thread copes fine — this isn't load-bearing. It's here because the handoff is
// the part you can't measure without it, and because the ring's cache-line
// layout has a cost worth measuring rather than asserting.
//
// The producer here only copies a 64-byte record out of an mmap, so it always
// outruns any real consumer. That fact shapes both measurements, so the bench
// reports two deliberately separate regimes:
//
//   1. RING-BOUND throughput (consumer does no payload work). Both threads
//      hammer the cursors, so this is where cache-line placement actually
//      matters. Run with the cursors on separate lines vs packed onto one;
//      the delta is the cost of false sharing, reported as measured (median of
//      several runs — WSL scheduling is noisy). No per-item timing in the loop.
//
//   2. REALISTIC pipeline (consumer validates each record). The validate is the
//      bottleneck, so the ring sits nearly full and the producer blocks on it.
//      "Handoff latency" here is therefore queue RESIDENCY — depth × consume
//      time, not the ring's ~tens-of-ns sync cost. Mean occupancy is printed so
//      that's visible. Cross-core TSC subtraction is guarded against underflow
//      (the tell that the per-core TSCs aren't synchronised).
//
// Build on an x86 Linux box (no external deps):
//   g++ -O3 -march=native -std=c++20 -pthread -I src \
//       src/bin/replay_bench_spsc.cpp src/validate/validator.cpp -o spsc_bench
// Run (producer + consumer pinned to two cores on the same CCD):
//   ./spsc_bench data/replay.bin 5 2 4
//
// Usage: replay_bench_spsc [dataset] [passes] [producer_core] [consumer_core]
//        defaults: data/replay.bin, 5, 2, 4

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>  // _mm_pause
#endif

#include "concurrent/spsc_ring.h"
#include "replay/binary_format.h"
#include "replay/mapped_file.h"
#include "util/histogram.h"
#include "util/tsc_timer.h"
#include "validate/validator.h"

namespace {

using ohlcv::concurrent::SpscRing;
using ohlcv::replay::FileHeader;
using ohlcv::replay::RecordType;
using ohlcv::replay::WireRecord;

constexpr std::size_t kRingCapacity = 1024;
constexpr int         kThroughputReps = 5;  // median over these (WSL is noisy)

inline std::uint32_t validate_one(ohlcv::validate::Validator& v,
                                  const WireRecord& rec) noexcept {
    if (rec.type == static_cast<std::uint8_t>(RecordType::Trade))
        return v.check(rec.body.trade).flags;
    if (rec.type == static_cast<std::uint8_t>(RecordType::Bar))
        return v.check(rec.body.bar).flags;
    return v.check(rec.body.quote).flags;
}

// Politely spin: on x86, PAUSE yields the pipeline to the SMT sibling and cuts
// the memory-order-violation penalty when the spin finally exits.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#endif
}

void pin_to_core([[maybe_unused]] int core) noexcept {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

// ----- Regime 1: ring-bound throughput (no payload work) -------------------
// The consumer only sinks a byte so the pop can't be optimised away; both ends
// are then bottlenecked on the ring itself, which is what makes the cursor
// cache-line placement observable. `make(i)` builds the payload, so the same
// body benchmarks a 64-byte record and an 8-byte word — the comparison that
// shows cursor padding only matters when the payload doesn't dominate the
// cross-core line transfer.
template <typename RingT, typename Make>
double ring_throughput_once(std::uint64_t count, std::uint64_t passes,
                            int pcore, int ccore, Make make) {
    using T = typename RingT::value_type;
    RingT               ring;
    const std::uint64_t total = count * passes;

    const auto t0 = std::chrono::steady_clock::now();

    std::thread producer([&] {
        pin_to_core(pcore);
        for (std::uint64_t p = 0; p < passes; ++p)
            for (std::uint64_t i = 0; i < count; ++i) {
                T item = make(i);
                while (!ring.try_push(item)) cpu_relax();
            }
    });

    pin_to_core(ccore);
    std::uint64_t sink   = 0;
    std::uint64_t popped = 0;
    T             out{};
    while (popped < total) {
        if (ring.try_pop(out)) {
            std::uint8_t b;
            std::memcpy(&b, &out, 1);  // touch the payload; defeats DCE
            sink += b;
            ++popped;
        } else {
            cpu_relax();
        }
    }
    producer.join();

    const auto   t1   = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    volatile std::uint64_t keep = sink;
    (void)keep;
    return secs > 0.0 ? static_cast<double>(total) / secs : 0.0;
}

template <typename RingT, typename Make>
double ring_throughput_median(std::uint64_t count, std::uint64_t passes,
                              int pcore, int ccore, Make make) {
    std::array<double, kThroughputReps> r{};
    for (int i = 0; i < kThroughputReps; ++i)
        r[static_cast<std::size_t>(i)] =
            ring_throughput_once<RingT>(count, passes, pcore, ccore, make);
    std::sort(r.begin(), r.end());
    return r[kThroughputReps / 2];
}

// ----- Regime 2: realistic pipeline (consumer validates) -------------------
struct Stamped {
    WireRecord    rec;
    std::uint64_t enqueue_tsc;
};

}  // namespace

int main(int argc, char** argv) {
    const std::string   path   = argc > 1 ? argv[1] : "data/replay.bin";
    const std::uint64_t passes = argc > 2 ? std::strtoull(argv[2], nullptr, 10)
                                          : 5ULL;
    const int pcore = argc > 3 ? std::atoi(argv[3]) : 2;
    const int ccore = argc > 4 ? std::atoi(argv[4]) : 4;

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
    const std::uint64_t total = count * passes;

    if (!ohlcv::util::tsc_available())
        std::printf("WARNING: not an x86 build — handoff numbers use a coarse "
                    "fallback timer and are not meaningful.\n");

    std::printf("\nSPSC pipeline — producer core %d, consumer core %d, "
                "ring capacity %zu\n", pcore, ccore, kRingCapacity);
    std::printf("  dataset : %s  (%llu records x %llu passes = %llu)\n",
                path.c_str(), (unsigned long long)count,
                (unsigned long long)passes, (unsigned long long)total);

    // --- Regime 1: ring-bound throughput, padded vs packed cursors ---------
    // Two payload sizes. 64B record: the per-item cross-core line transfer of
    // the payload dominates, so cursor padding is in the noise. 8B word: the
    // payload is cheap, so the cursors' cache-line placement is what's left to
    // pay for — and separating them onto their own lines should now win.
    constexpr std::size_t kPacked = alignof(std::atomic<std::size_t>);
    using RecPadded = SpscRing<WireRecord, kRingCapacity, 64>;
    using RecPacked = SpscRing<WireRecord, kRingCapacity, kPacked>;
    using U64Padded = SpscRing<std::uint64_t, kRingCapacity, 64>;
    using U64Packed = SpscRing<std::uint64_t, kRingCapacity, kPacked>;

    const auto rec_make = [records](std::uint64_t i) { return records[i]; };
    const auto u64_make = [](std::uint64_t i) { return i; };

    const double rec_pad =
        ring_throughput_median<RecPadded>(count, passes, pcore, ccore, rec_make);
    const double rec_pak =
        ring_throughput_median<RecPacked>(count, passes, pcore, ccore, rec_make);
    const double u64_pad =
        ring_throughput_median<U64Padded>(count, passes, pcore, ccore, u64_make);
    const double u64_pak =
        ring_throughput_median<U64Packed>(count, passes, pcore, ccore, u64_make);

    std::printf("\n  [1] ring-bound throughput, separate vs packed cursors "
                "(median of %d)\n", kThroughputReps);
    std::printf("      payload      separate-lines      packed         "
                "separate speedup\n");
    std::printf("      64B record   %6.1f M ops/s     %6.1f M ops/s   %+.1f%%\n",
                rec_pad / 1e6, rec_pak / 1e6,
                rec_pak > 0.0 ? (rec_pad - rec_pak) / rec_pak * 100.0 : 0.0);
    std::printf("      8B word      %6.1f M ops/s     %6.1f M ops/s   %+.1f%%\n",
                u64_pad / 1e6, u64_pak / 1e6,
                u64_pak > 0.0 ? (u64_pad - u64_pak) / u64_pak * 100.0 : 0.0);
    std::printf("      → padding pays off only when the payload doesn't "
                "dominate the cross-core transfer\n");

    // --- Regime 2: realistic pipeline (consumer validates) -----------------
    const double tpns = ohlcv::util::calibrate_ticks_per_ns();
    SpscRing<Stamped, kRingCapacity, 64> lring;
    ohlcv::util::LatencyHistogram        hist(1'000'000);  // ns buckets to 1 ms
    std::uint64_t                        occupancy_sum = 0;
    std::uint64_t                        tsc_anomalies = 0;  // now < enqueue

    const auto p0 = std::chrono::steady_clock::now();

    std::thread lproducer([&] {
        pin_to_core(pcore);
        for (std::uint64_t p = 0; p < passes; ++p)
            for (std::uint64_t i = 0; i < count; ++i) {
                Stamped s{records[i], ohlcv::util::cycle_start()};
                while (!lring.try_push(s)) cpu_relax();
            }
    });

    pin_to_core(ccore);
    ohlcv::validate::Validator lv;
    std::uint64_t              checksum = 0;
    std::uint64_t              popped   = 0;
    Stamped                    s{};
    while (popped < total) {
        if (lring.try_pop(s)) {
            const std::uint64_t now = ohlcv::util::cycle_end();
            occupancy_sum += lring.size_approx();
            if (now < s.enqueue_tsc) {
                ++tsc_anomalies;  // per-core TSCs not synchronised
            } else {
                const double d_ns =
                    static_cast<double>(now - s.enqueue_tsc) / tpns;
                hist.record(static_cast<std::uint64_t>(d_ns + 0.5));
            }
            checksum += validate_one(lv, s.rec);
            ++popped;
        } else {
            cpu_relax();
        }
    }
    lproducer.join();
    const auto   p1 = std::chrono::steady_clock::now();
    const double psecs = std::chrono::duration<double>(p1 - p0).count();
    volatile std::uint64_t keep = checksum;
    (void)keep;

    const double mean_occ =
        static_cast<double>(occupancy_sum) / static_cast<double>(total);
    const double pipe_tp = psecs > 0.0
        ? static_cast<double>(total) / psecs : 0.0;

    std::printf("\n  [2] realistic pipeline (consumer validates) — %s\n",
                ohlcv::util::tsc_available() ? "x86 rdtscp" : "FALLBACK");
    std::printf("      end-to-end throughput : %.1f M rec/s  (validate-bound)\n",
                pipe_tp / 1e6);
    std::printf("      mean ring occupancy   : %.0f / %zu  → producer outruns "
                "consumer, so the ring stays full\n",
                mean_occ, kRingCapacity);
    std::printf("      handoff = QUEUE RESIDENCY (≈ occupancy × consume time), "
                "not the ring's sync cost:\n");
    std::printf("        p50   : %llu ns\n",
                (unsigned long long)hist.percentile(0.50));
    std::printf("        p99   : %llu ns\n",
                (unsigned long long)hist.percentile(0.99));
    std::printf("        p99.9 : %llu ns\n",
                (unsigned long long)hist.percentile(0.999));
    std::printf("        max   : %llu ns\n", (unsigned long long)hist.max());
    std::printf("        mean  : %.0f ns\n", hist.mean());
    if (tsc_anomalies != 0)
        std::printf("      WARNING: %llu samples had dequeue-TSC < enqueue-TSC "
                    "(cross-core TSC desync) — excluded.\n",
                    (unsigned long long)tsc_anomalies);
    std::printf("\n");
    return 0;
}
