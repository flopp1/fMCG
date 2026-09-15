// test_dedup -- the track-content deduplication fast path must be invisible:
// identical FrameStats with it on or off, on both the sequential (streamed)
// and parallel (mmap) scan paths, and it must never produce a false hit.
//
// Cases:
//   1. duplicate tracks, sequential   -- replays engaged, exact hit count
//   2. duplicate tracks, parallel     -- replays engaged, equality
//   3. fully unique content           -- zero hits, zero false replays
//   4. held-note duplicates           -- excluded from the store; still equal
//   5. store budget exhaustion        -- many unique tracks: stops storing,
//                                        results still correct
//
// The duplicate fixture tracks are byte-identical AND fully closed (every
// note released), which is what the store admits; the held-note track is
// deliberately open-ended (end_refcount > 0) and must never be stored.
#include "../fMCG_core.h"
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static int fails = 0;
static void check(const char* name, bool ok) {
    printf("%-46s %s\n", name, ok ? "[OK]" : "[FAIL]");
    if (!ok) fails++;
}

static void put32(std::string& s, uint32_t v) {
    s += (char)(v >> 24); s += (char)(v >> 16); s += (char)(v >> 8); s += (char)v;
}
static void vlq(std::string& s, uint32_t v) {
    uint8_t b[5]; int n = 0;
    b[n++] = v & 0x7F; v >>= 7;
    while (v) { b[n++] = (v & 0x7F) | 0x80; v >>= 7; }
    for (int i = n - 1; i >= 0; --i) s += (char)b[i];
}

static bool write_midi(const std::string& path, uint16_t division,
                       const std::vector<std::string>& tracks) {
    std::string s = "MThd";
    put32(s, 6);
    s += (char)0; s += (char)0;
    s += (char)0; s += (char)0;                       // ntracks (patched below)
    s += (char)(division >> 8); s += (char)(division & 0xFF);
    for (const auto& t : tracks) {
        s += "MTrk";
        put32(s, (uint32_t)t.size());
        s += t;
    }
    const size_t n = tracks.size();
    s[10] = (char)(n >> 8); s[11] = (char)(n & 0xFF);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return true;
}

// A fully-closed track: N note on/off pairs at increasing pitches, one CC at
// the end. Identical bytes every time it is generated.
static std::string dup_track(int n, uint8_t channel) {
    std::string t;
    for (int i = 0; i < n; ++i) {
        vlq(t, 1);  t += (char)(0x90 | channel); t += (char)(i & 0x7F); t += (char)100;
        vlq(t, 2);  t += (char)(0x80 | channel); t += (char)(i & 0x7F); t += (char)0x40;
    }
    vlq(t, 1);  t += (char)(0xB0 | channel); t += (char)7; t += (char)100;
    vlq(t, 0);  t += (char)0xFF; t += (char)0x2F; t += (char)0;
    return t;
}

struct RunResult {
    std::vector<FrameStats> frames;
    uint64_t notes = 0, ticks = 0;
    uint16_t ppqn = 0;
    std::string log;                                  // all on_log lines, concatenated
    bool ok = false;
};

static RunResult run(const std::string& path, int parse_threads, bool dedup) {
    RunResult r;
    ProgressCallbacks cb;
    cb.on_log = [&r](const char* msg, bool) { r.log += msg; };
    r.frames = ScaleMidiProcessor::process_midi(path, 60.0, r.ppqn, r.notes,
                                                /*vel0_as_note_off=*/true, r.ticks,
                                                cb, /*end_delay=*/0.0, parse_threads, dedup);
    r.ok = !r.frames.empty();
    return r;
}

static std::string frames_repr(const std::vector<FrameStats>& fr) {
    std::string s;
    char buf[128];
    for (const auto& f : fr) {
        snprintf(buf, sizeof buf, "%zu %.6f %llu %llu %.0f %.0f %lld %lld %.4f %lld\n",
                 f.frame_index, f.timestamp_sec,
                 (unsigned long long)f.cumulative_notes, (unsigned long long)f.cumulative_cc,
                 f.notes_per_second, f.peak_nps,
                 (long long)f.polyphony, (long long)f.peak_polyphony, f.bpm,
                 (long long)f.tick);
        s += buf;
    }
    return s;
}

static bool equal(const RunResult& a, const RunResult& b) {
    return a.ok && b.ok && a.notes == b.notes && a.ticks == b.ticks &&
           a.ppqn == b.ppqn && frames_repr(a.frames) == frames_repr(b.frames);
}

static uint64_t dedup_hits(const std::string& log) {
    const size_t p = log.find("track(s) replayed");
    if (p == std::string::npos) return 0;
    size_t b = log.rfind(' ', p - 1);              // space after the number
    if (b == std::string::npos) return 0;
    while (b > 0 && isdigit((unsigned char)log[b - 1])) --b;   // back over digits
    return strtoull(log.c_str() + b, nullptr, 10);
}

// The Stats line's event count: must be identical with dedup on or off on
// both paths (a past bug accumulated the 1M-event ping deltas AND the body's
// return value, inflating the reported total ~2x on huge files).
static uint64_t reported_events(const std::string& log) {
    const size_t p = log.find("Stats:");
    if (p == std::string::npos) return (uint64_t)-1;
    std::string num;
    for (size_t i = p; i < log.size(); ++i) {
        if (log.compare(i, 7, " events") == 0) break;
        if (isdigit((unsigned char)log[i])) num += log[i];
    }
    return num.empty() ? (uint64_t)-1 : strtoull(num.c_str(), nullptr, 10);
}

int main() {
    const std::string path = "test/_dedup_case.mid";
    const std::string dup = dup_track(300, 0);

    // ---- 1. sequential: 6 identical tracks (1 parse + 5 replays) + 2 unique
    {
        std::string u1;                                // unique: different velocities
        for (int i = 0; i < 200; ++i) {
            vlq(u1, 1); u1 += (char)0x91; u1 += (char)(i & 0x7F); u1 += (char)(60 + (i % 40));
            vlq(u1, 2); u1 += (char)0x81; u1 += (char)(i & 0x7F); u1 += (char)0x40;
        }
        std::string u2;                                // unique: CC-only
        for (int i = 0; i < 100; ++i) {
            vlq(u2, 3); u2 += (char)0xB2; u2 += (char)(i & 0x7F); u2 += (char)64;
        }
        vlq(u1, 1); u1 += (char)0xFF; u1 += (char)0x2F; u1 += (char)0;
        vlq(u2, 1); u2 += (char)0xFF; u2 += (char)0x2F; u2 += (char)0;
        write_midi(path, 480, {dup, dup, dup, dup, dup, dup, u1, u2});

        RunResult s0 = run(path, 1, false), s1 = run(path, 1, true);
        check("dedup == plain (sequential)", equal(s0, s1));
        // First occurrence of a declared length can never replay (the gate),
        // so it parses plain without a summary; copies 2..6 see a repeated
        // length, parse as candidates, and copies 3..6 replay: 1+1 parses,
        // 4 replays.
        check("sequential replays engaged (4 hits)", dedup_hits(s1.log) == 4);
        check("reported events: sequential", reported_events(s1.log) == reported_events(s0.log));
        RunResult p0 = run(path, 4, false), p1 = run(path, 4, true);
        check("dedup == plain (parallel)", equal(p0, p1));
        check("parallel replays engaged", dedup_hits(p1.log) >= 1);
        check("reported events: parallel", reported_events(p1.log) == reported_events(p0.log));
        check("parallel plain == sequential plain", equal(s0, p0));
    }

    // ---- 2. held-note duplicates must not be stored, results still equal
    {
        std::string held;                              // notes never released
        for (int i = 0; i < 200; ++i) {
            vlq(held, 1); held += (char)0x93; held += (char)(i & 0x7F); held += (char)100;
        }
        vlq(held, 1); held += (char)0xFF; held += (char)0x2F; held += (char)0;
        write_midi(path, 480, {held, held, held});
        RunResult s0 = run(path, 1, false), s1 = run(path, 1, true);
        check("held-note dups: dedup == plain", equal(s0, s1));
        check("held-note dups: zero replays", dedup_hits(s1.log) == 0);
        RunResult p0 = run(path, 4, false), p1 = run(path, 4, true);
        check("held-note dups: parallel equal", equal(p0, p1));
    }

    // ---- 3. fully unique tracks: zero hits, no false replays
    {
        std::vector<std::string> t;
        for (int k = 0; k < 6; ++k) {
            std::string u = dup_track(100 + k * 17, (uint8_t)(k & 0x0F));
            u[u.size() - 12] = (char)(0x90 | (k & 0x0F));   // channel byte differs -> unique
            t.push_back(u);
        }
        write_midi(path, 480, t);
        RunResult s0 = run(path, 1, false), s1 = run(path, 1, true);
        check("unique content: dedup == plain", equal(s0, s1));
        check("unique content: zero replays", dedup_hits(s1.log) == 0);
        RunResult p1 = run(path, 4, true);
        check("unique content: parallel equal", equal(s0, p1));
    }

    // ---- 4. store budget exhaustion: many unique tracks, results still right
    {
        std::vector<std::string> t;
        for (int k = 0; k < 400; ++k) {
            std::string u = dup_track(50, 0);
            u[u.size() - 12] = (char)(0x90 | (k & 0x0F));   // make every track unique
            t.push_back(u);
        }
        write_midi(path, 480, t);
        RunResult s0 = run(path, 1, false), s1 = run(path, 1, true);
        check("store budget: dedup == plain", equal(s0, s1));
        RunResult p1 = run(path, 4, true);
        check("store budget: parallel equal", equal(s0, p1));
    }

    std::remove(path.c_str());

    if (fails == 0) { printf("test_dedup: all passed\n"); return 0; }
    printf("test_dedup: %d failure(s)\n", fails);
    return 1;
}
