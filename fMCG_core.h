#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <thread>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#ifndef _DIRENT_H
#include <dirent.h>
#endif

#include <sys/stat.h>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <set>
#include <map>
#include <limits>
#include <chrono>

// ---------------------------------------------------------------------------
// File size formatting
// ---------------------------------------------------------------------------

inline std::string format_file_size(uint64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    double val = (double)bytes;
    const char* units[] = {"KiB", "MiB", "GiB", "TiB"};
    for (int i = 0; i < 4; ++i) {
        val /= 1024.0;
        if (val < 1024.0 || i == 3) {
            char buf[32];
            if (val >= 100.0) snprintf(buf, sizeof(buf), "%.0f %s", val, units[i]);
            else if (val >= 10.0) snprintf(buf, sizeof(buf), "%.1f %s", val, units[i]);
            else snprintf(buf, sizeof(buf), "%.2f %s", val, units[i]);
            return buf;
        }
    }
    return std::to_string(bytes) + " B";
}

// ---------------------------------------------------------------------------
// Compressed archive support — libarchive-based in-memory decompression
// ---------------------------------------------------------------------------

#include "archive.h"
#include "archive_entry.h"

static bool is_7z_magic(const uint8_t* m) {
    return m[0]==0x37 && m[1]==0x7A && m[2]==0xBC && m[3]==0xAF && m[4]==0x27 && m[5]==0x1C;
}
static bool is_xz_magic(const uint8_t* m) {
    return m[0]==0xFD && m[1]==0x37 && m[2]==0x7A && m[3]==0x58 && m[4]==0x5A && m[5]==0x00;
}
static bool is_rar_magic(const uint8_t* m) {
    return m[0]==0x52 && m[1]==0x61 && m[2]==0x72 && m[3]==0x21 && m[4]==0x1A && m[5]==0x07;
}
static size_t read_file_header(const std::string& path, uint8_t* out, size_t n) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    size_t r = fread(out, 1, n, f);
    fclose(f);
    return r;
}
static bool is_compressed_file(const std::string& path) {
    uint8_t magic[6];
    return read_file_header(path, magic, 6) == 6
        && (is_7z_magic(magic) || is_xz_magic(magic) || is_rar_magic(magic));
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

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
};

struct TempoChange {
    uint64_t tick;
    double time_sec;
    uint32_t us_per_quarter;
    double bpm;
};

// ---------------------------------------------------------------------------
// Binary reader — buffered I/O (default) or memory-mapped modes.
// ---------------------------------------------------------------------------

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
    // Heap-allocated on first buffered read (mmap readers never allocate it).
    std::unique_ptr<uint8_t[]> buf;
    size_t buf_pos;
    size_t buf_len;

    void ensure_buf() {
        if (!buf) buf.reset(new uint8_t[BUFSIZE]);
    }

    void refill() {
        buf_pos = pos;
        buf_len = 0;
        if (pos >= file_size) return;
        ensure_buf();
        while (buf_len < BUFSIZE && buf_pos + buf_len < file_size) {
#if defined(_WIN32)
            OVERLAPPED ov = {};
            ov.Offset = (DWORD)((buf_pos + buf_len) & 0xFFFFFFFF);
            ov.OffsetHigh = (DWORD)(((buf_pos + buf_len) >> 32) & 0xFFFFFFFF);
            DWORD n = 0;
            if (!ReadFile(hFile, buf.get() + buf_len, (DWORD)(BUFSIZE - buf_len), &n, &ov) || n == 0) break;
            buf_len += n;
#else
            ssize_t n = pread(fd, buf.get() + buf_len, BUFSIZE - buf_len, buf_pos + buf_len);
            if (n <= 0) break;
            buf_len += n;
#endif
        }
    }

public:
    BinaryReader(const std::string& path, size_t start = 0, bool want_mmap = false)
        : use_mmap(want_mmap), pos(start), buf_pos(0), buf_len(0) {

#if defined(_WIN32)
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (wlen <= 0) { file_size = 0; return; }
        std::wstring wpath(static_cast<size_t>(wlen) - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);
        hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING,
                            use_mmap ? FILE_ATTRIBUTE_NORMAL
                                     : (FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN), nullptr);
        if (hFile == INVALID_HANDLE_VALUE) { file_size = 0; return; }
        LARGE_INTEGER li;
        if (!GetFileSizeEx(hFile, &li) || li.QuadPart <= 0) {
            CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; file_size = 0; return;
        }
        file_size = static_cast<size_t>(li.QuadPart);

        if (use_mmap && file_size > 0) {
            hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (hMap) {
                mmap_ptr = (uint8_t*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
                if (!mmap_ptr) { CloseHandle(hMap); hMap = nullptr; use_mmap = false; }
                else { mmap_len = file_size; }
            } else {
                use_mmap = false;
            }
        }
#else
        fd = open(path.c_str(), O_RDONLY);
        if (fd == -1) { file_size = 0; return; }
        struct stat st;
        fstat(fd, &st);
        file_size = st.st_size;
#if defined(__linux__)
        posix_fadvise(fd, 0, 0, use_mmap ? POSIX_FADV_RANDOM : POSIX_FADV_SEQUENTIAL);
#endif

        if (use_mmap && file_size > 0) {
            mmap_ptr = (uint8_t*)mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (mmap_ptr == MAP_FAILED) { mmap_ptr = nullptr; use_mmap = false; }
            else { mmap_len = file_size; }
        }
#endif
    }

    ~BinaryReader() {
        if (use_mmap) {
#if defined(_WIN32)
            if (mmap_ptr) UnmapViewOfFile(mmap_ptr);
            if (hMap) CloseHandle(hMap);
#else
            if (mmap_ptr) munmap(mmap_ptr, mmap_len);
#endif
        }
#if defined(_WIN32)
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
#else
        if (fd != -1) close(fd);
#endif
    }

    BinaryReader(BinaryReader&& o) noexcept
        : use_mmap(o.use_mmap),
          mmap_ptr(o.mmap_ptr), mmap_len(o.mmap_len),
          file_size(o.file_size), pos(o.pos), buf_pos(o.buf_pos), buf_len(o.buf_len) {
#if defined(_WIN32)
        hFile = o.hFile; o.hFile = INVALID_HANDLE_VALUE;
        hMap = o.hMap; o.hMap = nullptr;
#else
        fd = o.fd; o.fd = -1;
#endif
        o.mmap_ptr = nullptr;
        o.buf = std::move(buf); o.buf_pos = 0; o.buf_len = 0;
    }
    BinaryReader& operator=(BinaryReader&&) = delete;
    BinaryReader(const BinaryReader&) = delete;
    BinaryReader& operator=(const BinaryReader&) = delete;

    size_t  get_file_size() const { return file_size; }

    // Bulk sequential read. Consumes min(n, available) bytes; used by the
    // streaming engine's chunked file source.
    size_t read_raw(void* dst, size_t n) {
        if (use_mmap) {
            size_t avail = file_size - pos;
            if (n > avail) n = avail;
            std::memcpy(dst, mmap_ptr + pos, n);
            pos += n;
            return n;
        }
        ensure_buf();
        size_t out = 0;
        uint8_t* d = (uint8_t*)dst;
        while (out < n && pos < file_size) {
            if (pos < buf_pos || pos >= buf_pos + buf_len) {
                refill();
                if (buf_len == 0) break;
            }
            size_t take = std::min(n - out, buf_pos + buf_len - pos);
            std::memcpy(d + out, buf.get() + (pos - buf_pos), take);
            pos += take;
            out += take;
        }
        return out;
    }

};

// ---------------------------------------------------------------------------
// Callbacks for progress / logging (used by both CLI and GUI)
// ---------------------------------------------------------------------------

inline std::string format_with_commas(uint64_t val);   // defined below (formatting section)

struct ProgressCallbacks {
    std::function<void(const char* pass_name, int percent)> on_progress;
    std::function<void(const char* msg, bool is_error)>     on_log;
    // Continuous scan progress (checked every ~1M events so the hot path stays
    // branch-cheap): events parsed so far, elapsed seconds, events/second, and
    // a stream fraction 0..1 (negative when the total size is unknown).
    std::function<void(uint64_t events, double elapsed_sec, double ev_per_s, double frac)> on_scan_progress;
    bool cc_stats = false;   // count control-change events (hot path stays free when false)
    // Set (from another thread) to abort the scan at the next progress ping.
    std::atomic<bool>* cancel_flag = nullptr;
};

// ---------------------------------------------------------------------------
// Tick-space single-pass streaming engine
//
// One sequential walk over the image (mmap'd file, or libarchive chunks for
// compressed inputs -- the decompressed bytes are never materialized in full),
// accumulating note events into per-tick buckets and collecting tempo events.
// A final O(ticks + frames) sweep converts tick space to frame stats using
// anchor interpolation (arithmetic identical to the parallel engine's
// tick_to_sec). Memory is O(duration*ppqn + tempo events), independent of
// event count: terabyte-scale decompressed inputs need no temp file, no spill
// threshold, and no second read of the stream.
// ---------------------------------------------------------------------------

namespace fmcg_stream {

static void emit(const ProgressCallbacks& cb, const std::string& msg, bool is_error = false) {
    if (cb.on_log) cb.on_log(msg.c_str(), is_error);
    else if (is_error) std::cerr << msg << std::flush;
    else std::cout << msg << std::flush;
}

// ---- chunk source over the image -------------------------------------------

class DataSrc {
public:
    virtual ~DataSrc() {}
    virtual size_t read(void* dst, size_t n) = 0;  // short reads allowed; 0 = end
    virtual bool ok() const = 0;
    virtual const char* kind() const = 0;
    virtual uint64_t bytes_read() const { return 0; }  // decoded bytes pushed through
    virtual uint64_t total_bytes() const { return 0; } // 0 = unknown (archives)
    virtual bool errored() const { return false; }     // stream died before clean EOF
};

class FileSrc : public DataSrc {
    BinaryReader r;
public:
    explicit FileSrc(const std::string& path) : r(path, 0, /*mmap=*/true) {}
    bool ok() const override { return r.get_file_size() > 0; }
    size_t read(void* dst, size_t n) override { return r.read_raw(dst, n); }
    const char* kind() const override { return "mmap"; }
    uint64_t total_bytes() const override { return (uint64_t)r.get_file_size(); }
};

class ArchiveSrc : public DataSrc {
public:
    struct archive* a = nullptr;
    struct archive_entry* entry = nullptr;
    std::vector<uint8_t> staging;   // libarchive's block buffer is invalidated by
    size_t s_lo = 0;                // the next call, so blocks are copied here
    bool saw_error = false;
    uint64_t served = 0;

    // Returns the next full decompressed block, or nullptr at clean EOF / error.
    // The pointer is valid until the next call (backed by the staging vector).
    const uint8_t* next_block(size_t& sz) {
        if (a == nullptr) return nullptr;
        while (true) {
            const void* buf; size_t bsz; la_int64_t off;
            int r = archive_read_data_block(a, &buf, &bsz, &off);
            if (r == ARCHIVE_EOF) return nullptr;
            if (r != ARCHIVE_OK) { saw_error = true; return nullptr; }
            if (bsz == 0) continue;   // zero-size OK blocks occur mid-stream (7z)
            const uint8_t* p = (const uint8_t*)buf;
            staging.assign(p, p + bsz);
            s_lo = 0;
            served += bsz;
            sz = bsz;
            return staging.data();
        }
    }

public:
    explicit ArchiveSrc(const std::string& path) {
        a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);
        if (archive_read_open_filename(a, path.c_str(), 10240) != ARCHIVE_OK) {
            archive_read_free(a); a = nullptr; return;
        }
        if (archive_read_next_header(a, &entry) != ARCHIVE_OK) {
            archive_read_free(a); a = nullptr; return;
        }
        staging.reserve(1 << 20);
    }
    ~ArchiveSrc() override { if (a) archive_read_free(a); }
    bool ok() const override { return a != nullptr; }
    size_t read(void* dst, size_t n) override {
        if (a == nullptr) return 0;
        uint8_t* d = (uint8_t*)dst;
        size_t out = 0;
        while (out < n) {
            if (s_lo == staging.size()) {
                size_t sz = 0;
                if (!next_block(sz)) break;
            }
            size_t take = std::min(staging.size() - s_lo, n - out);
            std::memcpy(d + out, staging.data() + s_lo, take);
            s_lo += take;
            out += take;
        }
        return out;
    }
    const char* kind() const override { return "libarchive"; }
    uint64_t bytes_read() const override { return served; }
    bool errored() const override { return saw_error; }
};

// ---- pipelined archive source: decode on a producer thread --------------------
//
// Codec decompression and parsing are independent CPU work over the same byte
// stream; running libarchive on a producer thread that feeds a bounded queue
// overlaps them, so wall time approaches max(decode, parse) instead of their
// sum. Falls back to synchronous ArchiveSrc reads if the thread cannot spawn.

class PipelinedSrc : public DataSrc {
    ArchiveSrc arc;
    std::thread producer;
    std::mutex mu;
    std::condition_variable cv_data, cv_slot;
    std::deque<std::vector<uint8_t>> q;
    size_t q_bytes = 0;
    size_t b_lo = 0;                 // consume offset into q.front()
    static constexpr size_t Q_CAP = 4u << 20;   // decoded bytes in flight
    bool done = false;               // producer finished (clean EOF or error)
    bool piped = false;              // producer thread actually running
    bool closed = false;             // consumer abandoned the stream
    uint64_t decoded = 0;

    void produce() {
        std::unique_lock<std::mutex> lk(mu);
        try {
            while (true) {
                lk.unlock();
                size_t sz = 0;
                const uint8_t* p = arc.next_block(sz);   // decode outside the lock
                lk.lock();
                if (!p) { done = true; cv_data.notify_all(); return; }
                q.emplace_back(p, p + sz);
                q_bytes += sz;
                decoded += sz;
                cv_data.notify_one();
                while (q_bytes >= Q_CAP && !closed)
                    cv_slot.wait(lk);
                if (closed) return;
            }
        } catch (...) {
            done = true;
            cv_data.notify_all();
        }
    }

public:
    explicit PipelinedSrc(const std::string& path) : arc(path) {}

    void start() {
        if (!arc.ok()) { return; }   // reads fall through to arc.read() -> 0
        try { producer = std::thread(&PipelinedSrc::produce, this); piped = true; }
        catch (...) { piped = false; }   // synchronous fallback
    }

    ~PipelinedSrc() override {
        { std::lock_guard<std::mutex> lk(mu); closed = true; }
        cv_slot.notify_all();
        if (producer.joinable()) producer.join();
    }

    bool ok() const override { return arc.ok(); }
    const char* kind() const override { return piped ? "libarchive, pipelined decode" : "libarchive"; }
    uint64_t bytes_read() const override { return piped ? decoded : arc.bytes_read(); }
    bool errored() const override { return arc.errored(); }

    // Blocks until >=1 byte is available or true EOF; never returns 0 mid-stream
    // (ByteStream treats a 0 short read as permanent EOF).
    size_t read(void* dst, size_t n) override {
        if (!piped) return arc.read(dst, n);
        uint8_t* d = (uint8_t*)dst;
        size_t out = 0;
        std::unique_lock<std::mutex> lk(mu);
        while (out < n) {
            if (q.empty()) {
                if (done) break;
                cv_data.wait(lk);
                continue;
            }
            const auto& blk = q.front();
            size_t take = std::min(blk.size() - b_lo, n - out);
            std::memcpy(d + out, blk.data() + b_lo, take);
            out += take; b_lo += take; q_bytes -= take;
            if (b_lo == blk.size()) { q.pop_front(); b_lo = 0; cv_slot.notify_one(); }
        }
        return out;
    }
};

// ---- chunked reader with bounded pushback -----------------------------------
//
// Draws from a DataSrc through a 64KB buffer and enforces a *content limit*
// (the declared MTrk length) at consume time: bytes beyond the limit are
// neither consumed nor discarded, so the next chunk's header stays intact.

class ByteStream {
    DataSrc& src;
    static constexpr size_t CAP = 1 << 16;
    uint8_t data[CAP];
    size_t lo = 0, hi = 0;
    size_t limit_remaining = (size_t)-1;
    size_t limit_total = 0;
    bool eof = false;
    uint64_t pulled = 0;   // bytes drawn from the source (progress fraction)

    void fill() {
        if (eof) return;
        if (lo == hi) { lo = hi = 0; }
        if (hi == CAP) return;
        size_t got = src.read(data + hi, CAP - hi);
        if (got == 0) eof = true;
        pulled += got;
        hi += got;
    }
public:
    explicit ByteStream(DataSrc& s) : src(s) {}

    uint64_t bytes_pulled() const { return pulled; }   // source bytes drawn so far

    void set_limit(size_t n) { limit_remaining = n; limit_total = n; }
    size_t consumed() const { return limit_total - std::min(limit_total, limit_remaining); }

    size_t avail() {
        size_t a = hi - lo;
        if (a > limit_remaining) a = limit_remaining;
        if (a == 0 && limit_remaining > 0) { fill(); a = hi - lo; if (a > limit_remaining) a = limit_remaining; }
        return a;
    }
    bool at_end() { return avail() == 0; }

    int get() {
        if (limit_remaining == 0) return -1;
        if (lo == hi) { fill(); if (lo == hi) return -1; }
        limit_remaining--;
        return data[lo++];
    }
    void skip_back_one() { if (lo > 0 && limit_remaining < limit_total) { lo--; limit_remaining++; } }

    void skip(size_t n) {
        if (n > limit_remaining) n = limit_remaining;
        size_t remaining = n;
        size_t a = std::min(hi - lo, remaining);
        lo += a; remaining -= a;
        while (remaining > 0 && !eof) {
            size_t got = src.read(data, std::min(CAP, remaining));
            if (got == 0) { eof = true; break; }
            pulled += got;
            remaining -= got;
        }
        limit_remaining -= (n - remaining);
    }

    size_t read(void* dst, size_t n) {
        if (n > limit_remaining) n = limit_remaining;
        size_t out = 0;
        uint8_t* d = (uint8_t*)dst;
        size_t a = std::min(hi - lo, n);
        std::memcpy(d, data + lo, a);
        lo += a; out += a;
        while (out < n && !eof) {
            size_t got = src.read(d + out, n - out);
            if (got == 0) { eof = true; break; }
            out += got;
        }
        limit_remaining -= out;
        return out;
    }

    uint64_t vlq() {
        uint64_t val = 0;
        for (int i = 0; i < 4; ++i) {
            int b = get();
            if (b < 0) break;
            val = (val << 7) | (uint64_t)(b & 0x7F);
            if (!(b & 0x80)) break;
        }
        return val;
    }
};

// ---- accumulation targets ---------------------------------------------------

struct TickData {
    std::vector<uint64_t> dense_ons;     // per-tick note-on counts
    std::vector<int64_t>  dense_deltas;  // per-tick polyphony deltas (+1 on, -1 off)
    uint64_t max_tick = 0;
    uint64_t total_ons = 0;
    size_t ntracks = 0;
    size_t desync_tracks = 0;

    void ensure(uint64_t t) {
        if (t >= dense_ons.size()) {
            size_t n = dense_ons.size();
            if (n == 0) n = 4096;
            while (n <= t) n *= 2;
            dense_ons.resize(n, 0);
            dense_deltas.resize(n, 0);
        }
    }
    void note_on(uint64_t t) { ensure(t); dense_ons[t]++; dense_deltas[t]++; total_ons++; }
    void delta_at(uint64_t t, int64_t d) { ensure(t); dense_deltas[t] += d; }
    // CC accumulation (only touched when CC stats are enabled).
    std::vector<uint64_t> dense_cc;      // per-tick control-change counts
    uint64_t total_cc = 0;
    void cc_at(uint64_t t) {
        ensure(t);
        if (dense_cc.size() < dense_ons.size()) dense_cc.resize(dense_ons.size(), 0);
        dense_cc[t]++;
        total_cc++;
    }
};

// ---- single sequential walk over the image -----------------------------------

static bool scan_image(DataSrc& src, bool vel0_as_note_off, TickData& td,
                       std::vector<TempoChange>& tempo_raw, uint16_t& out_division,
                       const ProgressCallbacks& cb) {
    ByteStream bs(src);
    const bool count_cc = cb.cc_stats;   // CC stats off => zero extra work in the event loop

    // Continuous progress bookkeeping (checked once per ~1M events).
    using clock = std::chrono::steady_clock;
    const clock::time_point t_start = clock::now();
    uint64_t ev_total = 0;
    uint64_t ev_count = 0;
    const uint64_t total_bytes = src.total_bytes();   // 0 = unknown fraction

    auto ping = [&]() {
        if (!cb.on_scan_progress) return;
        double el = std::chrono::duration<double>(clock::now() - t_start).count();
        double frac = total_bytes > 0 ? (double)bs.bytes_pulled() / (double)total_bytes : -1.0;
        cb.on_scan_progress(ev_total + ev_count, el,
                            el > 0.0 ? (double)(ev_total + ev_count) / el : 0.0, frac);
    };

    auto rd32 = [&]() -> int64_t {
        int a = bs.get(), b = bs.get(), c = bs.get(), d = bs.get();
        if (a < 0 || b < 0 || c < 0 || d < 0) return -1;
        return ((int64_t)a << 24) | ((int64_t)b << 16) | ((int64_t)c << 8) | (int64_t)d;
    };

    if (rd32() != 0x4D546864) {
        emit(cb, "Error: not a MIDI file (missing MThd header).\n", true);
        return false;
    }
    int64_t hlen = rd32();
    if (hlen < 0) { emit(cb, "Error: truncated MIDI header.\n", true); return false; }
    int f2 = bs.get(), f1 = bs.get();   // format
    int n2 = bs.get(), n1 = bs.get();   // number of tracks
    int d2 = bs.get(), d1 = bs.get();   // division
    if (f2 < 0 || f1 < 0 || n2 < 0 || n1 < 0 || d2 < 0 || d1 < 0) {
        emit(cb, "Error: truncated MIDI header.\n", true); return false;
    }
    if (hlen > 6) bs.skip((size_t)hlen - 6);
    uint16_t division = (((uint16_t)d2 << 8) | (uint16_t)d1) & 0x7FFF;
    if (division == 0) division = 480;
    out_division = division;

    while (true) {
        int64_t tag = rd32();
        if (tag < 0) break;
        int64_t len = rd32();
        if (len < 0) { emit(cb, "Error: truncated chunk header.\n", true); return false; }
        if (tag != 0x4D54726B) { bs.skip((size_t)len); continue; }

        bs.set_limit((size_t)len);
        uint64_t tick = 0;
        uint8_t running = 0;
        uint32_t refcount[16][128] = {};   // black-MIDI tracks hold >255 overlapping instances of one note

        while (bs.at_end() == false) {
            tick += bs.vlq();
            int st = bs.get();
            if (st < 0) break;
            uint8_t status;
            if (st < 0x80) {
                // Running status: re-examine the data byte (mirrors the parallel
                // engine, including the running==0 fall-through that consumes
                // nothing further).
                bs.skip_back_one();
                status = running;
            } else {
                status = (uint8_t)st;
                running = (status < 0xF0) ? status : 0;
            }

            if (status == 0xFF) {
                int type = bs.get();
                if (type < 0) break;
                uint64_t mlen = bs.vlq();
                if (type == 0x51 && mlen == 3 && bs.avail() >= 3) {
                    uint32_t us = ((uint32_t)bs.get() << 16) | ((uint32_t)bs.get() << 8) | (uint32_t)bs.get();
                    if (us > 0) tempo_raw.push_back({tick, 0.0, us, 60000000.0 / us});
                } else {
                    bs.skip((size_t)mlen);
                }
            } else if (status == 0xF0 || status == 0xF7) {
                bs.skip((size_t)bs.vlq());
            } else if (status >= 0xF1 && status <= 0xF6) {
                if (status == 0xF1 || status == 0xF3) bs.skip(1);
                else if (status == 0xF2) bs.skip(2);
            } else if (status >= 0x80) {
                uint8_t et = status & 0xF0;
                int n1 = bs.get();
                if (n1 < 0) break;
                if (et != 0xC0 && et != 0xD0) {
                    int n2 = bs.get();
                    if (n2 < 0) break;
                    uint8_t ch = status & 0x0F, note = (uint8_t)n1, vel = (uint8_t)n2;
                    if (et == 0x90 && (vel > 0 || !vel0_as_note_off)) {
                        refcount[ch][note]++;
                        td.note_on(tick);
                    } else if (et == 0x90 || et == 0x80) {
                        if (refcount[ch][note] > 0) { refcount[ch][note]--; td.delta_at(tick, -1); }
                    }
                } else if (et == 0xB0 && count_cc) {
                    td.cc_at(tick);   // control change: counted only when the CC stat is on
                }
            }
            // status < 0x80 with running==0: consume nothing (matches parallel engine)

            // Continuous progress ping: a counter compare on the hot path, real
            // work (clock + callback) only once per ~1M events.
            if (++ev_count >= 1000000) {
                ev_count = 0;
                ev_total += 1000000;
                ping();
                if (cb.cancel_flag && cb.cancel_flag->load()) {
                    emit(cb, "  Cancelled.\n");
                    return false;
                }
            }
        }

        // Close held notes at the track's final tick (parallel-engine end-of-track flush).
        for (int ch = 0; ch < 16; ++ch)
            for (int n = 0; n < 128; ++n)
                if (refcount[ch][n] > 0) td.delta_at(tick, -(int64_t)refcount[ch][n]);
        if (tick > td.max_tick) td.max_tick = tick;

        size_t consumed = bs.consumed();
        bs.set_limit((size_t)-1);
        if (consumed < (size_t)len) {
            td.desync_tracks++;
            bs.skip((size_t)len - consumed);   // jump to next chunk boundary
        }
        td.ntracks++;
    }

    // Final progress ping so elapsed/ev-per-s cover the whole scan.
    ping();
    return true;
}

// ---- anchor-interpolated sweep: tick buckets -> FrameStats --------------------

static std::vector<FrameStats> sweep_to_frames(TickData& td,
                                               std::vector<TempoChange>& tempo_raw,
                                               uint16_t ppqn, double fps, bool with_cc = false) {
    // Build the tempo map exactly like the parallel engine: default entry,
    // stable sort by tick, dedup keeping the LAST change at each tick (the
    // track-major walk order makes this identical to the parallel engine's
    // stable_sort over per-track collections), then anchor times.
    std::vector<TempoChange> tm;
    tm.reserve(tempo_raw.size() + 1);
    tm.push_back({0, 0.0, 500000, 120.0});
    for (const auto& tc : tempo_raw) if (tc.us_per_quarter > 0) tm.push_back(tc);
    std::stable_sort(tm.begin(), tm.end(),
                     [](const TempoChange& a, const TempoChange& b) { return a.tick < b.tick; });
    {
        size_t w = 0;
        for (size_t i = 0; i < tm.size(); ++i)
            if (i + 1 == tm.size() || tm[i + 1].tick != tm[i].tick) tm[w++] = tm[i];
        tm.resize(w);
    }
    for (size_t i = 1; i < tm.size(); ++i) {
        uint64_t dtk = tm[i].tick - tm[i - 1].tick;
        tm[i].time_sec = tm[i - 1].time_sec
                       + (double)dtk * (double)tm[i - 1].us_per_quarter / ((double)ppqn * 1e6);
    }

    const uint64_t nticks = td.max_tick + 1;
    std::vector<uint64_t>& ons = td.dense_ons;
    std::vector<int64_t>& deltas = td.dense_deltas;
    // Track tails can extend past the last written tick; size arrays to cover
    // the whole tick span (also covers the no-events-at-all case).
    if (ons.size() < nticks) {
        ons.resize((size_t)nticks, 0);
        deltas.resize((size_t)nticks, 0);
    }
    const double inv = 1.0 / ((double)ppqn * 1e6);
    const size_t W = (size_t)std::round(fps);

    std::vector<FrameStats> out;
    uint64_t cum = 0;
    uint64_t cum_cc = 0;
    int64_t poly = 0, peak_poly = 0;
    double peak_nps = 0.0;
    size_t cur_frame = 0;
    size_t conv = 0;   // anchor for tick->sec conversion (walked by tick)
    size_t bpmw = 0;   // tempo walk for frame BPM (walked by time)

    auto push_frame = [&](size_t k) {
        while (bpmw + 1 < tm.size() && tm[bpmw + 1].time_sec <= (double)k / fps) bpmw++;
        uint64_t prev_cum = (k >= W) ? out[k - W].cumulative_notes : 0;
        double nps = (double)(cum - prev_cum);
        peak_nps = std::max(peak_nps, nps);
        peak_poly = std::max(peak_poly, poly);
        out.push_back({k, (double)k / fps, cum, cum_cc, nps, peak_nps,
                       std::max<int64_t>(0, poly), peak_poly, tm[bpmw].bpm});
    };

    for (uint64_t t = 0; t < nticks; ++t) {
        uint64_t o = ons[(size_t)t];
        int64_t  d = deltas[(size_t)t];
        if (o == 0 && d == 0) continue;

        while (conv + 1 < tm.size() && tm[conv + 1].tick <= t) conv++;
        double sec = tm[conv].time_sec + (double)(t - tm[conv].tick) * (double)tm[conv].us_per_quarter * inv;
        size_t fi = (size_t)(sec * fps);

        // Frames between the last processed tick and this one are event-free;
        // their end-of-frame state is the current running state.
        while (cur_frame < fi) push_frame(cur_frame++);

        cum += o;
        poly += d;
        if (with_cc) cum_cc += td.dense_cc[(size_t)t];
    }

    // Determine total frames from sec(max_tick) (catch up conversion anchors first).
    while (conv + 1 < tm.size() && tm[conv + 1].tick <= td.max_tick) conv++;
    double max_time = tm[conv].time_sec
                    + (double)(td.max_tick - tm[conv].tick) * (double)tm[conv].us_per_quarter * inv;
    size_t total_frames = (size_t)std::ceil(max_time * fps) + 1;
    while (cur_frame < total_frames) push_frame(cur_frame++);

    return out;
}

// ---- entry point --------------------------------------------------------------

static std::vector<FrameStats> process_streaming(
    const std::string& filename, double fps, uint16_t& out_division,
    uint64_t& out_total_notes, bool vel0_as_note_off, const ProgressCallbacks& cb)
{
    using clock = std::chrono::steady_clock;
    out_total_notes = 0;
    const bool compressed = is_compressed_file(filename);
    emit(cb, "Reading and parsing \"" + filename + "\"" + (compressed ? " (compressed)" : "") + "...\n");

    try {
        std::unique_ptr<DataSrc> src;
        std::unique_ptr<PipelinedSrc> psrc;
        if (compressed) {
            psrc.reset(new PipelinedSrc(filename));
            psrc->start();   // no-op -> synchronous fallback if open or spawn failed
            src.reset(psrc.release());   // single owner: src; psrc left empty
        } else {
            src.reset(new FileSrc(filename));
        }

        if (!src->ok()) {
            emit(cb, compressed ? "Error: failed to open archive with libarchive.\n"
                                : "Error: cannot open file.\n", true);
            return {};
        }

        TickData td;
        std::vector<TempoChange> tempo_raw;
        uint16_t division = 480;

        emit(cb, "  Single sequential pass (" + std::string(src->kind()) + " stream)...\n");
        const clock::time_point t_scan = clock::now();
        if (!scan_image(*src, vel0_as_note_off, td, tempo_raw, division, cb)) return {};
        const clock::time_point t_sweep = clock::now();
        out_division = division;

        emit(cb, "  " + std::to_string(td.ntracks) + " tracks\n");
        if (td.desync_tracks > 0)
            emit(cb, "Warning: " + std::to_string(td.desync_tracks)
                     + " track(s) consumed a different number of bytes than declared (possible parser desync).\n", true);

        auto frames = sweep_to_frames(td, tempo_raw, division, fps, cb.cc_stats);
        out_total_notes = td.total_ons;

        // Processing statistics.
        const double t_scan_s   = std::chrono::duration<double>(t_sweep - t_scan).count();
        const double t_sweep_s  = std::chrono::duration<double>(clock::now() - t_sweep).count();
        const uint64_t nev = td.total_ons + td.ntracks + td.total_cc;   // note-ons + EOT markers + CC
        auto rate = [](uint64_t n, double s) {
            return (s > 0.0) ? (double)n / s : 0.0;
        };
        std::ostringstream st;
        st << "  Stats: " << format_with_commas(nev) << " events in " << std::fixed << std::setprecision(2)
           << t_scan_s << "s scan (" << format_with_commas((uint64_t)rate(nev, t_scan_s)) << " ev/s)";
        if (compressed) {
            const uint64_t dec = src->bytes_read();
            if (dec > 0) st << ", " << (dec >> 20) << " MB decoded in "
                            << std::setprecision(2) << t_scan_s << "s ("
                            << std::setprecision(1) << rate(dec, t_scan_s) / 1e6 << " MB/s)";
            if (src->errored()) st << "\n  Warning: archive stream reported an error before EOF.";
        }
        st << ", " << std::setprecision(2) << t_sweep_s << "s sweep\n";
        emit(cb, st.str());
        return frames;
    } catch (const std::bad_alloc&) {
        emit(cb, "Error: not enough memory for tick-space arrays (song duration x ppqn too large).\n", true);
        return {};
    } catch (const std::exception& e) {
        emit(cb, std::string("Error: ") + e.what() + "\n", true);
        return {};
    }
}

} // namespace fmcg_stream

// ProgressCallbacks is defined above the streaming engine section.

// ---------------------------------------------------------------------------
// MIDI Processor — routes all inputs through the single-pass streaming engine.
// ---------------------------------------------------------------------------

class ScaleMidiProcessor {
public:
    static std::vector<FrameStats> process_midi(
        const std::string& filename, double fps, uint16_t& out_division,
        uint64_t& out_total_notes, bool vel0_as_note_off = true,
        const ProgressCallbacks& cb = {})
    {
        return fmcg_stream::process_streaming(filename, fps, out_division,
                                              out_total_notes, vel0_as_note_off, cb);
    }
};

// ---------------------------------------------------------------------------
// Formatting utilities
// ---------------------------------------------------------------------------

inline std::string format_with_commas(uint64_t val) {
    std::string s = std::to_string(val);
    int insert_pos = static_cast<int>(s.length()) - 3;
    while (insert_pos > 0) {
        s.insert(insert_pos, ",");
        insert_pos -= 3;
    }
    return s;
}

// Zero-pad to a minimum width (never truncates longer numbers). pad<=0 -> plain.
inline std::string pad_num(uint64_t val, int pad) {
    std::string s = std::to_string(val);
    if (pad > 0 && static_cast<int>(s.size()) < pad)
        s.insert(0, static_cast<size_t>(pad) - s.size(), '0');
    return s;
}

// Negative-aware, truncating toward zero (standard clock behaviour):
// -2.5 -> "-00:02.500". Used for the start-delay countdown, where current-time
// fields run from negative up to 0 as the song approaches.
inline std::string format_time(double total_sec, bool milli, int pad = 0) {
    (void)pad;   // padding applies to numeric fields only, not mm:ss
    bool neg = total_sec < 0;
    double av = neg ? -total_sec : total_sec;
    long long iv = static_cast<long long>(av);
    int ms = milli ? static_cast<int>(std::lround((av - (double)iv) * 1000.0)) : 0;
    if (ms >= 1000) { ms -= 1000; iv += 1; }
    unsigned long long a = (unsigned long long)iv;
    int mins = (int)(a / 60);
    int secs = (int)(a % 60);
    std::ostringstream ss;
    if (neg) ss << '-';
    ss << std::setfill('0') << std::setw(2) << mins << ":" << std::setw(2) << secs;
    if (milli) ss << "." << std::setw(3) << ms;
    return ss.str();
}

// Per-statistic comma separator selection.
struct CommaOpts {
    bool notes     = true;   // {nc} {nc-total} {nc-rem}
    bool polyphony = true;   // {plph} {plph-max}
    bool nps       = true;   // {nps} {nps-max}
    bool cc        = false;  // {cc} {cc-total} {cc-rem}
};

// Padding: a single "pad with leading zeros" checkbox. When on, each numeric
// stat is padded to the digit width of ITS OWN maximum (notes -> total notes,
// polyphony -> peak polyphony, NPS -> peak NPS, CC -> total CC, seconds ->
// total seconds), so every value of a stat lines up without a user-set width.
struct PadOpts {
    bool enabled = false;
};

inline std::string ProcessTemplateLine(const std::string& line, const FrameStats& fs,
                                       uint64_t total_notes, uint64_t total_cc_events,
                                       double max_time_sec, uint16_t ppqn,
                                       const CommaOpts& commas = {}, PadOpts pad = {}) {
    std::string result = line;
    auto replace = [&](const std::string& token, const std::string& val) {
        size_t pos = 0;
        while ((pos = result.find(token, pos)) != std::string::npos) {
            result.replace(pos, token.length(), val);
            pos += val.length();
        }
    };

    const bool p = pad.enabled;
    // Digit widths: each stat pads toward its own running maximum.
    auto w = [](uint64_t v) { int n = 1; while (v >= 10) { v /= 10; ++n; } return n; };
    const int w_notes = p ? w(total_notes) : 0;
    const int w_cc    = p ? w(total_cc_events) : 0;
    const int w_poly  = p ? w(static_cast<uint64_t>(std::max<int64_t>(0, fs.peak_polyphony))) : 0;
    const int w_nps   = p ? w(static_cast<uint64_t>(std::max<double>(0, std::round(fs.peak_nps)))) : 0;
    const int w_sec   = p ? w(static_cast<uint64_t>(std::max<double>(0, max_time_sec))) : 0;

    auto fmt = [&](bool commas_on, uint64_t v, int width) -> std::string {
        // Pad the raw digits first, then group with commas, so the pad always
        // reflects digit count.
        std::string s = std::to_string(v);
        if (width > 0 && (int)s.size() < width)
            s.insert(0, (size_t)width - s.size(), '0');
        if (commas_on) {
            std::string rev;
            int count = 0;
            for (int i = (int)s.size() - 1; i >= 0; --i) {
                rev.push_back(s[i]);
                if (++count % 3 == 0 && i > 0) rev.push_back(',');
            }
            s.assign(rev.rbegin(), rev.rend());
        }
        return s;
    };
    auto fmt_notes = [&](uint64_t v) { return fmt(commas.notes, v, w_notes); };
    auto fmt_poly  = [&](uint64_t v) { return fmt(commas.polyphony, v, w_poly); };
    auto fmt_nps   = [&](uint64_t v) { return fmt(commas.nps, v, w_nps); };
    auto fmt_cc    = [&](uint64_t v) { return fmt(commas.cc, v, w_cc); };

    replace("{nc}", fmt_notes(fs.cumulative_notes));
    replace("{nc-total}", fmt_notes(total_notes));
    replace("{nc-rem}", fmt_notes(total_notes > fs.cumulative_notes ? total_notes - fs.cumulative_notes : 0));

    replace("{cc}", fmt_cc(fs.cumulative_cc));
    replace("{cc-total}", fmt_cc(total_cc_events));
    replace("{cc-rem}", fmt_cc(total_cc_events > fs.cumulative_cc ? total_cc_events - fs.cumulative_cc : 0));

    // Signed seconds, truncating toward zero (start-delay countdown runs
    // negative and counts up to 0).
    auto fmt_sec = [&](double v) -> std::string {
        long long iv = static_cast<long long>(v);   // truncates toward zero
        bool neg = iv < 0;
        std::string s = std::to_string(neg ? (uint64_t)(-iv) : (uint64_t)iv);
        if (w_sec > 0 && (int)s.size() < w_sec)
            s.insert(0, (size_t)w_sec - s.size(), '0');
        return neg ? "-" + s : s;
    };

    replace("{sec}", fmt_sec(fs.timestamp_sec));
    replace("{sec-max}", fmt_sec(max_time_sec));
    replace("{sec-rem}", fmt_sec(max_time_sec > fs.timestamp_sec ? max_time_sec - fs.timestamp_sec : 0));

    replace("{time}", format_time(fs.timestamp_sec, false));
    replace("{time-max}", format_time(max_time_sec, false));
    replace("{time-rem}", format_time(max_time_sec > fs.timestamp_sec ? max_time_sec - fs.timestamp_sec : 0, false));

    replace("{time-milli}", format_time(fs.timestamp_sec, true));
    replace("{time-milli-max}", format_time(max_time_sec, true));
    replace("{time-milli-rem}", format_time(max_time_sec > fs.timestamp_sec ? max_time_sec - fs.timestamp_sec : 0, true));

    std::ostringstream bpm_ss;
    bpm_ss << std::fixed << std::setprecision(2) << fs.bpm;

    replace("{bpm}", bpm_ss.str());
    replace("{ppqn}", std::to_string(ppqn));

    replace("{plph}", fmt_poly(static_cast<uint64_t>(std::max<int64_t>(0, fs.polyphony))));
    replace("{plph-max}", fmt_poly(static_cast<uint64_t>(std::max<int64_t>(0, fs.peak_polyphony))));

    uint64_t round_nps = static_cast<uint64_t>(std::round(std::max<double>(0, fs.notes_per_second)));
    uint64_t round_peak_nps = static_cast<uint64_t>(std::round(std::max<double>(0, fs.peak_nps)));
    replace("{nps}", fmt_nps(round_nps));
    replace("{nps-max}", fmt_nps(round_peak_nps));

    return result;
}

inline std::string to_ass_time(double seconds) {
    int hrs = static_cast<int>(seconds) / 3600;
    int mins = (static_cast<int>(seconds) % 3600) / 60;
    int secs = static_cast<int>(seconds) % 60;
    int cs = static_cast<int>((seconds - static_cast<int>(seconds)) * 100);
    std::ostringstream ss;
    ss << hrs << ":" << std::setfill('0') << std::setw(2) << mins << ":"
       << std::setw(2) << secs << "." << std::setw(2) << cs;
    return ss.str();
}

// ---------------------------------------------------------------------------
// Font detection utilities
// ---------------------------------------------------------------------------

inline uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
inline uint16_t read_be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

inline std::string utf16be_to_utf8(const uint8_t* s, size_t len) {
    std::string out;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint32_t cp = ((uint32_t)s[i] << 8) | s[i + 1];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len) {
            uint32_t lo = ((uint32_t)s[i + 2] << 8) | s[i + 3];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// Extract one string from the sfnt 'name' table by name ID.
inline bool extract_sfnt_name_id(std::ifstream& f, uint64_t base, uint16_t want_id, std::string& out) {
    f.clear();
    f.seekg((std::streamoff)base, std::ios::beg);
    uint8_t hdr[12];
    if (!f.read((char*)hdr, 12)) return false;
    uint16_t num_tables = read_be16(hdr + 4);
    if (num_tables == 0 || num_tables > 512) return false;

    std::vector<uint8_t> dir((size_t)num_tables * 16);
    if (!f.read((char*)dir.data(), (std::streamsize)dir.size())) return false;

    uint64_t name_off = 0, name_len = 0;
    for (uint16_t i = 0; i < num_tables; ++i) {
        const uint8_t* rec = dir.data() + (size_t)i * 16;
        if (read_be32(rec) == 0x6E616D65) {
            name_off = read_be32(rec + 8);
            name_len = read_be32(rec + 12);
            break;
        }
    }
    if (name_off == 0 || name_len < 6) return false;

    f.clear();
    f.seekg((std::streamoff)(base + name_off), std::ios::beg);
    std::vector<uint8_t> nt((size_t)std::min<uint64_t>(name_len, 1u << 16));
    if (!f.read((char*)nt.data(), (std::streamsize)nt.size())) return false;

    uint16_t count = read_be16(nt.data() + 2);
    uint16_t str_off = read_be16(nt.data() + 4);
    size_t rec_pos = 6;
    for (uint32_t i = 0; i < count && rec_pos + 12 <= nt.size(); ++i, rec_pos += 12) {
        const uint8_t* rec = nt.data() + rec_pos;
        uint16_t platform = read_be16(rec);
        uint16_t encoding = read_be16(rec + 2);
        uint16_t name_id = read_be16(rec + 6);
        uint16_t length = read_be16(rec + 8);
        uint16_t offset = read_be16(rec + 10);
        if (name_id != want_id) continue;

        uint32_t off = (uint32_t)str_off + offset;
        if (length == 0 || off + length > nt.size()) continue;

        if ((platform == 3 && (encoding == 1 || encoding == 10)) || platform == 0)
            out = utf16be_to_utf8(nt.data() + off, length);
        else if (platform == 3 || platform == 1)
            out.assign((const char*)nt.data() + off, length);
        else continue;
        if (!out.empty()) return true;
    }
    return false;
}

// Family name: prefer the typographic family (ID 16), fall back to ID 1.
inline bool extract_family_from_sfnt(std::ifstream& f, uint64_t base, std::string& out_family) {
    if (extract_sfnt_name_id(f, base, 16, out_family) && !out_family.empty()) return true;
    return extract_sfnt_name_id(f, base, 1, out_family) && !out_family.empty();
}

inline std::string read_font_family(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    uint8_t hdr[12];
    if (!f.read((char*)hdr, 12)) return "";
    uint32_t version = read_be32(hdr);
    if (version == 0x74746366) {
        uint32_t num_fonts = read_be32(hdr + 8);
        if (num_fonts == 0 || num_fonts > 64) return "";
        std::string best;
        for (uint32_t i = 0; i < num_fonts; ++i) {
            uint8_t ob[4];
            if (!f.read((char*)ob, 4)) break;
            std::string fam;
            if (extract_family_from_sfnt(f, read_be32(ob), fam) && fam.size() > best.size()) best = fam;
        }
        return best;
    }
    if (version != 0x00010000 && version != 0x4F54544F && version != 0x74727565)
        return "";
    std::string fam;
    return extract_family_from_sfnt(f, 0, fam) ? fam : "";
}

inline void collect_font_files(const std::string& dir, std::vector<std::string>& out, int depth) {
    if (depth > 8) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collect_font_files(full, out, depth + 1);
        } else {
            size_t dot = name.find_last_of('.');
            std::string ext = (dot == std::string::npos) ? "" : name.substr(dot);
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") out.push_back(full);
        }
    }
    closedir(d);
}

inline bool ci_less_str(const std::string& a, const std::string& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = (unsigned char)tolower((unsigned char)a[i]);
        unsigned char cb = (unsigned char)tolower((unsigned char)b[i]);
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

inline std::vector<std::string> get_font_directories() {
    std::vector<std::string> dirs;
#if defined(_WIN32)
    const char* windir = std::getenv("WINDIR");
    if (windir) dirs.push_back(std::string(windir) + "/Fonts");
    const char* local = std::getenv("LOCALAPPDATA");
    if (local) dirs.push_back(std::string(local) + "/Microsoft/Windows/Fonts");
#else
    for (const char* d : {"/usr/share/fonts", "/usr/local/share/fonts", "/System/Library/Fonts", "/Library/Fonts"})
        dirs.push_back(d);
#endif
    const char* home = std::getenv("HOME");
#if defined(_WIN32)
    if (!home) home = std::getenv("USERPROFILE");
    if (home) dirs.push_back(std::string(home) + "/AppData/Local/Microsoft/Windows/Fonts");
#else
    if (home) {
        dirs.push_back(std::string(home) + "/.fonts");
        dirs.push_back(std::string(home) + "/.local/share/fonts");
        dirs.push_back(std::string(home) + "/Library/Fonts");
    }
#endif
    std::sort(dirs.begin(), dirs.end(), ci_less_str);
    dirs.erase(std::unique(dirs.begin(), dirs.end(), [](const std::string& a, const std::string& b) {
        return !ci_less_str(a, b) && !ci_less_str(b, a);
    }), dirs.end());
    return dirs;
}

inline std::vector<std::string> enumerate_system_fonts() {
    std::set<std::string, bool (*)(const std::string&, const std::string&)> families(ci_less_str);

    std::vector<std::string> files;
    for (const std::string& dir : get_font_directories()) {
        files.clear();
        collect_font_files(dir, files, 0);
        for (const auto& path : files) {
            std::string fam = read_font_family(path);
            if (!fam.empty()) families.insert(fam);
        }
    }
    return std::vector<std::string>(families.begin(), families.end());
}

// Resolve a font family name (as returned by enumerate_system_fonts) to a concrete
// font file that can be loaded for the preview. Falls back to the first font file
// found; returns "" if no font files exist at all.
// One concrete variant (weight/slant) of a font family: a single file.
struct FontVariant {
    std::string file;        // full path to the ttf/otf/ttc
    std::string style;       // human-readable subfamily, e.g. "Bold", "Light Italic"
    int         weight = 400; // OS/2 usWeightClass (100..900), 0 if unknown
    bool        italic = false;
};

// All files belonging to a family (style name read from ID 17, falling back
// to ID 2; weight/italic from the OS/2 table when present).
inline std::vector<FontVariant> enumerate_font_variants(const std::string& family) {
    std::vector<FontVariant> out;
    std::set<std::string> seen_files;
    for (const std::string& dir : get_font_directories()) {
        std::vector<std::string> files;
        collect_font_files(dir, files, 0);
        for (const auto& path : files) {
            if (!seen_files.insert(path).second) continue;
            std::ifstream f(path, std::ios::binary);
            if (!f) continue;
            uint8_t hdr[12];
            if (!f.read((char*)hdr, 12)) continue;
            uint32_t version = read_be32(hdr);

            std::vector<uint64_t> bases;   // sfnt offsets (TTC: several)
            if (version == 0x74746366) {
                uint32_t num = read_be32(hdr + 8);
                if (num == 0 || num > 64) continue;
                for (uint32_t i = 0; i < num; ++i) {
                    uint8_t ob[4];
                    if (!f.read((char*)ob, 4)) break;
                    bases.push_back(read_be32(ob));
                }
            } else if (version == 0x00010000 || version == 0x4F54544F || version == 0x74727565) {
                bases.push_back(0);
            } else continue;

            for (uint64_t base : bases) {
                std::string fam;
                if (!extract_family_from_sfnt(f, base, fam) || fam.empty()) continue;
                if (ci_less_str(fam, family) || ci_less_str(family, fam)) continue;

                FontVariant v;
                v.file = path;
                if (!extract_sfnt_name_id(f, base, 17, v.style) || v.style.empty())
                    extract_sfnt_name_id(f, base, 2, v.style);
                if (v.style.empty()) v.style = "Regular";

                // OS/2 table: usWeightClass at offset 4, fsSelection at 62
                // (bit 0 = italic).
                f.clear();
                f.seekg(0, std::ios::end);
                std::streamoff fsize = f.tellg();
                f.seekg((std::streamoff)base, std::ios::beg);
                uint8_t h2[12];
                if (f.read((char*)h2, 12)) {
                    uint16_t num_tables = read_be16(h2 + 4);
                    std::vector<uint8_t> dir((size_t)num_tables * 16);
                    if (num_tables > 0 && num_tables <= 512 && f.read((char*)dir.data(), (std::streamsize)dir.size())) {
                        for (uint16_t i = 0; i < num_tables; ++i) {
                            const uint8_t* rec = dir.data() + (size_t)i * 16;
                            if (read_be32(rec) != 0x4F532F32) continue;   // "OS/2"
                            uint64_t os2 = base + read_be32(rec + 8);
                            if (os2 + 64 > (uint64_t)fsize) break;
                            f.clear(); f.seekg((std::streamoff)os2, std::ios::beg);
                            uint8_t t[64];
                            if (f.read((char*)t, 64)) {
                                v.weight = read_be16(t + 4);
                                v.italic = (read_be16(t + 62) & 1) != 0;
                            }
                            break;
                        }
                    }
                }

                bool dup = false;
                for (const auto& e : out) if (e.file == v.file && e.style == v.style) { dup = true; break; }
                if (!dup) out.push_back(std::move(v));
            }
        }
    }
    // Stable presentation: Regular first, then by weight, then by style name.
    std::stable_sort(out.begin(), out.end(), [](const FontVariant& a, const FontVariant& b) {
        auto is_reg = [](const std::string& s) {
            return !ci_less_str(s, "regular") && !ci_less_str("regular", s);
        };
        bool ra = is_reg(a.style), rb = is_reg(b.style);
        if (ra != rb) return ra > rb;   // Regular first
        if (a.weight != b.weight) return a.weight < b.weight;
        return a.style < b.style;
    });
    return out;
}

// Resolve a family + style (subfamily) to a concrete font file. Empty style
// matches the Regular variant / first file of the family.
inline std::string find_font_file_for_family(const std::string& family);

inline std::string find_font_file_for_variant(const std::string& family, const std::string& style) {
    if (style.empty()) return find_font_file_for_family(family);
    for (const auto& v : enumerate_font_variants(family))
        if (v.style == style) return v.file;
    return find_font_file_for_family(family);   // style vanished: graceful fallback
}

inline std::string find_font_file_for_family(const std::string& family) {
    std::string first_file;
    for (const std::string& dir : get_font_directories()) {
        std::vector<std::string> files;
        collect_font_files(dir, files, 0);
        for (const auto& path : files) {
            if (first_file.empty()) first_file = path;
            if (family.empty()) continue;
            std::string fam = read_font_family(path);
            if (!fam.empty() &&
                !ci_less_str(fam, family) && !ci_less_str(family, fam))
                return path;
        }
    }
    return first_file;
}

// ---------------------------------------------------------------------------
// ASS generation helper
// ---------------------------------------------------------------------------

struct AssConfig {
    int width = 1920;
    int height = 1080;
    double fps = 60.0;
    int font_size = 36;
    std::string font_family = "Arial";
    int bold = 0;               // ASS Style Bold flag (from the chosen variant)
    int italic_flag = 0;        // ASS Style Italic flag
    std::string text_color_ass = "&H00FFFFFF";
    std::string bg_color_ass = "&H00000000";   // video background (&HAABBGGRR)
    int ass_alignment = 7;
    // Counter placement: mode 0 = corner alignment (pos derived from
    // ass_alignment), mode 1 = explicit (pos_x,pos_y) at the text block's
    // TOP-LEFT corner (libass \pos anchors the top-left with alignment 7).
    int pos_mode = 0;
    int pos_x = 30;
    int pos_y = 30;
    CommaOpts commas;
    PadOpts pad;
    double start_delay = 0.0;   // lead-in seconds: zero stats, negative countdown
};

inline void generate_ass(const std::string& ass_filename, const std::vector<FrameStats>& frames,
                          const std::vector<std::string>& template_lines, uint64_t total_notes,
                          uint64_t total_cc, uint16_t ppqn, const AssConfig& cfg) {
    std::ofstream ass(ass_filename);
    ass << "[Script Info]\nScriptType: v4.00+\nPlayResX: " << cfg.width << "\nPlayResY: " << cfg.height << "\n\n";
    ass << "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n";
    ass << "Style: Default," << cfg.font_family << "," << cfg.font_size << "," << cfg.text_color_ass << "," << cfg.text_color_ass << ",&H00000000,&H80000000," << cfg.bold << "," << cfg.italic_flag << ",0,0,100,100,0,0,1,1,0," << cfg.ass_alignment << ",30,30,30,1\n\n";
    ass << "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";

    int pos_x = 30, pos_y = 30;
    if (cfg.pos_mode == 1) {
        // Explicit placement: \pos anchors the text block's top-left corner.
        pos_x = cfg.pos_x;
        pos_y = cfg.pos_y;
    } else if (cfg.ass_alignment == 9)      { pos_x = cfg.width - 30;  pos_y = 30; }
    else if (cfg.ass_alignment == 1) { pos_x = 30;              pos_y = cfg.height - 30; }
    else if (cfg.ass_alignment == 3) { pos_x = cfg.width - 30;  pos_y = cfg.height - 30; }

    double total_duration = frames.empty() ? 0.0 : frames.back().timestamp_sec;

    // Start-delay lead-in: one Dialogue per frame with all-zero stats and
    // negative current-time fields counting up to 0 at the song's first frame.
    if (cfg.start_delay > 0.0 && !frames.empty()) {
        FrameStats zero_fs;                       // all counters zero
        zero_fs.bpm = frames.front().bpm;         // show the starting BPM, not 0
        const double frame_dur = 1.0 / cfg.fps;
        const long long n_delay = static_cast<long long>(std::ceil(cfg.start_delay * cfg.fps));
        for (long long k = 0; k < n_delay; ++k) {
            double t_start = k * frame_dur;
            double t_end = (k + 1) * frame_dur;
            if (t_end > cfg.start_delay) t_end = cfg.start_delay;
            zero_fs.timestamp_sec = t_start - cfg.start_delay;   // negative song time
            zero_fs.frame_index = (size_t)k;
            std::string text_block;
            for (size_t i = 0; i < template_lines.size(); ++i) {
                text_block += ProcessTemplateLine(template_lines[i], zero_fs, total_notes, 0, total_duration, ppqn, cfg.commas, cfg.pad);
                if (i + 1 < template_lines.size()) text_block += "\\N";
            }
            ass << "Dialogue: 0," << to_ass_time(t_start) << "," << to_ass_time(t_end)
                << ",Default,,0,0,0,,{\\pos(" << pos_x << "," << pos_y << ")}" << text_block << "\n";
        }
    }

    // Song frames, offset by the start delay.
    for (size_t fi = 0; fi < frames.size(); ++fi) {
        const auto& f = frames[fi];
        std::string text_block;
        for (size_t i = 0; i < template_lines.size(); ++i) {
            text_block += ProcessTemplateLine(template_lines[i], f, total_notes, total_cc, total_duration, ppqn, cfg.commas, cfg.pad);
            if (i + 1 < template_lines.size()) text_block += "\\N";
        }

        double t_start = f.timestamp_sec + cfg.start_delay;
        double t_end = (fi + 1 < frames.size()) ? frames[fi + 1].timestamp_sec + cfg.start_delay
                                                 : t_start + 1.0 / cfg.fps;
        ass << "Dialogue: 0," << to_ass_time(t_start) << "," << to_ass_time(t_end)
            << ",Default,,0,0,0,,{\\pos(" << pos_x << "," << pos_y << ")}" << text_block << "\n";
    }
    ass.close();
}

// ---------------------------------------------------------------------------
// FFmpeg invocation (writes batch file to avoid shell escaping issues)
// ---------------------------------------------------------------------------

// ffmpeg's filter arguments are parsed by its own grammar, not the shell's:
// backslashes are consumed as escapes (even inside quotes), apostrophes
// terminate the quoting, and colons must be escaped. A user-derived path can
// therefore never be passed through the subtitles= filter safely. Like the
// original fMCG (which rendered a fixed "temp_stats.ass" from the working
// directory), we move the ASS next to its own directory under a fixed bare
// name and cd there in the batch file, so the filter argument is a simple
// name no parser can mangle. Returns the renamed file's full path.
inline std::string prepare_ass_for_filter(const std::string& ass_filename) {
    size_t sep = ass_filename.find_last_of("/\\");
    std::string dir = (sep != std::string::npos) ? ass_filename.substr(0, sep + 1) : std::string("./");
    std::string safe = dir + "temp_stats.ass";
    std::remove(safe.c_str());
    if (std::rename(ass_filename.c_str(), safe.c_str()) != 0) {
        // rename can fail on exotic filesystem setups; fall back to a copy
        std::ifstream src(ass_filename.c_str(), std::ios::binary);
        if (src) {
            std::ofstream dst(safe.c_str(), std::ios::binary);
            dst << src.rdbuf();
        }
        std::remove(ass_filename.c_str());
    }
    return safe;
}

// Convert an ASS colour (&HAABBGGRR) to ffmpeg's "0xRRGGBB" colour spec for
// the lavfi color source. Falls back to black on malformed input.
inline std::string ffmpeg_color_spec(const std::string& ass_col) {
    if (ass_col.size() >= 8 && ass_col[0] == '&' && (ass_col[1] == 'H' || ass_col[1] == 'h')) {
        std::string hex = ass_col.substr(2);
        if (hex.size() == 8) hex = hex.substr(2);   // drop alpha -> BBGGRR
        if (hex.size() == 6) {
            std::string bb = hex.substr(0, 2), gg = hex.substr(2, 2), rr = hex.substr(4, 2);
            return "0x" + rr + gg + bb;
        }
    }
    return "0x000000";
}

inline int render_video(const std::string& ass_filename, const std::string& output_video,
                         const std::string& midi_dir, const std::string& midi_stem,
                         int width, int height, double fps, double total_duration,
                         const std::string& bg_color_ass = "&H00000000") {
    std::string safe_ass = prepare_ass_for_filter(ass_filename);
    size_t sep = safe_ass.find_last_of("/\\");
    std::string dir_cd = (sep != std::string::npos) ? safe_ass.substr(0, sep) : std::string(".");
    std::string out_fwd = output_video;
    std::replace(out_fwd.begin(), out_fwd.end(), '\\', '/');

    std::string bat_file = midi_dir + midi_stem + "_fMCG_ffmpeg.bat";
    {
        std::ofstream bat(bat_file);
        bat << "@echo off\ncd /d \"" << dir_cd << "\" || exit /b 1\n";
        bat << "ffmpeg -y -f lavfi -i \"color=c=" << ffmpeg_color_spec(bg_color_ass)
            << ":s=" << width << "x" << height
            << ":r=" << fps << ":d=" << total_duration
            << "\" -vf \"subtitles=temp_stats.ass\""
            << " -c:v libx264 -pix_fmt yuv420p \"" << out_fwd << "\"\n";
    }

    std::string cmd = "\"" + bat_file + "\"";
    int ret = std::system(cmd.c_str());

    std::remove(safe_ass.c_str());
    std::remove(ass_filename.c_str());
    std::remove(bat_file.c_str());

    return ret;
}

// ---------------------------------------------------------------------------
// File path helpers
// ---------------------------------------------------------------------------

inline std::string extract_dir(const std::string& path) {
    size_t sep = path.find_last_of("/\\");
    return (sep != std::string::npos) ? path.substr(0, sep + 1) : "./";
}

inline std::string extract_stem(const std::string& path) {
    size_t sep = path.find_last_of("/\\");
    std::string basename = (sep != std::string::npos) ? path.substr(sep + 1) : path;
    size_t dot = basename.find_last_of('.');
    return (dot != std::string::npos) ? basename.substr(0, dot) : basename;
}

inline std::string trim_path(std::string s) {
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
    auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && is_space(s.front())) s.erase(s.begin());
    while (!s.empty() && is_space(s.back())) s.pop_back();
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    while (!s.empty() && is_space(s.front())) s.erase(s.begin());
    while (!s.empty() && is_space(s.back())) s.pop_back();
    return s;
}

inline bool validate_midi(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open file"; return false; }
    char sig[4] = {0};
    f.read(sig, 4);
    f.close();
    if (std::memcmp(sig, "MThd", 4) == 0) return true;
    // Accept compressed archives (7z, xz, rar) — validated by libarchive during decompression
    if (is_compressed_file(path)) return true;
    err = "not a valid MIDI file (missing MThd header)";
    return false;
}
