// fmcg_engine_internal.h -- engine-private types shared by the scan modules
// (fmcg_engine.cpp: standard / parallel scans + sweep; fmcg_layered.cpp: the
// layered-archive dedup walk). Not part of the public API.
#pragma once
#include "fmcg_midi.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fmcg_stream {

void emit(const ProgressCallbacks& cb, const std::string& msg, bool is_error = false);

// ---- track deduplication summaries (standard-scan dedup fast path) ----------
struct TrackSummary {
    uint64_t bytes = 0;          // raw track byte count (with hash)
    uint64_t hash = 0;           // 64-bit hash over the track's bytes
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

// Aborted via a cancel poll (scan loop or frame sweep).
struct ScanCancelled {};

struct TickData {
    // One interleaved cell per tick: a note-on touches ons and delta of the
    // SAME tick, so keeping them in one struct halves the cache/TLB misses
    // versus two separate multi-GB arrays (the sweep then reads each tick's
    // whole cell in one line too).
    // 64-bit fields: every non-tick stat can reach 93,823,560,428,205 (a file
    // whose every event is a note-on), far beyond 32 bits, and layered
    // constructions pile all of it onto ONE tick.
    struct TickCell { uint64_t ons; int64_t delta; };
    static_assert(sizeof(TickCell) == 16, "TickCell must stay tightly packed");
    std::vector<TickCell> cells;         // dense per-tick cells (standard scans)
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
    void delta_at(uint64_t t, int64_t d) { ensure(t); cells[t].delta += d; }
    // CC accumulation (always counted; the {cc} stats read it).
    std::vector<uint32_t> dense_cc;      // per-tick control-change counts
    uint64_t total_cc = 0;
    // Arithmetic tick runs (O(1) memory): every tick start, start+step, ...,
    // start+(count-1)*step receives (ons, delta, cc). Runs may overlap each
    // other and the dense/sparse cells; the sweep sums all sources per tick.
    // Used by the layered walk for periodic structure (NoiseMidi: 1.43e9
    // consecutive one-note ticks x65535 copies would need ~23 GB dense).
    struct TickRun { uint64_t start = 0, step = 1, count = 0; uint64_t ons = 0; int64_t delta = 0; uint64_t cc = 0; };
    std::vector<TickRun> runs;
    // Sparse cells sorted by strictly increasing tick (layered walk output):
    // tick space can be unbounded (1.4e9+ ticks seen) while the touched ticks
    // are few, so a dense array would be mostly empty gigabytes.
    struct SparseCell { uint64_t tick; uint64_t ons; int64_t delta; uint64_t cc; };
    std::vector<SparseCell> sparse;
    void cc_at(uint64_t t) {
        ensure(t);
        if (dense_cc.size() < cells.size()) dense_cc.resize(cells.size(), 0);
        dense_cc[t]++;
        total_cc++;
    }
    // Current footprint of the dense arrays (bytes).
    size_t memory_bytes() const {
        return cells.size() * sizeof(TickCell)
             + dense_cc.size() * sizeof(uint32_t)
             + sparse.size() * sizeof(SparseCell);
    }
    void reset() {
        std::vector<TickCell>().swap(cells);
        std::vector<uint32_t>().swap(dense_cc);
        std::vector<SparseCell>().swap(sparse);
        runs.clear();
        max_tick = 0; total_ons = 0; ntracks = 0; desync_tracks = 0;
        total_events_seen = 0; total_cc = 0;
        dedup.reset();
    }
};

// Layered-archive dedup walk (fmcg_layered.cpp). Reads the raw file itself.
// Returns 1 = scan complete (td / tempo_raw / division filled),
//         0 = not applicable (not an xz layer chain): caller runs the standard scan,
//        -1 = error (message already emitted).
// Throws ScanCancelled on cancel.
// risky: the opt-in "risky fast dedup" assumption (see process_streaming).
int scan_layered(const std::string& filename, bool vel0_as_note_off, TickData& td,
                 std::vector<TempoChange>& tempo_raw, uint16_t& out_division,
                 const ProgressCallbacks& cb, bool risky = false);

} // namespace fmcg_stream
