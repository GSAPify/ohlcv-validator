// Replays a binary dataset through the validator and reports a latency
// histogram. This is the project's headline artifact: a reproducible,
// network-free, market-hours-free measurement of decode+validate cost per
// record on a single core.
//
// Measurement note: on Apple Silicon the system counter resolution is ~41ns,
// so timing a single sub-100ns validation directly would measure the clock,
// not the work. Instead we time a full pass over all records with two clock
// reads and divide by the record count — the per-call clock overhead amortizes
// to nothing. Each pass contributes one per-record sample to the histogram.
//
// Usage: replay_bench [dataset_path] [passes]
//        defaults: data/replay.bin, 200 passes

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "replay/binary_format.h"
#include "util/histogram.h"
#include "util/timing.h"
#include "validate/validator.h"

namespace {

using ohlcv::replay::FileHeader;
using ohlcv::replay::RecordType;
using ohlcv::replay::WireRecord;

struct Mapped {
    const std::byte* base = nullptr;
    std::size_t      size = 0;
    int              fd   = -1;
};

Mapped map_file(const std::string& path) {
    Mapped m;
    m.fd = ::open(path.c_str(), O_RDONLY);
    if (m.fd < 0) return m;

    struct stat st {};
    if (::fstat(m.fd, &st) != 0) {
        ::close(m.fd);
        m.fd = -1;
        return m;
    }
    m.size = static_cast<std::size_t>(st.st_size);
    void* p = ::mmap(nullptr, m.size, PROT_READ, MAP_PRIVATE, m.fd, 0);
    if (p == MAP_FAILED) {
        ::close(m.fd);
        m.fd = -1;
        return m;
    }
    m.base = static_cast<const std::byte*>(p);
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string   path   = argc > 1 ? argv[1] : "data/replay.bin";
    const std::uint64_t passes = argc > 2 ? std::strtoull(argv[2], nullptr, 10)
                                          : 200ULL;

    Mapped m = map_file(path);
    if (m.base == nullptr) {
        std::fprintf(stderr, "cannot mmap %s (run gen_dataset first)\n",
                     path.c_str());
        return 1;
    }

    const auto* hdr = reinterpret_cast<const FileHeader*>(m.base);
    if (hdr->magic != ohlcv::replay::kMagic) {
        std::fprintf(stderr, "bad magic: not a replay file\n");
        return 1;
    }
    const auto* records =
        reinterpret_cast<const WireRecord*>(m.base + sizeof(FileHeader));
    const std::uint64_t count = hdr->record_count;

    // Warm pass: fault pages in and warm caches so the first timed pass isn't
    // an outlier. Also collect the violation breakdown once.
    std::uint64_t bad_trades = 0, bad_bars = 0, seq_gaps = 0, ts_regress = 0;
    std::uint64_t recon_vol = 0, recon_cnt = 0, recon_vwap = 0, recon_ohlc = 0;
    std::uint64_t q_crossed = 0, q_locked = 0, q_nonpos = 0, q_zerosize = 0;
    {
        ohlcv::validate::Validator v;
        for (std::uint64_t i = 0; i < count; ++i) {
            const WireRecord& rec = records[i];
            if (rec.type == static_cast<std::uint8_t>(RecordType::Trade)) {
                auto r = v.check(rec.body.trade);
                if (r.has(ohlcv::validate::kTradeNonPositivePrice) ||
                    r.has(ohlcv::validate::kTradeNonPositiveSize))
                    ++bad_trades;
                if (r.has(ohlcv::validate::kSequenceGap)) ++seq_gaps;
                if (r.has(ohlcv::validate::kTimestampRegression)) ++ts_regress;
            } else if (rec.type == static_cast<std::uint8_t>(RecordType::Bar)) {
                auto r = v.check(rec.body.bar);
                if (r.flags & (ohlcv::validate::kBarLowAboveHigh |
                               ohlcv::validate::kBarOpenOutOfRange |
                               ohlcv::validate::kBarCloseOutOfRange |
                               ohlcv::validate::kBarVwapOutOfRange |
                               ohlcv::validate::kBarNonPositivePrice |
                               ohlcv::validate::kBarVolumeInconsistent))
                    ++bad_bars;
                if (r.has(ohlcv::validate::kSequenceGap)) ++seq_gaps;
                if (r.has(ohlcv::validate::kTimestampRegression)) ++ts_regress;
                if (r.has(ohlcv::validate::kBarVolumeReconstructMismatch)) ++recon_vol;
                if (r.has(ohlcv::validate::kBarTradeCountReconstructMismatch)) ++recon_cnt;
                if (r.has(ohlcv::validate::kBarVwapReconstructMismatch)) ++recon_vwap;
                if (r.has(ohlcv::validate::kBarOhlcReconstructMismatch)) ++recon_ohlc;
            } else {
                auto r = v.check(rec.body.quote);
                if (r.has(ohlcv::validate::kQuoteCrossed)) ++q_crossed;
                if (r.has(ohlcv::validate::kQuoteLocked)) ++q_locked;
                if (r.has(ohlcv::validate::kQuoteNonPositive)) ++q_nonpos;
                if (r.has(ohlcv::validate::kQuoteZeroSize)) ++q_zerosize;
                if (r.has(ohlcv::validate::kSequenceGap)) ++seq_gaps;
                if (r.has(ohlcv::validate::kTimestampRegression)) ++ts_regress;
            }
        }
    }

    ohlcv::util::LatencyHistogram hist;
    std::uint64_t checksum = 0;  // accumulate result flags to defeat DCE

    for (std::uint64_t p = 0; p < passes; ++p) {
        // Fresh validator per pass: its per-symbol state must start empty, or
        // pass N would see pass N-1's final seq and flag a false regression.
        // Constructed outside the timed region so its zero-init isn't measured.
        ohlcv::validate::Validator v;

        const std::uint64_t t0 = ohlcv::util::now_ns();
        for (std::uint64_t i = 0; i < count; ++i) {
            const WireRecord& rec = records[i];
            if (rec.type == static_cast<std::uint8_t>(RecordType::Trade)) {
                checksum += v.check(rec.body.trade).flags;
            } else if (rec.type == static_cast<std::uint8_t>(RecordType::Bar)) {
                checksum += v.check(rec.body.bar).flags;
            } else {
                checksum += v.check(rec.body.quote).flags;
            }
        }
        const std::uint64_t t1 = ohlcv::util::now_ns();

        const std::uint64_t per_record = (t1 - t0) / count;
        hist.record(per_record);
    }

    const double mean_ns = hist.mean();
    const double thru = mean_ns > 0.0 ? 1e9 / mean_ns : 0.0;

    std::printf("\n");
    std::printf("replay benchmark — %s\n", path.c_str());
    std::printf("  records/pass : %llu\n", (unsigned long long)count);
    std::printf("  passes       : %llu\n", (unsigned long long)passes);
    std::printf("  total checks : %llu\n",
                (unsigned long long)(count * passes));
    std::printf("\n");
    std::printf("decode+validate (single core, allocation-free):\n");
    std::printf("  throughput      : %.1f M records/sec\n", thru / 1e6);
    std::printf("  mean            : %.2f ns/record\n", mean_ns);
    std::printf("  pass-mean spread: %llu..%llu ns (over %llu passes)\n",
                (unsigned long long)hist.min(),
                (unsigned long long)hist.max(),
                (unsigned long long)passes);
    std::printf("\n");
    std::printf("  note: per-record cost is below the ~41ns M-series clock\n");
    std::printf("  resolution, so this is a THROUGHPUT measurement — the rows\n");
    std::printf("  above are pass-means, not a latency distribution. A true\n");
    std::printf("  per-record tail needs an x86 host with rdtscp (see README).\n");
    std::printf("\n");
    std::printf("violations caught (per pass):\n");
    std::printf("  bad trades             : %llu\n", (unsigned long long)bad_trades);
    std::printf("  bad bars               : %llu\n", (unsigned long long)bad_bars);
    std::printf("  sequence gaps          : %llu\n", (unsigned long long)seq_gaps);
    std::printf("  ts regressions         : %llu\n", (unsigned long long)ts_regress);
    std::printf("  recon volume mismatch  : %llu\n", (unsigned long long)recon_vol);
    std::printf("  recon count mismatch   : %llu\n", (unsigned long long)recon_cnt);
    std::printf("  recon vwap mismatch    : %llu\n", (unsigned long long)recon_vwap);
    std::printf("  recon ohlc mismatch    : %llu\n", (unsigned long long)recon_ohlc);
    std::printf("  quote crossed          : %llu\n", (unsigned long long)q_crossed);
    std::printf("  quote locked           : %llu\n", (unsigned long long)q_locked);
    std::printf("  quote non-positive     : %llu\n", (unsigned long long)q_nonpos);
    std::printf("  quote zero-size        : %llu\n", (unsigned long long)q_zerosize);
    std::printf("\n[checksum %llu]\n", (unsigned long long)checksum);

    ::munmap(const_cast<std::byte*>(m.base), m.size);
    ::close(m.fd);
    return 0;
}
