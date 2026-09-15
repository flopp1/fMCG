#include "fmcg_engine.h"

#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <deque>
#include <memory>
#include <unordered_set>
#include <unordered_map>

#include "fmcg_util.h"
#include "xxhash64.h"   // vendored single-header XXHash64 (Stephan Brumme, MIT)

#include "archive.h"
#include "archive_entry.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#include <time.h>
#endif


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

void emit(const ProgressCallbacks& cb, const std::string& msg, bool is_error = false) {
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
    // Optional capability: a direct pointer to the whole stream image (used by
    // the zero-copy RawStream walk). Only mmap-backed plain files provide it;
    // the default keeps archives/pipes on the chunked ByteStream path.
    virtual const uint8_t* image(size_t& n) { (void)n; return nullptr; }
};

class FileSrc : public DataSrc {
    BinaryReader r;
    static constexpr size_t READAHEAD = 16u << 20;   // 16MB async readahead
    uint64_t prefetched_to = 0;                      // prefetched up to this offset
public:
    explicit FileSrc(const std::string& path) : r(path, 0, /*mmap=*/true) {}
    bool ok() const override { return r.get_file_size() > 0; }
    size_t read(void* dst, size_t n) override {
        // Keep the OS pulling disk sectors ~16MB ahead of the parse position:
        // on a cold HDD this converts the 512KB request pattern into long
        // sequential reads instead of the reader stalling on page faults.
        const uint64_t pos = bytes_read() + n;
        if (pos + READAHEAD > prefetched_to) {
            const uint64_t from = prefetched_to ? prefetched_to : pos;
            r.prefetch((size_t)from, (size_t)((pos + READAHEAD) - from));
            prefetched_to = pos + READAHEAD;
        }
        return r.read_raw(dst, n);
    }
    uint64_t bytes_read() const override { return (uint64_t)r.tell(); }
    const char* kind() const override { return "mmap"; }
    uint64_t total_bytes() const override { return (uint64_t)r.get_file_size(); }
    const uint8_t* image(size_t& n) override {
        n = r.mmap_size();
        return r.mmap_data();
    }
};

class ArchiveSrc : public DataSrc {
public:
    // One layer of the decode stack. An ArchiveFeeder reads entries out of a
    // libarchive container; a DecompFeeder pushes its inner stream through
    // libarchive again (filters + raw format), so compressed-inside-compressed
    // inputs unwrap to arbitrary depth.
    struct Feeder {
        virtual ~Feeder() {}
        virtual size_t pull(void* dst, size_t n) = 0;   // 0 = end of stream
        virtual bool errored() const { return false; }
        // Restart the layer's decode from the stream's beginning.
        virtual void reset() = 0;
    };

    // Reads the selected entry's bytes out of a container via data blocks.
    // If the container path errors out -- the classic case being a filter
    // (xz/gz/zst) wrapped around a seek-dependent container (7z/zip), whose
    // end-of-file header demands a backward seek through forward-only filter
    // output -- it transparently re-opens in RAW mode (filter decode only)
    // and serves the decoded stream, letting the unwrap() layer stack above
    // handle the inner container with its seek-capable decoder.
    class ArchiveFeeder : public Feeder {
    public:
        struct archive* a = nullptr;
        std::vector<uint8_t> staging;   // libarchive's block buffer is invalidated
        size_t s_lo = 0;                // by the next call, so blocks are copied
        bool saw_error = false;

        explicit ArchiveFeeder(const std::string& path) : path_(path) {
            open_container();
        }
        ~ArchiveFeeder() override {
            if (a) archive_read_free(a);
            if (pf) fclose(pf);
        }

        size_t pull(void* dst, size_t n) override {
            uint8_t* d = (uint8_t*)dst;
            size_t out = 0;
            while (out < n) {
                if (s_lo == staging.size()) {
                    const void* buf; size_t bsz; la_int64_t off;
                    int r = a ? archive_read_data_block(a, &buf, &bsz, &off) : ARCHIVE_FATAL;
                    if (r == ARCHIVE_EOF) break;
                    if (r != ARCHIVE_OK) {
                        // Seek-through-filter and friends: retry in raw mode.
                        if (!raw_mode) {
                            reset();
                            open_raw();
                            continue;
                        }
                        saw_error = true; break;
                    }
                    if (bsz == 0) continue;   // zero-size OK blocks occur mid-stream (7z)
                    const uint8_t* p = (const uint8_t*)buf;
                    staging.assign(p, p + bsz);
                    s_lo = 0;
                }
                size_t take = std::min(staging.size() - s_lo, n - out);
                std::memcpy(d + out, staging.data() + s_lo, take);
                s_lo += take;
                out += take;
            }
            return out;
        }
        bool errored() const override { return saw_error; }

        // Restart the layer from the file's beginning in container mode.
        void reset() override {
            saw_error = false;
            raw_mode = false;
            if (pf) { fclose(pf); pf = nullptr; }
            open_container();
        }

    private:
        std::string path_;
        struct archive_entry* entry = nullptr;
        bool raw_mode = false;          // serving filter-decoded bytes only
        FILE* pf = nullptr;

        void open_container() {
            if (a) { archive_read_free(a); a = nullptr; }
            staging.clear(); s_lo = 0;
            a = archive_read_new();
            archive_read_support_filter_all(a);
            archive_read_support_format_all(a);
            archive_read_support_format_raw(a);   // bare filter-compressed files
            if (archive_read_open_filename(a, path_.c_str(), 10240) != ARCHIVE_OK) {
                archive_read_free(a); a = nullptr; return;
            }
            // First header; skip directory entries.
            while (true) {
                if (archive_read_next_header(a, &entry) != ARCHIVE_OK) {
                    archive_read_free(a); a = nullptr; return;
                }
                if (archive_entry_filetype(entry) != AE_IFDIR) break;
            }
            staging.reserve(1 << 20);
        }
        // Re-open serving only the filter's decoded output (no container
        // parsing): the raw reader's "data" entry yields exactly the decoded
        // byte stream, whatever the inner content is. If no filter matches,
        // raw mode simply serves the file bytes unchanged -- which is the
        // correct passthrough for a plain container reached via recursion.
        void open_raw() {
            if (a) { archive_read_free(a); a = nullptr; }
            staging.clear(); s_lo = 0;
            raw_mode = true;
            a = archive_read_new();
            archive_read_support_filter_all(a);
            archive_read_support_format_raw(a);
            if (archive_read_open_filename(a, path_.c_str(), 10240) != ARCHIVE_OK ||
                archive_read_next_header(a, &entry) != ARCHIVE_OK) {
                archive_read_free(a); a = nullptr;
                pf = fopen(path_.c_str(), "rb");   // last resort: copy the file verbatim
            }
        }
    };

    // Streams arbitrary bytes through libarchive decode (all filters, all
    // formats + raw). Inner may itself be a container or a compressed stream.
    // A full history of the consumed stream is buffered so a seek callback can
    // be offered: some inner formats (7z, rar, zip) require seeking into their
    // headers, which a plain unseekable stream cannot serve. The buffer holds
    // this layer's *compressed* bytes, which for nested archives is far
    // smaller than the decompressed payload.
    class DecompFeeder : public Feeder {
    public:
        std::unique_ptr<Feeder> inner;
        struct archive* a = nullptr;
        std::vector<uint8_t> staging;
        size_t s_lo = 0;
        bool saw_error = false;
        size_t prefix_n = 0;         // peeked header bytes prepended to buf
        std::vector<uint8_t> buf;    // full history of the decoded-input stream
        size_t lpos = 0;             // logical position within buf
        bool inner_eof = false;
        std::vector<uint8_t> rb;     // read-callback scratch buffer

        size_t fill_more() {
            if (inner_eof) return 0;
            uint8_t tmp[1 << 18];
            size_t n = inner->pull(tmp, sizeof(tmp));
            buf.insert(buf.end(), tmp, tmp + n);
            return n;
        }

        static la_ssize_t read_cb(struct archive*, void* self, const void** out) {
            auto* f = (DecompFeeder*)self;
            if (f->lpos == f->buf.size()) f->fill_more();
            size_t avail = f->buf.size() - f->lpos;
            size_t n = std::min(avail, f->rb.size());
            if (n) std::memcpy(f->rb.data(), f->buf.data() + f->lpos, n);
            f->lpos += n;
            *out = f->rb.data();
            return (la_ssize_t)n;
        }
        static la_int64_t seek_cb(struct archive*, void* self, la_int64_t off, int whence) {
            auto* f = (DecompFeeder*)self;
            la_int64_t target;
            switch (whence) {
                case SEEK_SET: target = off; break;
                case SEEK_CUR: target = (la_int64_t)f->lpos + off; break;
                case SEEK_END:
                    while (f->fill_more()) {}
                    target = (la_int64_t)f->buf.size() + off; break;
                default: return -1;
            }
            if (target < 0) return -1;
            while ((size_t)target > f->buf.size() && f->fill_more()) {}
            if ((size_t)target > f->buf.size()) { f->lpos = f->buf.size(); return -1; }
            f->lpos = (size_t)target;
            return (la_int64_t)f->lpos;
        }

        explicit DecompFeeder(std::unique_ptr<Feeder> in, const uint8_t* hdr = nullptr, size_t hdr_n = 0)
            : inner(std::move(in)), rb(1 << 18) {
            if (hdr && hdr_n) { buf.assign(hdr, hdr + hdr_n); prefix_n = hdr_n; }
            open_archive();
        }
        ~DecompFeeder() override { if (a) archive_read_free(a); }

        // Restart the layer's decode from its stream's beginning: drop the
        // decoded history, rewind the inner feeder, re-open the decoder.
        void reset() override {
            if (a) { archive_read_free(a); a = nullptr; }
            buf.resize(prefix_n);
            lpos = 0;
            inner_eof = false;
            saw_error = false;
            inner->reset();
            open_archive();
        }

        size_t pull(void* dst, size_t n) override {
            uint8_t* d = (uint8_t*)dst;
            size_t out = 0;
            while (out < n) {
                if (s_lo == staging.size()) {
                    const void* buf; size_t bsz; la_int64_t off;
                    int r = archive_read_data_block(a, &buf, &bsz, &off);
                    if (r == ARCHIVE_EOF) break;
                    if (r != ARCHIVE_OK) { saw_error = true; break; }
                    if (bsz == 0) continue;
                    const uint8_t* p = (const uint8_t*)buf;
                    staging.assign(p, p + bsz);
                    s_lo = 0;
                }
                size_t take = std::min(staging.size() - s_lo, n - out);
                std::memcpy(d + out, staging.data() + s_lo, take);
                s_lo += take;
                out += take;
            }
            return out;
        }
        bool errored() const override { return saw_error || inner->errored(); }

    private:
        struct archive_entry* entry = nullptr;

        void open_archive() {
            a = archive_read_new();
            archive_read_support_filter_all(a);
            archive_read_support_format_all(a);
            archive_read_support_format_raw(a);
            // Callbacks (and their client data) must be registered BEFORE
            // open: archive_read_open1 runs format bidding immediately, and
            // the 7z/rar bidders ask for seekability at bid time.
            archive_read_set_read_callback(a, read_cb);
            archive_read_set_seek_callback(a, seek_cb);
            archive_read_set_callback_data(a, this);
            if (archive_read_open1(a) != ARCHIVE_OK) {
                archive_read_free(a); a = nullptr; return;
            }
            if (archive_read_next_header(a, &entry) != ARCHIVE_OK) {
                archive_read_free(a); a = nullptr; return;
            }
        }
    };

    std::vector<std::unique_ptr<Feeder>> stack;   // outermost first
    std::vector<uint8_t> pending;                 // peeked bytes not yet served
    uint64_t served = 0;
    std::string open_err;
    static constexpr int MAX_LAYERS = 16;

    Feeder* top() { return stack.empty() ? nullptr : stack.back().get(); }

    // Peek the next bytes off the top feeder; if they carry a compression
    // signature, push a decoder layer and repeat. Peeked bytes are kept in
    // `pending` so no data is lost (MIDI never starts with a compression
    // signature, so this terminates at the innermost payload).
    void unwrap() {
        while ((int)stack.size() <= MAX_LAYERS) {
            Feeder* t = top();
            if (!t) return;
            uint8_t hdr[10];
            size_t got = 0;
            while (got < sizeof(hdr)) {
                size_t r = t->pull(hdr + got, sizeof(hdr) - got);
                if (!r) break;
                got += r;
            }
            if (!is_compressed_magic(hdr, got)) {
                // Payload reached; the peeked bytes ARE the first payload bytes.
                pending.insert(pending.end(), hdr, hdr + got);
                return;
            }
            // Compressed layer: the peeked bytes are the header of the stream
            // the new decoder must see -- hand them to it, not to the consumer.
            // Ownership note: pop the old layer FIRST -- emplace_back-then-
            // pop_back would remove the newly pushed layer instead, leaving
            // the moved-from husk on top of the stack.
            auto old_layer = std::move(stack.back());
            stack.pop_back();
            stack.emplace_back(new DecompFeeder(std::move(old_layer), hdr, got));
        }
    }

    explicit ArchiveSrc(const std::string& path) {
        auto f = std::unique_ptr<Feeder>(new ArchiveFeeder(path));
        if (!static_cast<ArchiveFeeder*>(f.get())->a) {
            open_err = "libarchive could not open the file";
            return;   // stack stays empty -> ok() == false
        }
        stack.push_back(std::move(f));
        unwrap();
    }
    bool ok() const override { return !stack.empty(); }
    size_t read(void* dst, size_t n) override {
        uint8_t* d = (uint8_t*)dst;
        size_t out = 0;
        while (out < n) {
            if (!pending.empty()) {
                size_t take = std::min(pending.size(), n - out);
                std::memcpy(d + out, pending.data(), take);
                pending.erase(pending.begin(), pending.begin() + take);
                out += take;
                served += take;
                continue;
            }
            Feeder* t = top();
            if (!t) break;
            size_t r = t->pull(d + out, n - out);
            if (r == 0) break;
            out += r;
            served += r;
        }
        return out;
    }
    const char* kind() const override {
        return stack.size() > 1 ? "libarchive, nested decode" : "libarchive";
    }
    uint64_t bytes_read() const override { return served; }
    bool errored() const override {
        for (auto& f : stack) if (f->errored()) return true;
        return false;
    }
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
    // Bound attribution: how long each side spent waiting on the other.
    // consumer_wait = parser starved (decode/disk could not keep up ->
    // decode-bound); producer_wait = decoder blocked because the parser was
    // still chewing the full queue (-> parser-bound). Stored as nanoseconds
    // in atomics (C++17 has no floating-point fetch_add).
    std::atomic<uint64_t> consumer_wait_ns{0};
    std::atomic<uint64_t> producer_wait_ns{0};

    void produce() {
        std::unique_lock<std::mutex> lk(mu);
        try {
            while (true) {
                lk.unlock();
                uint8_t blk[1 << 16];
                size_t n = arc.read(blk, sizeof(blk));          // decode outside the lock
                lk.lock();
                if (n == 0) { done = true; cv_data.notify_all(); return; }
                q.emplace_back(blk, blk + n);
                q_bytes += n;
                decoded += n;
                cv_data.notify_one();
                if (q_bytes >= Q_CAP && !closed) {
                    const auto p0 = std::chrono::steady_clock::now();
                    while (q_bytes >= Q_CAP && !closed)
                        cv_slot.wait(lk);
                    producer_wait_ns.fetch_add(
                        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - p0).count(),
                        std::memory_order_relaxed);
                }
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
    void get_waits(double& consumer_s, double& producer_s) const {
        consumer_s = (double)consumer_wait_ns.load(std::memory_order_relaxed) * 1e-9;
        producer_s = (double)producer_wait_ns.load(std::memory_order_relaxed) * 1e-9;
    }
    bool is_piped() const { return piped; }

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
                const auto w0 = std::chrono::steady_clock::now();
                cv_data.wait(lk);
                consumer_wait_ns.fetch_add(
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - w0).count(),
                    std::memory_order_relaxed);
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

class alignas(64) ByteStream {
    DataSrc& src;
    static constexpr size_t CAP = 1 << 19;   // 512 KB: fewer fill() round-trips (was 64 KB)
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

    // Absolute offset of the next unread byte within the source stream
    // (used by the parallel scan's prefetcher progress reporting).
    uint64_t stream_off() const { return pulled - (hi - lo); }

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
    // Non-consuming byte read: for running-status lookahead. Same bounds
    // checks as get() but never advances -- no skip_back_one() round-trip.
    int peek() {
        if (limit_remaining == 0) return -1;
        if (lo == hi) { fill(); if (lo == hi) return -1; }
        return data[lo];
    }

    // ---- pointer-batched fast path ---------------------------------------
    // A window into `data` with a guaranteed number of readable bytes,
    // clamped to the active track-chunk limit. The scanner bursts over
    // ordinary channel events straight from this window; commit_window()
    // makes the consumption official. A burst that speculatively parsed an
    // event it does not want to handle simply commits fewer bytes -- the
    // un-handled event stays in the stream for the byte-wise loop. No
    // slow-path call may happen between acquire and commit (it would move
    // lo/hi and invalidate fast_p); the scanner obeys that.
    uint8_t* fast_p = nullptr;

    size_t acquire_window() {
        size_t run = hi - lo;
        if (run < CAP / 2 && !eof) {
            if (lo > 0) {                     // reclaim the drained head
                std::memmove(data, data + lo, run);
                lo = 0; hi = run;
            }
            fill();                           // top up (no-op at EOF)
            run = hi - lo;
        }
        size_t n = run;
        if (limit_remaining < n) n = limit_remaining;
        fast_p = data + lo;
        return n;
    }
    void commit_window(size_t used) {         // used <= acquire_window()'s return
        lo += used;
        limit_remaining -= used;
        fast_p = nullptr;
    }

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
        // Fast path: the common 1-byte delta (zero-delta chains in black MIDIs
        // make this the overwhelmingly dominant case). Peak >= 4 buffered
        // bytes allows an unconditional 4-byte decode with no per-byte limit
        // checks (same shape as cobalt/cpp-midi's varlen_decode).
        if (limit_remaining > 0) {
            if (lo == hi) { fill(); if (lo == hi) return 0; }
            size_t buffered = hi - lo;
            if (buffered >= 4 && limit_remaining >= 4) {
                const uint8_t* p = data + lo;
                uint32_t b0 = *p++;
                if (__builtin_expect(b0 < 0x80, 1)) { lo++; limit_remaining--; return b0; }
                uint64_t v = b0 & 0x7F;
                uint32_t b1 = *p++;
                v = (v << 7) | (b1 & 0x7F);
                if (b1 < 0x80) { lo = (size_t)(p - data); limit_remaining -= 2; return v; }
                uint32_t b2 = *p++;
                v = (v << 7) | (b2 & 0x7F);
                if (b2 < 0x80) { lo = (size_t)(p - data); limit_remaining -= 3; return v; }
                uint32_t b3 = *p++;
                v = (v << 7) | (b3 & 0x7F);
                lo = (size_t)(p - data);
                limit_remaining -= 4;
                return v;
            }
        }
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

// ---- zero-copy stream over an in-memory image (raw mmap walk) ----------------
//
// Same interface as ByteStream, but the "window" is the mapping itself: no
// ring buffer, no memcpy, no virtual read() per fill. Plain mmap-backed files
// scan straight over the image; everything else (archives, pipes, failed
// mmaps) keeps the chunked ByteStream. Limits and EOF semantics mirror
// ByteStream exactly so the scanner bodies behave identically.
class RawStream {
    const uint8_t* data_;
    size_t len_;
    size_t lo_ = 0;
    size_t limit_remaining_ = (size_t)-1;
    size_t limit_total_ = 0;
public:
    explicit RawStream(const uint8_t* d, size_t n)
        : data_(d), len_(n), limit_remaining_(n), limit_total_(n) {}

    uint8_t* fast_p = nullptr;

    uint64_t bytes_pulled() const { return lo_; }
    void set_limit(size_t n) { limit_remaining_ = n; limit_total_ = n; }
    size_t consumed() const { return limit_total_ - std::min(limit_total_, limit_remaining_); }

    size_t avail() {
        size_t a = len_ - lo_;
        if (a > limit_remaining_) a = limit_remaining_;
        return a;
    }
    bool at_end() { return avail() == 0; }

    // Absolute offset of the next unread byte within the underlying image
    // (used by the parallel scan to publish prefetcher progress).
    size_t stream_off() const { return lo_; }

    int get() {
        if (limit_remaining_ == 0 || lo_ >= len_) return -1;
        limit_remaining_--;
        return data_[lo_++];
    }
    int peek() {
        if (limit_remaining_ == 0 || lo_ >= len_) return -1;
        return data_[lo_];
    }

    void skip(size_t n) {
        if (n > limit_remaining_) n = limit_remaining_;
        if (n > len_ - lo_) n = len_ - lo_;
        lo_ += n;
        limit_remaining_ -= n;
    }

    size_t read(void* dst, size_t n) {   // interface parity (not on the hot path)
        if (n > limit_remaining_) n = limit_remaining_;
        if (n > len_ - lo_) n = len_ - lo_;
        std::memcpy(dst, data_ + lo_, n);
        lo_ += n;
        limit_remaining_ -= n;
        return n;
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

    // Whole remaining track (clamped to the declared chunk limit) in one
    // window: the scanner bursts across it exactly as over ByteStream's ring.
    size_t acquire_window() {
        size_t run = len_ - lo_;
        if (run > limit_remaining_) run = limit_remaining_;
        fast_p = const_cast<uint8_t*>(data_ + lo_);
        return run;
    }
    void commit_window(size_t used) {
        lo_ += used;
        limit_remaining_ -= used;
        fast_p = nullptr;
    }
};

// ---- accumulation targets ---------------------------------------------------


// ---- track deduplication (optional fast path) --------------------------------
//
// Black-MIDI stress files are routinely built by concatenating one base
// MIDI's track bytes N times and then compressing. Most of the decoded stream
// is then byte-identical track content, and parsing every copy wastes work
// that produces exactly the same per-tick deltas. With dedup enabled, a track
// whose raw bytes match a summary recorded earlier in the same scan is not
// parsed at all: its cached per-tick deltas, CC counts and tempo events are
// replayed directly into the shared arrays.
//
// Correctness rests on two invariants:
//   1. A track's parse is a pure function of its bytes with empty per-track
//      state (refcount, running status) and tick origin 0 -- so identical
//      bytes always produce identical summaries.
//   2. All MIDI tracks play simultaneously: every track's tick 0 IS the
//      song's tick 0. There is no inter-track offset to apply, so replay
//      merges the cached per-tick deltas at face value.
// Tracks that end with held notes (end_refcount > 0) are excluded from
// replay: their net deltas depend on when the note-offs land, which the
// summary records only as a final-tick delta. Excluding them is always
// safe; the cache hit just misses.
//
// Archival sources are forward-only and have no image, so the sequential
// dedup path reads each chunk through the stream into a byte buffer, hashes
// the buffer, and parses misses FROM THE BUFFER (via RawStream) -- guaranteeing
// the hash covers exactly the bytes the parse sees. The parallel path hashes
// directly from the mmap image (zero copy) and parses from the image as
// usual. The plain-file sequential path also uses the read-through buffer:
// one chunk buffer at a time, freed after each track (dedup-only cost).
struct TrackSummary {
    uint64_t bytes = 0;          // raw track byte count (with hash)
    uint64_t hash = 0;           // 64-bit FNV-1a over the track's bytes
    uint64_t ons = 0, cc = 0;
    uint64_t max_tick = 0;       // tick of the track's final event
    uint32_t end_refcount = 0;   // notes still held at the track's final tick
    uint64_t events = 0;         // events walked for this track
    bool desync = false;         // source parse stopped before the chunk end
    // Per-tick (ons, net delta) exactly as the pending-register flush would
    // commit them -- ons drive nps/cumulative counts, delta drives polyphony.
    std::vector<std::pair<uint64_t, std::pair<uint32_t, int32_t>>> cells;
    std::vector<std::pair<uint64_t, uint32_t>> ccs;     // (tick, cc count)
    std::vector<std::pair<uint64_t, uint32_t>> tempos;  // (tick, us_per_quarter)
};

struct TrackDataDedup {
    // Shared store (parallel workers merge into it under g_dedup_mutex).
    // shared_ptr keeps replays stable while the store grows.
    std::vector<std::shared_ptr<const TrackSummary>> records;
    std::atomic<uint64_t> savings_events{0};   // events in replayed (not parsed) tracks
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    // Approximate bytes held by stored summaries. Guards against pathological
    // growth on unique-content files: past the budget, new records are simply
    // not cached (existing ones keep replaying; misses just parse normally).
    std::atomic<uint64_t> stored_bytes{0};
    static constexpr uint64_t kStoreBudget = (uint64_t)512 << 20;   // 512 MiB
    // Candidate tracks larger than this parse plain instead of being buffered
    // whole: buffering a tens-of-GB track (trillion-class files) both risks
    // RAM exhaustion and freezes live progress for the entire read.
    static constexpr uint64_t kMaxCandidateBytes = (uint64_t)1 << 30;   // 1 GiB

    void reset() {
        records.clear();
        savings_events.store(0, std::memory_order_relaxed);
        hits.store(0, std::memory_order_relaxed);
        misses.store(0, std::memory_order_relaxed);
        stored_bytes.store(0, std::memory_order_relaxed);
    }
};

// 64-bit FNV-1a over a byte range (the track's raw bytes).
// Track-content hash for dedup lookups. XXHash64 (vendored single header,
// MIT) — byte-at-a-time FNV-1a measured slower than the parser itself on
// large tracks and dominated dedup-mode scan time; XXHash64 processes 32
// bytes per round with a short dependency chain and removes that ceiling.
static uint64_t dedup_hash_track(const uint8_t* p, size_t n,
                                 const std::atomic<bool>* cancelled = nullptr) {
    // 64 MiB chunks with cancel polls between: hashing a tens-of-GB track in
    // one call made cancel unresponsive for the whole hash (~5s+ at 10GB/s).
    constexpr size_t kHashChunk = (size_t)64 << 20;
    XXHash64 hh(0);
    for (size_t off = 0; off < n; off += kHashChunk) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) return 0;
        hh.add(p + off, std::min(kHashChunk, n - off));
    }
    return hh.hash();
}

// Chunk size for the sequential dedup candidate read: the read is chunked so
// a cancel flag is polled between chunks (a single multi-GB read made the
// cancel button unresponsive for the whole decode of that chunk).
static constexpr size_t kDedupReadChunk = 4u << 20;   // 4 MiB

// Shared dedup store lock. Lookups take it too: a parallel worker's records
// merge mutates the vector, so unsynchronized iteration would be UB. One
// lock per track is negligible next to the parse itself.
static std::mutex g_dedup_mutex;

// Look up a cacheable summary for (len, hash). Returns true when `out` was
// populated with a REPLAYABLE record (end_refcount == 0).
static bool dedup_find_cachable(const TrackDataDedup& store, uint64_t len,
                                uint64_t hash, std::shared_ptr<const TrackSummary>& out) {
    std::lock_guard<std::mutex> lk(g_dedup_mutex);
    for (const auto& r : store.records)
        if (r->bytes == len && r->hash == hash && r->end_refcount == 0) { out = r; return true; }
    return false;
}

// Aborted via a cancel poll (scan loop or frame sweep).
struct ScanCancelled {};

struct TickData {
    // One interleaved cell per tick: a note-on touches ons and delta of the
    // SAME tick, so keeping them in one struct halves the cache/TLB misses
    // versus two separate multi-GB arrays (the sweep then reads each tick's
    // whole cell in one line too).
    struct TickCell { uint32_t ons; int32_t delta; };
    static_assert(sizeof(TickCell) == 8, "TickCell must stay tightly packed");
    std::vector<TickCell> cells;         // per-tick (>=2^31 ons on ONE tick is impossible in practice: a note-on is >=2 bytes, so it needs ~4 GB of pure 0x9x pairs on a single tick)
    uint64_t max_tick = 0;
    uint64_t total_ons = 0;
    size_t ntracks = 0;
    size_t desync_tracks = 0;
    uint64_t total_events_seen = 0;   // every event walked (ons+offs+CC+meta), matches live counter
    bool dedup_enabled = false;       // dedup fast path active for this scan
    TrackDataDedup dedup;             // optional per-scan track-content cache

    void ensure(uint64_t t) {
        if (t >= cells.size()) {
            size_t n = cells.size();
            if (n == 0) n = 4096;
            while (n <= t) n *= 2;
            cells.resize(n, TickCell{0, 0});
        }
    }
    void note_on(uint64_t t) { ensure(t); cells[t].ons++; cells[t].delta++; total_ons++; }
    void delta_at(uint64_t t, int64_t d) { ensure(t); cells[t].delta += (int32_t)d; }
    // CC accumulation (always counted; the {cc} stats read it).
    std::vector<uint32_t> dense_cc;      // per-tick control-change counts
    uint64_t total_cc = 0;
    void cc_at(uint64_t t) {
        ensure(t);
        if (dense_cc.size() < cells.size()) dense_cc.resize(cells.size(), 0);
        dense_cc[t]++;
        total_cc++;
    }
    // Current footprint of the dense arrays (bytes).
    size_t memory_bytes() const {
        return cells.size() * sizeof(TickCell)
             + dense_cc.size() * sizeof(uint32_t);
    }
    void reset() {
        std::vector<TickCell>().swap(cells);
        std::vector<uint32_t>().swap(dense_cc);
        max_tick = 0; total_ons = 0; ntracks = 0; desync_tracks = 0;
        total_events_seen = 0; total_cc = 0;
        dedup.reset();
    }
};


// Append-or-accumulate helper for the summaries' per-tick lists (ticks are
// visited in increasing order within a track, so the tail is the target).
static void sum_add(std::vector<std::pair<uint64_t, int32_t>>& v, uint64_t t, int32_t d) {
    if (!v.empty() && v.back().first == t) v.back().second += d;
    else v.push_back({t, d});
}
static void sum_add(std::vector<std::pair<uint64_t, uint32_t>>& v, uint64_t t, uint32_t d) {
    if (!v.empty() && v.back().first == t) v.back().second += d;
    else v.push_back({t, d});
}

// Replay a cached (end_refcount == 0) summary into `td`. In the parallel scan
// `td` is the worker's PRIVATE replay buffer (replays are never written to the
// shared arrays concurrently); the sequential scan passes the real TickData.
// NOTE: does not touch td.ntracks / td.total_events_seen -- callers account
// those (they may need to merge or defer).
static void replay_track_summary(const TrackSummary& s, TickData& td,
                                 std::vector<TempoChange>& tempo_raw) {
    for (const auto& c : s.cells) {
        td.ensure(c.first);
        td.cells[c.first].ons   += c.second.first;
        td.cells[c.first].delta += c.second.second;
    }
    for (const auto& c : s.ccs) {
        td.ensure(c.first);
        if (td.dense_cc.size() < td.cells.size()) td.dense_cc.resize(td.cells.size(), 0);
        td.dense_cc[c.first] += c.second;
    }
    for (const auto& tc : s.tempos)
        tempo_raw.push_back({tc.first, 0.0, tc.second, 60000000.0 / tc.second});
    td.total_ons += s.ons;
    td.total_cc  += s.cc;
    if (s.max_tick > td.max_tick) td.max_tick = s.max_tick;
    if (s.desync) td.desync_tracks++;
}

// Merge a worker's summary list into the shared store under a mutex. Hit
// store keeps only unique (bytes, hash) keys, so hit lookups stay O(unique
// tracks) -- normally one or two entries even for tens of thousands of copies.
// Records ending with held notes are dropped: they can never be replayed.
static void dedup_merge_records(std::vector<TrackSummary>& mine, TickData& td) {
    std::lock_guard<std::mutex> lk(g_dedup_mutex);
    for (auto& s : mine) {
        if (s.end_refcount) continue;
        bool known = false;
        for (const auto& r : td.dedup.records)
            if (r->bytes == s.bytes && r->hash == s.hash) { known = true; break; }
        if (!known) td.dedup.records.push_back(std::make_shared<TrackSummary>(std::move(s)));
    }
    mine.clear();
}

// Approximate resident size of a summary (the per-tick lists dominate).
static uint64_t summary_bytes(const TrackSummary& s) {
    return sizeof(TrackSummary)
         + s.cells.capacity() * sizeof(s.cells[0])
         + s.ccs.capacity()    * sizeof(s.ccs[0])
         + s.tempos.capacity() * sizeof(s.tempos[0]);
}

// Store one freshly parsed summary immediately (parallel workers call this
// per track so other workers can hit the record while the scan is running).
// Subject to the store budget so unique-content giant files cannot balloon.
static void dedup_store_record(TrackSummary& s, TrackDataDedup& store) {
    if (s.end_refcount) return;
    const uint64_t sz = summary_bytes(s);
    std::lock_guard<std::mutex> lk(g_dedup_mutex);
    for (const auto& r : store.records)
        if (r->bytes == s.bytes && r->hash == s.hash) return;
    if (store.stored_bytes.load(std::memory_order_relaxed) + sz > TrackDataDedup::kStoreBudget)
        return;
    store.records.push_back(std::make_shared<TrackSummary>(std::move(s)));
    store.stored_bytes.fetch_add(sz, std::memory_order_relaxed);
}

// ---- single sequential walk over the image -----------------------------------

// Result of a single walk over the image.
enum class ScanMode { ACCUMULATE, TEMPO_ONLY };
// Aborted via the MIDI-spec guard; the caller restarts the walk in TEMPO_ONLY
// mode (two-pass fallback for spec-breaking tick spans).
struct SpecAbort {};

// ---- per-track parser core (shared by every scan path) -----------------------
//
// Parses ONE MTrk chunk from `bs` (whose limit is already set to the chunk
// length). This is the extracted heart of the scanner -- the pending-register
// hot path, the pointer-batched burst loop, the byte-wise slow path and the
// end-of-track held-note closure -- shared verbatim by:
//   - scan_stream's chunk loop  (ByteStream over archives, RawStream over
//     plain mmap'd files, and RawStream over a dedup candidate's byte buffer)
//   - parse_track_range         (the parallel per-track scan)
//
// `hooks.spec_fire` implements the caller's spec-violation protocol: return
// true to continue, false to stop the track silently (the caller reports the
// cancel / performs the two-pass restart).
//
// When `sum` is non-null (dedup enabled, ACCUMULATE mode) the track's
// contribution is recorded into it: per-tick net deltas, CC counts, tempo
// events, totals and the final tick. The caller fills `sum.bytes/.hash` --
// for the sequential walk the hash stays 0 (records never matched, see the
// dedup block comment); only the parallel and archive paths match hashes.
// A truncated/desynced track yields bytes < the chunk length, so its record
// can never produce a false hit on a full-length candidate. The CALLER fills
// rec.bytes/.hash/.events (it owns the chunk length and the returned count).
// Returns the number of events walked in this track (including early exits).
struct TrackBodyHooks {
    // Optional absolute-position reporting for the parallel scan's
    // prefetcher: at each 1M-event ping the body stores `pos_base +
    // current stream offset` into *pos_out, so a worker grinding through a
    // multi-GB track still advances its published position (track-start-only
    // updates froze the prefetcher cap and reintroduced cold faults).
    std::atomic<uint64_t>* pos_out = nullptr;
    uint64_t  pos_base = 0;

    // Fires once per ~1M events with the DELTA since the previous ping
    // (always exactly 1,000,000 today; the final partial count reaches the
    // caller via parse_track_body's return value instead).
    std::function<void(uint64_t)> ping;
    std::function<bool()>         spec_fire;         // spec guard (see above)
    // Parallel scan only: per-worker live event counter. Pings add their
    // 1M-event deltas; the per-track remainder is added when the track
    // completes, so the sum across workers is the exact walked total and a
    // reporter thread can show live ev/s while the workers run.
    std::atomic<uint64_t>* ev_pub = nullptr;
    // Parallel scan: shared one-shot warning flag + shared cancel flag, both
    // null on the sequential paths (which use the local bool / cb only).
    std::atomic<bool>* data_oor_atomic = nullptr;
    const std::atomic<bool>* cancelled_flag = nullptr;
};

template <typename StreamT>
static uint64_t parse_track_body(StreamT& bs, bool accumulate, bool vel0_as_note_off,
                                 TickData& td, std::vector<TempoChange>& tempo_raw,
                                 const ProgressCallbacks& cb, uint64_t spec_limit,
                                 const TrackBodyHooks& hooks,
                                 bool& data_oor, TrackSummary* sum) {
    const bool count_cc = accumulate;
    auto note_data_oor = [&]() {
        if (hooks.data_oor_atomic) {
            if (!hooks.data_oor_atomic->exchange(true, std::memory_order_acq_rel))
                emit(cb, "  Warning: file contains note data bytes above 127 (invalid MIDI); "
                         "they are clamped to 127. Stats for those events may be approximate.\n", true);
            return;
        }
        if (!data_oor) {
            emit(cb, "  Warning: file contains note data bytes above 127 (invalid MIDI); "
                     "they are clamped to 127. Stats for those events may be approximate.\n", true);
            data_oor = true;
        }
    };
    auto stop_cancelled = [&]() {
        if (cb.cancel_flag && cb.cancel_flag->load(std::memory_order_relaxed)) return true;
        if (hooks.cancelled_flag && hooks.cancelled_flag->load(std::memory_order_relaxed)) return true;
        return false;
    };
    uint64_t tick = 0;
    uint8_t running = 0;
    uint32_t refcount[16][128] = {};   // black-MIDI tracks hold >255 overlapping instances of one note

    // ---- pending-tick registers (hot-path optimisation) ----------------
    // Black MIDIs stack thousands of events on the SAME tick via zero-delta
    // chains. Instead of a read-modify-write into the multi-GB cells[]
    // array per event, events accumulate in these register locals and are
    // committed to cells[] once, when the walk moves past the tick (or the
    // track ends). Cost drops from per-event memory ops to per-tick.
    bool     has_pending = false;
    uint64_t pend_tick = 0;
    uint32_t pend_ons = 0, pend_cc = 0;
    int32_t  pend_delta = 0;
    TrackSummary rec;                       // summary under construction
    uint64_t track_ev = 0;                  // events walked in THIS track (monotonic)
    uint64_t ping_at = 1000000;             // next event count that fires a ping
    uint64_t ping_last = 0;                 // track_ev at the previous ping
    auto flush_pending = [&]() {
        if (!has_pending) return;
        if (sum && (pend_delta || pend_ons || pend_cc)) {
            if (pend_ons || pend_delta) {
                if (!rec.cells.empty() && rec.cells.back().first == pend_tick) {
                    rec.cells.back().second.first  += pend_ons;
                    rec.cells.back().second.second += pend_delta;
                } else {
                    rec.cells.push_back({pend_tick, {pend_ons, pend_delta}});
                }
                rec.ons += pend_ons;
            }
            if (pend_cc)  { sum_add(rec.ccs, pend_tick, pend_cc); rec.cc += pend_cc; }
        }
        if (!accumulate) { has_pending = false; pend_ons = 0; pend_delta = 0; pend_cc = 0; return; }
        td.ensure(pend_tick);
        if (pend_ons)   td.cells[pend_tick].ons   += pend_ons;
        if (pend_delta) td.cells[pend_tick].delta += pend_delta;
        if (pend_cc) {
            if (td.dense_cc.size() < td.cells.size()) td.dense_cc.resize(td.cells.size(), 0);
            td.dense_cc[pend_tick] += pend_cc;
            td.total_cc += pend_cc;
        }
        td.total_ons += pend_ons;
        has_pending = false; pend_ons = 0; pend_delta = 0; pend_cc = 0;
    };

    while (bs.at_end() == false) {
        tick += bs.vlq();
        if (has_pending && tick != pend_tick) flush_pending();   // commit before the tick advances
        // MIDI spec guard: delta times are at most 28-bit VLQs, so a tick
        // beyond 1<<28 cannot be represented within the spec. At that
        // point the per-tick arrays also grow toward gigabytes. Fire the
        // callback once (proceed / restart-in-2-pass / cancel); without a
        // callback we proceed (CLI/tests, historic behaviour).
        if (accumulate && tick > spec_limit) {
            if (!hooks.spec_fire()) return track_ev;
        }
#ifndef FMCG_NO_BURST
        // ---- pointer-batched fast path --------------------------------
        // Consumes a run of ordinary two-data-byte channel events chained
        // by zero deltas, straight from the stream's own buffer (bounds
        // and state checks once per burst instead of per byte). Deltas --
        // including the zero ones chaining the run -- always stay in the
        // stream: this block never touches `tick`; the byte-wise code
        // below remains the sole owner of time and of every edge case.
        // The run stops at the first event it cannot fully classify
        // (meta/system family, 1-data-byte message, or a window tail
        // without both data bytes); that event is then handled by the
        // normal slow path via fall-through.
        bool burst_at_event = false;   // stopped on an unclassified event?
        {
            size_t win = bs.acquire_window();
            const uint8_t* p = bs.fast_p;
            const uint8_t* const pend = bs.fast_p + win;
            while (p < pend) {
                const uint8_t* q = p;                  // classification cursor
                uint8_t status;
                bool explicit_status = false;
                if (__builtin_expect(*q < 0x80, 1)) {
                    status = running;                  // running status
                } else {
                    status = *q++;                     // explicit status byte
                    explicit_status = true;
                }
                if (__builtin_expect(status < 0x80, 0)) { burst_at_event = true; break; }   // running == 0
                const uint8_t et = status & 0xF0;
                if (__builtin_expect(et == 0xC0 || et == 0xD0, 0)) { burst_at_event = true; break; }
                if (__builtin_expect(et != 0x80 && et != 0x90 && et != 0xA0 && et != 0xB0 && et != 0xE0, 0)) {
                    burst_at_event = true; break;      // meta/system family
                }
                if (pend - q < 2) { burst_at_event = true; break; }    // both data bytes must be in-window
                uint8_t n1 = *q++;
                const uint8_t n2 = *q++;
                p = q;                                 // event fully consumed
                if (explicit_status) running = status; // 0xFx never reaches here
                if (n1 > 0x7F) { note_data_oor(); n1 &= 0x7F; }   // invalid data byte: clamp (protects refcount[16][128])
                if (et == 0x90 && (n2 > 0 || !vel0_as_note_off)) {
                    refcount[status & 0x0F][n1]++;
                    if (accumulate) { pend_tick = tick; has_pending = true; pend_ons++; pend_delta++; }
                } else if (et == 0x90 || et == 0x80) {
                    if (refcount[status & 0x0F][n1] > 0) {
                        refcount[status & 0x0F][n1]--;
                        if (accumulate) { pend_tick = tick; has_pending = true; pend_delta--; }
                    }
                } else if (et == 0xB0 && count_cc) {
                    pend_tick = tick; has_pending = true; pend_cc++;
                }
                // 0xA0/0xE0 and uncounted CC consume 3 bytes, touch nothing.
                if (++track_ev >= ping_at) {
                    if (hooks.pos_out)   // prefetcher position: stream offset now
                        hooks.pos_out->store(hooks.pos_base + bs.stream_off(),
                                             std::memory_order_relaxed);
                    hooks.ping(track_ev - ping_last);
                    ping_last = track_ev;
                    ping_at = track_ev + 1000000;
                    if (stop_cancelled()) {
                        bs.commit_window(p - bs.fast_p);
                        return track_ev;   // cancelled; caller reports
                    }
                }
                if (p == pend) break;                  // window boundary: delta beyond it
                if (*p != 0) break;                    // nonzero delta: loop top reads it
                ++p;                                   // zero delta: consume it and chain
                if (p == pend) {                       // delta eaten but its event is beyond
                    burst_at_event = true;             // the window: hand the event to the
                    break;                             // slow path (a zero delta adds no tick)
                }
            }
            bs.commit_window(p - bs.fast_p);
        }
        if (!burst_at_event) continue;   // stream sits at a delta: back to loop top
        // Fall through: the event at the stream head goes through the slow path.
#endif   // FMCG_NO_BURST
        // Peek the next byte: if it is a data byte, this event uses the
        // held running status and the byte stays in the stream (it will
        // be re-read below as data). No unread round-trip needed.
        int st = bs.peek();
        if (st < 0) break;
        uint8_t status;
        if (__builtin_expect(st < 0x80, 1)) {
            status = running;
        } else {
            bs.get();   // consume the status byte
            status = (uint8_t)st;
            running = (status < 0xF0) ? status : 0;
        }

        if (__builtin_expect(status >= 0xF0, 0)) {
            // Rare system/meta family, kept out of the channel hot path.
            if (status == 0xFF) {
                int type = bs.get();
                if (type < 0) break;
                uint64_t mlen = bs.vlq();
                if (type == 0x51 && mlen == 3 && bs.avail() >= 3) {
                    uint32_t us = ((uint32_t)bs.get() << 16) | ((uint32_t)bs.get() << 8) | (uint32_t)bs.get();
                    if (us > 0) {
                        tempo_raw.push_back({tick, 0.0, us, 60000000.0 / us});
                        if (sum) rec.tempos.push_back({tick, us});
                    }
                } else {
                    bs.skip((size_t)mlen);
                }
            } else if (status == 0xF0 || status == 0xF7) {
                bs.skip((size_t)bs.vlq());
            } else if (status >= 0xF1 && status <= 0xF6) {
                if (status == 0xF1 || status == 0xF3) bs.skip(1);
                else if (status == 0xF2) bs.skip(2);
            }
            // status < 0x80 handled below (running==0 fall-through)
        } else if (status >= 0x80) {
            uint8_t et = status & 0xF0;
            int n1 = bs.get();
            if (n1 < 0) break;
            if (et != 0xC0 && et != 0xD0) {
                int n2 = bs.get();
                if (n2 < 0) break;
                uint8_t ch = status & 0x0F, note = (uint8_t)n1, vel = (uint8_t)n2;
                if (note > 0x7F) { note_data_oor(); note &= 0x7F; }   // invalid data byte: clamp (protects refcount[16][128])
                if (et == 0x90 && (vel > 0 || !vel0_as_note_off)) {
                    refcount[ch][note]++;
                    if (accumulate) { pend_tick = tick; has_pending = true; pend_ons++; pend_delta++; }
                } else if (et == 0x90 || et == 0x80) {
                    if (refcount[ch][note] > 0) { refcount[ch][note]--;                             if (accumulate) { pend_tick = tick; has_pending = true; pend_delta--; } }
                } else if (et == 0xB0 && count_cc) {
                                            pend_tick = tick; has_pending = true; pend_cc++;   // control change: counted only when the CC stat is on
                }
            }
        }
        // status < 0x80 with running==0: consume nothing (matches parallel engine)

        // Continuous progress ping: a counter compare on the hot path, real
        // work (clock + callback) only once per ~1M events.
        if (++track_ev >= ping_at) {
            if (hooks.pos_out)   // prefetcher position: stream offset now
                hooks.pos_out->store(hooks.pos_base + bs.stream_off(),
                                     std::memory_order_relaxed);
            hooks.ping(track_ev - ping_last);
            ping_last = track_ev;
            ping_at = track_ev + 1000000;
            if (stop_cancelled()) {
                return track_ev;
            }
        }
    }

    // Close held notes at the track's final tick (parallel-engine end-of-track flush).
    flush_pending();   // commit the final tick's pending events first
    uint32_t rc_sum = 0;
    for (int ch = 0; ch < 16; ++ch)
        for (int n = 0; n < 128; ++n)
            if (refcount[ch][n] > 0) {
                rc_sum += refcount[ch][n];
                if (accumulate) td.delta_at(tick, -(int64_t)refcount[ch][n]);
            }
    if (accumulate && tick > td.max_tick) td.max_tick = tick;
    if (sum) {
        if (rc_sum) {
            if (!rec.cells.empty() && rec.cells.back().first == tick)
                rec.cells.back().second.second -= (int32_t)rc_sum;
            else
                rec.cells.push_back({tick, {0u, -(int32_t)rc_sum}});
        }
        rec.end_refcount = rc_sum;
        rec.max_tick = tick;
        const uint64_t bytes = sum->bytes, hash = sum->hash;   // caller-provided identity
        *sum = std::move(rec);
        sum->bytes = bytes;
        sum->hash  = hash;
    }
    return track_ev;
}

// Forward declarations: the scan bodies are stream-generic (templated over
// ByteStream or RawStream) and defined further below, after FrameBuckets.
template <typename StreamT>
static bool scan_stream(StreamT& bs, DataSrc& src, bool vel0_as_note_off, TickData& td,
                        std::vector<TempoChange>& tempo_raw, uint16_t& out_division,
                        const ProgressCallbacks& cb, ScanMode mode);

bool scan_image(DataSrc& src, bool vel0_as_note_off, TickData& td,
                std::vector<TempoChange>& tempo_raw, uint16_t& out_division,
                const ProgressCallbacks& cb, ScanMode mode = ScanMode::ACCUMULATE) {
    ByteStream bs(src);
    return scan_stream(bs, src, vel0_as_note_off, td, tempo_raw, out_division, cb, mode);
}

// ---- single sequential walk over the image (stream-generic) ------------------
template <typename StreamT>
static bool scan_stream(StreamT& bs, DataSrc& src, bool vel0_as_note_off, TickData& td,
                        std::vector<TempoChange>& tempo_raw, uint16_t& out_division,
                        const ProgressCallbacks& cb, ScanMode mode) {
    const bool accumulate = (mode == ScanMode::ACCUMULATE);
    // In TEMPO_ONLY mode the accumulation arrays are never touched (that is
    // the point) and CC counting is meaningless.
    const bool count_cc = accumulate;
    if (!accumulate) td.reset();

    // Continuous progress bookkeeping (checked once per ~1M events).
    using clock = std::chrono::steady_clock;
    const clock::time_point t_start = clock::now();
    uint64_t ev_count = 0;      // authoritative: full counts of completed tracks
    uint64_t ev_pinged = 0;     // live display: pinged prefix of the current track
    const uint64_t total_bytes = src.total_bytes();   // 0 = unknown fraction
    const uint64_t spec_limit = cb.spec_tick_limit ? cb.spec_tick_limit : ((uint64_t)1 << 28);
    bool spec_warned = false;              // one-shot: ask at most once per scan
    bool data_oor = false;                 // one-shot: out-of-range data bytes seen
    auto note_data_oor = [&]() {
        if (!data_oor) {
            emit(cb, "  Warning: file contains note data bytes above 127 (invalid MIDI); "
                     "they are clamped to 127. Stats for those events may be approximate.\n", true);
            data_oor = true;
        }
    };

    auto ping = [&]() {
        if (!cb.on_scan_progress) return;
        double el = std::chrono::duration<double>(clock::now() - t_start).count();
        double frac = total_bytes > 0 ? (double)bs.bytes_pulled() / (double)total_bytes : -1.0;
        cb.on_scan_progress(ev_count + ev_pinged, el,
                            el > 0.0 ? (double)(ev_count + ev_pinged) / el : 0.0, frac);
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

    // Dedup protocol state. `dedup_store` is a non-owning pointer into td
    // (reset() clears it) so the replay helper can consult the cache and the
    // final summary can merge its records.
    TrackDataDedup* dedup_store = nullptr;
    std::vector<TrackSummary> dedup_records_mine;   // merged after the walk
    uint64_t dedup_replayed_events = 0;             // events from replays (not walked)
    if (accumulate && td.dedup_enabled) {
        dedup_store = &td.dedup;
        emit(cb, "  Track dedup enabled.\n");
    }
    auto dedup_replay = [&](const TrackSummary& s) {
        replay_track_summary(s, td, tempo_raw);
        td.dedup.hits.fetch_add(1, std::memory_order_relaxed);
        td.dedup.savings_events.fetch_add(s.events, std::memory_order_relaxed);
        dedup_replayed_events += s.events;
        td.ntracks++;
    };
    // One reusable read buffer for all dedup candidate chunks. A per-track
    // vector zero-initialises its whole length (a full extra pass over the
    // chunk) and re-walks the OS zero-page machinery on every fresh multi-GB
    // allocation; reserve-once + manual length avoids both. Measured on a
    // ~1 GB/s 7z decode: this buffer alone was most of the dedup overhead.
    std::vector<uint8_t> dedup_buf;
    // First-occurrence gate: a replay needs byte-identical content, hence an
    // identical declared chunk length, so the first track of any length can
    // never hit. Lengths recorded as "seen" (including desynced/uncachable
    // tracks) skip the candidate copy, hash, and summary entirely -- historic
    // plain-parse behaviour -- collapsing the dedup overhead on files whose
    // tracks are mostly unique (e.g. a 4.24B-event file saving 1.7%).
    std::unordered_set<uint64_t> seen_len;

    while (true) {
        if (cb.cancel_flag && cb.cancel_flag->load(std::memory_order_relaxed)) {
            emit(cb, "  Cancelled.\n"); throw ScanCancelled{};
        }
        int64_t tag = rd32();
        if (tag < 0) break;
        int64_t len = rd32();
        if (len < 0) { emit(cb, "Error: truncated chunk header.\n", true); return false; }
        if (tag != 0x4D54726B) { bs.skip((size_t)len); continue; }

        bs.set_limit((size_t)len);

        // ---- dedup candidate path ---------------------------------------
        // Read the whole chunk into a buffer, hash it, and either replay a
        // cached summary or parse from the buffer. The hash therefore covers
        // exactly the bytes the parse sees, and a replay consumes the same
        // number of stream bytes as a parse would. Without dedup the parse
        // streams directly from `bs` as before (zero copies).
        if (dedup_store) {
            // First occurrence of this declared length can never replay
            // (nothing with this length exists in the store yet): parse
            // straight from the stream, exactly like dedup-off.
            if (seen_len.insert((uint64_t)len).second) {
                TrackBodyHooks hooks;
                hooks.ping = [&](uint64_t n) {
                    ev_pinged += n;
                    ping();
                };
                hooks.spec_fire = [&]() -> bool {
                    if (spec_warned) return true;
                    const int choice = cb.on_spec_violation
                        ? cb.on_spec_violation(0, td.memory_bytes()) : 0;
                    if (choice == 2) { emit(cb, "  Cancelled.\n"); throw ScanCancelled{}; }
                    if (choice == 1) throw SpecAbort{};
                    spec_warned = true;
                    return true;
                };
                ev_count += parse_track_body(bs, accumulate, vel0_as_note_off, td, tempo_raw,
                                             cb, spec_limit, hooks, data_oor, nullptr);
                ev_pinged = 0;
                td.ntracks++;
                const size_t consumed = bs.consumed();
                bs.set_limit((size_t)-1);
                if (consumed < (size_t)len) {
                    td.desync_tracks++;
                    bs.skip((size_t)len - consumed);   // jump to next chunk boundary
                }
                continue;
            }
            // Repeat length: a genuine replay candidate -- unless it is so
            // large that buffering it could exhaust RAM and that hashing it
            // stalls live progress for the whole read. Above the cap the
            // track parses straight from the stream, byte-for-byte the same
            // as dedup-off (correctness is unaffected: it just never replays).
            if ((uint64_t)len > TrackDataDedup::kMaxCandidateBytes) {
                TrackBodyHooks hooks;
                hooks.ping = [&](uint64_t n) {
                    ev_pinged += n;
                    ping();
                };
                hooks.spec_fire = [&]() -> bool {
                    if (spec_warned) return true;
                    const int choice = cb.on_spec_violation
                        ? cb.on_spec_violation(0, td.memory_bytes()) : 0;
                    if (choice == 2) { emit(cb, "  Cancelled.\n"); throw ScanCancelled{}; }
                    if (choice == 1) throw SpecAbort{};
                    spec_warned = true;
                    return true;
                };
                ev_count += parse_track_body(bs, accumulate, vel0_as_note_off, td, tempo_raw,
                                             cb, spec_limit, hooks, data_oor, nullptr);
                ev_pinged = 0;
                td.ntracks++;
                const size_t consumed = bs.consumed();
                bs.set_limit((size_t)-1);
                if (consumed < (size_t)len) {
                    td.desync_tracks++;
                    bs.skip((size_t)len - consumed);
                }
                continue;
            }
            // Read the whole chunk into a reusable buffer, hashing as we go.
            // The read is chunked so cancellation stays responsive even on
            // multi-GB tracks (a single blocking read of a 2 GB track left
            // the cancel button dead for the whole decode of that chunk).
            // Only `got` bytes are valid; the reserve capacity never is.
            if (dedup_buf.size() < (size_t)len) dedup_buf.reserve((size_t)len);
            uint8_t* bufp = dedup_buf.data();
            size_t got = 0;
            XXHash64 hh(0);
            bool cancelled_read = false;
            while (got < (size_t)len) {
                if (cb.cancel_flag && cb.cancel_flag->load(std::memory_order_relaxed)) {
                    cancelled_read = true;
                    break;
                }
                const size_t want = std::min(kDedupReadChunk, (size_t)len - got);
                const size_t n = bs.read(bufp + got, want);
                if (n == 0) break;             // stream ended early
                hh.add(bufp + got, n);
                got += n;
                // Live progress during the candidate read: without a ping,
                // stats freeze for the whole read (tens of GB at decode
                // speed on trillion-class tracks), looking like a hang.
                ev_pinged += (n >> 9);   // approximate: ~1 ping-unit per 512B
                ping();
            }
            if (cancelled_read) { emit(cb, "  Cancelled.\n"); throw ScanCancelled{}; }
            const uint64_t hash = hh.hash();
            std::shared_ptr<const TrackSummary> hit;
            if (got == (size_t)len && dedup_find_cachable(*dedup_store, (uint64_t)len, hash, hit)) {
                dedup_replay(*hit);
            } else {
                TrackSummary rec;
                rec.bytes = got;
                rec.hash  = hash;
                bool data_oor_local = data_oor;
                RawStream tbs(bufp, got);
                TrackBodyHooks hooks;
                hooks.ping = [&](uint64_t n) {
                    // Live counter from the pinged prefix; the authoritative
                    // walked total comes from the body's return value per
                    // track (accumulating both double-counted every pinged
                    // event).
                    ev_pinged += n;
                    ping();
                };
                hooks.spec_fire = [&]() -> bool {
                    if (spec_warned) return true;
                    const int choice = cb.on_spec_violation
                        ? cb.on_spec_violation(0, td.memory_bytes()) : 0;
                    if (choice == 2) { emit(cb, "  Cancelled.\n"); throw ScanCancelled{}; }
                    if (choice == 1) throw SpecAbort{};
                    spec_warned = true;
                    return true;
                };
                rec.events = parse_track_body(tbs, accumulate, vel0_as_note_off, td, tempo_raw,
                                              cb, spec_limit, hooks, data_oor_local, &rec);
                ev_count += rec.events;   // walked events: same accounting as the ordinary path
                ev_pinged = 0;            // this track's prefix is now in ev_count
                data_oor = data_oor_local;
                td.ntracks++;
                const size_t consumed = (size_t)tbs.consumed();
                if (consumed < (size_t)got) td.desync_tracks++;
                // Store immediately: later tracks in this same walk must be
                // able to hit the record (the whole point of the cache).
                dedup_store_record(rec, *dedup_store);
                td.dedup.misses.fetch_add(1, std::memory_order_relaxed);
            }
            bs.set_limit((size_t)-1);
            continue;
        }

        // ---- ordinary parse path ----------------------------------------
        // TEMPO_ONLY passes accumulate=false: the body then walks the grammar
        // for tempo capture only (the shared body's `accumulate` flag keeps
        // every array write out).
        {
            TrackBodyHooks hooks;
            hooks.ping = [&](uint64_t n) {
                ev_pinged += n;
                ping();
            };
            hooks.spec_fire = [&]() -> bool {
                if (spec_warned) return true;
                const int choice = cb.on_spec_violation
                    ? cb.on_spec_violation(0, td.memory_bytes()) : 0;
                if (choice == 2) { emit(cb, "  Cancelled.\n"); throw ScanCancelled{}; }
                if (choice == 1) throw SpecAbort{};
                spec_warned = true;
                return true;
            };
            ev_count += parse_track_body(bs, accumulate, vel0_as_note_off, td, tempo_raw,
                                         cb, spec_limit, hooks, data_oor, nullptr);
            ev_pinged = 0;   // this track's prefix is now in ev_count
        }
        td.ntracks++;
        const size_t consumed = bs.consumed();
        bs.set_limit((size_t)-1);
        if (consumed < (size_t)len) {
            td.desync_tracks++;
            bs.skip((size_t)len - consumed);   // jump to next chunk boundary
        }
    }

    // Final progress ping so elapsed/ev-per-s cover the whole scan.
    ping();
    td.total_events_seen = ev_count + dedup_replayed_events;
    if (dedup_store && !dedup_records_mine.empty())
        dedup_merge_records(dedup_records_mine, td);
    if (dedup_store) {
        const uint64_t hits = td.dedup.hits.load(std::memory_order_relaxed);
        const uint64_t misses = td.dedup.misses.load(std::memory_order_relaxed);
        const uint64_t saved = td.dedup.savings_events.load(std::memory_order_relaxed);
        if (hits || misses)
            emit(cb, "  Dedup: " + std::to_string(hits) + " track(s) replayed, "
                     + std::to_string(misses) + " parsed, "
                     + std::to_string(saved) + " events skipped.\n");
    }
    return true;
}

// ---- anchor-interpolated sweep: tick buckets -> FrameStats --------------------

// {plph-max} and {nps-max} are both EXACT in the single-pass engine, no bins
// involved. The sweep walks every tick in strictly increasing time order and a
// tick is the finest time unit MIDI has (same-tick events are simultaneous by
// definition), so:
//   - peak polyphony = running max of the net-per-tick polyphony state;
//   - peak NPS       = max note-ons in any literal 1.000000-second window,
//     maintained with a sliding-window deque over (tick_time, ons) -- entries
//     are distinct note-carrying ticks, so its size scales with ticks-per-
//     second, not events-per-second (a million-note tick is one entry).
// Window convention: half-open (t-1, t] -- an onset exactly 1.000s older than
// the newest one is excluded (window span is exactly 1.0s).

// Peak NPS in the TWO-PASS fallback instead uses bins of width
// 1/(FMCG_FINE_BINS*fps) seconds (see FrameBuckets below): that architecture
// never materializes tick space, so the deque cannot be built there and the
// bin-quantized window is the accepted approximation.

// Build the tempo map exactly like the parallel engine: default entry,
// stable sort by tick, dedup keeping the LAST change at each tick, then
// anchor times. Shared by the tick-space sweep and the frame-bucket path.
static std::vector<TempoChange> build_tempo_map(const std::vector<TempoChange>& tempo_raw,
                                                uint16_t ppqn) {
    std::vector<TempoChange> tm;
    tm.reserve(tempo_raw.size() + 1);
    tm.push_back({0, 0.0, 500000, 120.0});
    for (const auto& tc : tempo_raw) if (tc.us_per_quarter > 0) tm.push_back(tc);
    std::stable_sort(tm.begin(), tm.end(),
                     [](const TempoChange& a, const TempoChange& b) { return a.tick < b.tick; });
    size_t w = 0;
    for (size_t i = 0; i < tm.size(); ++i)
        if (i + 1 == tm.size() || tm[i + 1].tick != tm[i].tick) tm[w++] = tm[i];
    tm.resize(w);
    for (size_t i = 1; i < tm.size(); ++i) {
        uint64_t dtk = tm[i].tick - tm[i - 1].tick;
        tm[i].time_sec = tm[i - 1].time_sec
                       + (double)dtk * (double)tm[i - 1].us_per_quarter / ((double)ppqn * 1e6);
    }
    return tm;
}

std::vector<FrameStats> sweep_to_frames(TickData& td,
                                               std::vector<TempoChange>& tempo_raw,
                                               uint16_t ppqn, double fps, bool with_cc = false,
                                               double end_delay = 0.0,
                                               const ProgressCallbacks* cb = nullptr) {
    std::vector<TempoChange> tm = build_tempo_map(tempo_raw, ppqn);

    const uint64_t nticks = td.max_tick + 1;
    std::vector<TickData::TickCell>& cells = td.cells;
    // Track tails can extend past the last written tick; size arrays to cover
    // the whole tick span (also covers the no-events-at-all case).
    if (cells.size() < nticks)
        cells.resize((size_t)nticks, TickData::TickCell{0, 0});
    if (with_cc && td.dense_cc.size() < nticks)
        td.dense_cc.resize((size_t)nticks, 0);   // dense_cc is sized to last-written tick only
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
    size_t song_frames = SIZE_MAX;   // set before the tail loop; frames >= it rest at zero

    // Tick-space inversion shared by the frame push: anchor walked by time.
    size_t iconv = 0;                       // anchor for sec->tick conversion
    auto tick_at = [&](double sec) -> uint64_t {
        while (iconv + 1 < tm.size() && tm[iconv + 1].time_sec <= sec) iconv++;
        return tm[iconv].tick + (uint64_t)std::max(0.0, (sec - tm[iconv].time_sec)
                    * ((double)ppqn * 1e6) / (double)tm[iconv].us_per_quarter);
    };

    // ---- exact peak-NPS sliding window -----------------------------------
    // Monotonic deque of (time_sec, ons) for the distinct note-carrying ticks
    // currently inside the last literal 1.000000 seconds. The window sum is
    // sampled exactly where a new maximum can occur: right after each onset
    // tick. Half-open (t-1, t] convention: an onset exactly 1.000s older is
    // excluded, so the window span is exactly 1.0s at any fps.
    std::deque<std::pair<double, uint64_t>> nps_window;
    uint64_t wsum = 0;               // note-ons inside the 1s window
    uint64_t peak_nps_exact = 0;     // exact maximum (monotonic)

    auto push_frame = [&](size_t k) {
        // Frame-end tempo: a tempo change inside frame k must show from k on
        // (the very last tempo is never hidden by the frame boundary).
        while (bpmw + 1 < tm.size() && tm[bpmw + 1].time_sec <= (double)(k + 1) / fps) bpmw++;
        uint64_t prev_cum = (k >= W) ? out[k - W].cumulative_notes : 0;
        double nps = (double)(cum - prev_cum);
        int64_t cur_poly = poly;
        if (k >= song_frames) {          // end-delay tail: all event activity
            nps = 0;                     // is over -- nps and polyphony drop
            cur_poly = 0;                // to their resting values
        } else {
            peak_nps = std::max(peak_nps, (double)peak_nps_exact);
            // peak_poly is maintained exactly per tick in the tick walk;
            // already monotonic and covers every event of frames < tail.
        }
        out.push_back({k, (double)k / fps, cum, cum_cc, nps, peak_nps,
                       std::max<int64_t>(0, cur_poly), peak_poly, tm[bpmw].bpm, (int64_t)tick_at((double)k / fps)});
    };

    for (uint64_t t = 0; t < nticks; ++t) {
        // The sweep can run long after the parse (spec-breaking tick spans),
        // and until now it was fully uncancellable: the parse's per-1M-event
        // polls were done, so cancel did nothing until the whole sweep
        // finished. Poll every 16M ticks (~every 64MB of cell data, a check
        // each few ms) and also emit a live ping so the GUI's bar reflects
        // sweep progress instead of freezing at 100% of the scan.
        if (cb && (t & 0xFFFFFF) == 0) {
            if (cb->cancel_flag && cb->cancel_flag->load(std::memory_order_relaxed)) {
                emit(*cb, "  Cancelled.\n");
                throw ScanCancelled{};
            }
            if (cb->on_scan_progress && (t & 0x0FFFFFF) == 0)
                cb->on_scan_progress(td.total_events_seen, 0.0, 0.0,
                                     (double)t / (double)std::max<uint64_t>(nticks, 1));
            if (cb->on_progress)
                cb->on_progress("Sweeping", (int)(100.0 * (double)t / (double)std::max<uint64_t>(nticks, 1)));
        }
        const TickData::TickCell cell = td.cells[(size_t)t];
        uint64_t o = cell.ons;
        int64_t  d = cell.delta;
        uint64_t c = with_cc ? td.dense_cc[(size_t)t] : 0;
        if (o == 0 && d == 0) {
            if (c) cum_cc += c;   // CC-only tick: accumulate, no frame needed
            continue;
        }

        while (conv + 1 < tm.size() && tm[conv + 1].tick <= t) conv++;
        double sec = tm[conv].time_sec + (double)(t - tm[conv].tick) * (double)tm[conv].us_per_quarter * inv;
        size_t fi = (size_t)(sec * fps);

        // Frames between the last processed tick and this one are event-free;
        // their end-of-frame state is the current running state.
        while (cur_frame < fi) push_frame(cur_frame++);

        // Exact peak NPS: fold this tick's note-ons into the 1s window.
        if (o > 0) {
            nps_window.emplace_back(sec, o);
            wsum += o;
            while (!nps_window.empty() && nps_window.front().first <= sec - 1.0) {
                wsum -= nps_window.front().second;
                nps_window.pop_front();
            }
            if (wsum > peak_nps_exact) peak_nps_exact = wsum;
        }

        cum += o;
        poly += d;
        if (poly > peak_poly) peak_poly = poly;   // exact per-tick peak
        if (c) cum_cc += c;
    }

    // Determine total frames from sec(max_tick) (catch up conversion anchors first).
    while (conv + 1 < tm.size() && tm[conv + 1].tick <= td.max_tick) conv++;
    double max_time = tm[conv].time_sec
                    + (double)(td.max_tick - tm[conv].tick) * (double)tm[conv].us_per_quarter * inv;
    song_frames = (size_t)std::ceil(max_time * fps) + 1;   // tail frames (>= this) rest at zero
    // End-delay tail: extra frames past the song's end. Notes/poly/NPS are at
    // their resting values (final polyphony -- 0 for a clean ending); time
    // keeps counting and {tick} advances at the last tempo via tick_at.
    if (end_delay > 0.0) max_time += end_delay;
    size_t total_frames = (size_t)std::ceil(max_time * fps) + 1;
    while (cur_frame < total_frames) push_frame(cur_frame++);

    return out;
}

// ---- two-pass fallback: tempo map, then frame-bucketed accumulation ---------
// For MIDI files whose tick span breaks the spec (a 28-bit VLQ delta caps
// ticks at 1<<28). Pass 1 collects only the tempo map (no accumulation, so
// memory stays O(tempo events)); pass 2 re-walks and, with the COMPLETE tempo
// map available up front, buckets every event straight into the video frame
// it lands in via the anchor-interpolated tick->second map. Memory becomes
// O(frames) instead of O(ticks) -- a ~1000x reduction on extreme files -- at
// the cost of a second decode+parse walk over the stream.

// Sub-frame peak resolution for the fallback: note events are accumulated
// into bins of width 1/(FMCG_FINE_BINS*fps) seconds. The single-pass engine
// computes exact peaks (deque/per-tick, see top of file); the bin-quantized
// window here is the accepted approximation for this low-memory path.
static constexpr int FMCG_FINE_BINS = 8;

struct FrameBuckets {
    std::vector<uint32_t> ons, cc;
    std::vector<int32_t> deltas;   // signed: transient negatives must not wrap
    // Fine-grid mirrors (FMCG_FINE_BINS bins per frame) feeding the sub-frame
    // peak computation, identical to the single-pass engine's.
    std::vector<uint32_t> fons;
    std::vector<int32_t>  ffdeltas;
    size_t nbins = 0;
    uint64_t total_ons = 0, total_cc = 0;
    uint64_t total_events_seen = 0;   // one walk's worth, same metric as the pings
    uint64_t max_tick = 0;            // highest tick seen (song-horizon source)
    size_t nframes = 0;

    void ensure(size_t fi) {
        if (fi >= nframes) {
            // Direct extent (plus geometric headroom), not pure doubling: a
            // huge frame index made every intermediate doubling re-copy the
            // live prefix, visibly halting the scan once per step.
            size_t n = nframes ? nframes * 2 : 4096;
            if (n < fi + 1) n = fi + 1;
            ons.resize(n, 0); deltas.resize(n, 0);
            if (!cc.empty()) cc.resize(n, 0);
            nframes = n;
        }
    }
    void fine_ensure(size_t bin) {
        if (bin >= fons.size()) {
            size_t n = fons.size() ? fons.size() * 2 : 4096;
            if (n < bin + 1) n = bin + 1;
            fons.resize(n, 0); ffdeltas.resize(n, 0);
            nbins = n;
        }
    }
    // Bucket growth is on demand; guarantee capacity for the final sizing.
    void reserve_exact(size_t n) {
        ons.resize(n, 0); deltas.resize(n, 0);
        if (!cc.empty()) cc.resize(n, 0);
        if (nframes < n) nframes = n;
        fons.resize(n * FMCG_FINE_BINS, 0); ffdeltas.resize(n * FMCG_FINE_BINS, 0);
        if (nbins < n * FMCG_FINE_BINS) nbins = n * FMCG_FINE_BINS;
    }
    // Note events are addressed by FINE bin; the frame index is bin/8.
    void note_on(size_t bin)  { size_t fi = bin / FMCG_FINE_BINS; ensure(fi); ons[fi]++; deltas[fi]++; total_ons++; fine_ensure(bin); fons[bin]++; ffdeltas[bin]++; }
    void note_off(size_t bin) { size_t fi = bin / FMCG_FINE_BINS; ensure(fi); deltas[fi]--;           fine_ensure(bin); ffdeltas[bin]--; }
    void add_cc(size_t fi)   { ensure(fi); cc[fi]++;     total_cc++; }
};

// Pass 2 walk: same event grammar as scan_image, but events land in frame
// buckets through the precomputed tempo map. Tick-space is never materialized.
template <typename StreamT>
static bool scan_frames_stream(StreamT& bs, DataSrc& src, bool vel0_as_note_off, FrameBuckets& fb,
                               const std::vector<TempoChange>& tm, uint16_t ppqn, double fps,
                               const ProgressCallbacks& cb) {
    const bool count_cc = true;

    using clock = std::chrono::steady_clock;
    const clock::time_point t_start = clock::now();
    uint64_t ev_total = 0, ev_count = 0;
    const uint64_t total_bytes = src.total_bytes();

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
    int f2 = bs.get(), f1 = bs.get();
    int n2 = bs.get(), n1 = bs.get();
    int d2 = bs.get(), d1 = bs.get();
    if (f2 < 0 || f1 < 0 || n2 < 0 || n1 < 0 || d2 < 0 || d1 < 0) {
        emit(cb, "Error: truncated MIDI header.\n", true); return false;
    }
    if (hlen > 6) bs.skip((size_t)hlen - 6);

    const double inv = 1.0 / ((double)ppqn * 1e6);
    size_t conv = 0;   // tempo anchor walked forward with the events

    // Segment state hoisted out of the hot path: the tempo segment changes
    // rarely, so its fields are loaded into locals only when conv advances
    // (instead of re-reading tm[conv] members per event). The frame formula
    // is kept character-for-character identical to the original
    //   sec = tm[conv].time_sec + (t - tm[conv].tick)*us*inv; f = sec*fps
    // so results are bit-identical and frame boundaries cannot shift.
    double seg_time = tm.empty() ? 0.0 : tm[0].time_sec;
    double seg_us   = tm.empty() ? 0.0 : (double)tm[0].us_per_quarter;
    uint64_t tick_base = tm.empty() ? 0ull : tm[0].tick;

    auto advance_conv = [&](uint64_t t) {
        bool moved = false;
        while (conv + 1 < tm.size() && tm[conv + 1].tick <= t) { ++conv; moved = true; }
        if (moved) {
            seg_time  = tm[conv].time_sec;
            seg_us    = (double)tm[conv].us_per_quarter;
            tick_base = tm[conv].tick;
        }
    };

    // Maps a tick to its frame index using the current segment; advance_conv
    // must already have covered the tick (called in lockstep with the walk).
    auto frame_of = [&](uint64_t t) -> size_t {
        const double sec = seg_time + (double)(t - tick_base) * seg_us * inv;
        double f = sec * fps;
        return f <= 0.0 ? 0 : (size_t)f;
    };
    // Fine bin for note events (sub-frame peak resolution; bin/8 = frame).
    auto fine_of = [&](uint64_t t) -> size_t {
        const double sec = seg_time + (double)(t - tick_base) * seg_us * inv;
        double f = sec * fps * (double)FMCG_FINE_BINS;
        return f <= 0.0 ? 0 : (size_t)f;
    };

    while (true) {
        if (cb.cancel_flag && cb.cancel_flag->load(std::memory_order_relaxed)) {
            emit(cb, "  Cancelled.\n"); throw ScanCancelled{};
        }
        int64_t tag = rd32();
        if (tag < 0) break;
        int64_t len = rd32();
        if (len < 0) { emit(cb, "Error: truncated chunk header.\n", true); return false; }
        if (tag != 0x4D54726B) { bs.skip((size_t)len); continue; }

        bs.set_limit((size_t)len);
        uint64_t tick = 0;
        uint8_t running = 0;
        uint32_t refcount[16][128] = {};
        // Tick restarts at 0 for each track: rewind the tempo anchor and its
        // hoisted segment state with it.
        conv = 0;
        seg_time  = tm.empty() ? 0.0 : tm[0].time_sec;
        seg_us    = tm.empty() ? 0.0 : (double)tm[0].us_per_quarter;
        tick_base = tm.empty() ? 0ull : tm[0].tick;

        while (bs.at_end() == false) {
            tick += bs.vlq();
            advance_conv(tick);
#ifndef FMCG_NO_BURST
            // ---- pointer-batched fast path --------------------------------
            // Consumes a run of ordinary two-data-byte channel events chained
            // by zero deltas, straight from the stream's own buffer (bounds
            // and state checks once per burst instead of per byte). Deltas --
            // including the zero ones chaining the run -- always stay in the
            // stream: this block never touches `tick`; the byte-wise code
            // below remains the sole owner of time and of every edge case.
            // The run stops at the first event it cannot fully classify
            // (meta/system family, 1-data-byte message, or a window tail
            // without both data bytes); that event is then handled by the
            // normal slow path via fall-through.
            bool burst_at_event = false;   // stopped on an unclassified event?
            {
                size_t win = bs.acquire_window();
                const uint8_t* p = bs.fast_p;
                const uint8_t* const pend = bs.fast_p + win;
                while (p < pend) {
                    const uint8_t* q = p;                  // classification cursor
                    uint8_t status;
                    bool explicit_status = false;
                    if (__builtin_expect(*q < 0x80, 1)) {
                        status = running;                  // running status
                    } else {
                        status = *q++;                     // explicit status byte
                        explicit_status = true;
                    }
                    if (__builtin_expect(status < 0x80, 0)) { burst_at_event = true; break; }   // running == 0
                    const uint8_t et = status & 0xF0;
                    if (__builtin_expect(et == 0xC0 || et == 0xD0, 0)) { burst_at_event = true; break; }
                    if (__builtin_expect(et != 0x80 && et != 0x90 && et != 0xA0 && et != 0xB0 && et != 0xE0, 0)) {
                        burst_at_event = true; break;      // meta/system family
                    }
                    if (pend - q < 2) { burst_at_event = true; break; }    // both data bytes must be in-window
                    uint8_t n1 = *q++;
                    const uint8_t n2 = *q++;
                    p = q;                                 // event fully consumed
                    if (explicit_status) running = status; // 0xFx never reaches here
                    if (n1 > 0x7F) n1 &= 0x7F;   // invalid data byte: clamp (protects refcount[16][128])
                    if (et == 0x90 && (n2 > 0 || !vel0_as_note_off)) {
                        refcount[status & 0x0F][n1]++;
                        fb.note_on(fine_of(tick));
                    } else if (et == 0x90 || et == 0x80) {
                        if (refcount[status & 0x0F][n1] > 0) {
                            refcount[status & 0x0F][n1]--;
                            fb.note_off(fine_of(tick));
                        }
                    } else if (et == 0xB0 && count_cc) {
                        fb.add_cc(frame_of(tick));
                    }
                    // 0xA0/0xE0 and uncounted CC consume 3 bytes, touch nothing.
                    if (++ev_count >= 1000000) {
                        ev_count = 0;
                        ev_total += 1000000;
                        ping();
                        if (cb.cancel_flag && cb.cancel_flag->load()) {
                            bs.commit_window(p - bs.fast_p);
                            emit(cb, "  Cancelled.\n");
                            return false;
                        }
                    }
                    if (p == pend) break;                  // window boundary: delta beyond it
                    if (*p != 0) break;                    // nonzero delta: loop top reads it
                    ++p;                                   // zero delta: consume it and chain
                    if (p == pend) {                       // delta eaten but its event is beyond
                        burst_at_event = true;             // the window: hand the event to the
                        break;                             // slow path (a zero delta adds no tick)
                    }
                }
                bs.commit_window(p - bs.fast_p);
            }
            if (!burst_at_event) continue;   // stream sits at a delta: back to loop top
            // Fall through: the event at the stream head goes through the slow path.
#endif   // FMCG_NO_BURST
            // Peek-based running-status handling (mirrors scan_image).
            int st = bs.peek();
            if (st < 0) break;
            uint8_t status;
            if (st < 0x80) { status = running; }
            else { bs.get(); status = (uint8_t)st; running = (status < 0xF0) ? status : 0; }

            if (status >= 0xF0) {
                if (status == 0xFF) {
                    int type = bs.get();
                    if (type < 0) break;
                    uint64_t mlen = bs.vlq();
                    bs.skip((size_t)mlen);   // tempo map is already complete
                } else if (status == 0xF0 || status == 0xF7) {
                    bs.skip((size_t)bs.vlq());
                } else if (status >= 0xF1 && status <= 0xF6) {
                    if (status == 0xF1 || status == 0xF3) bs.skip(1);
                    else if (status == 0xF2) bs.skip(2);
                }
            } else if (status >= 0x80) {
                uint8_t et = status & 0xF0;
                int n1e = bs.get();
                if (n1e < 0) break;
                if (et != 0xC0 && et != 0xD0) {
                    int n2e = bs.get();
                    if (n2e < 0) break;
                    uint8_t ch = status & 0x0F, note = (uint8_t)n1e, vel = (uint8_t)n2e;
                    if (note > 0x7F) note &= 0x7F;   // invalid data byte: clamp (protects refcount[16][128])
                    if (et == 0x90 && (vel > 0 || !vel0_as_note_off)) {
                        refcount[ch][note]++;
                        fb.note_on(fine_of(tick));
                    } else if (et == 0x90 || et == 0x80) {
                        if (refcount[ch][note] > 0) { refcount[ch][note]--;                             fb.note_off(fine_of(tick)); }
                    } else if (et == 0xB0 && count_cc) {
                        fb.add_cc(frame_of(tick));
                    }
                }
            }

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

        // Held notes close at the track's final tick.
        for (int ch = 0; ch < 16; ++ch)
            for (int n = 0; n < 128; ++n)
                if (refcount[ch][n] > 0)                             fb.note_off(fine_of(tick));
        if (tick > fb.max_tick) fb.max_tick = tick;

        size_t consumed = bs.consumed();
        bs.set_limit((size_t)-1);
        if (consumed < (size_t)len) bs.skip((size_t)len - consumed);
    }

    ping();
    fb.total_events_seen = ev_total + ev_count;   // identical metric to the live pings
    return true;
}

// ---- dispatch: raw mmap walk when the source exposes an image -----------------

static bool scan_any(DataSrc& src, bool vel0_as_note_off, TickData& td,
                     std::vector<TempoChange>& tempo_raw, uint16_t& out_division,
                     const ProgressCallbacks& cb, ScanMode mode = ScanMode::ACCUMULATE) {
    size_t n = 0;
    if (const uint8_t* img = src.image(n); img && n > 0) {
        RawStream bs(img, n);
        return scan_stream(bs, src, vel0_as_note_off, td, tempo_raw, out_division, cb, mode);
    }
    ByteStream bs(src);
    return scan_stream(bs, src, vel0_as_note_off, td, tempo_raw, out_division, cb, mode);
}

static bool scan_any_frames(DataSrc& src, bool vel0_as_note_off, FrameBuckets& fb,
                            const std::vector<TempoChange>& tm, uint16_t ppqn, double fps,
                            const ProgressCallbacks& cb) {
    size_t n = 0;
    if (const uint8_t* img = src.image(n); img && n > 0) {
        RawStream bs(img, n);
        return scan_frames_stream(bs, src, vel0_as_note_off, fb, tm, ppqn, fps, cb);
    }
    ByteStream bs(src);
    return scan_frames_stream(bs, src, vel0_as_note_off, fb, tm, ppqn, fps, cb);
}

// Convert frame buckets to FrameStats. Pass-1-style semantics: running sums
// and extrema accumulate in frame order (buckets already hold exactly the
// per-frame values the sweep would have produced).
std::vector<FrameStats> buckets_to_frames(FrameBuckets& fb, double fps,
                                          const std::vector<TempoChange>& tm,
                                          double max_time_sec, bool with_cc, uint16_t ppqn,
                                          double end_delay = 0.0,
                                          const ProgressCallbacks* cb = nullptr) {
    if (end_delay > 0.0) max_time_sec += end_delay;   // frozen-stats tail frames
    const size_t total_frames = (size_t)std::ceil(max_time_sec * fps) + 1;
    const size_t song_frames = (size_t)std::ceil((end_delay > 0.0
        ? max_time_sec - end_delay : max_time_sec) * fps) + 1;
    fb.reserve_exact(total_frames);   // grow (or zero-extend) to the horizon

    std::vector<FrameStats> out;
    out.reserve(total_frames);
    uint64_t cum = 0, cum_cc = 0;
    int64_t poly = 0, peak_poly = 0;
    double peak_nps = 0.0;
    size_t bpmw = 0;
    const size_t W = (size_t)std::round(fps);

    // Sub-frame peaks from the fine-grid buckets -- same sliding window and
    // prefix sums as sweep_to_frames, so both engines agree.
    const size_t wlen = W * FMCG_FINE_BINS;
    size_t fine_next = 0;
    uint64_t wsum = 0;
    int64_t  psum = 0;
    uint32_t run_nps = 0;
    int64_t  run_poly = 0;

    // Tick-space inversion, same as sweep_to_frames: anchor walked by time.
    size_t iconv = 0;
    auto tick_at = [&](double sec) -> uint64_t {
        while (iconv + 1 < tm.size() && tm[iconv + 1].time_sec <= sec) iconv++;
        return tm[iconv].tick + (uint64_t)std::max(0.0, (sec - tm[iconv].time_sec)
                    * ((double)ppqn * 1e6) / (double)tm[iconv].us_per_quarter);
    };

    for (size_t k = 0; k < total_frames; ++k) {
        // Same cancel/progress rationale as sweep_to_frames: pass 2 can run
        // long after the parse and was fully uncancellable until now.
        if (cb && (k & 0xFFFFF) == 0) {
            if (cb->cancel_flag && cb->cancel_flag->load(std::memory_order_relaxed)) {
                emit(*cb, "  Cancelled.\n");
                throw ScanCancelled{};
            }
            if (cb->on_scan_progress)
                cb->on_scan_progress(fb.total_events_seen, 0.0, 0.0,
                                     (double)k / (double)std::max<size_t>(total_frames, 1));
            if (cb->on_progress)
                cb->on_progress("Sweeping", (int)(100.0 * (double)k / (double)std::max<size_t>(total_frames, 1)));
        }
        // Frame-end tempo: a tempo change inside frame k must show from k on.
        while (bpmw + 1 < tm.size() && tm[bpmw + 1].time_sec <= (double)(k + 1) / fps) bpmw++;
        cum  += fb.ons[k];
        poly += fb.deltas[k];
        if (with_cc) cum_cc += fb.cc[k];
        size_t cend = (k + 1) * FMCG_FINE_BINS;
        if (cend > fb.nbins) cend = fb.nbins;
        for (; fine_next < cend; ++fine_next) {
            wsum += fb.fons[fine_next];
            if (fine_next >= wlen) wsum -= fb.fons[fine_next - wlen];
            if (wsum > run_nps) run_nps = (uint32_t)wsum;
            psum += fb.ffdeltas[fine_next];
            if (psum > run_poly) run_poly = psum;
        }
        uint64_t prev_cum = (k >= W) ? out[k - W].cumulative_notes : 0;
        double nps = (double)(cum - prev_cum);
        int64_t cur_poly = poly;
        if (k >= song_frames) {          // end-delay tail: all event activity
            nps = 0;                     // is over -- nps and polyphony drop
            cur_poly = 0;                // to their resting values
        } else {
            peak_nps = std::max(peak_nps, (double)run_nps);
            peak_poly = std::max(peak_poly, run_poly);
        }
        out.push_back({k, (double)k / fps, cum, cum_cc, nps, peak_nps,
                       std::max<int64_t>(0, cur_poly), peak_poly, tm[bpmw].bpm, (int64_t)tick_at((double)k / fps)});
    }
    return out;
}

// ---- per-track parallel scan (plain mmap'd files only) -----------------------
//
// The scan body itself is sequential and stream-generic; its ACCUMULATE work
// is, however, commutative per tick: every track's contribution is a sum of
// +1/-1 deltas and counters into cells[]/dense_cc[], and per-track state
// (refcount, running status, pending registers) is entirely local. So tracks
// can parse independently into private partial TickData and merge in any
// order. Only plain files qualify: they expose the whole image through
// DataSrc::image() with per-track (offset, length) readable from the chunk
// headers, and the mmap stays shared-read-only across threads. Compressed
// streams (libarchive) cannot split and keep the sequential walk.

#include <algorithm>

// Result of locating every MTrk chunk in an image.
struct TrackSpan { uint64_t off, len; };

// Ask the OS to bring [p, p+n) of a mapped image into the page cache.
// Windows: PrefetchVirtualMemory (a true async read-ahead primitive that
// issues one large read; falls back to a probing touch on old systems).
// Linux: madvise(MADV_WILLNEED). Other platforms: no-op.
#ifdef _WIN32
static void mem_prefetch(const void* p, size_t n) {
    static const bool have_pvm = [] {
        return GetProcAddress(GetModuleHandleA("kernel32.dll"),
                              "PrefetchVirtualMemory") != nullptr;
    }();
    if (have_pvm) {
        WIN32_MEMORY_RANGE_ENTRY e{};
        e.VirtualAddress = const_cast<void*>(p);
        e.NumberOfBytes  = n;
        PrefetchVirtualMemory(GetCurrentProcess(), 1, &e, 0);
    } else {
        // Probe one byte per 64 KiB page: pulls the range in with 1/64th of
        // the traffic and keeps cluster reads mostly sequential.
        const volatile uint8_t* q = (const volatile uint8_t*)p;
        for (size_t i = 0; i < n; i += 65536) (void)q[i];
        if (n) (void)q[n - 1];
    }
}
#else
#include <sys/mman.h>
static void mem_prefetch(const void* p, size_t n) {
    madvise(const_cast<void*>(p), n, MADV_WILLNEED);
}
#endif

// Walk only the chunk headers of an already-validated MIDI image. Returns
// false if a chunk header is truncated (the caller then falls back to the
// sequential scan, which has the fine-grained error reporting).
static bool locate_mtrk_spans(const uint8_t* img, size_t n,
                              size_t expected_tracks, std::vector<TrackSpan>& out) {
    out.clear();
    size_t pos = 8;                       // past "MThd"
    uint32_t hlen = ((uint32_t)img[4] << 24) | ((uint32_t)img[5] << 16)
                  | ((uint32_t)img[6] << 8) | (uint32_t)img[7];
    if (hlen < 6 || 8 + (size_t)hlen > n) return false;
    pos = 8 + hlen;
    out.reserve(expected_tracks);
    // The walk only touches 8 bytes per chunk header, but on a cold mapping
    // each untouched header is its own page fault — thousands of scattered
    // faults collapse HDD readahead into a seek storm (measured 10-14 MB/s).
    // Prefetch one modest window at a time so the disk sees a forward
    // sequential stream while headers are read from warm pages.
    size_t covered = 0;                   // image bytes already prefetched
    while (pos + 8 <= n) {
        if (pos >= covered) {
            mem_prefetch(img + covered, std::min<size_t>(4u << 20, n - covered));
            covered = std::min<size_t>(covered + (4u << 20), n);
        }
        const uint32_t tag = ((uint32_t)img[pos] << 24) | ((uint32_t)img[pos+1] << 16)
                           | ((uint32_t)img[pos+2] << 8) | (uint32_t)img[pos+3];
        const uint32_t len = ((uint32_t)img[pos+4] << 24) | ((uint32_t)img[pos+5] << 16)
                           | ((uint32_t)img[pos+6] << 8) | (uint32_t)img[pos+7];
        pos += 8;
        if (tag != 0x4D54726B) {           // not "MTrk": skip its payload like the
            const uint64_t skip = len;     // sequential scan does (by length)
            pos += (size_t)std::min<uint64_t>(skip, n - pos);
            continue;
        }
        const uint64_t end = (uint64_t)pos + len;
        if (end > n) return false;         // declared length past the image
        out.push_back({pos, len});
        pos = (size_t)end;
    }
    return !out.empty();
}

// Parse tracks [first, last) of the located spans into a private TickData.
// Mirrors scan_stream's per-track state and semantics exactly, including the
// pointer-batched zero-delta burst loop (the hot path in black MIDIs). No
// progress pings: the parallel pass is short and the merged sequential path
// would double-count its live counters.
//
// Tempo events are appended to a per-worker vector (mutex-free; merged after
// the join). The spec-violation guard fires through cb exactly as the
// sequential scan does: the first worker to cross the limit prompts, all
// workers observe the choice, and `spec_abort` is set if two-pass was chosen
// (the caller then restarts sequentially in TEMPO_ONLY mode, as with the
// sequential scan's SpecAbort exception).
//
// Cancellation is polled on every worker's per-1M-event ping: one atomic
// flag flips, each worker flips `cancelled` and exits; joined, and the
// caller reports the cancel.
static uint64_t parse_track_range(const uint8_t* img, const std::vector<TrackSpan>& spans,
                              size_t first, size_t last, bool vel0_as_note_off,
                              bool count_cc, TickData& td,
                              std::vector<TempoChange>& tempo_raw,
                              const ProgressCallbacks& cb,
                              std::atomic<bool>& spec_prompted,
                              std::atomic<int>& spec_choice,
                              std::atomic<bool>& cancelled,
                              std::atomic<bool>& spec_abort,
                              std::atomic<bool>& data_oor,
                              const uint64_t spec_limit,
                              TrackDataDedup* dedup,
                              const std::function<void(uint64_t)>& on_track = {},
                              std::atomic<uint64_t>* pos_cell = nullptr,
                              std::atomic<uint64_t>* ev_pub = nullptr,
                              const std::vector<char>* len_once = nullptr) {
    uint64_t events = 0;
    // Spec-violation protocol shared by all workers. Returns false when the
    // scan must stop (cancel or two-pass abort). Exactly one worker prompts;
    // the others wait for the shared answer.
    auto spec_stop = [&]() -> bool {
        int c = spec_choice.load(std::memory_order_acquire);
        if (c >= 0) {
            if (c == 2) { cancelled.store(true, std::memory_order_relaxed); return false; }
            if (c == 1) { spec_abort = true; return false; }
            return true;
        }
        bool expect = false;
        if (spec_prompted.compare_exchange_strong(expect, true, std::memory_order_acq_rel)) {
            c = cb.on_spec_violation ? cb.on_spec_violation(td.max_tick, td.memory_bytes()) : 0;
            spec_choice.store(c, std::memory_order_release);
            if (c == 2) { cancelled.store(true, std::memory_order_relaxed); return false; }
            if (c == 1) { spec_abort = true; return false; }
            return true;
        }
        while ((c = spec_choice.load(std::memory_order_acquire)) < 0) std::this_thread::yield();
        if (c == 2) { cancelled.store(true, std::memory_order_relaxed); return false; }
        if (c == 1) { spec_abort = true; return false; }
        return true;
    };

    TrackBodyHooks hooks;
    hooks.data_oor_atomic = &data_oor;
    hooks.cancelled_flag  = &cancelled;
    bool data_oor_local = false;   // unused: the shared atomic handles warnings
    uint64_t pub_track = 0;   // events of the current track already published
    hooks.ping = [&](uint64_t n) {
        // Cancel propagation + live event publication (the walked total
        // itself comes from the body's return value per track; ev_pub is
        // display only).
        pub_track += n;
        if (ev_pub) ev_pub->fetch_add(n, std::memory_order_relaxed);
        if (cb.cancel_flag && cb.cancel_flag->load(std::memory_order_relaxed))
            cancelled.store(true, std::memory_order_relaxed);
    };
    hooks.spec_fire = [&]() { return spec_stop(); };

    for (size_t i = first; i < last; ++i) {
        if (cancelled.load(std::memory_order_relaxed)) return events;
        if (on_track) on_track(spans[i].off);   // prefetcher progress: track start
        hooks.pos_base = spans[i].off;          // updated per track; pos_out stays

        // Dedup hit: replay the cached summary into this worker's partial.
        // Hashes come straight from the image (no copy); lookups share the
        // store lock with record merging. Spans whose declared length occurs
        // exactly once in the file are pre-marked by the caller (len_once)
        // and skip the whole dedup block: they can never replay, so they
        // keep the plain parse's zero overhead (no hash, no summary, no
        // store churn).
        const bool dedup_this = dedup && !(len_once && (*len_once)[i]);
        if (dedup_this) {
            const uint64_t hash = dedup_hash_track(img + spans[i].off, (size_t)spans[i].len,
                                                   &cancelled);
            std::shared_ptr<const TrackSummary> hit;
            if (dedup_find_cachable(*dedup, (uint64_t)spans[i].len, hash, hit)) {
                replay_track_summary(*hit, td, tempo_raw);
                td.ntracks++;
                dedup->hits.fetch_add(1, std::memory_order_relaxed);
                dedup->savings_events.fetch_add(hit->events, std::memory_order_relaxed);
                events += hit->events;
                if (ev_pub) ev_pub->fetch_add(hit->events - pub_track,
                                              std::memory_order_relaxed);
                pub_track = 0;
                continue;
            }
            TrackSummary rec;
            rec.bytes = (uint64_t)spans[i].len;
            rec.hash  = hash;
            RawStream bs(img + spans[i].off, (size_t)spans[i].len);
            rec.events = parse_track_body(bs, /*accumulate=*/true, vel0_as_note_off,
                                          td, tempo_raw, cb, spec_limit, hooks,
                                          data_oor_local, &rec);
            events += rec.events;   // walked events count like every other parse
            rec.desync = ((size_t)bs.consumed() < (size_t)spans[i].len);
            if (rec.desync) td.desync_tracks++;
            td.ntracks++;
            dedup->misses.fetch_add(1, std::memory_order_relaxed);
            dedup_store_record(rec, *dedup);
            if (ev_pub) ev_pub->fetch_add(rec.events - pub_track,
                                          std::memory_order_relaxed);
            pub_track = 0;
            continue;
        }

        RawStream bs(img + spans[i].off, (size_t)spans[i].len);
        const uint64_t walked = parse_track_body(bs, /*accumulate=*/true, vel0_as_note_off,
                                                 td, tempo_raw, cb, spec_limit, hooks,
                                                 data_oor_local, nullptr);
        events += walked;
        if (ev_pub) ev_pub->fetch_add(walked - pub_track, std::memory_order_relaxed);
        pub_track = 0;
        td.ntracks++;
        if ((size_t)bs.consumed() < (size_t)spans[i].len) td.desync_tracks++;
    }
    return events;
}

// Parallel ACCUMULATE scan over a plain mmap'd file. Return codes:
//   0 = file does not qualify (no image, bad chunk table, <2 tracks, or fewer
//       than two requested threads) -- caller falls back to the sequential scan
//  -1 = cancelled
//  -2 = spec-violation prompt answered "two-pass" -- caller restarts in
//       TEMPO_ONLY mode, exactly like the sequential scan's SpecAbort
//   1 = success (td holds the merged accumulation, tempo_raw the tempo map)
static int scan_parallel(DataSrc& src, bool vel0_as_note_off, TickData& td,
                         std::vector<TempoChange>& tempo_raw, int nthreads,
                         const ProgressCallbacks& cb, bool track_dedup,
                         uint16_t& out_division) {
    // 0 = "all cores" (GUI default). Resolve here so the sequential fallback
    // below only triggers for an explicit 1 (or a genuinely single-core box).
    if (nthreads <= 0) {
        nthreads = (int)std::thread::hardware_concurrency();
        if (nthreads <= 0) nthreads = 1;
    }
    if (nthreads < 2) return 0;
    size_t n = 0;
    const uint8_t* img = src.image(n);
    if (!img || n < 14) return 0;
    if (!(img[0] == 'M' && img[1] == 'T' && img[2] == 'h' && img[3] == 'd')) return 0;
    // MThd division (big-endian at bytes 12..13): the parallel path bypasses
    // scan_stream's header parse, so it must set the PPQN itself or the sweep
    // converts ticks at the 480 default -- stretching song time (and every
    // time-derived stat) by 480/actual on files with a different PPQN.
    // Same semantics as the sequential scan: mask the SMPTE flag, default 0.
    {
        uint16_t division = (uint16_t)(((img[12] << 8) | img[13]) & 0x7FFF);
        if (division == 0) division = 480;
        out_division = division;
    }

    std::vector<TrackSpan> spans;
    if (!locate_mtrk_spans(img, n, 0, spans) || spans.size() < 2) return 0;

    emit(cb, "  Parsing " + std::to_string(spans.size()) + " tracks on "
             + std::to_string(nthreads) + " threads...\n");

    td.reset();
    td.dedup_enabled = track_dedup;
    if (track_dedup) emit(cb, "  Track dedup enabled.\n");
    // Candidate marks (see parse_track_range): for plain files every track's
    // declared length is known upfront, so a track is a dedup candidate iff
    // its length occurs at least twice -- first occurrences of repeated
    // lengths are candidates too (a later duplicate can hit them directly),
    // and unique lengths never are.
    std::vector<char> len_once(spans.size(), 1);
    if (track_dedup) {
        std::unordered_map<uint64_t, uint32_t> len_count;
        len_count.reserve(spans.size() * 2);
        for (const auto& s : spans) ++len_count[(uint64_t)s.len];
        for (size_t i = 0; i < spans.size(); ++i) {
            auto it = len_count.find((uint64_t)spans[i].len);
            if (it != len_count.end() && it->second >= 2) len_once[i] = 0;
        }
    }
    const uint64_t spec_limit = cb.spec_tick_limit ? cb.spec_tick_limit : ((uint64_t)1 << 28);
    std::atomic<bool> spec_prompted{false};
    std::atomic<int>  spec_choice{-1};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> data_oor{false};
    std::atomic<bool> spec_abort{false};   // shared: "two-pass" answer

    std::vector<TickData> partial((size_t)nthreads);
    std::vector<std::vector<TempoChange>> partial_tempo((size_t)nthreads);
    std::vector<uint64_t> partial_events((size_t)nthreads, 0);
    std::vector<std::thread> workers;
    workers.reserve((size_t)nthreads + 1);

    // Contiguous, byte-balanced track ranges in file order. The old scheme
    // striped the track list round-robin, so N workers faulted pages at N
    // far-apart offsets of the same mapping simultaneously — each worker's
    // touches became a seek for the kernel's readahead on cold spinning
    // disks (measured 200 MB/s -> 7 MB/s). Contiguous ranges keep every
    // worker inside one file region, and the prefetcher below keeps the
    // disk stream strictly sequential regardless of worker pacing.
    std::vector<std::pair<size_t, size_t>> ranges;   // [begin, end) into spans
    {
        ranges.reserve((size_t)nthreads);
        uint64_t total_bytes = 0;
        for (const auto& s : spans) total_bytes += s.len;
        size_t b = 0;
        uint64_t acc = 0;
        const uint64_t target = (total_bytes + (uint64_t)nthreads - 1) / (uint64_t)nthreads;
        for (size_t i = 0; i < spans.size(); ++i) {
            acc += spans[i].len;
            if (acc >= target && (int)ranges.size() < nthreads - 1) {
                ranges.push_back({b, i + 1});
                b = i + 1;
                acc = 0;
            }
        }
        if (b < spans.size()) ranges.push_back({b, spans.size()});
        else if (ranges.empty()) ranges.push_back({0, 0});
    }

    // Dedicated sequential prefetcher: keeps one window ahead of the
    // *fastest* parser, so the disk always sees a single forward sequential
    // stream while parsers consume warm pages. Workers publish their current
    // absolute offset at each track start (worker_at); the prefetcher cap is
    // the maximum over live workers plus the window, so a hot cache costs
    // only cheap no-op PrefetchVirtualMemory calls and a fast worker never
    // has to fault cold pages. Capping at the fastest (not slowest) reader
    // bounds wasted prefetch on cancel/spec-abort without ever stalling
    // readers below the cap.
    const size_t ngroups = ranges.size();
    std::atomic<bool> parsers_done{false};
    const uint64_t kPrefetchAhead = 64ull << 20;     // 64 MiB window
    const uint64_t kPrefetchStep  = 4ull << 20;      // 4 MiB requests
    std::vector<std::atomic<uint64_t>> worker_at(ngroups);
    for (auto& a : worker_at) a.store(0, std::memory_order_relaxed);
    std::vector<std::atomic<uint64_t>> worker_ev(ngroups);
    for (auto& a : worker_ev) a.store(0, std::memory_order_relaxed);

    // Live progress reporter: on_scan_progress is normally driven by the scan
    // thread's per-1M-event pings, but the parallel workers never run it and
    // the GUI's bar/live stats froze for the whole parallel scan. A tiny
    // reporter aggregates worker byte positions (progress fraction + MB/s)
    // and exact walked-event counts (ev/s) every 100 ms into the same
    // callback, so the GUI behaves identically in both modes.
    std::thread reporter;
    using rclock = std::chrono::steady_clock;
    const rclock::time_point reporter_t0 = rclock::now();
    // Per-worker range starts: positions are absolute image offsets, so
    // consumed bytes per worker = pos - start (summing raw positions would
    // multiply-count overlapping progress and push frac above 1). Lives at
    // function scope: the reporter thread joins after this whole block ends,
    // so capturing these by reference from an inner scope would dangle.
    std::vector<uint64_t> reporter_starts(ngroups, 0);
    for (size_t g = 0; g < ngroups && g < ranges.size(); ++g)
        reporter_starts[g] = spans[ranges[g].first].off;
    if (cb.on_scan_progress) {
        reporter = std::thread([&]() {
            while (!parsers_done.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (parsers_done.load(std::memory_order_relaxed)) break;
                uint64_t ev = 0, at = 0;
                for (size_t g = 0; g < ngroups; ++g) {
                    ev += worker_ev[g].load(std::memory_order_relaxed);
                    const uint64_t pos = std::min<uint64_t>(
                        worker_at[g].load(std::memory_order_relaxed), n);
                    at += pos > reporter_starts[g] ? pos - reporter_starts[g] : 0;
                }
                const double el = std::chrono::duration<double>(rclock::now() - reporter_t0).count();
                const double evps = el > 0.0 ? (double)ev / el : 0.0;
                double frac = -1.0;
                if (n > 0) {
                    frac = (double)std::min<uint64_t>(at, n) / (double)n;
                    if (frac > 1.0) frac = 1.0;
                }
                cb.on_scan_progress(ev, el, evps, frac);
            }
        });
    }
    std::thread prefetcher;
    {
        prefetcher = std::thread([&]() {
            uint64_t next = 0;
            while (!parsers_done.load(std::memory_order_relaxed)) {
                uint64_t fastest = 0;               // max over live workers
                for (const auto& a : worker_at)
                    fastest = std::max(fastest, a.load(std::memory_order_relaxed));
                const uint64_t limit =
                    fastest >= n ? n : std::min<uint64_t>(n, fastest + kPrefetchAhead);
                if (next >= n) break;               // image fully prefetched
                if (next < limit) {
                    const uint64_t len = std::min<uint64_t>(kPrefetchStep, n - next);
                    mem_prefetch(img + next, (size_t)len);
                    next += len;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        });
    }

    for (size_t g = 0; g < ngroups; ++g) {
        const size_t b = ranges[g].first, e = ranges[g].second;
        if (b == e) continue;
        workers.emplace_back([&, g, b, e]() {
            auto on_track = [&](uint64_t off) {
                worker_at[g].store(off, std::memory_order_relaxed);
            };
            try {
                partial_events[g] = parse_track_range(
                    img, spans, b, e, vel0_as_note_off, /*count_cc=*/true, partial[g],
                    partial_tempo[g], cb, spec_prompted, spec_choice, cancelled,
                    spec_abort, data_oor, spec_limit,
                    track_dedup ? &td.dedup : nullptr, on_track, &worker_at[g],
                    &worker_ev[g], &len_once);
            } catch (const ScanCancelled&) {
                // Growth-cancel (or an escaped poll) inside this worker:
                // flag the shared cancel so peers stop, then exit cleanly.
                cancelled.store(true, std::memory_order_relaxed);
            }
            worker_at[g].store(spans[e - 1].off + spans[e - 1].len,
                               std::memory_order_relaxed);
        });
    }
    for (auto& t : workers) t.join();
    parsers_done.store(true);
    if (prefetcher.joinable()) prefetcher.join();
    if (reporter.joinable()) reporter.join();   // reporter exits within 50ms of parsers_done
    // Terminal ping: the sampled reporter's last tick usually lands just
    // before the workers finish, leaving the bar at ~95%. Emit one exact
    // final callback so the GUI's bar and live stats complete.
    if (cb.on_scan_progress) {
        uint64_t ev = 0;
        for (const auto& a : worker_ev) ev += a.load(std::memory_order_relaxed);
        const double el = std::chrono::duration<double>(rclock::now() - reporter_t0).count();
        cb.on_scan_progress(ev, el, el > 0.0 ? (double)ev / el : 0.0, n > 0 ? 1.0 : -1.0);
    }

    if (cancelled.load()) { emit(cb, "  Cancelled.\n"); return -1; }
    if (spec_abort) return -2;

    // K-way merge: concatenate every partial's occupied prefix. The sweep
    // resizes to the true tick horizon and treats untouched cells as zero, so
    // holes and overlapping growth are fine. Partials can be far larger than
    // their occupied prefix (doubling growth), so the copy is bounded by each
    // worker group's max tick, not by the arrays' allocated size.
    size_t need = 1;
    for (const auto& p : partial) need = std::max(need, (size_t)p.max_tick + 1);
    td.cells.assign(need, TickData::TickCell{0, 0});
    size_t cc_need = 0;
    for (const auto& p : partial) cc_need = std::max(cc_need, p.dense_cc.size());
    if (cc_need) td.dense_cc.assign(cc_need, 0);
    for (size_t w = 0; w < partial.size(); ++w) {
        auto& p = partial[w];
        const size_t occ = std::min((size_t)p.max_tick + 1, p.cells.size());
        for (size_t t = 0; t < occ; ++t) {
            const auto& c = p.cells[t];
            if (c.ons | c.delta) { td.cells[t].ons += c.ons; td.cells[t].delta += c.delta; }
        }
        if (!p.dense_cc.empty()) {
            const size_t cocc = std::min(p.dense_cc.size(), td.dense_cc.size());
            for (size_t t = 0; t < cocc; ++t) if (p.dense_cc[t]) td.dense_cc[t] += p.dense_cc[t];
        }
        for (const auto& tc : partial_tempo[w]) tempo_raw.push_back(tc);
        td.total_ons  += p.total_ons;
        td.total_cc   += p.total_cc;
        td.max_tick    = std::max(td.max_tick, p.max_tick);
        td.ntracks    += p.ntracks;
        td.desync_tracks += p.desync_tracks;
        td.total_events_seen += partial_events[w];
    }
    if (track_dedup) {
        const uint64_t hits = td.dedup.hits.load(std::memory_order_relaxed);
        const uint64_t misses = td.dedup.misses.load(std::memory_order_relaxed);
        const uint64_t saved = td.dedup.savings_events.load(std::memory_order_relaxed);
        if (hits || misses)
            emit(cb, "  Dedup: " + std::to_string(hits) + " track(s) replayed, "
                     + std::to_string(misses) + " parsed, "
                     + std::to_string(saved) + " events skipped.\n");
    }
    return 1;
}

// CPU time consumed by the calling thread (the scan thread), for the
// disk-bound vs parser-bound attribution. Falls back to wall time if the
// platform call fails (attribution then reads as parser-bound, the historic
// behaviour).
static double thread_cpu_seconds() {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) {
        auto to_s = [](const FILETIME& ft) {
            return (double)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) * 1e-7;
        };
        return to_s(k) + to_s(u);
    }
    return 0.0;
#elif defined(__unix__) || defined(__APPLE__)
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    return 0.0;
#else
    return 0.0;
#endif
}

// ---- entry point --------------------------------------------------------------

std::vector<FrameStats> process_streaming(
    const std::string& filename, double fps, uint16_t& out_division,
    uint64_t& out_total_notes, bool vel0_as_note_off, uint64_t& out_total_ticks,
    const ProgressCallbacks& cb, double end_delay, int parse_threads,
    bool track_dedup)
{
    using clock = std::chrono::steady_clock;
    out_total_notes = 0;
    out_total_ticks = 0;
    const bool compressed = is_compressed_file(filename);
    emit(cb, "Reading and parsing \"" + filename + "\"" + (compressed ? " (compressed)" : "") + "...\n");

    try {
        std::unique_ptr<DataSrc> src;

        TickData td;
        std::vector<TempoChange> tempo_raw;
        uint16_t division = 480;

        // Fresh input stream for each walk. Plain files are mmap'd (rewind =
        // re-open, free); compressed archives rebuild the whole libarchive
        // decode stack, which restarts the decode from the first byte.
        auto open_src = [&]() -> std::unique_ptr<DataSrc> {
            if (compressed) {
                std::unique_ptr<PipelinedSrc> p(new PipelinedSrc(filename));
                p->start();
                return std::unique_ptr<DataSrc>(p.release());
            }
            return std::unique_ptr<DataSrc>(new FileSrc(filename));
        };

        emit(cb, "  Single sequential pass (" + std::string(compressed ? "libarchive" : "mmap") + " stream)...\n");
        clock::time_point t_scan = clock::now();
        bool two_pass = false;
        uint64_t dec_bytes = 0;
        bool dec_err = false;
        double bound_cpu_s = 0.0;      // scan-thread CPU time (bound attribution)
        double cons_wait_s = 0.0;      // pipeliner: parser starved of decoded bytes
        double prod_wait_s = 0.0;      // pipeliner: decoder blocked on full queue
        bool pipeliner_waits_known = false;
        bool parsed_parallel = false;  // plain-file multi-track parallel scan ran
        {
            src = open_src();
            if (!src->ok()) {
                emit(cb, compressed ? "Error: failed to open archive with libarchive.\n"
                                    : "Error: cannot open file.\n", true);
                return {};
            }
            const double cpu0 = thread_cpu_seconds();
            try {
                // Parallel per-track parse of plain files (never compressed,
                // which cannot split). The spec-violation prompt is shared
                // with the sequential scan: the first worker past the limit
                // asks through cb and every worker obeys the answer, so the
                // modal appears exactly as before. -2 = two-pass chosen;
                // -1 = cancelled; 0 = file does not qualify (sequential
                // fallback); 1 = merged accumulation done.
                int pr = 0;
                if (!compressed && !two_pass) {
                    pr = scan_parallel(*src, vel0_as_note_off, td, tempo_raw,
                                       parse_threads, cb, track_dedup, division);
                    if (pr == -1) return {};      // cancel message already emitted
                    if (pr == -2) {               // user chose the low-memory path
                        pr = 0;
                        two_pass = true;
                        td.reset();
                        tempo_raw.clear();
                    } else if (pr == 0) {
                        td.reset();
                        tempo_raw.clear();
                    }
                }
                if (pr <= 0) {
                    // The parallel path may have cleared td (decline/reset);
                    // the sequential dedup protocol needs the flag back.
                    td.dedup_enabled = track_dedup;
                    // Sequential walk. TEMPO_ONLY when two-pass was already
                    // chosen (at the parallel prompt) so the guard does not
                    // fire a second time; ACCUMULATE otherwise, whose guard
                    // may throw SpecAbort for the same low-memory restart.
                    if (!scan_any(*src, vel0_as_note_off, td, tempo_raw, division, cb,
                                  two_pass ? ScanMode::TEMPO_ONLY : ScanMode::ACCUMULATE))
                        return {};
                }
            } catch (const SpecAbort&) {
                // Sequential path's spec prompt: same low-memory restart.
                two_pass = true;
                td.reset();
                tempo_raw.clear();
            } catch (const ScanCancelled&) {
                // Spec guard's cancel choice: the message was already emitted;
                // release the source and report the cancel like path -1.
                src.reset();
                return {};
            }
            bound_cpu_s = thread_cpu_seconds() - cpu0;
            if (compressed) {
                if (auto* p = dynamic_cast<PipelinedSrc*>(src.get()); p && p->is_piped()) {
                    p->get_waits(cons_wait_s, prod_wait_s);
                    pipeliner_waits_known = true;
                }
            }
            dec_bytes = src->bytes_read();
            // The raw mmap walk never advances the reader's position, so for
            // plain files report the image size instead of bytes served.
            if (dec_bytes == 0) dec_bytes = src->total_bytes();
            dec_err = src->errored();
            src.reset();   // release before any re-open
        }

        std::vector<FrameStats> frames;
        uint64_t nev = 0;
        clock::time_point t_sweep;

        if (!two_pass) {
            t_sweep = clock::now();
            out_division = division;
            emit(cb, "  " + std::to_string(td.ntracks) + " tracks\n");
            if (td.desync_tracks > 0)
                emit(cb, "Warning: " + std::to_string(td.desync_tracks)
                         + " track(s) consumed a different number of bytes than declared (possible parser desync).\n", true);
            try {
                // The sweep can run long after the parse (spec-breaking tick
                // spans); its cancel polls throw ScanCancelled, which must
                // not escape process_midi (same protocol as the scans).
                frames = sweep_to_frames(td, tempo_raw, division, fps, true,
                                         end_delay, &cb);
            } catch (const ScanCancelled&) {
                return {};   // message already emitted by the throw site
            }
            out_total_notes = td.total_ons;
            out_total_ticks = td.max_tick;
            nev = td.total_events_seen;   // all events walked, same as the live counter
        } else {
            // Pass 1: tempo map only. Memory stays O(tempo events).
            emit(cb, "  Restarting in low-memory two-pass mode (pass 1/2: tempo map)...\n");
            t_scan = clock::now();   // restart the clock: the stats line should reflect the two passes, not the aborted attempt
            src = open_src();
            if (!src->ok()) { emit(cb, "Error: failed to re-open input for pass 1.\n", true); return {}; }
            try {
                // Pass 1 cancels throw ScanCancelled (same protocol as the
                // first attempt); without a catch here the exception would
                // escape process_midi and terminate the job thread.
                if (!scan_any(*src, vel0_as_note_off, td, tempo_raw, division, cb, ScanMode::TEMPO_ONLY))
                    return {};
            } catch (const ScanCancelled&) {
                src.reset();   // message already emitted by the throw site
                return {};
            }
            out_division = division;
            const std::vector<TempoChange> tm = build_tempo_map(tempo_raw, division);

            emit(cb, "  Pass 2/2: accumulating into frame buckets...\n");
            src = open_src();
            if (!src->ok()) { emit(cb, "Error: failed to re-open input for pass 2.\n", true); return {}; }
            FrameBuckets fb;
            fb.cc.resize(4096, 0);
            try {
                if (!scan_any_frames(*src, vel0_as_note_off, fb, tm, division, fps, cb))
                    return {};
            } catch (const ScanCancelled&) {
                src.reset();
                return {};
            }
            dec_bytes = src->bytes_read();
            dec_err = src->errored();
            src.reset();
            t_sweep = clock::now();

            // Song horizon from the last tempo anchor + the highest tick seen.
            {
                size_t conv = 0;
                while (conv + 1 < tm.size() && tm[conv + 1].tick <= fb.max_tick) conv++;
                const double max_time = tm[conv].time_sec
                    + (double)(fb.max_tick - tm[conv].tick)
                      * (double)tm[conv].us_per_quarter / ((double)division * 1e6);
                try {
                    frames = buckets_to_frames(fb, fps, tm, max_time, true, division,
                                               end_delay, &cb);
                } catch (const ScanCancelled&) {
                    return {};   // message already emitted by the throw site
                }
            }
            out_total_notes = fb.total_ons;
            out_total_ticks = fb.max_tick;
            nev = fb.total_events_seen;
        }

        const double t_scan_s   = std::chrono::duration<double>(t_sweep - t_scan).count();
        const double t_sweep_s  = std::chrono::duration<double>(clock::now() - t_sweep).count();
        auto rate = [](uint64_t n, double s) {
            return (s > 0.0) ? (double)n / s : 0.0;
        };
        std::ostringstream st;
        st << "  Stats: " << format_with_commas(nev) << " events in " << std::fixed << std::setprecision(2)
           << t_scan_s << "s scan (" << format_with_commas((uint64_t)rate(nev, t_scan_s)) << " ev/s)";
        if (dec_bytes > 0) st << ", " << format_file_size(dec_bytes) << " decoded in "
                        << std::setprecision(2) << t_scan_s << "s ("
                        << format_file_size_rate(dec_bytes, t_scan_s) << ")";
        if (compressed) {
            if (dec_err) st << "\n  Warning: archive stream reported an error before EOF.";
        } else if (parsed_parallel) {
            // Multi-threaded parse: per-thread duty cycle is not measured (the
            // spawning thread's CPU time says nothing useful here), so just
            // record the configuration; the ev/s rate tells the rest.
            st << "\n  Bound: parallel parse on " << parse_threads << " threads";
        } else if (!two_pass && bound_cpu_s > 0.0) {
            // Plain files: the scan thread either burns CPU (parser-bound) or
            // sleeps in the kernel on page faults (disk-bound). CPU/wall is a
            // direct duty cycle; ~1.0 -> parser-bound, well below -> disk-bound.
            const double duty = bound_cpu_s / t_scan_s;
            st << "\n  Bound: ";
            if (duty >= 0.85)      st << "parser (CPU " << std::setprecision(0) << duty * 100.0 << "% of wall)";
            else if (duty >= 0.50) st << "mixed (CPU " << std::setprecision(0) << duty * 100.0 << "% of wall)";
            else                   st << "disk (CPU " << std::setprecision(0) << duty * 100.0 << "% of wall)";
        }
        if (compressed && pipeliner_waits_known) {
            // Decode vs parse attribution from the pipeliner's wait side.
            const double waits = cons_wait_s + prod_wait_s;
            if (waits > 0.05 * t_scan_s) {
                st << "\n  Bound: ";
                if (cons_wait_s >= prod_wait_s)
                    st << "decode (parser starved " << std::setprecision(0) << cons_wait_s << "s of "
                       << std::setprecision(2) << t_scan_s << "s)";
                else
                    st << "parser (decoder blocked " << std::setprecision(0) << prod_wait_s << "s of "
                       << std::setprecision(2) << t_scan_s << "s)";
            } else {
                st << "\n  Bound: balanced (decode and parser kept pace)";
            }
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

