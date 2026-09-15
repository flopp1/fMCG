// test_parallel -- the per-track parallel parse (plain mmap'd files) must be
// byte-identical to the sequential walk.
//
// The parallel path splits the MTrk chunks across threads into private
// TickData partials and merges them; because per-tick accumulation is
// commutative, the merged frame stats must equal the sequential scan's for
// every file shape. This test pins that with structural cases that stress
// the merge exactly where it could diverge:
//
//   1. two tracks, same tick range  (merge order must not matter)
//   2. held notes across track end  (per-track refcount closure)
//   3. dense same-tick bursts       (pending-register commits interleave)
//   4. CC events                    (dense_cc merge)
//   5. a >512KB track               (sequential walk's window boundary vs
//                                    the parallel path's whole-track window)
//   6. single-track file            (parallel path must decline; sequential runs)
//   7. tempo-only second track      (tempo map comes from the sequential probe)
//   8. parse_threads = 1            (must take the sequential path)
//
// Each case runs process_midi twice over the same file -- once forced
// sequential (threads=1) and once parallel (threads=4) -- and compares every
// FrameStats field of every frame, plus total_notes/total_ticks.
#include "../fMCG_core.h"
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <thread>
#include <chrono>
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
    uint8_t b[4]; int n = 0;
    b[n++] = v & 0x7F; v >>= 7;
    while (v) { b[n++] = (v & 0x7F) | 0x80; v >>= 7; }
    for (int i = n - 1; i >= 0; --i) s += (char)b[i];
}
enum Ev { NON, NOFF, CC };
static void ev(std::string& t, Ev e, uint8_t ch, uint8_t d1, uint8_t d2) {
    switch (e) {
        case NON:  t += (char)(0x90 | ch); t += (char)d1; t += (char)d2; break;
        case NOFF: t += (char)(0x80 | ch); t += (char)d1; t += (char)0x40; break;
        case CC:   t += (char)(0xB0 | ch); t += (char)d1; t += (char)d2; break;
    }
}

static bool write_midi(const std::string& path, uint16_t division,
                       const std::vector<std::string>& tracks) {
    std::string s = "MThd";
    put32(s, 6);
    s += (char)0; s += (char)0;                       // format (0/1; the engine only walks chunks)
    s += (char)0; s += (char)0;                       // ntracks placeholder (patched below)
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

struct RunResult {
    std::vector<FrameStats> frames;
    uint64_t notes = 0, ticks = 0;
    uint16_t ppqn = 0;
    bool ok = false;
};

static RunResult run(const std::string& path, int parse_threads) {
    RunResult r;
    ProgressCallbacks cb;   // no callbacks: spec guard proceeds silently
    r.frames = ScaleMidiProcessor::process_midi(path, 60.0, r.ppqn, r.notes,
                                                /*vel0_as_note_off=*/true, r.ticks,
                                                cb, /*end_delay=*/0.0, parse_threads);
    r.ok = !r.frames.empty();
    return r;
}

static bool compare(const RunResult& seq, const RunResult& par) {
    if (!par.ok) return false;
    if (seq.notes != par.notes || seq.ticks != par.ticks || seq.ppqn != par.ppqn) return false;
    return frames_repr(seq.frames) == frames_repr(par.frames);
}

int main() {
    const std::string path = "test/_parallel_case.mid";

    // ---- 1. two tracks, same tick range; interleaved on/off, tail crossings
    {
        std::string a, b;
        for (int i = 0; i < 2000; ++i) {
            vlq(a, 1); ev(a, NON, 0, (uint8_t)(i & 0x7F), 100);
            vlq(a, 1); ev(a, NOFF, 0, (uint8_t)(i & 0x7F), 0);
        }
        for (int i = 0; i < 2000; ++i) {
            vlq(b, i == 0 ? 0 : 1); ev(b, NON, 1, (uint8_t)((i * 7) & 0x7F), 90);
            vlq(b, 0);              ev(b, NOFF, 1, (uint8_t)((i * 7) & 0x7F), 0);
        }
        write_midi(path, 480, {a, b});
        RunResult s1 = run(path, 1), p4 = run(path, 4);
        check("two tracks, same range", compare(s1, p4));
    }

    // ---- 2. held notes across the track end (refcount closure) + CC
    {
        std::string a, b;
        vlq(a, 0); ev(a, NON, 0, 60, 100);
        vlq(a, 0); ev(a, NON, 0, 60, 100);      // same note twice: refcount 2
        vlq(a, 0); ev(a, NON, 0, 61, 100);      // never released
        vlq(a, 100); ev(a, NOFF, 0, 60, 0);     // release one instance
        vlq(a, 0); ev(a, CC, 0, 7, 100);
        vlq(a, 0); ev(a, CC, 0, 7, 64);
        vlq(a, 500); ev(a, NOFF, 0, 61, 0);
        for (int i = 0; i < 500; ++i) {
            vlq(b, i == 0 ? 0 : 2); ev(b, CC, 1, (uint8_t)(i & 0x7F), 33);
            vlq(b, 1); ev(b, NON, 1, 40, 80);
            vlq(b, 3); ev(b, NOFF, 1, 40, 0);
        }
        write_midi(path, 480, {a, b});
        RunResult s1 = run(path, 1), p4 = run(path, 4);
        check("held-note closure + CC merge", compare(s1, p4));
    }

    // ---- 3. dense same-tick bursts (pending registers interleaving)
    {
        std::string a, b;
        vlq(a, 0);
        for (int i = 0; i < 20000; ++i) {
            ev(a, NON, (uint8_t)(i & 0x0F), (uint8_t)(i & 0x7F), 100);   // zero deltas: one giant tick
        }
        for (int i = 0; i < 20000; i += 2) { vlq(a, 0); ev(a, NOFF, (uint8_t)(i & 0x0F), (uint8_t)(i & 0x7F), 0); }
        vlq(a, 10);
        for (int i = 1; i < 20000; i += 2) { vlq(a, 0); ev(a, NOFF, (uint8_t)(i & 0x0F), (uint8_t)(i & 0x7F), 0); }
        vlq(a, 5);
        for (int i = 0; i < 3000; ++i) { vlq(b, i == 0 ? 0 : 1); ev(b, NON, 2, (uint8_t)(i & 0x7F), 70); }
        vlq(b, 7);
        for (int i = 0; i < 3000; ++i) { vlq(b, 0); ev(b, NOFF, 2, (uint8_t)(i & 0x7F), 0); }
        write_midi(path, 480, {a, b});
        RunResult s1 = run(path, 1), p4 = run(path, 4), p2 = run(path, 2);
        check("dense same-tick bursts", compare(s1, p4));
        check("dense same-tick bursts (2 threads)", compare(s1, p2));
    }

    // ---- 4. sparse wide-range track vs dense narrow track (hole tolerance)
    {
        std::string a, b;
        vlq(a, 0); ev(a, NON, 0, 60, 100);
        vlq(a, 1000000); ev(a, NOFF, 0, 60, 0);       // 1M ticks: sparse
        vlq(a, 1000000); ev(a, NON, 0, 62, 100);
        vlq(a, 10);      ev(a, NOFF, 0, 62, 0);       // ends past the other track's horizon
        for (int i = 0; i < 100; ++i) {
            vlq(b, i == 0 ? 0 : 1); ev(b, NON, 1, 30, 90);
            vlq(b, 2);              ev(b, NOFF, 1, 30, 0);
        }
        write_midi(path, 480, {a, b});
        RunResult s1 = run(path, 1), p4 = run(path, 4);
        check("sparse/dense hole tolerance", compare(s1, p4));
    }

    // ---- 5. a >512KB track: ByteStream window boundaries vs whole-track window
    {
        std::string a, b;
        vlq(a, 0);
        for (int i = 0; i < 300000; ++i) {            // ~1.5MB of events
            ev(a, NON, (uint8_t)(i & 0x0F), (uint8_t)(i & 0x7F), 100);
            ev(a, NOFF, (uint8_t)(i & 0x0F), (uint8_t)(i & 0x7F), 0);
        }
        vlq(b, 0); ev(b, NON, 3, 50, 80);
        vlq(b, 400); ev(b, NOFF, 3, 50, 0);
        write_midi(path, 480, {a, b});
        RunResult s1 = run(path, 1), p4 = run(path, 4);
        check("track larger than one window", compare(s1, p4));
    }

    // ---- 6. single track: parallel must decline, results must still match
    {
        std::string a;
        for (int i = 0; i < 500; ++i) {
            vlq(a, i == 0 ? 0 : 1); ev(a, NON, 0, (uint8_t)(i & 0x7F), 100);
            vlq(a, 1);              ev(a, NOFF, 0, (uint8_t)(i & 0x7F), 0);
        }
        write_midi(path, 480, {a});
        RunResult s1 = run(path, 1), p4 = run(path, 4);
        check("single-track file (fallback)", compare(s1, p4));
    }

    // ---- 7. tempo map from the probe track
    {
        std::string a, b;
        vlq(a, 0); a += (char)0xFF; a += (char)0x51; a += (char)3;
        a += (char)0x07; a += (char)0xA1; a += (char)0x20;      // 500000 us
        for (int i = 0; i < 400; ++i) {
            vlq(a, i == 0 ? 0 : 1); ev(a, NON, 0, (uint8_t)(i & 0x7F), 100);
            vlq(a, 1);              ev(a, NOFF, 0, (uint8_t)(i & 0x7F), 0);
        }
        vlq(a, 240); a += (char)0xFF; a += (char)0x51; a += (char)3;
        a += (char)0x0F; a += (char)0x42; a += (char)0x40;      // 1000000 us
        vlq(a, 240); ev(a, NON, 0, 60, 100); vlq(a, 60); ev(a, NOFF, 0, 60, 0);
        vlq(b, 0); ev(b, NON, 1, 45, 90);
        vlq(b, 1000); ev(b, NOFF, 1, 45, 0);                    // long tail track
        write_midi(path, 480, {a, b});
        RunResult s1 = run(path, 1), p4 = run(path, 4);
        check("tempo map preserved", compare(s1, p4));
    }

    // ---- 9. spec-violation prompt through the parallel workers ------------
    // A tick span beyond the forced limit must trigger exactly one prompt and
    // honor the answer, in both directions (proceed / two-pass).
    {
        std::string a, b;
        vlq(a, 0);
        for (int i = 0; i < 100; ++i) { vlq(a, 64); ev(a, NON, 0, (uint8_t)(i & 0x7F), 100); }
        vlq(b, 0);
        for (int i = 0; i < 100; ++i) { vlq(b, 64); ev(b, NON, 1, (uint8_t)(i & 0x7F), 100); }
        write_midi(path, 480, {a, b});

        int prompts = 0;
        ProgressCallbacks cb;
        cb.spec_tick_limit = 1 << 12;                    // tiny: every track crosses it
        cb.on_spec_violation = [&](uint64_t, size_t) -> int { prompts++; return 0; };
        uint16_t ppqn; uint64_t notes, ticks;
        auto f_proceed = ScaleMidiProcessor::process_midi(path, 60.0, ppqn, notes, true, ticks, cb, 0.0, 4);
        check("spec prompt: proceed (asked once)", prompts == 1 && !f_proceed.empty());

        int prompts2 = 0;
        ProgressCallbacks cb2;
        cb2.spec_tick_limit = 1 << 12;
        cb2.on_spec_violation = [&](uint64_t, size_t) -> int { prompts2++; return 1; };   // two-pass
        uint16_t ppqn2; uint64_t notes2, ticks2;
        auto f_2pass = ScaleMidiProcessor::process_midi(path, 60.0, ppqn2, notes2, true, ticks2, cb2, 0.0, 4);
        check("spec prompt: two-pass restart", prompts2 == 1 && !f_2pass.empty());

        // Proceed-parallel and sequential-with-proceed must agree.
        int prompts3 = 0;
        ProgressCallbacks cb3;
        cb3.spec_tick_limit = 1 << 12;
        cb3.on_spec_violation = [&](uint64_t, size_t) -> int { prompts3++; return 0; };
        uint16_t ppqn3; uint64_t notes3, ticks3;
        auto f_seq = ScaleMidiProcessor::process_midi(path, 60.0, ppqn3, notes3, true, ticks3, cb3, 0.0, 1);
        check("spec proceed: parallel == sequential",
              notes == notes3 && ticks == ticks3 && frames_repr(f_proceed) == frames_repr(f_seq));
    }

    // ---- 10. cancellation mid-scan -----------------------------------------
    {
        std::string a, b;
        vlq(a, 0);
        for (int i = 0; i < 3000000; ++i) {            // ~12M events: a few 1M polls
            ev(a, NON, (uint8_t)(i & 0x0F), (uint8_t)(i & 0x7F), 100);
        }
        vlq(b, 0);
        for (int i = 0; i < 3000000; ++i) {
            ev(b, NON, (uint8_t)(i & 0x0F), (uint8_t)(i & 0x7F), 100);
        }
        write_midi(path, 480, {a, b});
        std::atomic<bool> cancel{false};
        ProgressCallbacks cb;
        cb.cancel_flag = &cancel;
        uint16_t ppqn; uint64_t notes, ticks;
        std::thread kick([&]() { std::this_thread::sleep_for(std::chrono::milliseconds(30)); cancel.store(true); });
        auto f = ScaleMidiProcessor::process_midi(path, 60.0, ppqn, notes, true, ticks, cb, 0.0, 4);
        kick.join();
        check("cancel aborts parallel scan", f.empty());
    }

    std::remove(path.c_str());

    if (fails == 0) { printf("test_parallel: all passed\n"); return 0; }
    printf("test_parallel: %d failure(s)\n", fails);
    return 1;
}
