// Proves the single-pass slowdown on extreme-tick files is tick-SPAN cost,
// not event cost: two files with the SAME tiny event count, differing only
// in the maximum tick. The wide one forces multi-GB tick-array doublings.
#include "../../fMCG_core.h"
#include <cstdio>
#include <chrono>

static void exe_dir(char* out, size_t cap) {
#ifdef _WIN32
    GetModuleFileNameA(NULL, out, (DWORD)cap);
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n < 0) n = 0;
    out[n] = 0;
#endif
    char* slash = strrchr(out, '/');
#ifdef _WIN32
    char* bs = strrchr(out, '\\'); if (bs && (!slash || bs > slash)) slash = bs;
#endif
    if (slash) *slash = 0; else strcpy(out, ".");
}

static void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x >> 24) & 0xFF); v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 8) & 0xFF);  v.push_back(x & 0xFF);
}
static void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((x >> 8) & 0xFF); v.push_back(x & 0xFF);
}
// VLQ delta (big-endian 7-bit groups)
static void put_vlq(std::vector<uint8_t>& v, uint64_t x) {
    uint8_t tmp[8]; int n = 0;
    do { tmp[n++] = (uint8_t)(x & 0x7F); x >>= 7; } while (x);
    for (int i = n - 1; i >= 0; --i) v.push_back(tmp[i] | (i ? 0x80 : 0));
}

// One track: nsp pairs of (note on, +1 tick, note off), pair i starting at
// tick i*stride. Track ends at the last pair's tick.
static std::vector<uint8_t> make_midi(uint32_t nsp, uint64_t stride) {
    std::vector<uint8_t> trk;
    uint64_t tick = 0;
    for (uint32_t i = 0; i < nsp; ++i) {
        put_vlq(trk, i == 0 ? 0 : stride - 1);   // advance to next start tick
        tick += (i == 0 ? 0 : stride - 1);
        trk.push_back(0x90); trk.push_back(60); trk.push_back(100);
        put_vlq(trk, 1); ++tick;
        trk.push_back(0x80); trk.push_back(60); trk.push_back(0);
    }
    put_vlq(trk, 0);
    trk.push_back(0xFF); trk.push_back(0x2F); trk.push_back(0x00);

    std::vector<uint8_t> f;
    f.insert(f.end(), {'M','T','h','d'}); put32(f, 6);
    put16(f, 0); put16(f, 1); put16(f, 480);
    f.insert(f.end(), {'M','T','r','k'}); put32(f, (uint32_t)trk.size());
    f.insert(f.end(), trk.begin(), trk.end());
    return f;
}

static void run(const char* label, const std::vector<uint8_t>& mid) {
    char dir[1024], name[1200];
    exe_dir(dir, sizeof dir);
    snprintf(name, sizeof name, "%s/_tickspace_%s.mid", dir, label);
    FILE* fp = fopen(name, "wb");
    fwrite(mid.data(), 1, mid.size(), fp);
    fclose(fp);

    ProgressCallbacks cb;
    cb.on_spec_violation = [](uint64_t, size_t) -> int { return 0; };   // proceed (single pass)

    auto t0 = std::chrono::steady_clock::now();
    uint64_t notes = 0; uint16_t div = 0;
    auto frames = ScaleMidiProcessor::process_midi(name, 60.0, div, notes, true, cb);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    remove(name);

    printf("%-10s file=%4zu bytes  notes=%llu  frames=%zu  time=%6.2fs  ->  %s ev/s\n",
           label, mid.size(), (unsigned long long)notes, frames.size(), secs,
           secs > 0 ? format_with_commas((uint64_t)(notes / (secs < 1e-9 ? 1e-9 : secs))).c_str() : "-");
    fflush(stdout);
}

int main() {
    uint64_t narrow_stride = 1000;                 // max tick ~ 4M  (fits under 1<<28)
    uint64_t wide_stride   = 1ull << 20;           // max tick ~ 4G  (far beyond 1<<28)
    run("narrow", make_midi(4096, narrow_stride));
    run("wide",   make_midi(4096, wide_stride));
    return 0;
}
