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
    };

    // Reads the selected entry's bytes out of a container via data blocks.
    class ArchiveFeeder : public Feeder {
    public:
        struct archive* a = nullptr;
        std::vector<uint8_t> staging;   // libarchive's block buffer is invalidated
        size_t s_lo = 0;                // by the next call, so blocks are copied
        bool saw_error = false;

        explicit ArchiveFeeder(const std::string& path) {
            a = archive_read_new();
            archive_read_support_filter_all(a);
            archive_read_support_format_all(a);
            archive_read_support_format_raw(a);   // bare filter-compressed files
            if (archive_read_open_filename(a, path.c_str(), 10240) != ARCHIVE_OK) {
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
        ~ArchiveFeeder() override { if (a) archive_read_free(a); }

        size_t pull(void* dst, size_t n) override {
            uint8_t* d = (uint8_t*)dst;
            size_t out = 0;
            while (out < n) {
                if (s_lo == staging.size()) {
                    const void* buf; size_t bsz; la_int64_t off;
                    int r = archive_read_data_block(a, &buf, &bsz, &off);
                    if (r == ARCHIVE_EOF) break;
                    if (r != ARCHIVE_OK) { saw_error = true; break; }
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

    private:
        struct archive_entry* entry = nullptr;
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
            if (hdr && hdr_n) buf.assign(hdr, hdr + hdr_n);   // peeked header bytes
            fprintf(stderr, "DBG DecompFeeder ctor hdr_n=%zu\n", hdr_n);
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
        ~DecompFeeder() override { if (a) archive_read_free(a); }

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
            fprintf(stderr, "DBG unwrap layer=%d got=%zu magic=%02x%02x%02x%02x\n", (int)stack.size(), got, hdr[0], hdr[1], hdr[2], hdr[3]);
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
    uint64_t total_events_seen = 0;   // every event walked (ons+offs+CC+meta), matches live counter

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

bool scan_image(DataSrc& src, bool vel0_as_note_off, TickData& td,
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
    td.total_events_seen = ev_total + ev_count;   // identical metric to the live pings
    return true;
}

// ---- anchor-interpolated sweep: tick buckets -> FrameStats --------------------

std::vector<FrameStats> sweep_to_frames(TickData& td,
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
        uint64_t o = ons[(size_t)t];
        int64_t  d = deltas[(size_t)t];
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
        const uint64_t nev = td.total_events_seen;   // all events walked, same as the live counter
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

