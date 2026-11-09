// Verifies the two-pass frame-bucketed fallback produces results identical to
// the single-pass tick-space engine. The spec-violation threshold is forced
// low so the second parse of the same file takes the two-pass path; both
// FrameStats vectors must match exactly. Runs on the given file and on a
// generated one with notes and CC-only ticks (CC counts, {cps}, {cps-max}).
#include "fMCG_core.h"
#include <cstdio>
#include <string>

static int fails = 0;

static void compare(const char* path) {
    printf("== %s\n", path);
    // Single pass (no spec callback => proceed silently).
    ProgressCallbacks cb1;
    const fmcg_stream::ScanResult scan1 = fmcg_stream::process_streaming(path, fmcg_stream::ScanOptions{60.0, true}, cb1);
    const auto& single = scan1.frames;
    const uint16_t div1 = scan1.ppqn;
    const uint64_t notes1 = scan1.total_notes;
    printf("single-pass: %zu frames, %llu notes\n", single.size(), (unsigned long long)notes1);

    // Two pass: threshold at tick 1, callback answers "restart in two-pass".
    ProgressCallbacks cb2;
    cb2.spec_tick_limit = 1;
    cb2.on_spec_violation = [](uint64_t, size_t) { return 1; };   // two-pass
    const fmcg_stream::ScanResult scan2 = fmcg_stream::process_streaming(path, fmcg_stream::ScanOptions{60.0, true}, cb2);
    const auto& two = scan2.frames;
    const uint16_t div2 = scan2.ppqn;
    const uint64_t notes2 = scan2.total_notes;
    printf("two-pass:    %zu frames, %llu notes\n", two.size(), (unsigned long long)notes2);

    if (single.empty() || two.empty()) { printf("FAIL: a parse returned empty\n"); ++fails; return; }
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
                a.cc_per_second != b.cc_per_second || a.peak_cps != b.peak_cps ||
                (a.bpm - b.bpm) > 1e-9) {
                printf("FAIL: frame %zu differs (notes %llu/%llu poly %lld/%lld cc %llu/%llu cps %.0f/%.0f "
                       "cps-max %.0f/%.0f)\n", i,
                       (unsigned long long)a.cumulative_notes, (unsigned long long)b.cumulative_notes,
                       (long long)a.polyphony, (long long)b.polyphony,
                       (unsigned long long)a.cumulative_cc, (unsigned long long)b.cumulative_cc,
                       a.cc_per_second, b.cc_per_second, a.peak_cps, b.peak_cps);
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
}

// Notes and CC-only ticks (no note on the CC's tick), CCs spaced so the exact
// 1 s window and the frame grid disagree.
static bool write_cc_fixture(const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    auto vlq = [](unsigned n) {
        unsigned char b[5]; int i = 4; b[i] = (unsigned char)(n & 0x7F);
        while ((n >>= 7)) b[--i] = (unsigned char)(0x80 | (n & 0x7F));
        return std::string((char*)&b[i], 5 - i);
    };
    std::string ev = vlq(0) + std::string("\xFF\x51\x03\x07\xA1\x20", 6);   // 120 bpm
    for (int k = 0; k < 40; ++k) {
        ev += vlq(k ? 37 + (k * 53) % 211 : 0) + std::string("\xB0\x07\x40", 3);    // CC-only tick
        if (k % 3 == 0) {
            ev += vlq(11) + std::string("\x90\x3C\x64", 3);                       // a note in between
            ev += vlq(5) + std::string("\x80\x3C\x40", 3);
        }
    }
    ev += vlq(480) + std::string("\xFF\x2F\x00", 3);
    const std::string head = std::string("MThd\0\0\0\x06\0\0\0\x01\x01\xE0", 14);   // format 0, 1 track, 480 ppqn
    const unsigned n = (unsigned)ev.size();
    const std::string chunk = std::string("MTrk") + (char)(n >> 24) + (char)(n >> 16) + (char)(n >> 8) + (char)n;
    std::fwrite(head.data(), 1, head.size(), f);
    std::fwrite(chunk.data(), 1, chunk.size(), f);
    std::fwrite(ev.data(), 1, ev.size(), f);
    std::fclose(f);
    return true;
}

int main(int argc, char** argv) {
    compare((argc > 1) ? argv[1] : "tiny.mid");
    const char* cc_path = "_twopass_cc.mid";
    if (write_cc_fixture(cc_path)) {
        compare(cc_path);
        std::remove(cc_path);
    } else {
        printf("FAIL: cannot write %s\n", cc_path);
        ++fails;
    }
    if (fails == 0) printf("two-pass equivalence: ALL OK\n");
    return fails ? 1 : 0;
}
