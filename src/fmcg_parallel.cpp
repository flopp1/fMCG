// fmcg_parallel.cpp -- per-track parallel scan of plain mmap'd MIDI files.
#include "fmcg_reader.h"

#include <algorithm>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fmcg_stream {

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
                              TickData& td,
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
                              std::atomic<uint64_t>* ev_pub = nullptr,
                              const std::vector<char>* len_once = nullptr) {
    uint64_t events = 0;
    // Spec-violation protocol shared by all workers. Returns false when the
    // scan must stop (cancel or two-pass abort). Exactly one worker prompts;
    // the others wait for the shared answer.
    auto spec_stop = [&](uint64_t at) -> bool {
        int c = spec_choice.load(std::memory_order_acquire);
        if (c >= 0) {
            if (c == 2) { cancelled.store(true, std::memory_order_relaxed); return false; }
            if (c == 1) { spec_abort = true; return false; }
            return true;
        }
        bool expect = false;
        if (spec_prompted.compare_exchange_strong(expect, true, std::memory_order_acq_rel)) {
            c = cb.on_spec_violation ? cb.on_spec_violation(at, td.memory_bytes()) : 0;
            spec_choice.store(c, std::memory_order_release);
            if (c == 2) { cancelled.store(true, std::memory_order_relaxed); return false; }
            if (c == 1) { spec_abort = true; return false; }
            return true;
        }
        // The answer waits on a modal dialog (seconds): sleep, don't spin a core.
        while ((c = spec_choice.load(std::memory_order_acquire)) < 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
    hooks.spec_fire = [&](uint64_t at) { return spec_stop(at); };

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
            TickSink ts{td};
            rec.events = parse_track_body(bs, /*accumulate=*/true, vel0_as_note_off,
                                          ts, tempo_raw, cb, spec_limit, hooks,
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
        TickSink ts{td};
        const uint64_t walked = parse_track_body(bs, /*accumulate=*/true, vel0_as_note_off,
                                                 ts, tempo_raw, cb, spec_limit, hooks,
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
int scan_parallel(DataSrc& src, bool vel0_as_note_off, TickData& td,
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
        // Same division rule as scan_stream: only d2 >= 0xE7 is not ppqn.
        uint16_t division = (uint16_t)(((img[12] << 8) | img[13]) & 0xFFFF);
        if ((img[12] & 0xFF) < 0xE7) division &= 0x7FFF;
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
    std::vector<std::atomic<uint64_t>> worker_tracks(ngroups);   // tracks finished per group
    for (auto& a : worker_tracks) a.store(0, std::memory_order_relaxed);
    auto tracks_finished = [&]() {
        uint64_t t = 0;
        for (const auto& a : worker_tracks) t += a.load(std::memory_order_relaxed);
        return t;
    };

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
            unsigned reports = 0;
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
                if (cb.on_tracks && (++reports & 1) == 0) cb.on_tracks(tracks_finished(), spans.size());
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
            uint64_t started = 0;   // a track starts => the previous one in this group finished
            auto on_track = [&, g](uint64_t off) {
                worker_at[g].store(off, std::memory_order_relaxed);
                if (started) worker_tracks[g].store(started, std::memory_order_relaxed);
                ++started;
            };
            try {
                partial_events[g] = parse_track_range(
                    img, spans, b, e, vel0_as_note_off, partial[g],
                    partial_tempo[g], cb, spec_prompted, spec_choice, cancelled,
                    spec_abort, data_oor, spec_limit,
                    track_dedup ? &td.dedup : nullptr, on_track,
                    &worker_ev[g], &len_once);
            } catch (const ScanCancelled&) {
                // Growth-cancel (or an escaped poll) inside this worker:
                // flag the shared cancel so peers stop, then exit cleanly.
                cancelled.store(true, std::memory_order_relaxed);
            }
            worker_at[g].store(spans[e - 1].off + spans[e - 1].len,
                               std::memory_order_relaxed);
            worker_tracks[g].store(e - b, std::memory_order_relaxed);
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
    if (cb.on_tracks) cb.on_tracks(tracks_finished(), spans.size());

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

} // namespace fmcg_stream
