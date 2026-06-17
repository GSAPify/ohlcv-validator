#pragma once

#include <cstdint>
#include <fstream>
#include <string>

#include "replay/binary_format.h"

namespace ohlcv::replay {

// Streams WireRecords to a file in the binary replay format, so a live session
// can record exactly what gen_dataset synthesizes — and the captured file then
// replays through the same mmap path (replay_bench, the validator, the benches).
// This is the "capture the live feed and re-encode to the wire format" step that
// gen_dataset's header comment says it stands in for; capture makes it real.
//
// The header's record_count isn't known until the stream ends, so the ctor
// writes a placeholder (0) and finalize() patches it. finalize() is idempotent
// and runs from the destructor, so a graceful stop (Ctrl+C → drain → destruct)
// always leaves a valid file.
//
// Caveats (documented, not bugs):
//   * A hard kill (SIGKILL/crash) before finalize() leaves the header saying 0
//     records even though records are on disk — the graceful path covers Ctrl+C.
//   * The capture stores the *real* ts_ns but a *synthetic* per-symbol seq (the
//     counter the live path assigns, since IEX has no feed seq). So it's a replay
//     artifact fit for validation/measurement, NOT a faithful raw-feed archive:
//     seq is fabricated and the original JSON can't be reconstructed.
class CaptureWriter {
public:
    explicit CaptureWriter(const std::string& path)
        : out_(path, std::ios::binary | std::ios::trunc) {
        if (out_) {
            const FileHeader hdr{kMagic, kVersion, 0};  // count patched in finalize()
            out_.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        }
    }

    ~CaptureWriter() { finalize(); }

    CaptureWriter(const CaptureWriter&)            = delete;
    CaptureWriter& operator=(const CaptureWriter&) = delete;

    [[nodiscard]] bool          ok() const noexcept { return static_cast<bool>(out_); }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

    void write(const WireRecord& rec) {
        if (!out_) return;
        out_.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
        ++count_;
    }

    // Rewrite the header with the final record_count and flush. Idempotent —
    // safe to call explicitly and again from the destructor.
    void finalize() {
        if (!out_ || finalized_) return;
        finalized_ = true;
        out_.seekp(0);
        const FileHeader hdr{kMagic, kVersion, count_};
        out_.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        out_.flush();
    }

private:
    std::ofstream out_;
    std::uint64_t count_     = 0;
    bool          finalized_ = false;
};

}  // namespace ohlcv::replay
