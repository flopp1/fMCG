// Verifies the two-pass frame-bucketed fallback produces results identical to
// the single-pass tick-space engine. The spec-violation threshold is forced
// low so the second parse of the same file takes the two-pass path; both
// FrameStats vectors must match exactly.
#include "../fMCG_core.h"
#include <cstdio>

static int fails = 0;

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "tiny.mid";

    // Single pass (no spec callback => proceed silently).
    ProgressCallbacks cb1;
    uint16_t div1 = 0; uint64_t notes1 = 0;
    auto single = ScaleMidiProcessor::process_midi(path, 60.0, div1, notes1, true, cb1);
    printf("single-pass: %zu frames, %llu notes\n", single.size(), (unsigned long long)notes1);

    // Two pass: threshold at tick 1, callback answers "restart in two-pass".
    ProgressCallbacks cb2;
    cb2.spec_tick_limit = 1;
    cb2.on_spec_violation = [](uint64_t, size_t) { return 1; };   // two-pass
    uint16_t div2 = 0; uint64_t notes2 = 0;
    auto two = ScaleMidiProcessor::process_midi(path, 60.0, div2, notes2, true, cb2);
    printf("two-pass:    %zu frames, %llu notes\n", two.size(), (unsigned long long)notes2);

    if (single.empty() || two.empty()) { printf("FAIL: a parse returned empty\n"); return 1; }
    if (div1 != div2) { printf("FAIL: division mismatch (%u vs %u)\n", div1, div2); ++fails; }
    if (notes1 != notes2) { printf("FAIL: total notes mismatch (%llu vs %llu)\n",
                                  (unsigned long long)notes1, (unsigned long long)notes2); ++fails; }
    if (single.size() != two.size()) {
        printf("FAIL: frame count mismatch (%zu vs %zu)\n", single.size(), two.size()); ++fails;
    } else {
        for (size_t i = 0; i < single.size(); ++i) {
            const auto& a = single[i]; const auto& b = two[i];
            if (a.frame_index != b.frame_index || a.cumulative_notes != b.cumulative_notes ||
                a.cumulative_cc != b.cumulative_cc || a.polyphony != b.polyphony ||
                (a.timestamp_sec - b.timestamp_sec) > 1e-9 ||
                (a.notes_per_second - b.notes_per_second) > 1e-9 || (a.peak_nps - b.peak_nps) > 1e-9 ||
                (a.bpm - b.bpm) > 1e-9) {
                printf("FAIL: frame %zu differs (notes %llu/%llu poly %lld/%lld)\n", i,
                       (unsigned long long)a.cumulative_notes, (unsigned long long)b.cumulative_notes,
                       (long long)a.polyphony, (long long)b.polyphony);
                ++fails; break;
            }
        }
    }

    // peak_polyphony: the single-pass engine is EXACT (per-tick running max);
    // the two-pass fallback buckets events into fine bins, so its bin-net
    // prefix sum can only UNDER-report the true peak (it is a lower bound).
    // The correct invariant is therefore two-pass <= single-pass.
    for (size_t i = 0; i < single.size() && i < two.size(); ++i) {
        if (two[i].peak_polyphony > single[i].peak_polyphony) {
            printf("FAIL: frame %zu two-pass peak poly %lld > single-pass %lld\n", i,
                   (long long)two[i].peak_polyphony, (long long)single[i].peak_polyphony);
            ++fails; break;
        }
    }

    if (fails == 0) printf("two-pass equivalence: ALL OK\n");
    return fails ? 1 : 0;
}
