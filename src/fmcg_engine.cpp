#include "fmcg_engine.h"

#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <chrono>

#include "archive.h"
#include "archive_entry.h"


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
    // CC accumulation (only touched when CC stats are enabled).
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
    }
};

// ---- single sequential walk over the image -----------------------------------

// Result of a single walk over the image.
enum class ScanMode { ACCUMULATE, TEMPO_ONLY };

// Aborted via the MIDI-spec guard; the caller restarts the walk in TEMPO_ONLY
// mode (two-pass fallback for spec-breaking tick spans).
struct SpecAbort {};

bool scan_image(DataSrc& src, bool vel0_as_note_off, TickData& td,
                       std::vector<TempoChange>& tempo_raw, uint16_t& out_division,
                       const ProgressCallbacks& cb, ScanMode mode = ScanMode::ACCUMULATE) {
    ByteStream bs(src);
    const bool accumulate = (mode == ScanMode::ACCUMULATE);
    // In TEMPO_ONLY mode the accumulation arrays are never touched (that is
    // the point) and CC counting is meaningless.
    const bool count_cc = cb.cc_stats && accumulate;
    if (!accumulate) td.reset();

    // Continuous progress bookkeeping (checked once per ~1M events).
    using clock = std::chrono::steady_clock;
    const clock::time_point t_start = clock::now();
    uint64_t ev_total = 0;
    uint64_t ev_count = 0;
    const uint64_t total_bytes = src.total_bytes();   // 0 = unknown fraction
    const uint64_t spec_limit = cb.spec_tick_limit ? cb.spec_tick_limit : ((uint64_t)1 << 28);
    bool spec_warned = false;              // one-shot: ask at most once per scan

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
            // MIDI spec guard: delta times are at most 28-bit VLQs, so a tick
            // beyond 1<<28 cannot be represented within the spec. At that
            // point the per-tick arrays also grow toward gigabytes. Fire the
            // callback once (proceed / restart-in-2-pass / cancel); without a
            // callback we proceed (CLI/tests, historic behaviour).
            if (accumulate && tick > spec_limit && !spec_warned) {
                const int choice = cb.on_spec_violation
                    ? cb.on_spec_violation(tick, td.memory_bytes())
                    : 0;
                if (choice == 2) { emit(cb, "  Cancelled.\n"); return false; }
                if (choice == 1) throw SpecAbort{};   // caller restarts in TEMPO_ONLY mode
                spec_warned = true;              // 0 (or absent cb): ask only once
            }
            // Peek the next byte: if it is a data byte, this event uses the
            // held running status and the byte stays in the stream (it will
            // be re-read below as data). No unread round-trip needed.
            int st = bs.peek();
            if (st < 0) break;
            uint8_t status;
            if (st < 0x80) {
                status = running;
            } else {
                bs.get();   // consume the status byte
                status = (uint8_t)st;
                running = (status < 0xF0) ? status : 0;
            }

            if (status >= 0xF0) {
                // Rare system/meta family, kept out of the channel hot path.
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
                    if (et == 0x90 && (vel > 0 || !vel0_as_note_off)) {
                        refcount[ch][note]++;
                        if (accumulate) td.note_on(tick);
                    } else if (et == 0x90 || et == 0x80) {
                        if (refcount[ch][note] > 0) { refcount[ch][note]--; if (accumulate) td.delta_at(tick, -1); }
                    } else if (et == 0xB0 && count_cc) {
                        td.cc_at(tick);   // control change: counted only when the CC stat is on
                    }
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
                if (refcount[ch][n] > 0 && accumulate) td.delta_at(tick, -(int64_t)refcount[ch][n]);
        if (accumulate && tick > td.max_tick) td.max_tick = tick;

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
    td.total_events_seen = ev_total + ev_count;   // identical metric to the live pings
    return true;
}

// ---- anchor-interpolated sweep: tick buckets -> FrameStats --------------------

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
                                               uint16_t ppqn, double fps, bool with_cc = false) {
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
        const TickData::TickCell cell = cells[(size_t)t];
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

        cum += o;
        poly += d;
        if (c) cum_cc += c;
    }

    // Determine total frames from sec(max_tick) (catch up conversion anchors first).
    while (conv + 1 < tm.size() && tm[conv + 1].tick <= td.max_tick) conv++;
    double max_time = tm[conv].time_sec
                    + (double)(td.max_tick - tm[conv].tick) * (double)tm[conv].us_per_quarter * inv;
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

struct FrameBuckets {
    std::vector<uint32_t> ons, cc;
    std::vector<int32_t> deltas;   // signed: transient negatives must not wrap
    uint64_t total_ons = 0, total_cc = 0;
    uint64_t total_events_seen = 0;   // one walk's worth, same metric as the pings
    uint64_t max_tick = 0;            // highest tick seen (song-horizon source)
    size_t nframes = 0;

    void ensure(size_t fi) {
        if (fi >= nframes) {
            size_t n = nframes ? nframes : 4096;
            while (n <= fi) n *= 2;
            ons.resize(n, 0); deltas.resize(n, 0);
            if (!cc.empty()) cc.resize(n, 0);
            nframes = n;
        }
    }
    // Bucket growth is on demand; guarantee capacity for the final sizing.
    void reserve_exact(size_t n) {
        ons.resize(n, 0); deltas.resize(n, 0);
        if (!cc.empty()) cc.resize(n, 0);
        if (nframes < n) nframes = n;
    }
    void note_on(size_t fi)  { ensure(fi); ons[fi]++;    deltas[fi]++;    total_ons++; }
    void note_off(size_t fi) { ensure(fi); deltas[fi]--; }
    void add_cc(size_t fi)   { ensure(fi); cc[fi]++;     total_cc++; }
};

// Pass 2 walk: same event grammar as scan_image, but events land in frame
// buckets through the precomputed tempo map. Tick-space is never materialized.
bool scan_image_frames(DataSrc& src, bool vel0_as_note_off, FrameBuckets& fb,
                       const std::vector<TempoChange>& tm, uint16_t ppqn, double fps,
                       const ProgressCallbacks& cb) {
    ByteStream bs(src);
    const bool count_cc = cb.cc_stats;

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

    while (true) {
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
                    if (et == 0x90 && (vel > 0 || !vel0_as_note_off)) {
                        refcount[ch][note]++;
                        fb.note_on(frame_of(tick));
                    } else if (et == 0x90 || et == 0x80) {
                        if (refcount[ch][note] > 0) { refcount[ch][note]--; fb.note_off(frame_of(tick)); }
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
                if (refcount[ch][n] > 0) fb.note_off(frame_of(tick));
        if (tick > fb.max_tick) fb.max_tick = tick;

        size_t consumed = bs.consumed();
        bs.set_limit((size_t)-1);
        if (consumed < (size_t)len) bs.skip((size_t)len - consumed);
    }

    ping();
    fb.total_events_seen = ev_total + ev_count;   // identical metric to the live pings
    return true;
}

// Convert frame buckets to FrameStats. Pass-1-style semantics: running sums
// and extrema accumulate in frame order (buckets already hold exactly the
// per-frame values the sweep would have produced).
std::vector<FrameStats> buckets_to_frames(FrameBuckets& fb, double fps,
                                          const std::vector<TempoChange>& tm,
                                          double max_time_sec, bool with_cc) {
    const size_t total_frames = (size_t)std::ceil(max_time_sec * fps) + 1;
    fb.reserve_exact(total_frames);   // grow (or zero-extend) to the horizon

    std::vector<FrameStats> out;
    out.reserve(total_frames);
    uint64_t cum = 0, cum_cc = 0;
    int64_t poly = 0, peak_poly = 0;
    double peak_nps = 0.0;
    size_t bpmw = 0;
    const size_t W = (size_t)std::round(fps);

    for (size_t k = 0; k < total_frames; ++k) {
        while (bpmw + 1 < tm.size() && tm[bpmw + 1].time_sec <= (double)k / fps) bpmw++;
        cum  += fb.ons[k];
        poly += fb.deltas[k];
        if (with_cc) cum_cc += fb.cc[k];
        uint64_t prev_cum = (k >= W) ? out[k - W].cumulative_notes : 0;
        double nps = (double)(cum - prev_cum);
        peak_nps = std::max(peak_nps, nps);
        peak_poly = std::max(peak_poly, poly);
        out.push_back({k, (double)k / fps, cum, cum_cc, nps, peak_nps,
                       std::max<int64_t>(0, poly), peak_poly, tm[bpmw].bpm});
    }
    return out;
}

// ---- entry point --------------------------------------------------------------

std::vector<FrameStats> process_streaming(
    const std::string& filename, double fps, uint16_t& out_division,
    uint64_t& out_total_notes, bool vel0_as_note_off, const ProgressCallbacks& cb)
{
    using clock = std::chrono::steady_clock;
    out_total_notes = 0;
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
        {
            src = open_src();
            if (!src->ok()) {
                emit(cb, compressed ? "Error: failed to open archive with libarchive.\n"
                                    : "Error: cannot open file.\n", true);
                return {};
            }
            try {
                if (!scan_image(*src, vel0_as_note_off, td, tempo_raw, division, cb)) return {};
            } catch (const SpecAbort&) {
                // User chose the low-memory path at the spec-violation prompt:
                // re-walk collecting the tempo map only (no per-tick arrays),
                // then walk again bucketing events into video frames.
                two_pass = true;
                td.reset();
                tempo_raw.clear();
            }
            dec_bytes = src->bytes_read();
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
            frames = sweep_to_frames(td, tempo_raw, division, fps, cb.cc_stats);
            out_total_notes = td.total_ons;
            nev = td.total_events_seen;   // all events walked, same as the live counter
        } else {
            // Pass 1: tempo map only. Memory stays O(tempo events).
            emit(cb, "  Restarting in low-memory two-pass mode (pass 1/2: tempo map)...\n");
            t_scan = clock::now();   // restart the clock: the stats line should reflect the two passes, not the aborted attempt
            src = open_src();
            if (!src->ok()) { emit(cb, "Error: failed to re-open input for pass 1.\n", true); return {}; }
            if (!scan_image(*src, vel0_as_note_off, td, tempo_raw, division, cb, ScanMode::TEMPO_ONLY))
                return {};
            out_division = division;
            const std::vector<TempoChange> tm = build_tempo_map(tempo_raw, division);

            emit(cb, "  Pass 2/2: accumulating into frame buckets...\n");
            src = open_src();
            if (!src->ok()) { emit(cb, "Error: failed to re-open input for pass 2.\n", true); return {}; }
            FrameBuckets fb;
            if (cb.cc_stats) fb.cc.resize(4096, 0);
            if (!scan_image_frames(*src, vel0_as_note_off, fb, tm, division, fps, cb))
                return {};
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
                frames = buckets_to_frames(fb, fps, tm, max_time, cb.cc_stats);
            }
            out_total_notes = fb.total_ons;
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
        if (compressed) {
            if (dec_bytes > 0) st << ", " << (dec_bytes >> 20) << " MB decoded in "
                            << std::setprecision(2) << t_scan_s << "s ("
                            << std::setprecision(1) << rate(dec_bytes, t_scan_s) / 1e6 << " MB/s)";
            if (dec_err) st << "\n  Warning: archive stream reported an error before EOF.";
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

