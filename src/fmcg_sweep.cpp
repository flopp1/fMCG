// fmcg_sweep.cpp -- tick space -> per-frame stats: the tempo map, the
// meter (beats / measures), the single-pass sweep and the two-pass fallback's
// pass 2 (frame buckets). See fmcg_reader.h for the reader they consume.
#include "fmcg_reader.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace fmcg_stream {

// ---- anchor-interpolated sweep: tick buckets -> FrameStats --------------------

// {plph-max}, {nps-max} and {cps-max} are all EXACT in the single-pass engine, no bins
// involved. The sweep walks every tick in strictly increasing time order and a
// tick is the finest time unit MIDI has (same-tick events are simultaneous by
// definition), so:
//   - peak polyphony = running max of the net-per-tick polyphony state;
//   - peak NPS       = max note-ons in any literal 1.000000-second window,
//     maintained with a sliding-window deque over (tick_time, ons) -- entries
//     are distinct note-carrying ticks, so its size scales with ticks-per-
//     second, not events-per-second (a million-note tick is one entry).
// Window convention: closed [t-1, t] -- an onset exactly 1.000 s older than
// the newest one still counts; times are exact integers (see the sweep).

// Peak NPS in the TWO-PASS fallback instead uses bins of width
// 1/(FMCG_FINE_BINS*fps) seconds (see FrameBuckets below): that architecture
// never materializes tick space, so the deque cannot be built there and the
// bin-quantized window is the accepted approximation.

// Build the tempo map exactly like the parallel engine: default entry,
// stable sort by tick, dedup keeping the LAST change at each tick, then
// anchor times. Shared by the tick-space sweep and the frame-bucket path.
std::vector<TempoChange> build_tempo_map(const std::vector<TempoChange>& tempo_raw,
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

// ---- meter (time signatures -> beat / measure numbers) ------------------------
//
// Time-signature records (TempoChange::is_time_sig) are applied like tempo:
// stable-sorted by tick, the last one at a tick wins; default 4/4 from tick 0.
// A record that restates the signature in force changes nothing (black MIDIs
// repeat them in every copied track). A real change starts a new measure: a
// measure (or beat) cut short by it still counts as one. Numerators of 0 and
// denominators past 2^32 are ignored as garbage.
//
// The song covers ticks [0, end_tick): a frame at or past end_tick (the final
// frame, the end-delay tail) shows the last tick's values, so the totals and
// the final frame agree. Beat and measure numbers are 1-based.
namespace {
struct MeterSeg { uint64_t tick; uint8_t num, dd; uint64_t beats0, measures0; };
using u128m = unsigned __int128;
inline uint64_t sat64(u128m v) { return v > (u128m)UINT64_MAX ? UINT64_MAX : (uint64_t)v; }
// floor / ceil of dt ticks in beats (per = 1) or measures (per = numerator):
// dt * 2^dd / (4 * ppqn * per).
inline u128m meter_units(uint64_t dt, uint8_t dd, uint16_t ppqn, uint64_t per, bool ceil) {
    const u128m num = (u128m)dt << dd, den = (u128m)4 * ppqn * per;
    return ceil ? (num + den - 1) / den : num / den;
}
} // namespace

SongMeter apply_meter(std::vector<FrameStats>& frames, const std::vector<TempoChange>& tempo_raw,
                      uint16_t ppqn, uint64_t end_tick) {
    if (ppqn == 0) ppqn = 480;
    std::vector<TempoChange> ts;
    for (const TempoChange& t : tempo_raw)
        if (t.is_time_sig() && t.ts_dd <= 32) ts.push_back(t);
    std::stable_sort(ts.begin(), ts.end(),
                     [](const TempoChange& a, const TempoChange& b) { return a.tick < b.tick; });
    std::vector<MeterSeg> segs{{0, 4, 2, 0, 0}};
    for (size_t i = 0; i < ts.size(); ++i) {
        if (i + 1 < ts.size() && ts[i + 1].tick == ts[i].tick) continue;   // last one at a tick wins
        MeterSeg& b = segs.back();
        if (ts[i].ts_num == b.num && ts[i].ts_dd == b.dd) continue;         // restatement
        if (ts[i].tick == b.tick) { b.num = ts[i].ts_num; b.dd = ts[i].ts_dd; continue; }   // only at tick 0
        const uint64_t len = ts[i].tick - b.tick;
        const MeterSeg n{ts[i].tick, ts[i].ts_num, ts[i].ts_dd,
                         sat64(b.beats0 + meter_units(len, b.dd, ppqn, 1, true)),
                         sat64(b.measures0 + meter_units(len, b.dd, ppqn, b.num, true))};
        segs.push_back(n);
    }
    auto seg_of = [&](uint64_t t) -> const MeterSeg& {   // the segment holding tick t
        size_t lo = 0, hi = segs.size();
        while (hi - lo > 1) {
            const size_t m = (lo + hi) / 2;
            if (segs[m].tick <= t) lo = m; else hi = m;
        }
        return segs[lo];
    };
    SongMeter out;
    const uint64_t last = end_tick > 0 ? end_tick - 1 : 0;   // last tick inside the song
    {
        const MeterSeg& g = seg_of(last);
        const uint64_t span = end_tick > g.tick ? end_tick - g.tick : 0;
        out.beats = std::max<uint64_t>(1, sat64(g.beats0 + meter_units(span, g.dd, ppqn, 1, true)));
        out.measures = std::max<uint64_t>(1, sat64(g.measures0 + meter_units(span, g.dd, ppqn, g.num, true)));
        out.max_num = 0;
        for (const MeterSeg& m : segs)
            if (m.tick <= last && m.num > out.max_num) out.max_num = m.num;
    }
    for (FrameStats& f : frames) {
        const uint64_t t = std::min<uint64_t>(f.tick > 0 ? (uint64_t)f.tick : 0, last);
        const MeterSeg& g = seg_of(t);
        const uint64_t b = sat64(meter_units(t - g.tick, g.dd, ppqn, 1, false));
        const uint64_t m = sat64(meter_units(t - g.tick, g.dd, ppqn, g.num, false));
        f.beat = std::min(out.beats, g.beats0 + b + 1);
        f.measure = std::min(out.measures, g.measures0 + m + 1);
        f.measure_beat = (uint32_t)(b - m * g.num + 1);
        f.ts_num = g.num;
        f.ts_dd = g.dd;
    }
    return out;
}

std::vector<FrameStats> sweep_to_frames(TickData& td, std::vector<TempoChange>& tempo_raw,
                                        uint16_t ppqn, double fps, bool with_cc,
                                        double end_delay, const ProgressCallbacks* cb) {
    std::vector<TempoChange> tm = build_tempo_map(tempo_raw, ppqn);

    const uint64_t nticks = td.max_tick + 1;
    std::vector<TickData::TickCell>& cells = td.cells;
    // Event-driven mode: the layered walk delivers sparse cells and/or
    // arithmetic runs whose tick span can be billions of ticks; only touched
    // ticks are visited. The dense-only path (standard scans) keeps the
    // original per-tick loop and its array padding.
    const bool event_mode = !td.runs.empty() || !td.sparse.empty();
    if (!event_mode) {
        if (cells.size() < nticks)
            cells.resize((size_t)nticks, TickData::TickCell{0, 0});
        if (with_cc && td.dense_cc.size() < nticks)
            td.dense_cc.resize((size_t)nticks, 0);   // dense_cc is sized to last-written tick only
    }
    const double inv = 1.0 / ((double)ppqn * 1e6);
    const size_t W = std::max<size_t>(1, (size_t)std::round(fps));   // >= 1 frame (fps < 0.5)

    std::vector<FrameStats> out;
    {   // Pre-size: song length from the tempo map + the tail (no regrowth copies).
        const TempoChange& last = tm.back();
        const double song_s = last.time_sec + (double)(td.max_tick - std::min(td.max_tick, last.tick))
                              * (double)last.us_per_quarter / ((double)ppqn * 1e6);
        const double est = (song_s + std::max(0.0, end_delay)) * fps + 2.0;
        if (est > 0 && est < 2e8) out.reserve((size_t)est);
    }
    uint64_t cum = 0;
    uint64_t cum_cc = 0;
    int64_t poly = 0, peak_poly = 0;
    double peak_nps = 0.0, peak_cps = 0.0;
    size_t cur_frame = 0;
    size_t conv = 0;   // anchor for tick->sec conversion (walked by tick)
    size_t bpmw = 0;   // tempo walk for frame BPM (walked by time)
    size_t song_frames = SIZE_MAX;   // set before the tail loop; frames >= it rest at zero

    // Exact integer time for the NPS window: T(t) in units of 1/ppqn
    // microseconds = sum over tempo segments of (ticks x us_per_quarter).
    // One second is exactly 1e6 * ppqn of these units, so the window test
    // has no floating-point boundary fuzz.
    typedef unsigned __int128 u128;
    std::vector<u128> tanchor(tm.size(), 0);
    for (size_t i = 1; i < tm.size(); ++i)
        tanchor[i] = tanchor[i - 1] + (u128)(tm[i].tick - tm[i - 1].tick) * tm[i - 1].us_per_quarter;
    const u128 one_sec = (u128)1000000u * ppqn;

    // Tick-space inversion shared by the frame push: anchor walked by time.
    size_t iconv = 0;                       // anchor for sec->tick conversion
    auto tick_at = [&](double sec) -> uint64_t {
        while (iconv + 1 < tm.size() && tm[iconv + 1].time_sec <= sec) iconv++;
        return tm[iconv].tick + (uint64_t)std::max(0.0, (sec - tm[iconv].time_sec)
                    * ((double)ppqn * 1e6) / (double)tm[iconv].us_per_quarter);
    };

    // ---- exact peak-NPS / peak-CPS sliding windows -------------------------
    // Deque of (T, count) for the distinct ticks carrying note-ons (CC events)
    // inside the last second. Window is CLOSED: an event exactly 1.000 s older
    // than the newest one still counts (user decision 2026-10-10).
    struct PeakWindow {
        std::deque<std::pair<u128, uint64_t>> q;
        uint64_t sum = 0;    // events inside the 1 s window
        uint64_t peak = 0;   // exact maximum (monotonic)
        void add(u128 T, uint64_t n, u128 one_sec) {
            q.emplace_back(T, n);
            sum += n;
            while (!q.empty() && q.front().first + one_sec < T) {
                sum -= q.front().second;
                q.pop_front();
            }
            if (sum > peak) peak = sum;
        }
    };
    PeakWindow nps_win, cps_win;

    auto push_frame = [&](size_t k) {
        // Frame-end tempo: a tempo change inside frame k must show from k on
        // (the very last tempo is never hidden by the frame boundary).
        while (bpmw + 1 < tm.size() && tm[bpmw + 1].time_sec <= (double)(k + 1) / fps) bpmw++;
        uint64_t prev_cum = (k >= W) ? out[k - W].cumulative_notes : 0;
        uint64_t prev_cc = (k >= W) ? out[k - W].cumulative_cc : 0;
        double nps = (double)(cum - prev_cum);
        double cps = (double)(cum_cc - prev_cc);
        int64_t cur_poly = poly;
        if (k >= song_frames) {          // end-delay tail: all event activity
            nps = 0;                     // is over -- nps, cps and polyphony
            cps = 0;                     // drop to their resting values
            cur_poly = 0;
        } else {
            peak_nps = std::max(peak_nps, (double)nps_win.peak);
            peak_cps = std::max(peak_cps, (double)cps_win.peak);
            // peak_poly is maintained exactly per tick in the tick walk;
            // already monotonic and covers every event of frames < tail.
        }
        out.push_back({k, (double)k / fps, cum, cum_cc, nps, peak_nps,
                       std::max<int64_t>(0, cur_poly), peak_poly, tm[bpmw].bpm, (int64_t)tick_at((double)k / fps)});
        out.back().cc_per_second = cps;
        out.back().peak_cps = peak_cps;
    };

    // One touched tick, visited in strictly increasing tick order; poly/peak
    // update per tick in tick order. A CC-only tick lands in its own frame
    // like any other (so {cc} and {cps} count it from that frame on).
    auto process_tick = [&](uint64_t t, uint64_t o, int64_t d, uint64_t c) {
        if (o == 0 && d == 0 && c == 0) return;
        while (conv + 1 < tm.size() && tm[conv + 1].tick <= t) conv++;
        double sec = tm[conv].time_sec + (double)(t - tm[conv].tick) * (double)tm[conv].us_per_quarter * inv;
        size_t fi = (size_t)(sec * fps);

        // Frames between the last processed tick and this one are event-free;
        // their end-of-frame state is the current running state.
        while (cur_frame < fi) push_frame(cur_frame++);

        if (o > 0 || c > 0) {
            const u128 T = tanchor[conv] + (u128)(t - tm[conv].tick) * tm[conv].us_per_quarter;
            if (o > 0) nps_win.add(T, o, one_sec);
            if (c > 0) cps_win.add(T, c, one_sec);
        }

        cum += o;
        poly += d;
        if (poly > peak_poly) peak_poly = poly;   // exact per-tick peak
        if (c) cum_cc += c;
    };

    // Cancel poll + live progress, every 16M visited ticks.
    uint64_t visited = 0;
    auto poll = [&](uint64_t t) {
        if (!cb || (visited++ & 0xFFFFFF) != 0) return;
        if (cb->cancel_flag && cb->cancel_flag->load(std::memory_order_relaxed)) {
            emit(*cb, "  Cancelled.\n");
            throw ScanCancelled{};
        }
        if (cb->on_scan_progress)
            cb->on_scan_progress(td.total_events_seen, 0.0, 0.0,
                                 (double)t / (double)std::max<uint64_t>(nticks, 1));
        if (cb->on_progress)
            cb->on_progress("Sweeping", (int)(100.0 * (double)t / (double)std::max<uint64_t>(nticks, 1)));
    };

    if (!event_mode) {
        for (uint64_t t = 0; t < nticks; ++t) {
            poll(t);
            const TickData::TickCell cell = cells[(size_t)t];
            const uint64_t c = (with_cc && t < td.dense_cc.size()) ? td.dense_cc[(size_t)t] : 0;
            process_tick(t, cell.ons, cell.delta, c);
        }
    } else {
        // k-way merge of dense cells, sparse cells and every run (min-heap on
        // each run's next tick). Runs may overlap anything.
        struct RunCur { uint64_t next; size_t idx; uint64_t left; };
        auto cmp = [](const RunCur& a, const RunCur& b) { return a.next > b.next; };
        std::vector<RunCur> heap;
        for (size_t i = 0; i < td.runs.size(); ++i)
            if (td.runs[i].count) heap.push_back({td.runs[i].start, i, td.runs[i].count});
        std::make_heap(heap.begin(), heap.end(), cmp);
        size_t si = 0;          // sparse cursor
        uint64_t di = 0;        // dense cursor
        const uint64_t dn = cells.size();
        const uint64_t kNone = UINT64_MAX;
        for (;;) {
            uint64_t t = kNone;
            if (di < dn) t = di;
            if (si < td.sparse.size() && td.sparse[si].tick < t) t = td.sparse[si].tick;
            if (!heap.empty() && heap.front().next < t) t = heap.front().next;
            if (t == kNone) break;
            poll(t);
            uint64_t o = 0, c = 0;
            int64_t d = 0;
            if (di < dn && t == di) {
                o += cells[(size_t)t].ons; d += cells[(size_t)t].delta;
                if (t < td.dense_cc.size()) c += td.dense_cc[(size_t)t];
                ++di;
            }
            while (si < td.sparse.size() && td.sparse[si].tick == t) {
                o += td.sparse[si].ons; d += td.sparse[si].delta; c += td.sparse[si].cc;
                ++si;
            }
            while (!heap.empty() && heap.front().next == t) {
                std::pop_heap(heap.begin(), heap.end(), cmp);
                RunCur& rc = heap.back();
                const TickData::TickRun& rn = td.runs[rc.idx];
                o += rn.ons; d += rn.delta; c += rn.cc;
                if (--rc.left) { rc.next += rn.step; std::push_heap(heap.begin(), heap.end(), cmp); }
                else heap.pop_back();
            }
            if (!with_cc) c = 0;
            process_tick(t, o, d, c);
        }
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

// Pass 2 of the two-pass fallback: the same walk, events bucketed by frame.
bool scan_any_frames(DataSrc& src, bool vel0_as_note_off, FrameBuckets& fb,
                            const std::vector<TempoChange>& tm, uint16_t ppqn, double fps,
                            const ProgressCallbacks& cb) {
    TickData counters;                     // tracks / events; no tick arrays
    std::vector<TempoChange> tempo_again;  // pass 1 already has the map
    uint16_t division = ppqn;
    FrameSink sink(fb, tm, ppqn, fps);
    const bool ok = scan_any(src, vel0_as_note_off, counters, sink, tempo_again, division, cb,
                             ScanMode::ACCUMULATE);
    fb.total_events_seen = counters.total_events_seen;
    return ok;
}

// Convert frame buckets to FrameStats. Pass-1-style semantics: running sums
// and extrema accumulate in frame order (buckets already hold exactly the
// per-frame values the sweep would have produced).
std::vector<FrameStats> buckets_to_frames(FrameBuckets& fb, double fps,
                                          const std::vector<TempoChange>& tm,
                                          double max_time_sec, bool with_cc, uint16_t ppqn,
                                          double end_delay, const ProgressCallbacks* cb) {
    if (end_delay > 0.0) max_time_sec += end_delay;   // frozen-stats tail frames
    const size_t total_frames = (size_t)std::ceil(max_time_sec * fps) + 1;
    const size_t song_frames = (size_t)std::ceil((end_delay > 0.0
        ? max_time_sec - end_delay : max_time_sec) * fps) + 1;
    fb.reserve_exact(total_frames);   // grow (or zero-extend) to the horizon

    std::vector<FrameStats> out;
    out.reserve(total_frames);
    uint64_t cum = 0, cum_cc = 0;
    int64_t poly = 0, peak_poly = 0;
    double peak_nps = 0.0, peak_cps = 0.0;
    size_t bpmw = 0;
    const size_t W = std::max<size_t>(1, (size_t)std::round(fps));   // >= 1 frame (fps < 0.5)

    // Sub-frame peaks from the fine-grid buckets -- same sliding window and
    // prefix sums as sweep_to_frames, so both engines agree.
    const size_t wlen = W * FMCG_FINE_BINS;
    size_t fine_next = 0;
    uint64_t wsum = 0, csum = 0;
    int64_t  psum = 0;
    uint64_t run_nps = 0, run_cps = 0;
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
            if (wsum > run_nps) run_nps = wsum;
            if (with_cc) {
                csum += fb.fcc[fine_next];
                if (fine_next >= wlen) csum -= fb.fcc[fine_next - wlen];
                if (csum > run_cps) run_cps = csum;
            }
            psum += fb.ffdeltas[fine_next];
            if (psum > run_poly) run_poly = psum;
        }
        uint64_t prev_cum = (k >= W) ? out[k - W].cumulative_notes : 0;
        uint64_t prev_cc = (k >= W) ? out[k - W].cumulative_cc : 0;
        double nps = (double)(cum - prev_cum);
        double cps = (double)(cum_cc - prev_cc);
        int64_t cur_poly = poly;
        if (k >= song_frames) {          // end-delay tail: all event activity
            nps = 0;                     // is over -- nps, cps and polyphony
            cps = 0;                     // drop to their resting values
            cur_poly = 0;
        } else {
            peak_nps = std::max(peak_nps, (double)run_nps);
            peak_cps = std::max(peak_cps, (double)run_cps);
            peak_poly = std::max(peak_poly, run_poly);
        }
        out.push_back({k, (double)k / fps, cum, cum_cc, nps, peak_nps,
                       std::max<int64_t>(0, cur_poly), peak_poly, tm[bpmw].bpm, (int64_t)tick_at((double)k / fps)});
        out.back().cc_per_second = cps;
        out.back().peak_cps = peak_cps;
    }
    return out;
}

} // namespace fmcg_stream
