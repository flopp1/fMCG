// fmcg_probe -- non-interactive CLI over the engine, for oracle/property tests.
// Prints one machine-parsable line:
//   OK notes=<n> ticks=<n> ppqn=<n> peak_poly=<n> peak_nps=<n> frames=<n>
// or FAIL (empty result / cancelled / unsupported).
// usage: fmcg_probe <file> [fps] [dedup 0/1] [threads] [close_held 0/1]
#include "src/fmcg_engine.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: fmcg_probe <file> [fps] [dedup] [threads]\n"); return 2; }
    const double fps = argc > 2 ? atof(argv[2]) : 60.0;
    const bool dedup = argc > 3 ? atoi(argv[3]) != 0 : true;
    const int threads = argc > 4 ? atoi(argv[4]) : 0;
    fmcg_stream::set_close_held_notes(argc > 5 && atoi(argv[5]) != 0);   // arg5: close held notes (default 0)
    ProgressCallbacks cb;
    bool lossy = false;
    cb.on_log = [&lossy](const char* m, bool) { fputs(m, stderr); if (strstr(m, "NOT counted")) lossy = true; };
    uint16_t ppqn = 0; uint64_t notes = 0, ticks = 0;
    auto fr = fmcg_stream::process_streaming(argv[1], fps, ppqn, notes, true, ticks, cb, 0.0, threads, dedup);
    if (fr.empty()) { printf("FAIL\n"); return 1; }
    printf("OK notes=%llu ticks=%llu ppqn=%u peak_poly=%lld peak_nps=%.0f frames=%zu lossy=%d\n",
           (unsigned long long)notes, (unsigned long long)ticks, (unsigned)ppqn,
           (long long)fr.back().peak_polyphony, fr.back().peak_nps, fr.size(), lossy ? 1 : 0);
    return 0;
}
