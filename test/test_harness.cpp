#include <cstdio>
#include <chrono>
#include <algorithm>
#include <string>
#include <cstring>
#include "../fMCG_core.h"

static int failures = 0;

static void expect(const char* name, long long got, long long want) {
    printf("%-34s got=%lld want=%lld %s\n", name, got, want,
           got == want ? "[OK]" : "[FAIL]");
    if (got != want) failures++;
}

// Parse with the full ProgressCallbacks (CC stats enabled, log to stdout).
static std::vector<FrameStats> parse_full(const char* path, double fps,
                                          uint16_t& div, uint64_t& total,
                                          uint64_t& total_cc, bool vel0 = true) {
    ProgressCallbacks cb;
    cb.cc_stats = true;
    cb.on_log = [](const char* msg, bool is_error) { fputs(msg, stdout); };
    total_cc = 0;
    auto frames = ScaleMidiProcessor::process_midi(path, fps, div, total, vel0, cb);
    if (!frames.empty()) total_cc = frames.back().cumulative_cc;
    return frames;
}

int main(int argc, char** argv) {
    // --- ad-hoc mode: parse a file (supports compressed archives), print summary ---
    if (argc >= 2 && argv[1][0] != '-') {
        uint16_t div = 0; uint64_t total = 0, tcc = 0;
        auto t0 = std::chrono::steady_clock::now();
        auto frames = parse_full(argv[1], 60.0, div, total, tcc);
        auto t1 = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(t1 - t0).count();
        if (frames.empty()) { printf("FAILED to parse %s\n", argv[1]); return 1; }
        uint64_t peak_poly = 0; double peak_nps = 0;
        for (auto& f : frames) {
            peak_poly = std::max<uint64_t>(peak_poly, (uint64_t)f.peak_polyphony);
            peak_nps = std::max(peak_nps, f.peak_nps);
        }
        printf("RESULT %s\n  division=%u total_notes=%llu total_cc=%llu frames=%zu duration=%.2fs\n"
               "  peak_poly=%llu peak_nps=%.0f parse_time=%.1fs\n",
               argv[1], div, (unsigned long long)total, (unsigned long long)tcc,
               frames.size(), frames.back().timestamp_sec,
               (unsigned long long)peak_poly, peak_nps, elapsed);
        return 0;
    }

    bool want_csv = false;
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "--csv")) want_csv = true;

    // --- font enumeration sanity ---
    auto fonts = enumerate_system_fonts();
    printf("fonts detected: %zu\n", fonts.size());
    for (size_t i = 0; i < fonts.size() && i < 5; ++i) printf("  [%zu] %s\n", i + 1, fonts[i].c_str());

    // --- test.mid: full parse correctness (CC stats on) ---
    {
        uint16_t div = 0; uint64_t total = 0, tcc = 0;
        auto frames = parse_full("test.mid", 60.0, div, total, tcc);
        printf("test.mid: division=%u frames=%zu\n", div, frames.size());
        expect("test total_notes", (long long)total, 3);
        expect("test total_cc", (long long)tcc, 0);   // test.mid has no CC events
        if (!frames.empty()) {
            printf("  duration=%.3fs\n", frames.back().timestamp_sec);
            long long peak_poly = 0; double peak_nps = 0; double cum_at_f30 = -1;
            for (auto& f : frames) {
                peak_poly = std::max(peak_poly, f.peak_polyphony);
                peak_nps = std::max(peak_nps, f.notes_per_second);
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
        uint16_t div = 0; uint64_t total = 0, tcc = 0;
        parse_full("test.mid", 60.0, div, total, tcc, false);
        expect("test total_notes (vel0=on)", (long long)total, 4);
    }

    // --- short.mid ---
    {
        uint16_t div = 0; uint64_t total = 0, tcc = 0;
        auto frames = parse_full("short.mid", 60.0, div, total, tcc);
        expect("short total_notes", (long long)total, 2);
        if (!frames.empty()) expect("short duration_ms", (long long)(frames.back().timestamp_sec * 1000 + 0.5), 1000);
    }

    // --- tempo2.mid: multi-tempo, cross-track staleness, held notes ---
    std::vector<FrameStats> t2;
    {
        uint16_t div = 0; uint64_t total = 0, tcc = 0;
        t2 = parse_full("tempo2.mid", 60.0, div, total, tcc);
        printf("tempo2.mid: division=%u frames=%zu\n", div, t2.size());
        // Corrected timeline (div=480; 0.5s/quarter until t=1.0s, then 1s/quarter):
        // tick 0->0.0s, 960->1.0s, 1920->3.0s, 2400->4.0s, 2640->4.5s -> 271 frames
        expect("tempo2 frames", (long long)t2.size(), 271);
        expect("tempo2 total_notes", (long long)total, 4);
        if (t2.size() == 271) {
            auto at = [&](size_t k) -> const FrameStats& { return t2[k]; };
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
            for (auto& f : t2) {
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
        uint16_t dp = 0; uint64_t tp = 0, cp = 0;
        auto plain = parse_full("test.mid", 60.0, dp, tp, cp);

        auto check_archive = [&](const char* path) {
            uint16_t d = 0; uint64_t t = 0, c = 0;
            auto z = parse_full(path, 60.0, d, t, c);
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

    // --- CC counting: gen CC events into a temp midi and count them ---
    //     5 CC events at ticks 0,240,480,720,960 (120bpm = 0.5s/quarter).
    //     CC during lead-in and CC total must both be reflected.
    {
        FILE* f = fopen("_cctest.mid", "wb");
        if (f) {
            auto vlq = [](unsigned n) {
                unsigned char b[5]; int i = 4; b[i] = (unsigned char)(n & 0x7F);
                while ((n >>= 7)) b[--i] = (unsigned char)(0x80 | (n & 0x7F));
                return std::string((char*)&b[i], 5 - i);
            };
            auto be32 = [](unsigned v) {
                std::string s;
                s += (char)((v >> 24) & 0xFF); s += (char)((v >> 16) & 0xFF);
                s += (char)((v >> 8) & 0xFF);  s += (char)(v & 0xFF);
                return s;
            };
            auto be16 = [](unsigned v) {
                std::string s;
                s += (char)((v >> 8) & 0xFF); s += (char)(v & 0xFF);
                return s;
            };
            std::string ev;
            ev += vlq(0) + std::string("\xFF\x51\x03\x07\xA1\x20", 6);   // tempo 120
            for (int k = 0; k < 5; ++k)
                ev += vlq(k ? 240 : 0) + std::string("\xB0\x07\x40", 3); // CC7=64 each quarter
            ev += vlq(960) + std::string("\x90\x3C\x64", 3);             // a note at 2.0s
            ev += vlq(240) + std::string("\x80\x3C\x40", 3);
            ev += vlq(480) + std::string("\xFF\x2F\x00", 3);
            std::string head = std::string("MThd") + be32(6) + be16(0) + be16(1) + be16(480);
            std::string chunk = std::string("MTrk") + be32((unsigned)ev.size());
            fwrite(head.data(), 1, head.size(), f);
            fwrite(chunk.data(), 1, chunk.size(), f);
            fwrite(ev.data(), 1, ev.size(), f);
            fclose(f);

            uint16_t div = 0; uint64_t total = 0, tcc = 0;
            auto frames = parse_full("_cctest.mid", 60.0, div, total, tcc);
            expect("cc total", (long long)tcc, 5);
            expect("cc note still counted", (long long)total, 1);
            if (!frames.empty()) {
                // CCs land at 0.0/0.5/1.0/1.5/2.0s; the note-on is at 2.0s too,
                // so the first frame already carries all 5 (CC@t0=frame 0).
                expect("cc cum@f0", (long long)frames[0].cumulative_cc, 5);
                expect("cc cum@last", (long long)frames.back().cumulative_cc, 5);
            }
            remove("_cctest.mid");
        }
    }

    // --- optional CSV dump for compare_csv.py against ref_csv.py ---
    if (want_csv && !t2.empty()) {
        FILE* f = fopen("fmcg.csv", "w");
        if (f) {
            fprintf(f, "frame,timestamp_sec,cumulative_notes,current_nps,peak_nps,polyphony,peak_polyphony,bpm\n");
            for (auto& s : t2)
                fprintf(f, "%zu,%.3f,%llu,%lld,%lld,%lld,%lld,%.2f\n",
                        s.frame_index, s.timestamp_sec,
                        (unsigned long long)s.cumulative_notes,
                        (long long)s.notes_per_second, (long long)s.peak_nps,
                        (long long)s.polyphony, (long long)s.peak_polyphony, s.bpm);
            fclose(f);
            printf("CSV written: fmcg.csv (tempo2.mid)\n");
        }
    }

    printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL OK", failures);
    return failures ? 1 : 0;
}
