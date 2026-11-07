// bench_scan_rate -- scan throughput over the first N seconds of a file (not
// part of the suite): runs process_streaming, prints the live event rate
// every few seconds and cancels after N seconds, so very large inputs can be
// measured without a full run.
//   bench_scan_rate <file> [seconds=60] [threads=0]
#include "fMCG_core.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: bench_scan_rate <file> [seconds] [threads]\n"); return 2; }
    const double limit = argc > 2 ? std::atof(argv[2]) : 60.0;
    std::atomic<bool> cancel{false};
    uint64_t last_ev = 0;
    double last_el = 0, last_print = -1e9;
    ProgressCallbacks cb;
    cb.cancel_flag = &cancel;
    cb.on_log = [](const char* m, bool) { std::fputs(m, stderr); };
    cb.on_scan_progress = [&](uint64_t ev, double el, double, double) {
        if (el - last_print >= 5.0) {
            std::printf("%7.1fs %16llu events  %8.1f Mev/s (last %.1fs: %8.1f Mev/s)\n", el, (unsigned long long)ev,
                        el > 0 ? (double)ev / el / 1e6 : 0.0, el - last_el,
                        el > last_el ? (double)(ev - last_ev) / (el - last_el) / 1e6 : 0.0);
            std::fflush(stdout);
            last_print = el; last_ev = ev; last_el = el;
        }
        if (el >= limit) cancel.store(true);
    };
    fmcg_stream::ScanOptions o;
    o.parse_threads = argc > 3 ? std::atoi(argv[3]) : 0;
    const fmcg_stream::ScanResult r = fmcg_stream::process_streaming(argv[1], o, cb);
    std::printf("%s after %.1fs: %llu events\n", r.ok() ? "finished" : "stopped", last_el, (unsigned long long)last_ev);
    return 0;
}
