#include "fmcg_reader.h"

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
#include "fmcg_path.h"   // UTF-8 file paths

#include "archive.h"
#include "archive_entry.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__)
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

static std::atomic<bool> g_close_held_notes{false};
void set_close_held_notes(bool close) { g_close_held_notes.store(close, std::memory_order_relaxed); }
bool close_held_notes() { return g_close_held_notes.load(std::memory_order_relaxed); }


void emit(const ProgressCallbacks& cb, const std::string& msg, bool is_error) {
    if (cb.on_log) cb.on_log(msg.c_str(), is_error);
    else if (is_error) std::cerr << msg << std::flush;
    else std::cout << msg << std::flush;
}

// libarchive's narrow open uses the ANSI code page on Windows; fMCG paths are UTF-8.
static int archive_open_utf8(struct archive* a, const std::string& path, size_t block) {
#if defined(_WIN32)
    return archive_read_open_filename_w(a, widen_utf8(path).c_str(), block);
#else
    return archive_read_open_filename(a, path.c_str(), block);
#endif
}

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
        const uint8_t* blk_p = nullptr;   // current libarchive output block: served in
        size_t blk_n = 0, s_lo = 0;       // place, valid until the next data_block call
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
                if (s_lo == blk_n) {
                    const void* buf; size_t bsz; la_int64_t off;
                    if (!a && pf) {                    // last resort: the file's bytes verbatim
                        const size_t r = std::fread(d + out, 1, n - out, pf);
                        if (r == 0) break;
                        out += r;
                        continue;
                    }
                    if (saw_error && raw_mode) break;  // raw decoder poisoned: fail-stop
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
                    blk_p = (const uint8_t*)buf;   // libarchive's buffer, valid until the next call
                    blk_n = bsz;
                    s_lo = 0;
                }
                size_t take = std::min(blk_n - s_lo, n - out);
                std::memcpy(d + out, blk_p + s_lo, take);
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
            blk_p = nullptr; blk_n = 0; s_lo = 0;
            a = archive_read_new();
            archive_read_support_filter_all(a);
            archive_read_support_format_all(a);
            archive_read_support_format_raw(a);   // bare filter-compressed files
            if (archive_open_utf8(a, path_, 10240) != ARCHIVE_OK) {
                archive_read_free(a); a = nullptr; return;
            }
            // First header; skip directory entries.
            while (true) {
                if (archive_read_next_header(a, &entry) != ARCHIVE_OK) {
                    archive_read_free(a); a = nullptr; return;
                }
                if (archive_entry_filetype(entry) != AE_IFDIR) break;
            }
        }
        // Re-open serving only the filter's decoded output (no container
        // parsing): the raw reader's "data" entry yields exactly the decoded
        // byte stream, whatever the inner content is. If no filter matches,
        // raw mode simply serves the file bytes unchanged -- which is the
        // correct passthrough for a plain container reached via recursion.
        void open_raw() {
            if (a) { archive_read_free(a); a = nullptr; }
            blk_p = nullptr; blk_n = 0; s_lo = 0;
            raw_mode = true;
            a = archive_read_new();
            archive_read_support_filter_all(a);
            archive_read_support_format_raw(a);
            if (archive_open_utf8(a, path_, 10240) != ARCHIVE_OK ||
                archive_read_next_header(a, &entry) != ARCHIVE_OK) {
                archive_read_free(a); a = nullptr;
                pf = fopen_utf8(path_, "rb");   // last resort: copy the file verbatim
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
        const uint8_t* blk_p = nullptr;   // current libarchive output block (not owned)
        size_t blk_n = 0, s_lo = 0;
        bool saw_error = false;
        bool saw_eof = false;        // decoder reached a real stream terminator
        size_t prefix_n = 0;         // peeked header bytes prepended to buf
        std::vector<uint8_t> buf;    // full history of the decoded-input stream
        size_t lpos = 0;             // logical position within buf
        bool inner_eof = false;
        std::vector<uint8_t> rb;     // read-callback scratch buffer
        std::vector<uint8_t> tmp64;  // private fill scratch (no shared-scratch aliasing)
        bool done = false;           // one decode pass only: the feeder serves
                                     // exactly ONE member's payload; a second
                                     // stream in the input (concatenated
                                     // members) belongs to the NEXT member,
                                     // which the layered walker hashes raw.

        size_t fill_more() {
            if (inner_eof || done) return 0;
            // PRIVATE scratch: a shared buffer was once aliased between the
            // outer read and a nested feeder's pull() (corrupted decoded bytes
            // fed to libarchive mid-member).
            tmp64.resize(1 << 16);
            uint8_t* tmp = tmp64.data();
            size_t n = inner->pull(tmp, tmp64.size());
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

        // Single-layer decode selector: NONE = legacy auto-bid (multi-layer),
        // XZ / SEVENZ = exactly one decode pass for the layered dedup walker.
        enum class Mode { NONE, XZ, SEVENZ };

        explicit DecompFeeder(std::unique_ptr<Feeder> in, const uint8_t* hdr = nullptr, size_t hdr_n = 0,
                              Mode m = Mode::NONE)
            : inner(std::move(in)), rb(1 << 18) {
            mode = m;
            if (hdr && hdr_n) { buf.assign(hdr, hdr + hdr_n); prefix_n = hdr_n; }
            open_archive();
        }
        ~DecompFeeder() override { if (a) archive_read_free(a); }

        // Restart the layer's decode from its stream's beginning: drop the
        // decoded history, rewind the inner feeder, re-open the decoder.
        void reset() override {
            if (a) { archive_read_free(a); a = nullptr; }
            blk_p = nullptr; blk_n = 0; s_lo = 0;
            buf.resize(prefix_n);
            lpos = 0;
            inner_eof = false;
            saw_error = false;
            saw_eof = false;
            done = false;
            inner->reset();
            open_archive();
        }

        size_t pull(void* dst, size_t n) override {
            uint8_t* d = (uint8_t*)dst;
            size_t out = 0;
            while (out < n) {
                if (s_lo == blk_n) {
                    const void* buf; size_t bsz; la_int64_t off;
                    if (saw_error || !a) break;   // decoder poisoned: never re-read (UB in xz)
                    int r = archive_read_data_block(a, &buf, &bsz, &off);
                    if (r == ARCHIVE_EOF) { saw_eof = true; break; }
                    if (r != ARCHIVE_OK) { saw_error = true; break; }
                    if (bsz == 0) continue;
                    blk_p = (const uint8_t*)buf;   // libarchive's buffer, valid until the next call
                    blk_n = bsz;
                    s_lo = 0;
                }
                size_t take = std::min(blk_n - s_lo, n - out);
                std::memcpy(d + out, blk_p + s_lo, take);
                s_lo += take;
                out += take;
            }
            return out;
        }
        bool errored() const override { return saw_error || inner->errored(); }
        const char* errstr() const { return (a && saw_error) ? archive_error_string(a) : nullptr; }
        bool stream_ended() const { return saw_eof; }

    private:
        struct archive_entry* entry = nullptr;

        Mode mode = Mode::NONE;

        void open_archive() {
            a = archive_read_new();
            switch (mode) {
                case Mode::XZ:
                    // Forced xz filter: no filter bidding at all, so libarchive
                    // cannot stack a second xz on the decoded output. The raw
                    // format reader passes the payload through untouched.
                    archive_read_support_format_raw(a);
                    archive_read_append_filter(a, ARCHIVE_FILTER_XZ);
                    break;
                case Mode::SEVENZ:
                    archive_read_support_format_7zip(a);
                    break;
                case Mode::NONE:
                    archive_read_support_filter_all(a);
                    archive_read_support_format_all(a);
                    archive_read_support_format_raw(a);
                    break;
            }
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
    std::vector<std::vector<uint8_t>> spare;   // drained blocks, reused by the producer
    size_t q_bytes = 0;
    size_t b_lo = 0;                 // consume offset into q.front()
    static constexpr size_t Q_CAP = 4u << 20;   // decoded bytes in flight
    static constexpr size_t kBlock = 256u << 10; // producer block size
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
                // Decode straight into a queue block (recycled: no allocation
                // or extra copy per block), outside the lock.
                std::vector<uint8_t> blk;
                if (!spare.empty()) { blk = std::move(spare.back()); spare.pop_back(); }
                lk.unlock();
                blk.resize(kBlock);
                const size_t n = arc.read(blk.data(), blk.size());
                lk.lock();
                if (n == 0) { done = true; cv_data.notify_all(); return; }
                blk.resize(n);
                q.push_back(std::move(blk));
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
            if (b_lo == blk.size()) {
                spare.push_back(std::move(q.front()));
                q.pop_front(); b_lo = 0; cv_slot.notify_one();
            }
        }
        return out;
    }
};

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
#elif defined(__unix__)
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    return 0.0;
#else
    return 0.0;
#endif
}

// ---- entry point --------------------------------------------------------------

// True when the file's outer compression filter(s) decode to a 7z archive:
// libarchive's 7z reader needs to seek to the end-of-archive header, which a
// filtered (forward-only) stream cannot provide.
static bool filtered_7z(const std::string& filename) {
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_raw(a);
    bool hit = false;
    struct archive_entry* e = nullptr;
    if (archive_open_utf8(a, filename, 1 << 16) == ARCHIVE_OK &&
        archive_read_next_header(a, &e) == ARCHIVE_OK && archive_filter_count(a) > 1) {
        uint8_t m[6] = {};
        if (archive_read_data(a, m, sizeof m) == (la_ssize_t)sizeof m) hit = is_7z_magic(m);
    }
    archive_read_free(a);
    return hit;
}

ScanResult process_streaming(const std::string& filename, const ScanOptions& opt,
                             const ProgressCallbacks& cb)
{
    using clock = std::chrono::steady_clock;
    const double fps = opt.fps, end_delay = opt.end_delay;
    const bool vel0_as_note_off = opt.vel0_as_note_off;
    const int parse_threads = opt.parse_threads;
    const bool track_dedup = opt.track_dedup, layered_dedup = opt.layered_dedup, risky_dedup = opt.risky_dedup;
    ScanResult res;
    uint16_t& out_division = res.ppqn;
    uint64_t& out_total_notes = res.total_notes;
    uint64_t& out_total_ticks = res.total_ticks;
    const bool compressed = is_compressed_file(filename);
    const bool cj_name = has_cjcmcg_name(filename);   // CJCMCG preprocessed file (by name)
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
        bool is_cjcmcg = false;        // input was a CJCMCG preprocessed file
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
                if (compressed && filtered_7z(filename))
                    emit(cb, "Error: this is a 7z archive inside a compression filter (e.g. .7z.xz). "
                             "7z keeps its header at the end and needs random access, so it cannot be "
                             "streamed; decompress the outer layer to a .7z first.\n", true);
                else
                    emit(cb, compressed ? "Error: failed to open archive with libarchive.\n"
                                        : "Error: cannot open file.\n", true);
                return {};
            }
            const double cpu0 = thread_cpu_seconds();
            bool layered_done = false;   // td filled by a non-MIDI-walk reader (layered / .cjcmcg)
            DataSrc* cj_src = src.get();
            const ByteReader cj_read = [&cj_src](void* d, size_t n) { return cj_src->read(d, n); };
            if (cj_name) {
                int cr = 0;
                try {
                    cr = scan_cjcmcg(cj_read, compressed ? 0 : src->total_bytes(), td, tempo_raw, division,
                                     cb, /*sniffing=*/false, vel0_as_note_off);
                } catch (const ScanCancelled&) {
                    emit(cb, "  Cancelled.\n");
                    return {};
                }
                if (cr < 0) return {};
                layered_done = true;
                is_cjcmcg = true;
            }
            if (!layered_done && layered_dedup && compressed) {
                // The layered walk reads the RAW file and does all decoding
                // itself (it must see every compressed member boundary), so
                // the pipelined decoder is not used for it.
                src.reset();
                int lr = 0;
                try {
                    lr = scan_layered(filename, vel0_as_note_off, td, tempo_raw, division, cb,
                                      risky_dedup);
                } catch (const ScanCancelled&) {
                    emit(cb, "  Cancelled.\n");
                    return {};
                }
                bound_cpu_s = thread_cpu_seconds() - cpu0;
                if (lr < 0) return {};
                if (lr == 1) {
                    layered_done = true;
                } else {
                    td.reset();
                    tempo_raw.clear();
                    division = 480;
                    src = open_src();
                    if (!src->ok()) {
                        emit(cb, "Error: failed to open archive with libarchive.\n", true);
                        return {};
                    }
                }
            }
            if (!layered_done) try {
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
                    if (pr == 1) parsed_parallel = true;
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
                                  two_pass ? ScanMode::TEMPO_ONLY : ScanMode::ACCUMULATE)) {
                        if (!td.not_midi) return {};
                        // Not a MIDI stream: it may be a CJCMCG preprocessed file
                        // (e.g. an archive holding song.cjcmcg). Accepted only if
                        // its whole structure parses to the end of the stream.
                        src = open_src();
                        cj_src = src.get();
                        std::string why;
                        const int cr = src->ok()
                            ? scan_cjcmcg(cj_read, compressed ? 0 : src->total_bytes(), td, tempo_raw,
                                          division, cb, /*sniffing=*/true, vel0_as_note_off, &why)
                            : 0;
                        if (cr != 1) {
                            emit(cb, "Error: not a MIDI file (missing MThd header)"
                                     + (why.empty() ? std::string() : " and not a CJCMCG .cjcmcg file (" + why + ")")
                                     + ".\n", true);
                            return {};
                        }
                        layered_done = true;
                        is_cjcmcg = true;
                    }
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
            if (!layered_done) {
                dec_bytes = src->bytes_read();
                // The raw mmap walk never advances the reader's position, so for
                // plain files report the image size instead of bytes served.
                if (dec_bytes == 0) dec_bytes = src->total_bytes();
                dec_err = src->errored();
                src.reset();   // release before any re-open
            }
        }

        std::vector<FrameStats> frames;
        uint64_t nev = 0;
        clock::time_point t_sweep;

        if (!two_pass) {
            t_sweep = clock::now();
            out_division = division;
            if (!is_cjcmcg)   // a .cjcmcg has no track structure
                emit(cb, "  " + format_with_commas(td.ntracks) + (td.ntracks == 1 ? " track\n" : " tracks\n"));
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
            const int nthr = parse_threads > 0 ? parse_threads : (int)std::max(1u, std::thread::hardware_concurrency());
            st << "\n  Bound: parallel parse on " << nthr << " threads";
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
        res.meter = apply_meter(frames, tempo_raw, out_division, out_total_ticks);
        res.frames = std::move(frames);
        return res;
    } catch (const ScanCancelled&) {
        // Cancel fired inside the layered walk / scan / sweep. Without this
        // handler the empty-struct exception escaped process_streaming
        // entirely (it is not a std::exception), bypassing the clean
        // empty-result contract the GUI's cancel path relies on.
        emit(cb, "Cancelled.\n", true);
        return {};
    } catch (const std::bad_alloc&) {
        emit(cb, "Error: not enough memory for tick-space arrays (song duration x ppqn too large).\n", true);
        return {};
    } catch (const std::exception& e) {
        emit(cb, std::string("Error: ") + e.what() + "\n", true);
        return {};
    }
}

} // namespace fmcg_stream
