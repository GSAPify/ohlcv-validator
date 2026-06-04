// Multi-threaded replay benchmark.
//
// Shards records by symbol across N threads — each thread owns a disjoint set of
// symbols and its own Validator, so there is ZERO shared mutable state and no
// locks on the hot path. Measures aggregate throughput and scaling vs thread
// count. Max useful parallelism is bounded by the dataset's symbol count, so
// generate with plenty of symbols (gen_dataset … <symbol_count>).
//
// Usage: replay_bench_mt [dataset] [passes] [threads]
//        threads omitted  -> sweep {1,2,4,8,10,14,hw}
//        threads = N       -> run just N threads
//        defaults: data/replay.bin, 20 passes

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "replay/binary_format.h"
#include "replay/mapped_file.h"
#include "validate/validator.h"

namespace {

using ohlcv::replay::FileHeader;
using ohlcv::replay::RecordType;
using ohlcv::replay::WireRecord;

std::uint64_t symbol_key(const char* sym8) noexcept {
    std::uint64_t k;
    std::memcpy(&k, sym8, sizeof(k));
    return k;
}

// Trade and bar both begin their body with char symbol[8], so the routing key
// is read the same way for either record type.
const char* record_symbol(const WireRecord& r) noexcept {
    return r.type == static_cast<std::uint8_t>(RecordType::Trade)
               ? r.body.trade.symbol
               : r.body.bar.symbol;
}

std::uint64_t wall_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Run the validation across T threads and return aggregate throughput (M rec/s).
double run(const WireRecord* records, std::uint64_t count, unsigned T,
           std::uint64_t passes) {
    // --- off the clock: partition records into per-thread buffers by symbol ---
    // shard = key % T routes a symbol's ENTIRE stream to one thread, so each
    // thread's Validator sees that symbol in order and owns its state alone.
    std::vector<std::vector<WireRecord>> shards(T);
    for (auto& s : shards) s.reserve(count / T + 1);
    for (std::uint64_t i = 0; i < count; ++i) {
        // Mix the key before taking % T — the raw symbol bytes cluster in the
        // low bits, so `key % T` would dump prefix-sharing symbols onto one
        // thread and starve the rest. (Same lesson as the validator's table.)
        const std::uint64_t h =
            symbol_key(record_symbol(records[i])) * 0x9E3779B97F4A7C15ULL;
        const unsigned owner = static_cast<unsigned>((h >> 32) % T);
        shards[owner].push_back(records[i]);
    }

    // Start gate: workers spin until main flips `go`, so thread-spawn and
    // partition time stay out of the measured region.
    std::atomic<unsigned> ready{0};
    std::atomic<bool>     go{false};
    std::vector<std::uint64_t> checksums(T, 0);

    std::vector<std::thread> threads;
    threads.reserve(T);
    for (unsigned t = 0; t < T; ++t) {
        threads.emplace_back([&, t] {
            const std::vector<WireRecord>& buf = shards[t];
            std::uint64_t sum = 0;
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                // spin until the start signal
            }
            for (std::uint64_t p = 0; p < passes; ++p) {
                ohlcv::validate::Validator v;  // fresh per pass: per-symbol state
                for (const WireRecord& rec : buf) {
                    if (rec.type == static_cast<std::uint8_t>(RecordType::Trade))
                        sum += v.check(rec.body.trade).flags;
                    else
                        sum += v.check(rec.body.bar).flags;
                }
            }
            checksums[t] = sum;  // each thread writes its own slot — no sharing
        });
    }

    while (ready.load(std::memory_order_acquire) < T) {
        // wait until every worker is parked at the gate
    }
    const std::uint64_t t0 = wall_ns();
    go.store(true, std::memory_order_release);
    for (std::thread& th : threads) th.join();
    const std::uint64_t t1 = wall_ns();

    std::uint64_t cs = 0;
    for (std::uint64_t c : checksums) cs ^= c;
    volatile std::uint64_t sink = cs;  // defeat dead-code elimination
    (void)sink;

    const double secs  = static_cast<double>(t1 - t0) / 1e9;
    const double total = static_cast<double>(count) * static_cast<double>(passes);
    return secs > 0.0 ? total / secs / 1e6 : 0.0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string   path   = argc > 1 ? argv[1] : "data/replay.bin";
    const std::uint64_t passes = argc > 2 ? std::strtoull(argv[2], nullptr, 10)
                                          : 20ULL;
    const int           fixed  = argc > 3 ? std::atoi(argv[3]) : 0;  // 0 => sweep

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

    const unsigned hw = std::thread::hardware_concurrency();
    std::printf("dataset: %llu records · %u hardware threads · %llu passes\n\n",
                (unsigned long long)count, hw, (unsigned long long)passes);

    std::vector<unsigned> sweep;
    if (fixed > 0) {
        sweep.push_back(static_cast<unsigned>(fixed));
    } else {
        for (unsigned t : {1u, 2u, 4u, 8u, 10u, 14u})
            if (t <= hw) sweep.push_back(t);
        if (sweep.empty() || sweep.back() != hw) sweep.push_back(hw);
    }

    std::printf("threads   throughput (M rec/s)   speedup   efficiency\n");
    std::printf("-------   --------------------   -------   ----------\n");
    double base = 0.0;
    for (unsigned T : sweep) {
        const double mrps = run(records, count, T, passes);
        if (T == sweep.front()) base = mrps;
        const double speedup = base > 0.0 ? mrps / base : 0.0;
        const double eff     = speedup / static_cast<double>(T) * 100.0;
        std::printf("%5u     %18.1f   %6.2fx   %8.0f%%\n", T, mrps, speedup, eff);
    }
    return 0;
}
