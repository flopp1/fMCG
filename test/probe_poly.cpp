#define FMC_QUIET
#include "../fMCG_core.h"
#include <cstdio>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        uint16_t d1 = 0, d2 = 0; uint64_t t1 = 0, t2 = 0;
        auto a = ScaleMidiProcessor::process_midi(argv[i], 60.0, d1, t1);
        auto b = ScaleMidiProcessor::process_midi_parallel(argv[i], 60.0, d2, t2);
        long long pa = a.empty() ? -1 : a[0].polyphony;
        long long pb = b.empty() ? -1 : b[0].polyphony;
        long long pka = 0, pkb = 0;
        long long first_diff = -1;
        size_t n = std::min(a.size(), b.size());
        for (size_t f = 0; f < n; ++f) {
            if (a[f].polyphony != b[f].polyphony) { first_diff = (long long)f; break; }
        }
        for (auto& f : a) if (f.peak_polyphony > pka) pka = f.peak_polyphony;
        for (auto& f : b) if (f.peak_polyphony > pkb) pkb = f.peak_polyphony;
        if (first_diff >= 0 && a.size() == b.size()) {
            double ts = a[(size_t)first_diff].timestamp_sec;
            printf("%s: poly@0 %lld/%lld peak %lld/%lld FIRST_DIFF frame=%lld t=%.3fs gap=%lld %s\n",
                   argv[i], pa, pb, pka, pkb, first_diff, ts,
                   (long long)a[(size_t)first_diff].polyphony - (long long)b[(size_t)first_diff].polyphony,
                   (pa == pb && pka == pkb) ? "MATCH" : "DIFF");
        } else {
            printf("%s: poly@0 %lld/%lld peak %lld/%lld %s\n",
                   argv[i], pa, pb, pka, pkb, (pa == pb && pka == pkb) ? "MATCH" : "DIFF");
        }
    }
    return 0;
}
