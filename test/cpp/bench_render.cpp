// bench_render -- render-loop benchmark (not part of the suite): MIDI -> frames
// -> FrameRenderer -> encoder, timing text rendering and encoding separately,
// plus a full fmcg_video::render_video run.
//   bench_render <midi> <out_dir> [mov|mp4|mova] [W H] [font_size] [seconds] [font] [threads]
// seconds = how much of the song to render (0 = all).
#include "fMCG_core.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using clk = std::chrono::steady_clock;
static double secs(clk::time_point a, clk::time_point b) { return std::chrono::duration<double>(b - a).count(); }

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: bench_render <midi> <out_dir> [mov|mp4|mova] [W H] [size] [seconds] [font]\n"); return 2; }
    const std::string midi = argv[1], dir = argv[2];
    const std::string fmt = argc > 3 ? argv[3] : "mov";
    const int W = argc > 5 ? std::atoi(argv[4]) : 1920, H = argc > 5 ? std::atoi(argv[5]) : 1080;
    const int size = argc > 6 ? std::atoi(argv[6]) : 36;
    const double seconds = argc > 7 ? std::atof(argv[7]) : 0.0;
    const std::string font = argc > 8 ? argv[8] : "C:/Windows/Fonts/arial.ttf";
    const int threads = argc > 9 ? std::atoi(argv[9]) : 0;
    const double fps = 60.0;

    ProgressCallbacks cb;
    cb.on_log = [](const char*, bool) {};
    fmcg_stream::ScanResult scan = fmcg_stream::process_streaming(midi, fmcg_stream::ScanOptions{fps}, cb);
    if (!scan.ok()) { std::printf("scan failed\n"); return 1; }
    if (seconds > 0 && scan.frames.size() > (size_t)(seconds * fps)) scan.frames.resize((size_t)(seconds * fps));

    FrameTextSource src;
    src.frames = &scan.frames;
    src.template_lines = {"Time: {time-milli}/{time-milli-max}", "Notes: {nc}/{nc-total}/{nc-rem}",
                          "NPS: {nps}/{nps-max}", "Polyphony: {plph}/{plph-max}", "BPM: {bpm}"};
    src.song.notes = scan.total_notes;
    src.song.cc = scan.frames.back().cumulative_cc;
    src.song.ticks = scan.total_ticks;
    src.song.ppqn = scan.ppqn;
    src.song.meter = scan.meter;
    src.fps = fps;
    const size_t total = src.frame_count();

    fmcg_text::TextStyle st;
    st.font_file = font;
    st.size = size;
    st.outline = true;
    st.outline_width = 1.0;
    fmcg_text::Placement pl;
    pl.alignment = 7;
    pl.frame_w = W; pl.frame_h = H;
    fmcg_video::Options o;
    o.format = fmt == "mp4" ? fmcg_video::Format::MP4_H264 : fmcg_video::Format::MOV_QTRLE;
    o.transparent = fmt == "mova";
    o.width = W; o.height = H; o.fps = fps;
    o.threads = threads;
    const fmcg_text::RGBA bg = o.transparent ? 0u : 0x000000FFu;
    const std::string path = dir + "/_bench_render" + (fmt == "mp4" ? ".mp4" : ".mov");

    // 1) split timing: text rendering vs encoder
    fmcg_text::FrameRenderer fr;
    std::string e;
    if (!fr.init(st, pl, bg, src.block_spec(), &e)) { std::printf("init: %s\n", e.c_str()); return 1; }
    auto w = fmcg_video::create_writer(path, o, &e);
    if (!w) { std::printf("writer: %s\n", e.c_str()); return 1; }
    double t_text = 0, t_enc = 0;
    const auto t0 = clk::now();
    for (size_t k = 0; k < total; ++k) {
        const auto a = clk::now();
        fr.render_pieces(src.pieces(k));
        const auto b = clk::now();
        const fmcg_text::Rect d = k == 0 ? fmcg_text::Rect{0, 0, W, H} : fr.dirty();
        if (!w->add_frame(fr.canvas(), d)) { std::printf("add_frame: %s\n", w->error().c_str()); return 1; }
        const auto c = clk::now();
        t_text += secs(a, b);
        t_enc += secs(b, c);
    }
    w->finish();
    const auto t1 = clk::now();
    std::printf("%s %dx%d size %d: %zu frames, split loop %.2fs (text %.2fs, encode %.2fs, finish %.2fs), %.1f MB\n",
                fmt.c_str(), W, H, size, total, secs(t0, t1), t_text, t_enc, secs(t0, t1) - t_text - t_enc,
                (double)w->bytes_written() / 1e6);
    w.reset();

    // 2) the real render loop
    const auto t2 = clk::now();
    const int rc = fmcg_video::render_video(path, o, src, st, pl, bg, nullptr, {}, &e);
    const auto t3 = clk::now();
    std::printf("render_video rc=%d %.2fs = %.0f fps\n", rc, secs(t2, t3), (double)total / secs(t2, t3));
    return rc;
}
