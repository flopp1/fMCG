// Functional checks for the formatting layer: auto-padding, per-stat commas,
// CC tokens, negative countdown times.
#include "../fMCG_core.h"
#include <cstdio>
#include <fstream>
#include <vector>
#include <string>

static int fails = 0;
static void check(const char* name, const std::string& got, const std::string& want) {
    bool ok = got == want;
    printf("%-38s got='%s' want='%s' %s\n", name, got.c_str(), want.c_str(), ok ? "[OK]" : "[FAIL]");
    if (!ok) fails++;
}

int main() {
    CommaOpts c;                    // all-on defaults (cc off by default)
    PadOpts p0, p1; p1.enabled = true;
    uint16_t ppqn = 480;
    double tmax = 300.0;

    {   // Baseline: commas everywhere, no padding.
        FrameStats fs; fs.cumulative_notes = 1234; fs.cumulative_cc = 5;
        fs.peak_polyphony = 300; fs.peak_nps = 987; fs.bpm = 120;
        auto out = ProcessTemplateLine("N:{nc}/{nc-total} P:{plph-max} S:{nps-max}",
                                       fs, 678901, 0, tmax, ppqn, c, p0);
        check("commas-on baseline", out, "N:1,234/678,901 P:300 S:987");
    }
    {   // Commas off for a stat -> plain digits.
        CommaOpts c2 = c; c2.notes = false;
        FrameStats fs; fs.cumulative_notes = 1234;
        auto out = ProcessTemplateLine("N:{nc}/{nc-total}", fs, 678901, 0, tmax, ppqn, c2, p0);
        check("notes commas off", out, "N:1234/678901");
    }
    {   // Auto-pad: notes pad to total-notes width, poly to peak width.
        FrameStats fs; fs.cumulative_notes = 34; fs.polyphony = 7; fs.peak_polyphony = 912;
        auto out = ProcessTemplateLine("N:{nc}/{nc-total} P:{plph}/{plph-max}",
                                       fs, 678901, 0, tmax, ppqn, c, p1);
        check("auto-pad to own max", out, "N:000,034/678,901 P:007/912");
    }
    {   // Auto-pad respects commas: comma groups are stripped before counting digits.
        CommaOpts c3 = c; c3.notes = false;
        FrameStats fs; fs.cumulative_notes = 34;
        auto out = ProcessTemplateLine("N:{nc}/{nc-total}", fs, 678901, 0, tmax, ppqn, c3, p1);
        check("auto-pad no-comma", out, "N:000034/678901");
    }
    {   // CC tokens: on/off, remainder, commas, auto-pad to total-CC width.
        FrameStats fs; fs.cumulative_cc = 7;
        auto out = ProcessTemplateLine("C:{cc}/{cc-total}/{cc-rem}", fs, 0, 1205, tmax, ppqn, c, p0);
        check("cc commas off (default)", out, "C:7/1205/1198");
        CommaOpts c4 = c; c4.cc = true;
        auto out2 = ProcessTemplateLine("C:{cc}/{cc-total}/{cc-rem}", fs, 0, 1205000, tmax, ppqn, c4, p0);
        check("cc commas on", out2, "C:7/1,205,000/1,204,993");
        auto out3 = ProcessTemplateLine("C:{cc}/{cc-total}", fs, 0, 1205, tmax, ppqn, c4, p1);
        check("cc auto-pad", out3, "C:0,007/1,205");
    }
    {   // BPM decimals: 0..6, default 2; out-of-range input is clamped by the caller.
        FrameStats fs; fs.bpm = 123.456789;
        auto b = [&](int d) {
            BpmOpts bo; bo.decimals = d;
            return ProcessTemplateLine("B:{bpm}", fs, 0, 0, tmax, ppqn, c, p0, bo);
        };
        check("bpm 0 decimals", b(0), "B:123");
        check("bpm 2 decimals (default)", b(2), "B:123.46");
        check("bpm 4 decimals", b(4), "B:123.4568");
        check("bpm 6 decimals (cap)", b(6), "B:123.456789");
        BpmOpts bover; bover.decimals = 99;   // formatting layer clamps too
        auto outb = ProcessTemplateLine("B:{bpm}", fs, 0, 0, tmax, ppqn, c, p0, bover);
        check("bpm clamped to 6", outb, "B:123.456789");
    }
    {   // Start-delay countdown: negative times truncate toward zero and count UP.
        FrameStats z; z.bpm = 150; z.timestamp_sec = -2.5;
        auto out = ProcessTemplateLine("T:{time-milli} S:{sec}", z, 0, 0, tmax, ppqn, c, p0);
        check("negative countdown", out, "T:-00:02.500 S:-2");
        z.timestamp_sec = -0.2;
        auto out2 = ProcessTemplateLine("T:{time-milli}", z, 0, 0, tmax, ppqn, c, p0);
        check("countdown near zero", out2, "T:-00:00.200");
        z.timestamp_sec = 0.0;
        auto out3 = ProcessTemplateLine("T:{time-milli}", z, 0, 0, tmax, ppqn, c, p0);
        check("countdown at zero", out3, "T:00:00.000");
    }
    {   // Zero-stats block (delay lead-in) shows 0 counters, starting BPM.
        FrameStats z; z.bpm = 150.0;
        auto out = ProcessTemplateLine("N:{nc} T:{time-milli} B:{bpm}", z, 5000, 0, tmax, ppqn, c, p0);
        check("delay zero block", out, "N:0 T:00:00.000 B:150.00");
    }
    {   // ASS start-delay lead-in lines exist and carry the negative first frame.
        std::vector<FrameStats> frames(2);
        frames[0].timestamp_sec = 0.0; frames[1].timestamp_sec = 1.0;
        AssConfig ac; ac.start_delay = 2.0; ac.pos_mode = 1; ac.pos_x = 55; ac.pos_y = 66;
        generate_ass("_t.ass", frames, {"T:{time-milli}"}, 10, 0, 480, ac);
        std::ifstream f("_t.ass");
        std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::remove("_t.ass");
        check("delay lead-in present", all.find("-00:02.000") != std::string::npos ? "yes" : "no", "yes");
        check("custom pos in ass", all.find("\\pos(55,66)") != std::string::npos ? "yes" : "no", "yes");
        int npos = 0;
        for (size_t at = all.find("\\pos("); at != std::string::npos; at = all.find("\\pos(", at + 1)) npos++;
        char buf[8]; snprintf(buf, sizeof(buf), "%d", npos);
        check("lead-in frame count", buf, "122");   // 2s delay x 60fps + 2 song frames
    }
    {   // ffmpeg colour spec conversion (ASS &H00BBGGRR -> 0xRRGGBB).
        check("ffmpeg color spec", ffmpeg_color_spec("&H00FF8040"), "0x4080FF");
        check("ffmpeg color default", ffmpeg_color_spec("junk"), "0x000000");
    }
    {   // Center alignments place the anchor mid-width; x/y clamp to the frame.
        std::vector<FrameStats> frames(1);
        frames[0].timestamp_sec = 0.0;
        AssConfig ac; ac.width = 1000; ac.height = 500;
        ac.ass_alignment = 8;   // Top Center
        generate_ass("_t.ass", frames, {"X"}, 0, 0, 480, ac);
        {
            std::ifstream f("_t.ass");
            std::string all((std::istreambuf_iterator<char>(f)), {});
            std::remove("_t.ass");
            check("top-center anchor", all.find("\\pos(500,30)") != std::string::npos ? "yes" : "no", "yes");
        }
        ac.ass_alignment = 2;   // Bottom Center
        generate_ass("_t.ass", frames, {"X"}, 0, 0, 480, ac);
        {
            std::ifstream f("_t.ass");
            std::string all((std::istreambuf_iterator<char>(f)), {});
            std::remove("_t.ass");
            check("bottom-center anchor", all.find("\\pos(500,470)") != std::string::npos ? "yes" : "no", "yes");
        }
        ac.ass_alignment = 7;
        ac.pos_mode = 1; ac.pos_x = -50; ac.pos_y = 5000;   // anchor out of range
        generate_ass("_t.ass", frames, {"X"}, 0, 0, 480, ac);
        {
            std::ifstream f("_t.ass");
            std::string all((std::istreambuf_iterator<char>(f)), {});
            std::remove("_t.ass");
            // pos_y=5000 clamps so the one-line block (1.5em x 36px = 54px) fits:
            // y -> 500-54 = 446; x -> 0. With font_size 36 and row "X":
            // est_w = 1*0.62*36 = 23 -> x bound 1000-23 = 977; but pos_x=-50 -> 0
            check("pos clamped to frame", all.find("\\pos(0,446)") != std::string::npos ? "yes" : "no", "yes");
        }
        {   // Bottom-right clamp: a huge x/y anchor is pulled back so the block
            // stays fully visible (est. width for a long row, height for 1 line).
            ac.pos_x = 2000; ac.pos_y = 1000;   // beyond 1000x500 frame
            std::string long_row(60, '0');      // 60 digits -> 60*0.62*36 = 1339 px wide
            generate_ass("_t.ass", frames, {long_row}, 0, 0, 480, ac);
            std::ifstream f("_t.ass");
            std::string all((std::istreambuf_iterator<char>(f)), {});
            std::remove("_t.ass");
            // x -> min(0, 1000-1339<0 -> 0); y -> 500 - 54 = 446
            check("bottom-right clamp", all.find("\\pos(0,446)") != std::string::npos ? "yes" : "no", "yes");
        }
    }
    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL OK", fails);
    return fails ? 1 : 0;
}
