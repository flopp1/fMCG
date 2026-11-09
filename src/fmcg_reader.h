// fmcg_reader.h -- engine-private: the MIDI reader shared by every scan path.
//
// DataSrc (byte source), ByteStream / RawStream (chunked and zero-copy
// streams), the track-dedup store helpers, the per-tick sinks (TickSink for
// the single-pass scans, FrameSink for two-pass pass 2), parse_track_body
// (one MTrk) and scan_stream / scan_any (a whole file). Templates, so the
// scan TUs (fmcg_engine.cpp, fmcg_sweep.cpp, fmcg_parallel.cpp) each
// instantiate the hot loop with their own sink inlined.
#pragma once
#include "fmcg_engine.h"
#include "fmcg_engine_internal.h"
#include "fmcg_util.h"
#include "xxhash64.h"   // vendored single-header XXHash64 (Stephan Brumme, MIT)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace fmcg_stream {

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

// ---- chunked reader with bounded pushback -----------------------------------
//
// Draws from a DataSrc through a 64KB buffer and enforces a *content limit*
// (the declared MTrk length) at consume time: bytes beyond the limit are
// neither consumed nor discarded, so the next chunk's header stays intact.

class alignas(64) ByteStream {
    DataSrc& src;
    static constexpr size_t CAP = 1 << 19;   // 512 KB: fewer fill() round-trips (was 64 KB)
    // HEAP, not inline: walk_layer nests read_member frames one level per
    // container depth, and an inline 512KB window per frame overflowed the
    // thread stack at depth 3 (___chkstk_ms SIGSEGV, MIDI File). One
    // indirection per access is noise next to the decompression cost.
    std::unique_ptr<uint8_t[]> data{new uint8_t[CAP]};
    size_t lo = 0, hi = 0;
    size_t limit_remaining = (size_t)-1;
    size_t limit_total = 0;
    bool eof = false;
    uint64_t pulled = 0;   // bytes drawn from the source (progress fraction)
    const std::atomic<bool>* cancel_flag = nullptr;   // cooperative skip abort

    void fill() {
        if (eof) return;
        if (lo == hi) { lo = hi = 0; }
        if (hi == CAP) return;
        size_t got = src.read(data.get() + hi, CAP - hi);
        if (got == 0) eof = true;
        pulled += got;
        hi += got;
    }
public:
    explicit ByteStream(DataSrc& s) : src(s) {}
    // Cancel propagation for long skip() drains: a cancelled track's
    // remainder jump must not itself become a multi-GB un-cancellable crawl.
    void set_cancel_flag(const std::atomic<bool>* f) { cancel_flag = f; }

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
        return data.get()[lo++];
    }
    // Non-consuming byte read: for running-status lookahead. Same bounds
    // checks as get() but never advances -- no skip_back_one() round-trip.
    int peek() {
        if (limit_remaining == 0) return -1;
        if (lo == hi) { fill(); if (lo == hi) return -1; }
        return data.get()[lo];
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
                std::memmove(data.get(), data.get() + lo, run);
                lo = 0; hi = run;
            }
            fill();                           // top up (no-op at EOF)
            run = hi - lo;
        }
        size_t n = run;
        if (limit_remaining < n) n = limit_remaining;
        fast_p = data.get() + lo;
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
            if (cancel_flag && cancel_flag->load(std::memory_order_relaxed)) {
                // Drop the rest of the skip; leave limit as-is so the outer
                // loop's next cancel check ends the scan cleanly.
                limit_remaining -= (n - remaining);
                return;
            }
            size_t got = src.read(data.get(), std::min(CAP, remaining));
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
        std::memcpy(d, data.get() + lo, a);
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
                const uint8_t* p = data.get() + lo;
                uint32_t b0 = *p++;
                if (__builtin_expect(b0 < 0x80, 1)) { lo++; limit_remaining--; return b0; }
                uint64_t v = b0 & 0x7F;
                uint32_t b1 = *p++;
                v = (v << 7) | (b1 & 0x7F);
                if (b1 < 0x80) { lo = (size_t)(p - data.get()); limit_remaining -= 2; return v; }
                uint32_t b2 = *p++;
                v = (v << 7) | (b2 & 0x7F);
                if (b2 < 0x80) { lo = (size_t)(p - data.get()); limit_remaining -= 3; return v; }
                uint32_t b3 = *p++;
                v = (v << 7) | (b3 & 0x7F);
                lo = (size_t)(p - data.get());
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
    void set_cancel_flag(const std::atomic<bool>*) {}   // no decode in skip: no-op

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


// 64-bit FNV-1a over a byte range (the track's raw bytes).
// Track-content hash for dedup lookups. XXHash64 (vendored single header,
// MIT) — byte-at-a-time FNV-1a measured slower than the parser itself on
// large tracks and dominated dedup-mode scan time; XXHash64 processes 32
// bytes per round with a short dependency chain and removes that ceiling.
inline uint64_t dedup_hash_track(const uint8_t* p, size_t n,
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
constexpr size_t kDedupReadChunk = 4u << 20;   // 4 MiB

// Shared dedup store lock. Lookups take it too: a parallel worker's records
// merge mutates the vector, so unsynchronized iteration would be UB. One
// lock per track is negligible next to the parse itself.
inline std::mutex g_dedup_mutex;

// Look up a cacheable summary for (len, hash). Returns true when `out` was
// populated with a REPLAYABLE record (end_refcount == 0).
inline bool dedup_find_cachable(const TrackDataDedup& store, uint64_t len,
                                uint64_t hash, std::shared_ptr<const TrackSummary>& out) {
    std::lock_guard<std::mutex> lk(g_dedup_mutex);
    for (const auto& r : store.records)
        if (r->bytes == len && r->hash == hash && r->end_refcount == 0) { out = r; return true; }
    return false;
}




// Append-or-accumulate helper for the summaries' per-tick lists (ticks are
// visited in increasing order within a track, so the tail is the target).
inline void sum_add(std::vector<std::pair<uint64_t, uint64_t>>& v, uint64_t t, uint64_t d) {
    if (!v.empty() && v.back().first == t) v.back().second += d;
    else v.push_back({t, d});
}

// Replay a cached (end_refcount == 0) summary into `td`. In the parallel scan
// `td` is the worker's PRIVATE replay buffer (replays are never written to the
// shared arrays concurrently); the sequential scan passes the real TickData.
// NOTE: does not touch td.ntracks / td.total_events_seen -- callers account
// those (they may need to merge or defer).
inline void replay_track_summary(const TrackSummary& s, TickData& td,
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
    tempo_raw.insert(tempo_raw.end(), s.tempos.begin(), s.tempos.end());
    td.total_ons += s.ons;
    td.total_cc  += s.cc;
    if (s.max_tick > td.max_tick) td.max_tick = s.max_tick;
    if (s.desync) td.desync_tracks++;
}

// Approximate resident size of a summary (the per-tick lists dominate).
inline uint64_t summary_bytes(const TrackSummary& s) {
    return sizeof(TrackSummary)
         + s.cells.capacity() * sizeof(s.cells[0])
         + s.ccs.capacity()    * sizeof(s.ccs[0])
         + s.tempos.capacity() * sizeof(s.tempos[0]);
}

// Store one freshly parsed summary immediately (parallel workers call this
// per track so other workers can hit the record while the scan is running).
// Subject to the store budget so unique-content giant files cannot balloon.
inline void dedup_store_record(TrackSummary& s, TrackDataDedup& store) {
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

// Aborted via the MIDI-spec guard; the caller restarts the walk in TEMPO_ONLY
// mode (two-pass fallback for spec-breaking tick spans).
struct SpecAbort {};

enum class ScanMode { ACCUMULATE, TEMPO_ONLY };

// Where a track's per-tick totals go. parse_track_body batches the events of
// one tick (pending registers) and hands each tick to the sink once:
//   begin_track()                    before the track's first event
//   commit(tick, ons, delta, cc)     one tick's note-ons, net polyphony delta, CC count
//   end_track(final_tick)            after the held-note closure
// Ticks arrive in non-decreasing order within a track. TickSink fills the
// per-tick arrays (single-pass scans); FrameSink (two-pass fallback, below)
// maps each tick straight to its video frame.
struct TickSink {
    static constexpr bool kSpecGuard = true;    // the 2^28 tick guard applies
    static constexpr bool kDedup = true;        // track dedup may replay into td
    static constexpr bool kWarnDataOor = true;  // first walk: warn about data bytes > 127
    static constexpr bool kInlineFlush = false; // see parse_track_body's flush_pending
    TickData& td;
    void begin_track() {}
    void commit(uint64_t t, uint64_t ons, int64_t delta, uint64_t cc) {
        td.ensure(t);
        if (ons)   td.cells[t].ons   += ons;
        if (delta) td.cells[t].delta += delta;
        if (cc) {
            if (td.dense_cc.size() < td.cells.size()) td.dense_cc.resize(td.cells.size(), 0);
            td.dense_cc[t] += cc;
            td.total_cc += cc;
        }
        td.total_ons += ons;
    }
    void end_track(uint64_t t) { if (t > td.max_tick) td.max_tick = t; }
};

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
    std::function<bool(uint64_t)> spec_fire;         // spec guard (see above); arg = the tick past the limit
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

template <typename StreamT, typename Sink>
static uint64_t parse_track_body(StreamT& bs, bool accumulate, bool vel0_as_note_off,
                                 Sink& sink_ref, std::vector<TempoChange>& tempo_raw,
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
    uint64_t refcount[16][128] = {};   // black-MIDI tracks hold huge numbers of overlapping instances of one note
    // A local copy: sinks keep only references and per-track state, and a
    // local the compiler can see whole stays in registers across the
    // bucket writes (through the reference it was reloaded per event).
    Sink sink = sink_ref;
    if (accumulate) sink.begin_track();

    // ---- pending-tick registers (hot-path optimisation) ----------------
    // Black MIDIs stack thousands of events on the SAME tick via zero-delta
    // chains. Instead of a read-modify-write into the multi-GB cells[]
    // array per event, events accumulate in these register locals and are
    // committed to cells[] once, when the walk moves past the tick (or the
    // track ends). Cost drops from per-event memory ops to per-tick.
    bool     has_pending = false;
    uint64_t pend_tick = 0;
    uint64_t pend_ons = 0, pend_cc = 0;   // 64-bit: one tick of one track can exceed 2^32 events
    int64_t  pend_delta = 0;
    TrackSummary rec;                       // summary under construction
    uint64_t track_ev = 0;                  // events walked in THIS track (monotonic)
    uint64_t ping_at = 1000000;             // next event count that fires a ping
    uint64_t ping_last = 0;                 // track_ev at the previous ping
    uint32_t burst_polls = 0;               // throttles the window-boundary cancel check
    auto flush_body = [&]() __attribute__((always_inline)) {
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
        if (accumulate) sink.commit(pend_tick, pend_ons, pend_delta, pend_cc);
        has_pending = false; pend_ons = 0; pend_delta = 0; pend_cc = 0;
    };
    // Inlined or called, per sink (Sink::kInlineFlush): inlining the frame
    // sink's flush into the hot loop makes the two-pass walk ~15% faster,
    // while inlining the tick sink's costs the single-pass walk ~10%
    // (register pressure in the burst loop).
    auto flush_call = [&]() __attribute__((noinline)) { flush_body(); };
    auto flush_pending = [&]() __attribute__((always_inline)) {
        if constexpr (Sink::kInlineFlush) flush_body(); else flush_call();
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
            if (!hooks.spec_fire(tick)) return track_ev;
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
        // Window-boundary cancel check: fires once per buffer window
        // regardless of event density, so sparse tracks (huge delta-times,
        // <1M events total) still observe a cancel press promptly. The
        // per-1M-event check alone left the fallback standard parse of a
        // 20GB declared track grinding for minutes after Cancel. Every 64th
        // pass only: on tracks whose deltas are all nonzero the burst ends
        // after every event, and two atomic loads per event cost ~30% there.
        if ((++burst_polls & 63) == 0 && stop_cancelled()) {
            bs.set_limit(0);   // force a clean EOF for the caller's accounting
            return track_ev;   // cancelled; caller reports
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
            // Sysex / system common / meta cancel running status; real-time
            // bytes (F8..FE) leave it alone (MIDI spec).
            running = (status < 0xF0) ? status : (status >= 0xF8 && status != 0xFF) ? running : 0;
        }

        if (__builtin_expect(status >= 0xF0, 0)) {
            // Rare system/meta family, kept out of the channel hot path.
            if (status == 0xFF) {
                int type = bs.get();
                if (type < 0) break;
                uint64_t mlen = bs.vlq();
                if (type == 0x51 && mlen == 3) {
                    // Three sequenced reads: avail() only reports the bytes
                    // buffered in the current window, so gating on it dropped
                    // tempo metas that straddled a 512 KB window boundary;
                    // and operands of | are unsequenced (byte order was
                    // compiler-dependent). A truncated tempo is not applied.
                    const int b0 = bs.get(), b1 = bs.get(), b2 = bs.get();
                    const uint32_t us = (b2 < 0) ? 0u
                        : ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | (uint32_t)b2;
                    if (us > 0) {
                        tempo_raw.push_back({tick, 0.0, us, 60000000.0 / us});
                        if (sum) rec.tempos.push_back(tempo_raw.back());
                    }
                } else if (type == 0x58 && mlen == 4) {
                    // Time signature nn dd cc bb (only nn / dd matter); like a
                    // tempo, applied only when all 4 bytes are in the track.
                    const int nn = bs.get(), dd = bs.get(), cc = bs.get(), bb = bs.get();
                    (void)cc;
                    if (bb >= 0) {
                        tempo_raw.push_back(TempoChange::time_sig(tick, (uint8_t)nn, (uint8_t)dd));
                        if (sum) rec.tempos.push_back(tempo_raw.back());
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
    // Held-note closure is OPTIONAL (default off, see set_close_held_notes):
    // canonical polyphony is ons - offs. With it off nothing is released, the
    // summary records no end-of-track debt, and such tracks stay replayable.
    const bool close_held = close_held_notes();
    uint64_t rc_sum = 0;
    if (close_held)
    for (int ch = 0; ch < 16; ++ch)
        for (int n = 0; n < 128; ++n)
            rc_sum += refcount[ch][n];
    if (accumulate) {
        if (rc_sum) sink.commit(tick, 0, -(int64_t)rc_sum, 0);
        sink.end_track(tick);
    }
    if (sum) {
        if (rc_sum) {
            if (!rec.cells.empty() && rec.cells.back().first == tick)
                rec.cells.back().second.second -= (int64_t)rc_sum;
            else
                rec.cells.push_back({tick, {0, -(int64_t)rc_sum}});
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

// Result of a single walk over the image.

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
// Returns the number of events walked in this track (including early exits).// ---- single sequential walk over the image (stream-generic) ------------------
// `td` keeps the walk's counters (tracks, desyncs, events) and the dedup
// store; `sink` receives the per-tick totals (TickSink{td} for the
// single-pass scans).
template <typename StreamT, typename Sink>
static bool scan_stream(StreamT& bs, DataSrc& src, bool vel0_as_note_off, TickData& td, Sink& sink,
                        std::vector<TempoChange>& tempo_raw, uint16_t& out_division,
                        const ProgressCallbacks& cb, ScanMode mode) {
    const bool accumulate = (mode == ScanMode::ACCUMULATE);
    // In TEMPO_ONLY mode the accumulation arrays are never touched (that is
    // the point).
    if (!accumulate) td.reset();
    bs.set_cancel_flag(cb.cancel_flag);   // cancellable skip() drains

    // Continuous progress bookkeeping (checked once per ~1M events).
    using clock = std::chrono::steady_clock;
    const clock::time_point t_start = clock::now();
    uint64_t ev_count = 0;      // authoritative: full counts of completed tracks
    uint64_t ev_pinged = 0;     // live display: pinged prefix of the current track
    const uint64_t total_bytes = src.total_bytes();   // 0 = unknown fraction
    const uint64_t spec_limit = !Sink::kSpecGuard ? UINT64_MAX
                              : cb.spec_tick_limit ? cb.spec_tick_limit : ((uint64_t)1 << 28);
    bool spec_warned = false;              // one-shot: ask at most once per scan
    bool data_oor = !Sink::kWarnDataOor;   // one-shot: out-of-range data bytes warned (by parse_track_body)

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
        td.not_midi = true;   // process_streaming tries .cjcmcg, then reports
        return false;
    }
    int64_t hlen = rd32();
    if (hlen < 0) { emit(cb, "Error: truncated MIDI header.\n", true); return false; }
    int f2 = bs.get(), f1 = bs.get();   // format
    int n2 = bs.get(), n1 = bs.get();   // number of tracks
    bool mt_split = false;
    int d2 = bs.get(), d1 = bs.get();   // division
    if (f2 < 0 || f1 < 0 || n2 < 0 || n1 < 0 || d2 < 0 || d1 < 0) {
        emit(cb, "Error: truncated MIDI header.\n", true); return false;
    }
    if (hlen > 6) bs.skip((size_t)hlen - 6);
    // Division: 0xE728 is the SMBTE high byte; anything at or above it is
    // not a legal SMPTE value -- spam constructors use ffff (= 65535 ppqn,
    // "pure noteons" black MIDIs), and masking to 0x7FFF halved every
    // derived time. SMPTE high bytes (0xE2..0xE8, 25/24/29/15 fps) are
    // below this threshold, so they keep the classic interpretation.
    uint16_t division = (uint16_t)(((uint16_t)d2 << 8) | (uint16_t)d1);
    if (division == 0x4D54) {
        // Impossible division 'M','T': the header chunk was the 12-byte
        // constructor variant with no division field, and the header reader
        // consumed the next chunk tag's first half as the division (the RAW
        // walk keeps the following chunk aligned by counting those bytes
        // into the header chunk). Those files are standard type-1 MIDI at
        // 480 ppqn; reconstruct it instead of producing nonsensical times.
        division = 480;
        mt_split = true;   // the first chunk tag's 'M','T' are already consumed
    } else if (d2 < 0xE7) {
        division &= 0x7FFF;
    }
    if (division == 0) division = 480;
    out_division = division;

    // Dedup protocol state. `dedup_store` is a non-owning pointer into td
    // (reset() clears it) so the replay helper can consult the cache and the
    // final summary can merge its records.
    TrackDataDedup* dedup_store = nullptr;
    uint64_t dedup_replayed_events = 0;             // events from replays (not walked)
    if (Sink::kDedup && accumulate && td.dedup_enabled) {
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
    std::unique_ptr<uint8_t[]> dedup_buf;   // uninitialised on purpose (no zero-fill pass)
    size_t dedup_cap = 0;
    // First-occurrence gate: a replay needs byte-identical content, hence an
    // identical declared chunk length, so the first track of any length can
    // never hit. Lengths recorded as "seen" (including desynced/uncachable
    // tracks) skip the candidate copy, hash, and summary entirely -- historic
    // plain-parse behaviour -- collapsing the dedup overhead on files whose
    // tracks are mostly unique (e.g. a 4.24B-event file saving 1.7%).
    std::unordered_set<uint64_t> seen_len;

    clock::time_point tracks_reported{};
    auto report_tracks = [&](bool force) {
        if (!cb.on_tracks) return;
        const clock::time_point now = clock::now();
        if (!force && now - tracks_reported < std::chrono::milliseconds(100)) return;
        tracks_reported = now;
        cb.on_tracks(td.ntracks, 0);
    };
    // Per-track hooks: live counter from the pinged prefix (the authoritative
    // walked total is each body's return value; adding both double-counted
    // every pinged event), per-1M-event cancel, and the spec-violation prompt.
    auto make_hooks = [&]() {
        TrackBodyHooks hooks;
        hooks.cancelled_flag = cb.cancel_flag;
        hooks.ping = [&](uint64_t n) {
            ev_pinged += n;
            ping();
        };
        hooks.spec_fire = [&](uint64_t at) -> bool {
            if (spec_warned) return true;
            const int choice = cb.on_spec_violation
                ? cb.on_spec_violation(at, td.memory_bytes()) : 0;
            if (choice == 2) { emit(cb, "  Cancelled.\n"); throw ScanCancelled{}; }
            if (choice == 1) throw SpecAbort{};
            spec_warned = true;
            return true;
        };
        return hooks;
    };

    while (true) {
        if (cb.cancel_flag && cb.cancel_flag->load(std::memory_order_relaxed)) {
            emit(cb, "  Cancelled.\n"); throw ScanCancelled{};
        }
        report_tracks(false);
        int64_t tag;
        if (mt_split) {          // 12-byte MThd: finish the tag whose 'M','T' became the division
            mt_split = false;
            const int a = bs.get(), c = bs.get();
            tag = (a < 0 || c < 0) ? -1 : (int64_t)(0x4D540000u | ((unsigned)a << 8) | (unsigned)c);
        } else tag = rd32();
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
                const TrackBodyHooks hooks = make_hooks();
                ev_count += parse_track_body(bs, accumulate, vel0_as_note_off, sink, tempo_raw,
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
                const TrackBodyHooks hooks = make_hooks();
                ev_count += parse_track_body(bs, accumulate, vel0_as_note_off, sink, tempo_raw,
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
            if (dedup_cap < (size_t)len) {
                dedup_buf.reset(new uint8_t[(size_t)len]);
                dedup_cap = (size_t)len;
            }
            uint8_t* bufp = dedup_buf.get();
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
                const TrackBodyHooks hooks = make_hooks();
                rec.events = parse_track_body(tbs, accumulate, vel0_as_note_off, sink, tempo_raw,
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
            const TrackBodyHooks hooks = make_hooks();
            ev_count += parse_track_body(bs, accumulate, vel0_as_note_off, sink, tempo_raw,
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
    report_tracks(true);
    td.total_events_seen = ev_count + dedup_replayed_events;
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
constexpr int FMCG_FINE_BINS = 8;

struct FrameBuckets {
    std::vector<uint64_t> ons, cc;   // 64-bit: a frame can hold > 2^32 events
    std::vector<int64_t> deltas;   // signed: transient negatives must not wrap
    // Fine-grid mirrors (FMCG_FINE_BINS bins per frame) feeding the sub-frame
    // peak computation, identical to the single-pass engine's.
    std::vector<uint64_t> fons, fcc;
    std::vector<int64_t>  ffdeltas;
    size_t nbins = 0;
    uint64_t total_ons = 0, total_cc = 0;
    uint64_t total_events_seen = 0;   // one walk's worth, same metric as the pings
    uint64_t max_tick = 0;            // highest tick seen (song-horizon source)
    size_t nframes = 0;

    [[gnu::noinline]] void ensure(size_t fi) {
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
    [[gnu::noinline]] void fine_ensure(size_t bin) {
        if (bin >= fons.size()) {
            size_t n = fons.size() ? fons.size() * 2 : 4096;
            if (n < bin + 1) n = bin + 1;
            fons.resize(n, 0); ffdeltas.resize(n, 0); fcc.resize(n, 0);
            nbins = n;
        }
    }
    // Bucket growth is on demand; guarantee capacity for the final sizing.
    void reserve_exact(size_t n) {
        ons.resize(n, 0); deltas.resize(n, 0);
        if (!cc.empty()) cc.resize(n, 0);
        if (nframes < n) nframes = n;
        fons.resize(n * FMCG_FINE_BINS, 0); ffdeltas.resize(n * FMCG_FINE_BINS, 0);
        fcc.resize(n * FMCG_FINE_BINS, 0);
        if (nbins < n * FMCG_FINE_BINS) nbins = n * FMCG_FINE_BINS;
    }
    // Note events are addressed by FINE bin; the frame index is bin/8.
    void add_notes(size_t bin, uint64_t n_ons, int64_t delta) {
        const size_t fi = bin / FMCG_FINE_BINS;
        if (__builtin_expect(fi >= nframes, 0)) ensure(fi);
        ons[fi] += n_ons; deltas[fi] += delta; total_ons += n_ons;
        if (__builtin_expect(bin >= fons.size(), 0)) fine_ensure(bin);
        fons[bin] += n_ons; ffdeltas[bin] += delta;
    }
    // CC events by fine bin too (frame = bin / 8, the same frame as before:
    // scaling by 8 is exact in floating point), for the sub-frame peak CPS.
    void add_cc(size_t bin, uint64_t n) {
        const size_t fi = bin / FMCG_FINE_BINS;
        if (fi >= nframes) ensure(fi);
        cc[fi] += n; total_cc += n;
        if (bin >= fcc.size()) fine_ensure(bin);
        fcc[bin] += n;
    }
};

// Pass-2 sink: each tick's totals go straight to the frame (CC) or fine bin
// (notes) the tick falls in, through the complete tempo map from pass 1. The
// frame formula is kept character-for-character identical to the original
// per-event pass-2 walk,
//   sec = tm[conv].time_sec + (t - tm[conv].tick)*us*inv; f = sec*fps,
// so batching a tick's events changes no bin. The tempo segment is hoisted
// into locals and only reloaded when the anchor moves.
struct FrameSink {
    static constexpr bool kSpecGuard = false;    // the whole point of this pass
    static constexpr bool kDedup = false;
    static constexpr bool kWarnDataOor = false;  // pass 1 already warned
    static constexpr bool kInlineFlush = true;
    FrameBuckets& fb;
    const std::vector<TempoChange>& tm;
    double inv, fps;
    size_t conv = 0;
    double seg_time = 0.0, seg_us = 0.0;
    uint64_t tick_base = 0;
    uint64_t seg_end = UINT64_MAX;     // first tick of the next tempo segment
    // Cached fine bin: every tick in [bin_t0, bin_t1) maps to cur_bin. Within
    // a tempo segment the bin formula is non-decreasing in the tick (each step
    // -- integer to double, multiply by a positive constant, add -- is
    // monotone under round-to-nearest), so once bin_of(c) == bin_of(t) for
    // some c > t, every tick in between has that bin too. Black MIDIs put
    // hundreds of ticks in one bin; this skips the FP work for all but one.
    uint64_t bin_t0 = 1, bin_t1 = 0;   // empty
    size_t cur_bin = 0;

    FrameSink(FrameBuckets& b, const std::vector<TempoChange>& t, uint16_t ppqn, double f)
        : fb(b), tm(t), inv(1.0 / ((double)ppqn * 1e6)), fps(f) { load(); }
    void load() {
        seg_time  = tm.empty() ? 0.0 : tm[conv].time_sec;
        seg_us    = tm.empty() ? 0.0 : (double)tm[conv].us_per_quarter;
        tick_base = tm.empty() ? 0ull : tm[conv].tick;
        seg_end   = conv + 1 < tm.size() ? tm[conv + 1].tick : UINT64_MAX;
        bin_t0 = 1; bin_t1 = 0;
    }
    void begin_track() { conv = 0; load(); }   // tick restarts at 0 per track
    double sec_of(uint64_t t) const { return seg_time + (double)(t - tick_base) * seg_us * inv; }
    size_t bin_of(uint64_t t) const {
        const double f = sec_of(t) * fps * (double)FMCG_FINE_BINS;
        return f <= 0.0 ? 0 : (size_t)(int64_t)f;   // f < 2^63: the signed conversion is one instruction
    }
    [[gnu::noinline]] size_t note_bin_slow(uint64_t t) {
        const size_t b = bin_of(t);
        // Predicted last tick of bin b (inverse formula), clamped to the segment, then verified.
        const double d = ((double)(b + 1) / (fps * (double)FMCG_FINE_BINS) - seg_time) / (seg_us * inv);
        uint64_t c = t;
        if (d > (double)(t - tick_base) + 1.0)
            c = d >= 1.8e19 ? UINT64_MAX : tick_base + (uint64_t)d - 1;
        if (c >= seg_end) c = seg_end - 1;
        if (c < t) c = t;
        for (int k = 0; k < 4 && c > t && bin_of(c) != b; ++k) c = t + (c - t) / 2;
        if (bin_of(c) != b) c = t;
        cur_bin = b; bin_t0 = t; bin_t1 = c + 1;
        return b;
    }
    size_t note_bin(uint64_t t) {
        const size_t b = (t >= bin_t0 && t < bin_t1) ? cur_bin : note_bin_slow(t);
#ifdef FMCG_CHECK_BINS
        if (b != bin_of(t)) { fprintf(stderr, "FrameSink bin cache mismatch at tick %llu\n", (unsigned long long)t); abort(); }
#endif
        return b;
    }
    [[gnu::always_inline]] inline void commit(uint64_t t, uint64_t n_ons, int64_t delta, uint64_t cc) {
        if (t >= seg_end) {
            while (conv + 1 < tm.size() && tm[conv + 1].tick <= t) ++conv;
            load();
        }
        if (n_ons || delta || cc) {
            const size_t b = note_bin(t);
            if (n_ons || delta) fb.add_notes(b, n_ons, delta);
            if (cc) fb.add_cc(b, cc);
        }
    }
    void end_track(uint64_t t) { if (t > fb.max_tick) fb.max_tick = t; }
};

// ---- dispatch: raw mmap walk when the source exposes an image -----------------

template <typename Sink>
static bool scan_any(DataSrc& src, bool vel0_as_note_off, TickData& td, Sink& sink,
                     std::vector<TempoChange>& tempo_raw, uint16_t& out_division,
                     const ProgressCallbacks& cb, ScanMode mode) {
    size_t n = 0;
    if (const uint8_t* img = src.image(n); img && n > 0) {
        RawStream bs(img, n);
        return scan_stream(bs, src, vel0_as_note_off, td, sink, tempo_raw, out_division, cb, mode);
    }
    ByteStream bs(src);
    return scan_stream(bs, src, vel0_as_note_off, td, sink, tempo_raw, out_division, cb, mode);
}

inline bool scan_any(DataSrc& src, bool vel0_as_note_off, TickData& td,
                     std::vector<TempoChange>& tempo_raw, uint16_t& out_division,
                     const ProgressCallbacks& cb, ScanMode mode = ScanMode::ACCUMULATE) {
    TickSink sink{td};
    return scan_any(src, vel0_as_note_off, td, sink, tempo_raw, out_division, cb, mode);
}


// ---- defined in the scan modules ----------------------------------------------

// fmcg_sweep.cpp: tempo map, tick -> frame sweep, two-pass pass 2.
std::vector<TempoChange> build_tempo_map(const std::vector<TempoChange>& tempo_raw, uint16_t ppqn);
std::vector<FrameStats> sweep_to_frames(TickData& td, std::vector<TempoChange>& tempo_raw,
                                        uint16_t ppqn, double fps, bool with_cc = false,
                                        double end_delay = 0.0, const ProgressCallbacks* cb = nullptr);
bool scan_any_frames(DataSrc& src, bool vel0_as_note_off, FrameBuckets& fb,
                     const std::vector<TempoChange>& tm, uint16_t ppqn, double fps,
                     const ProgressCallbacks& cb);
std::vector<FrameStats> buckets_to_frames(FrameBuckets& fb, double fps,
                                          const std::vector<TempoChange>& tm,
                                          double max_time_sec, bool with_cc, uint16_t ppqn,
                                          double end_delay = 0.0,
                                          const ProgressCallbacks* cb = nullptr);

// fmcg_parallel.cpp: per-track parallel scan of a plain mmap'd file.
int scan_parallel(DataSrc& src, bool vel0_as_note_off, TickData& td,
                  std::vector<TempoChange>& tempo_raw, int nthreads,
                  const ProgressCallbacks& cb, bool track_dedup,
                  uint16_t& out_division);

} // namespace fmcg_stream
