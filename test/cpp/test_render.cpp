// test_render -- end to end: MIDI -> engine -> FrameTextSource -> in-built
// renderer -> encoder -> file (fmcg_video::render_video, the function the GUI's
// Render button calls). Checks frame count and duration for both formats,
// bit-exact MOV frames against a fresh render of the same text, transparent
// output, and that cancelling deletes the partial file.
// Needs a TrueType font ($FMCG_TEST_FONT or a common system font); SKIPs otherwise.
#include "fMCG_core.h"
#include "video_test_util.h"

#include <atomic>
#include <cstdlib>

static int fails = 0;
static void check(const std::string& what, bool ok) {
    std::printf("%-64s [%s]\n", what.c_str(), ok ? "OK" : "FAIL");
    if (!ok) ++fails;
}

static std::string find_font() {
    if (const char* e = std::getenv("FMCG_TEST_FONT")) return e;
    const char* cands[] = {"C:/Windows/Fonts/arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf"};
    for (const char* c : cands)
        if (FILE* f = std::fopen(c, "rb")) { std::fclose(f); return c; }
    return "";
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const std::string midi = argc > 2 ? argv[2] : "test/data/tempo2.mid";
    const std::string font = find_font();
    if (font.empty()) { std::printf("SKIP: no TrueType font found\n"); return 0; }

    ProgressCallbacks cb;
    cb.on_log = [](const char*, bool) {};
    uint16_t ppqn = 0;
    uint64_t notes = 0, ticks = 0;
    const double fps = 30.0;
    std::vector<FrameStats> frames = fmcg_stream::process_streaming(midi, fps, ppqn, notes, true, ticks, cb, 0.5);
    check("engine processed " + midi, !frames.empty());
    if (frames.empty()) return 1;

    FrameTextSource src;
    src.frames = &frames;
    src.template_lines = {"Notes: {nc}/{nc-total}", "NPS {nps}  Poly {plph}/{plph-max}", "{time-milli} {bpm} BPM"};
    src.total_notes = notes;
    src.total_cc = frames.back().cumulative_cc;
    src.total_ticks = ticks;
    src.ppqn = ppqn;
    src.fps = fps;
    src.start_delay = 1.0;
    src.end_delay = 0.5;
    const size_t total = src.frame_count();
    std::printf("video frames: %zu (lead-in %zu)\n", total, src.lead_in_frames());

    fmcg_text::TextStyle st;
    st.font_file = font;
    st.size = 28;
    st.outline = true;
    st.outline_width = 1.5;
    fmcg_text::Placement pl;
    pl.alignment = 3;
    const int W = 480, H = 270;
    const fmcg_text::RGBA bg = 0x101828FFu;

    for (int fmt = 0; fmt < 3; ++fmt) {     // 0 = mp4, 1 = mov24, 2 = mov32 transparent
        fmcg_video::Options o;
        o.format = fmt == 0 ? fmcg_video::Format::MP4_H264 : fmcg_video::Format::MOV_QTRLE;
        o.width = W; o.height = H; o.fps = fps; o.transparent = fmt == 2;
        const std::string tag = fmt == 0 ? "mp4" : fmt == 1 ? "mov24" : "mov32";
        const std::string path = dir + "/_test_render_" + tag + (fmt == 0 ? ".mp4" : ".mov");
        const fmcg_text::RGBA b = fmt == 2 ? (bg & 0xFFFFFF00u) : bg;
        std::string err;
        size_t last_done = 0;
        const int rc = fmcg_video::render_video(path, o, src, st, pl, b, nullptr,
                                                [&](size_t d, size_t) { last_done = d; }, &err);
        check(tag + ": render_video ok", rc == 0);
        if (rc) { std::printf("  %s\n", err.c_str()); continue; }
        check(tag + ": progress reached the last frame", last_done == total);
        const std::vector<uint8_t> file = slurp(path);
        const Samples s = samples(file);
        check(tag + ": one sample per video frame", s.ok && s.size.size() == total);
        size_t mb, me;
        bool dur_ok = find_box(file.data(), file.size(), "moov/mvhd", mb, me);
        if (dur_ok) {   // version 1: ts at +20, duration at +24
            const uint32_t ts = be32(file.data() + mb + 20);
            const uint64_t du = be64(file.data() + mb + 24);
            dur_ok = ts == 30000 && du == (uint64_t)total * 1000;
        }
        check(tag + ": declared duration == frames / fps", dur_ok);
        if (fmt > 0) {
            const int bpp = fmt == 2 ? 4 : 3;
            fmcg_text::FrameRenderer ref;
            fmcg_text::Placement p2 = pl;
            p2.frame_w = W; p2.frame_h = H;
            ref.init(st, p2, b, src.block_spec());
            std::vector<uint8_t> img((size_t)W * H * bpp, 0);
            bool exact = s.ok;
            for (size_t k = 0; k < total && exact; ++k) {
                exact = qtrle_decode(file.data() + s.off[k], s.size[k], img, bpp, W, H);
                const fmcg_text::Canvas& c = ref.render_pieces(src.pieces(k));
                for (int y = 0; y < H && exact; ++y)
                    for (int x = 0; x < W && exact; ++x) {
                        const uint8_t* p = c.row(y) + x * 4;
                        const uint8_t* q = img.data() + ((size_t)y * W + x) * bpp;
                        exact = bpp == 3 ? (q[0] == p[0] && q[1] == p[1] && q[2] == p[2])
                                         : (q[0] == p[3] && q[1] == p[0] && q[2] == p[1] && q[3] == p[2]);
                    }
                if (!exact) std::printf("  first mismatch at frame %zu\n", k);
            }
            check(tag + ": every frame == fresh render of the same text", exact);
            if (fmt == 2) check(tag + ": corner pixel transparent", img[0] == 0);
        }
        std::remove(path.c_str());
    }

    // cancel half way: -2 and no file left behind
    {
        const std::string path = dir + "/_test_render_cancel.mov";
        fmcg_video::Options o;
        o.format = fmcg_video::Format::MOV_QTRLE;
        o.width = W; o.height = H; o.fps = fps;
        std::atomic<bool> cancel{true};   // set before the first frame: deterministic
        std::string err;
        const int rc = fmcg_video::render_video(path, o, src, st, pl, bg, &cancel, nullptr, &err);
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f) std::fclose(f);
        check("cancel: returns -2 and the partial file is deleted", rc == -2 && !f);
    }

    std::printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL OK", fails);
    return fails ? 1 : 0;
}
