// fmcg_midi.h -- MIDI data model, compressed-format detection, buffered/mmap
// binary reader, and the ScaleMidiProcessor facade.
#pragma once
#include <string>
#include <cstdint>
#include <memory>
#include <atomic>
#include <functional>
#include <fstream>
#include <iostream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#endif

struct FrameStats {
    size_t frame_index = 0;
    double timestamp_sec = 0.0;
    uint64_t cumulative_notes = 0;
    uint64_t cumulative_cc = 0;          // control-change events so far (optional stat)
    double notes_per_second = 0.0;
    double peak_nps = 0.0;
    int64_t polyphony = 0;
    int64_t peak_polyphony = 0;
    double bpm = 120.0;
    int64_t tick = 0;                    // song tick at this frame ({tick} stat); negative during the start-delay countdown
};

struct TempoChange {
    uint64_t tick;
    double time_sec;
    uint32_t us_per_quarter;
    double bpm;
};

// Compressed-format detection (magic bytes).
bool is_7z_magic(const uint8_t* m);
bool is_xz_magic(const uint8_t* m);
bool is_rar_magic(const uint8_t* m);
// Compression signature check over the first bytes of any stream (file header
// or decoded archive content). Covers 7z, xz, rar, gzip, bzip2, zstd, lz4.
bool is_compressed_magic(const uint8_t* m, size_t n);
bool is_compressed_file(const std::string& path);

// Continuous progress / logging callbacks (used by both CLI and GUI).
struct ProgressCallbacks {
    std::function<void(const char* pass_name, int percent)> on_progress;
    std::function<void(const char* msg, bool is_error)>     on_log;
    // Continuous scan progress (checked every ~1M events so the hot path stays
    // branch-cheap): events parsed so far, elapsed seconds, events/second, and
    // a stream fraction 0..1 (negative when the total size is unknown).
    std::function<void(uint64_t events, double elapsed_sec, double ev_per_s, double frac)> on_scan_progress;
    // CC events are always counted (kept for source compatibility; ignored).
    bool cc_stats = true;
    // Set (from another thread) to abort the scan at the next progress ping.
    std::atomic<bool>* cancel_flag = nullptr;
    // MIDI spec guard: a running tick beyond the 28-bit VLQ range (1 << 28)
    // breaks the spec and forces gigantic per-tick arrays. When first crossed,
    // the scanner invokes on_spec_violation (on the worker thread) with the
    // tick and the bytes already allocated; it returns 0 = proceed anyway,
    // 1 = abort and restart in the low-memory two-pass mode, 2 = cancel.
    // Unset => proceed (keeps CLI/tests non-interactive).
    // spec_tick_limit overrides the threshold (tests lower it to exercise the
    // path); 0 = the spec default of 1 << 28.
    std::function<int(uint64_t tick, size_t dense_bytes)> on_spec_violation;
    uint64_t spec_tick_limit = 0;
};

// Buffered (default) or memory-mapped binary reader.
class BinaryReader {
    bool use_mmap = false;
#if defined(_WIN32)
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMap = nullptr;
#else
    int fd = -1;
#endif
    uint8_t* mmap_ptr = nullptr;
    size_t mmap_len = 0;
    size_t file_size;
    size_t pos;
    static constexpr size_t BUFSIZE = 1 << 20;
    std::unique_ptr<uint8_t[]> buf;   // heap-allocated on first buffered read
    size_t buf_pos;
    size_t buf_len;

    void ensure_buf();
    void refill();

public:
    BinaryReader(const std::string& path, size_t start = 0, bool want_mmap = false);
    ~BinaryReader();
    BinaryReader(BinaryReader&& o) noexcept;
    BinaryReader& operator=(BinaryReader&&) = delete;
    BinaryReader(const BinaryReader&) = delete;
    BinaryReader& operator=(const BinaryReader&) = delete;

    size_t  get_file_size() const { return file_size; }
    size_t read_raw(void* dst, size_t n);
};

// MIDI Processor -- routes all inputs through the single-pass streaming engine.
class ScaleMidiProcessor {
public:
    // Preferred form: also reports the song's total tick count ({tick-total}).
    static std::vector<FrameStats> process_midi(
        const std::string& filename, double fps, uint16_t& out_division,
        uint64_t& out_total_notes, bool vel0_as_note_off,
        uint64_t& out_total_ticks, const ProgressCallbacks& cb = {},
        double end_delay = 0.0);
    // Back-compat form used by tests/benchmarks (total ticks not reported).
    static std::vector<FrameStats> process_midi(
        const std::string& filename, double fps, uint16_t& out_division,
        uint64_t& out_total_notes, bool vel0_as_note_off = true,
        const ProgressCallbacks& cb = {});
};

// Formatting helper needed by the engine's stats line (defined in fmcg_util).
std::string format_with_commas(uint64_t val);
