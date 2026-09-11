#include <cstdio>
#include <chrono>
#include "../fMCG_core.h"

static void expect(const char* name, long long got, long long want) {
    printf("%-28s got=%lld want=%lld %s\n", name, got, want, got == want ? "[OK]" : "[FAIL]");
}

int main(int argc, char** argv) {
    // --- ad-hoc mode: parse a file (supports compressed archives), print summary, no render ---
    if (argc >= 2) {
        uint16_t div = 0; uint64_t total = 0;
        auto t0 = std::chrono::steady_clock::now();
        auto frames = ScaleMidiProcessor::process_midi(argv[1], 60.0, div, total, true);
        auto t1 = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(t1 - t0).count();
        if (frames.empty()) { printf("FAILED to parse %s\n", argv[1]); return 1; }
        uint64_t peak_poly = 0; double peak_nps = 0;
        for (auto& f : frames) {
            if ((uint64_t)f.peak_polyphony > peak_poly) peak_poly = f.peak_polyphony;
            if (f.peak_nps > peak_nps) peak_nps = f.peak_nps;
        }
        printf("RESULT %s\n  division=%u total_notes=%llu frames=%zu duration=%.2fs\n"
               "  peak_poly=%llu peak_nps=%.0f parse_time=%.1fs\n",
               argv[1], div, (unsigned long long)total, frames.size(),
               frames.back().timestamp_sec, (unsigned long long)peak_poly, peak_nps, elapsed);
        return 0;
    }

    // --- font enumeration sanity ---
    auto fonts = enumerate_system_fonts();
    printf("fonts detected: %zu\n", fonts.size());
    for (size_t i = 0; i < fonts.size() && i < 5; ++i) printf("  [%zu] %s\n", i + 1, fonts[i].c_str());

    // --- test.mid: full parse correctness ---
    {
        uint16_t div = 0; uint64_t total = 0;
        auto frames = ScaleMidiProcessor::process_midi("test.mid", 60.0, div, total, true);
        printf("test.mid: division=%u frames=%zu\n", div, frames.size());
        expect("test total_notes", (long long)total, 3);
        if (!frames.empty()) {
            printf("  duration=%.3fs\n", frames.back().timestamp_sec);
            long long peak_poly = 0; double peak_nps = 0; double cum_at_f30 = -1;
            for (auto& f : frames) {
                if (f.polyphony > peak_poly) peak_poly = f.polyphony;
                if (f.notes_per_second > peak_nps) peak_nps = f.notes_per_second;
                if (f.frame_index == 30) cum_at_f30 = (double)f.cumulative_notes;
            }
            expect("test peak_polyphony", peak_poly, 2);
            expect("test peak_nps", (long long)peak_nps, 3);
            expect("test cum_notes@frame30", (long long)cum_at_f30, 2);
            expect("test final_cum", (long long)frames.back().cumulative_notes, 3);
            expect("test bpm", (long long)(frames.back().bpm + 0.5), 120);
        }
    }

    // --- vel0_as_note_off = false: vel-0 note-on counts as a note ---
    {
        uint16_t div = 0; uint64_t total = 0;
        auto frames = ScaleMidiProcessor::process_midi("test.mid", 60.0, div, total, false);
        expect("test total_notes (vel0=on)", (long long)total, 4);
    }

    // --- short.mid ---
    {
        uint16_t div = 0; uint64_t total = 0;
        auto frames = ScaleMidiProcessor::process_midi("short.mid", 60.0, div, total, true);
        expect("short total_notes", (long long)total, 2);
        if (!frames.empty()) expect("short duration_ms", (long long)(frames.back().timestamp_sec * 1000 + 0.5), 1000);
    }

    // --- tempo2.mid: multi-tempo, cross-track staleness, held notes ---
    {
        uint16_t div = 0; uint64_t total = 0;
        auto frames = ScaleMidiProcessor::process_midi("tempo2.mid", 60.0, div, total, true);
        printf("tempo2.mid: division=%u frames=%zu\n", div, frames.size());
        // Corrected timeline (div=480; 0.5s/quarter until t=1.0s, then 1s/quarter):
        // tick 0->0.0s, 960->1.0s, 1920->3.0s, 2400->4.0s, 2640->4.5s -> 271 frames
        expect("tempo2 frames", (long long)frames.size(), 271);
        expect("tempo2 total_notes", (long long)total, 4);
        if (frames.size() == 271) {
            auto at = [&](size_t k) -> const FrameStats& { return frames[k]; };
            expect("tempo2 cum@0",    (long long)at(0).cumulative_notes, 2);
            expect("tempo2 poly@0",   (long long)at(0).polyphony, 2);
            expect("tempo2 cum@60",   (long long)at(60).cumulative_notes, 3);
            expect("tempo2 poly@70",  (long long)at(70).polyphony, 2);   // 62 + 64 held
            expect("tempo2 poly@130", (long long)at(130).polyphony, 2);  // still 62 + 64
            expect("tempo2 cum@180",  (long long)at(180).cumulative_notes, 3);
            expect("tempo2 poly@190", (long long)at(190).polyphony, 0);  // 62,64 closed @f180
            expect("tempo2 poly@210", (long long)at(210).polyphony, 0);
            expect("tempo2 cum@240",  (long long)at(240).cumulative_notes, 4);
            expect("tempo2 poly@250", (long long)at(250).polyphony, 1);  // 72 held
            expect("tempo2 poly@270", (long long)at(270).polyphony, 0);  // 72 closed @f270
            long long peak_poly = 0, peak_nps = 0;
            for (auto& f : frames) {
                peak_poly = std::max(peak_poly, f.peak_polyphony);
                peak_nps = std::max(peak_nps, (long long)f.peak_nps);
            }
            expect("tempo2 peak_poly", peak_poly, 2);
            expect("tempo2 peak_nps", peak_nps, 2);
            expect("tempo2 bpm@50",  (long long)(at(50).bpm + 0.5), 120);  // before tempo change
            expect("tempo2 bpm@150", (long long)(at(150).bpm + 0.5), 60);  // after it
            expect("tempo2 bpm@250", (long long)(at(250).bpm + 0.5), 60);
            expect("tempo2 duration_ms", (long long)(at(270).timestamp_sec * 1000 + 0.5), 4500);
        }
    }

    // --- compressed archives via libarchive must produce identical stats to plain parse ---
    {
        uint16_t dp = 0; uint64_t tp = 0;
        auto plain = ScaleMidiProcessor::process_midi("test.mid", 60.0, dp, tp, true);

        auto check_archive = [&](const char* path) {
            uint16_t d = 0; uint64_t t = 0;
            auto z = ScaleMidiProcessor::process_midi(path, 60.0, d, t, true);
            printf("%s: division=%u frames=%zu decompressed_ok=%s\n", path, d, z.size(), z.empty() ? "NO" : "YES");
            expect((std::string(path) + " total==plain").c_str(), (long long)t, (long long)tp);
            expect((std::string(path) + " frames==plain").c_str(), (long long)z.size(), (long long)plain.size());
            bool same = !z.empty() && z.size() == plain.size();
            for (size_t i = 0; same && i < z.size(); ++i)
                same = z[i].cumulative_notes == plain[i].cumulative_notes
                    && z[i].polyphony == plain[i].polyphony
                    && z[i].peak_polyphony == plain[i].peak_polyphony
                    && z[i].timestamp_sec == plain[i].timestamp_sec;
            expect((std::string(path) + " per-frame==plain").c_str(), same ? 1LL : 0LL, 1LL);
        };

        check_archive("test.mid.7z");
        check_archive("test.mid.tar.xz");
    }

    // --- invalid file returns empty ---
    {
        uint16_t div = 0; uint64_t total = 0;
        auto frames = ScaleMidiProcessor::process_midi("notmidi.txt", 60.0, div, total, true);
        expect("notmidi frames", (long long)frames.size(), 0);
    }
    return 0;
}
