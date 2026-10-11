// fmcg_probe -- non-interactive CLI over the engine, for oracle/property tests.
// Prints one machine-parsable line:
//   OK notes=<n> ticks=<n> ppqn=<n> peak_poly=<n> peak_nps=<n> frames=<n> lossy=<0|1> twopass=<0|1> assumed=<0|1>
// or FAIL (empty result / cancelled / unsupported).
// usage: fmcg_probe <file> [fps] [dedup 0/1] [threads] [close_held 0/1] [spec 0/1] [risky 0/1]
//   risky (default 0): the opt-in risky fast dedup; assumed=1 when its
//   assumption was applied to at least one layer.
//   spec (default 1): when a standard scan crosses the MIDI-spec tick limit
//   (2^28), restart in the low-memory two-pass mode (twopass=1) instead of
//   growing dense tick arrays to many GB. 0 = proceed single-pass (high RAM).
//   Two-pass peak_poly / peak_nps are bin-quantized; compare only notes,
//   ticks, ppqn and frames when twopass=1.
#include "src/fmcg_engine.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: fmcg_probe <file> [fps] [dedup] [threads] [close_held] [spec] [risky]\n");
        return 2;
    }
    const double fps = argc > 2 ? atof(argv[2]) : 60.0;
    const int dedup = argc > 3 ? atoi(argv[3]) : 1;   // 0 none, 1 layered, 2 track (traditional)
    const int threads = argc > 4 ? atoi(argv[4]) : 0;
    fmcg_stream::set_close_held_notes(argc > 5 && atoi(argv[5]) != 0);
    const bool spec_twopass = argc > 6 ? atoi(argv[6]) != 0 : true;
    const bool risky = argc > 7 && atoi(argv[7]) != 0;
    ProgressCallbacks cb;
    bool lossy = false, twopass = false, assumed = false;
    cb.on_log = [&lossy, &assumed](const char* m, bool) {
        fputs(m, stderr);
        if (strstr(m, "NOT counted")) lossy = true;
        if (strstr(m, "risky fast dedup ASSUMED")) assumed = true;
    };
    if (spec_twopass)
        cb.on_spec_violation = [&twopass](uint64_t, size_t) -> int { twopass = true; return 1; };
    uint16_t ppqn = 0; uint64_t notes = 0, ticks = 0;
    auto fr = fmcg_stream::process_streaming(argv[1], fps, ppqn, notes, true, ticks, cb, 0.0, threads, dedup == 2, dedup == 1, risky);
    if (fr.empty()) { printf("FAIL\n"); return 1; }
    printf("OK notes=%llu ticks=%llu ppqn=%u peak_poly=%lld peak_nps=%.0f frames=%zu lossy=%d twopass=%d assumed=%d\n",
           (unsigned long long)notes, (unsigned long long)ticks, (unsigned)ppqn,
           (long long)fr.back().peak_polyphony, fr.back().peak_nps, fr.size(), lossy ? 1 : 0, twopass ? 1 : 0,
           assumed ? 1 : 0);
    return 0;
}
