// test_video -- in-built encoders + muxer (src/fmcg_video).
//  * qtrle .mov (24-bit and 32-bit/alpha): decoded by an independent decoder
//    written here from the format description and compared bit-exactly.
//  * H.264 .mp4: box structure (sample count, avcC, keyframes, chunk offsets).
//  * If `ffmpeg` is on PATH: both files decoded by FFmpeg as an outside
//    reference (qtrle must match exactly, H.264 within PSNR >= 38 dB).
#include "src/fmcg_video.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

using fmcg_text::Canvas;
using fmcg_text::Rect;

static int fails = 0;
static void check(const std::string& what, bool ok) {
    std::printf("%-62s [%s]\n", what.c_str(), ok ? "OK" : "FAIL");
    if (!ok) ++fails;
}

// ---- synthetic frames -----------------------------------------------------------
static const int W = 320, H = 180, N = 150;
static const double FPS = 30.0;

struct Frame { Canvas c; Rect dirty; };

static std::vector<Frame> make_frames(bool transparent) {
    std::vector<Frame> v;
    Canvas c;
    c.resize(W, H);
    const fmcg_text::RGBA bg = transparent ? 0x00000000u : 0x203040FFu;
    c.fill(bg);
    Rect last{};
    for (int k = 0; k < N; ++k) {
        c.fill_rect(last, bg);
        // a moving block + a "counter" block whose width changes, semi-transparent edge pixels
        Rect a{10 + (k * 3) % 250, 20 + (k % 7) * 3, 40, 24};
        Rect b{200, 120, 20 + (k % 5) * 17, 30};
        c.fill_rect(a, 0xFFFFFFFFu);
        c.fill_rect(b, 0xE0C010FFu);
        c.fill_rect({a.x, a.y + a.h, a.w, 2}, transparent ? 0xFF000080u : 0x804020FFu);
        if (k % 40 == 39) c.fill_rect({0, 0, W, 4}, 0x10FF10FFu);   // occasional full-width change
        Rect now = a.united(b).united({a.x, a.y + a.h, a.w, 2});
        if (k % 40 == 39) now = now.united({0, 0, W, 4});
        Rect dirty = last.united(now);
        last = now;
        v.push_back({c, dirty});
    }
    return v;
}

#include "video_test_util.h"

static std::vector<uint8_t> packed(const Canvas& c, int bpp) {
    std::vector<uint8_t> o((size_t)W * H * bpp);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const uint8_t* s = c.row(y) + x * 4;
            uint8_t* t = o.data() + ((size_t)y * W + x) * bpp;
            if (bpp == 3) { t[0] = s[0]; t[1] = s[1]; t[2] = s[2]; }
            else { t[0] = s[3]; t[1] = s[0]; t[2] = s[1]; t[3] = s[2]; }
        }
    return o;
}

// FFmpeg decode to raw frames (empty when ffmpeg is unavailable).
static std::vector<uint8_t> ffmpeg_raw(const std::string& file, const char* pixfmt) {
    std::vector<uint8_t> out;
    const std::string cmd = "ffmpeg -v error -i \"" + file + "\" -f rawvideo -pix_fmt " + pixfmt + " -";
#if defined(_WIN32)
    FILE* p = popen(cmd.c_str(), "rb");
#else
    FILE* p = popen((cmd + " 2>/dev/null").c_str(), "r");
#endif
    if (!p) return out;
    uint8_t buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, p)) > 0) out.insert(out.end(), buf, buf + got);
    pclose(p);
    return out;
}
static bool have_ffmpeg() {
#if defined(_WIN32)
    return std::system("ffmpeg -version >NUL 2>&1") == 0;
#else
    return std::system("ffmpeg -version >/dev/null 2>&1") == 0;
#endif
}

static void test_mov(const std::string& dir, bool transparent, bool ff) {
    const int bpp = transparent ? 4 : 3;
    const std::string tag = transparent ? "mov32" : "mov24";
    const std::string path = dir + "/_test_video_" + tag + ".mov";
    std::vector<Frame> frames = make_frames(transparent);
    fmcg_video::Options o;
    o.format = fmcg_video::Format::MOV_QTRLE;
    o.width = W; o.height = H; o.fps = FPS; o.transparent = transparent; o.keyframe_seconds = 1.0;
    std::string err;
    auto w = fmcg_video::create_writer(path, o, &err);
    check(tag + ": writer created", w != nullptr);
    if (!w) { std::printf("  %s\n", err.c_str()); return; }
    bool ok = true;
    for (auto& f : frames) ok &= w->add_frame(f.c, f.dirty);
    ok &= w->finish();
    check(tag + ": frames written + finished", ok);
    const std::vector<uint8_t> file = slurp(path);
    const Samples s = samples(file);
    check(tag + ": sample table has every frame", s.ok && s.size.size() == (size_t)N);
    check(tag + ": keyframe every second (1, 31, 61, ...)", s.keys.size() == 5 && s.keys[0] == 1 && s.keys[1] == 31);
    std::vector<uint8_t> img((size_t)W * H * bpp, 0);
    bool exact = s.ok;
    for (int k = 0; k < N && exact; ++k) {
        if (!qtrle_decode(file.data() + s.off[k], s.size[k], img, bpp, W, H)) { exact = false; break; }
        if (img != packed(frames[k].c, bpp)) { exact = false; std::printf("  first mismatch at frame %d\n", k); }
    }
    check(tag + ": independent decode is bit-exact for all frames", exact);
    if (ff) {
        const std::vector<uint8_t> raw = ffmpeg_raw(path, transparent ? "argb" : "rgb24");
        bool same = raw.size() == (size_t)N * W * H * bpp;
        for (int k = 0; k < N && same; ++k)
            same = std::memcmp(raw.data() + (size_t)k * W * H * bpp, packed(frames[k].c, bpp).data(), (size_t)W * H * bpp) == 0;
        check(tag + ": FFmpeg decode is bit-exact for all frames", same);
    }
    std::remove(path.c_str());
}

static void test_mp4(const std::string& dir, bool ff) {
    const std::string path = dir + "/_test_video.mp4";
    std::vector<Frame> frames = make_frames(false);
    fmcg_video::Options o;
    o.width = W; o.height = H; o.fps = FPS; o.keyframe_seconds = 2.0;
    std::string err;
    auto w = fmcg_video::create_writer(path, o, &err);
    check("mp4: writer created", w != nullptr);
    if (!w) { std::printf("  %s\n", err.c_str()); return; }
    bool ok = true;
    for (auto& f : frames) ok &= w->add_frame(f.c, f.dirty);
    ok &= w->finish();
    check("mp4: frames written + finished", ok);
    const std::vector<uint8_t> file = slurp(path);
    const Samples s = samples(file);
    size_t b, e;
    check("mp4: sample table has every frame", s.ok && s.size.size() == (size_t)N);
    check("mp4: avcC present", find_box(file.data(), file.size(), "moov/trak/mdia/minf/stbl/stsd/avc1", b, e));
    check("mp4: first sample is a keyframe", s.keys.empty() || s.keys[0] == 1);
    bool inside = s.ok;
    for (size_t k = 0; k < s.off.size(); ++k) inside &= s.off[k] + s.size[k] <= file.size();
    check("mp4: every sample lies inside the file", inside);
    // samples are 4-byte length-prefixed NAL units filling the sample exactly
    bool nal_ok = s.ok;
    for (size_t k = 0; k < s.off.size() && nal_ok; ++k) {
        size_t q = s.off[k], end = s.off[k] + s.size[k];
        while (q + 4 <= end) q += 4 + be32(file.data() + q);
        nal_ok = q == end;
    }
    check("mp4: samples are well-formed length-prefixed NAL units", nal_ok);
    if (ff) {
        const std::vector<uint8_t> raw = ffmpeg_raw(path, "rgb24");
        const size_t fb = (size_t)W * H * 3;
        bool count = raw.size() == (size_t)N * fb;
        check("mp4: FFmpeg decodes all frames", count);
        if (count) {
            double se = 0;
            for (int k = 0; k < N; ++k) {
                const std::vector<uint8_t> ref = packed(frames[k].c, 3);
                for (size_t i = 0; i < fb; ++i) { const double dd = (double)raw[k * fb + i] - ref[i]; se += dd * dd; }
            }
            const double psnr = 10 * std::log10(255.0 * 255.0 / (se / ((double)N * fb) + 1e-9));
            std::printf("  H.264 PSNR vs source: %.1f dB\n", psnr);
            check("mp4: FFmpeg decode PSNR >= 38 dB", psnr >= 38.0);
        }
    }
    std::remove(path.c_str());
}

// Encodes the synthetic frames with the given options; returns the file size (0 = failed).
static size_t encode_mp4(const std::string& path, const fmcg_video::Options& o, Samples* out = nullptr) {
    std::string err;
    auto w = fmcg_video::create_writer(path, o, &err);
    if (!w) return 0;
    bool ok = true;
    for (auto& f : make_frames(false)) ok &= w->add_frame(f.c, f.dirty);
    ok &= w->finish();
    const std::vector<uint8_t> file = slurp(path);
    if (out) *out = samples(file);
    std::remove(path.c_str());
    return ok ? file.size() : 0;
}

// The GUI's encoder settings (CRF, preset, tune, keyframe interval) reach x264.
static void test_mp4_options(const std::string& dir) {
    const std::string path = dir + "/_test_video_opts.mp4";
    fmcg_video::Options base;
    base.width = W; base.height = H; base.fps = FPS;
    const size_t def = encode_mp4(path, base);
    fmcg_video::Options lo = base;
    lo.crf = 40;
    const size_t small = encode_mp4(path, lo);
    check("mp4 opts: CRF 40 file is smaller than CRF 18", def && small && small < def);
    bool all = true;
    for (const char* p : fmcg_video::k_presets) {
        if (std::string(p) == "placebo") continue;   // slow; same code path as the others
        fmcg_video::Options o = base;
        o.preset = p;
        all &= encode_mp4(path, o) > 0;
    }
    check("mp4 opts: every listed preset encodes", all);
    all = true;
    for (const char* t : fmcg_video::k_tunes) {
        fmcg_video::Options o = base;
        o.tune = t;
        all &= encode_mp4(path, o) > 0;
    }
    check("mp4 opts: every listed tune encodes", all);
    fmcg_video::Options ll = base;
    ll.crf = 0;
    check("mp4 opts: CRF 0 (lossless) encodes", encode_mp4(path, ll) > 0);
    fmcg_video::Options kf = base;
    kf.keyframe_seconds = 0.5;   // 15 frames at 30 fps; x264 may add scene-cut keys on top
    Samples s;
    const bool kf_ok = encode_mp4(path, kf, &s) > 0 && s.ok && s.keys.size() >= 10;
    bool gaps = kf_ok;
    for (size_t i = 1; i < s.keys.size() && gaps; ++i) gaps = s.keys[i] - s.keys[i - 1] <= 15;
    check("mp4 opts: keyframe interval 0.5 s honoured (gap <= 15 frames)", gaps);
    std::string err;
    fmcg_video::Options bad = base;
    bad.preset = "warp9";
    check("mp4 opts: unknown preset is refused with an error",
          !fmcg_video::create_writer(path, bad, &err) && !err.empty());
    bad = base;
    bad.crf = 52;
    err.clear();
    check("mp4 opts: CRF > 51 is refused with an error",
          !fmcg_video::create_writer(path, bad, &err) && !err.empty());
    std::remove(path.c_str());
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const bool ff = have_ffmpeg();
    std::printf("ffmpeg cross-check: %s\n", ff ? "yes" : "no (not on PATH)");
    test_mov(dir, false, ff);
    test_mov(dir, true, ff);
    test_mp4(dir, ff);
    test_mp4_options(dir);
    std::printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL OK", fails);
    return fails ? 1 : 0;
}
