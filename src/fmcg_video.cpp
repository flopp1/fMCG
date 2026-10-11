// fmcg_video.cpp -- see fmcg_video.h.
#include "fmcg_video.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <x264.h>
}

namespace fmcg_video {

using fmcg_text::Canvas;
using fmcg_text::Rect;

const char* extension(Format f) { return f == Format::MOV_QTRLE ? ".mov" : ".mp4"; }

// ---- file helpers ----------------------------------------------------------------

static FILE* open_utf8(const std::string& path, const char* mode) {
#if defined(_WIN32)
    std::wstring w, wm;
    for (size_t i = 0; i < path.size();) {   // UTF-8 -> UTF-16
        unsigned c = (unsigned char)path[i++], cp = c;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) cp = c & (0x3F >> extra);
        while (extra-- > 0 && i < path.size()) cp = (cp << 6) | ((unsigned char)path[i++] & 0x3F);
        if (cp >= 0x10000) { cp -= 0x10000; w += (wchar_t)(0xD800 + (cp >> 10)); w += (wchar_t)(0xDC00 + (cp & 0x3FF)); }
        else w += (wchar_t)cp;
    }
    for (const char* m = mode; *m; ++m) wm += (wchar_t)*m;
    return _wfopen(w.c_str(), wm.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

static void remove_utf8(const std::string& path) {
#if defined(_WIN32)
    std::wstring w;
    for (size_t i = 0; i < path.size();) {
        unsigned c = (unsigned char)path[i++], cp = c;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) cp = c & (0x3F >> extra);
        while (extra-- > 0 && i < path.size()) cp = (cp << 6) | ((unsigned char)path[i++] & 0x3F);
        if (cp >= 0x10000) { cp -= 0x10000; w += (wchar_t)(0xD800 + (cp >> 10)); w += (wchar_t)(0xDC00 + (cp & 0x3FF)); }
        else w += (wchar_t)cp;
    }
    _wremove(w.c_str());
#else
    std::remove(path.c_str());
#endif
}

static int64_t ftell64(FILE* f) {
#if defined(_WIN32)
    return _ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}
static int fseek64(FILE* f, int64_t off) {
#if defined(_WIN32)
    return _fseeki64(f, off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

// ---- ISO-BMFF box builder ----------------------------------------------------------

struct Buf {
    std::vector<uint8_t> b;
    void u8(unsigned v) { b.push_back((uint8_t)v); }
    void u16(unsigned v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xFFFF); }
    void u64(uint64_t v) { u32((uint32_t)(v >> 32)); u32((uint32_t)v); }
    void tag(const char* t) { b.insert(b.end(), t, t + 4); }
    void bytes(const void* p, size_t n) { const uint8_t* q = (const uint8_t*)p; b.insert(b.end(), q, q + n); }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    size_t open(const char* t) { const size_t at = b.size(); u32(0); tag(t); return at; }
    size_t open_full(const char* t, unsigned version, uint32_t flags) {
        const size_t at = open(t); u8(version); u8(flags >> 16); u8(flags >> 8); u8(flags); return at;
    }
    void close(size_t at) {
        const uint32_t n = (uint32_t)(b.size() - at);
        b[at] = (uint8_t)(n >> 24); b[at + 1] = (uint8_t)(n >> 16); b[at + 2] = (uint8_t)(n >> 8); b[at + 3] = (uint8_t)n;
    }
    void matrix() {   // unity transformation matrix
        u32(0x00010000); u32(0); u32(0); u32(0); u32(0x00010000); u32(0); u32(0); u32(0); u32(0x40000000);
    }
};

// Streams samples into one mdat and writes moov at the end (64-bit offsets).
class Muxer {
public:
    bool open(const std::string& path, bool mov, std::string& err) {
        path_ = path; mov_ = mov;
        f_ = open_utf8(path, "wb");
        if (!f_) { err = "cannot create " + path; return false; }
        Buf h;
        const size_t ft = h.open("ftyp");
        if (mov) { h.tag("qt  "); h.u32(0x20050300); h.tag("qt  "); }
        else { h.tag("isom"); h.u32(0x200); h.tag("isom"); h.tag("iso2"); h.tag("avc1"); h.tag("mp41"); }
        h.close(ft);
        h.u32(1); h.tag("mdat"); h.u64(0);   // 64-bit size, patched in finish()
        mdat_at_ = (int64_t)h.b.size() - 16;
        return write(h.b.data(), h.b.size());
    }
    bool add(const uint8_t* p, size_t n, bool key) {
        offsets_.push_back((uint64_t)ftell64(f_));
        sizes_.push_back((uint32_t)n);
        keys_.push_back(key);
        return write(p, n);
    }
    // `entry` = complete sample-description box (avc1 / 'rle ').
    bool finish(const Buf& entry, int width, int height, uint32_t timescale, uint32_t delta) {
        const int64_t end = ftell64(f_);
        const uint64_t mdat_size = (uint64_t)(end - mdat_at_);
        const uint64_t n = sizes_.size();
        const uint64_t dur = n * delta;
        Buf m;
        const size_t moov = m.open("moov");
        {   // mvhd (version 1: 64-bit times)
            const size_t mv = m.open_full("mvhd", 1, 0);
            m.u64(0); m.u64(0); m.u32(timescale); m.u64(dur);
            m.u32(0x00010000); m.u16(0x0100); m.zeros(10); m.matrix(); m.zeros(24); m.u32(2);
            m.close(mv);
        }
        const size_t trak = m.open("trak");
        {
            const size_t tk = m.open_full("tkhd", 1, 3);   // enabled | in movie
            m.u64(0); m.u64(0); m.u32(1); m.u32(0); m.u64(dur);
            m.zeros(8); m.u16(0); m.u16(0); m.u16(0); m.u16(0); m.matrix();
            m.u32((uint32_t)width << 16); m.u32((uint32_t)height << 16);
            m.close(tk);
        }
        const size_t mdia = m.open("mdia");
        {
            const size_t md = m.open_full("mdhd", 1, 0);
            m.u64(0); m.u64(0); m.u32(timescale); m.u64(dur); m.u16(0x55C4 /* und */); m.u16(0);
            m.close(md);
            const size_t hd = m.open_full("hdlr", 0, 0);
            if (mov_) m.tag("mhlr"); else m.u32(0);
            m.tag("vide"); m.zeros(12);
            const char* name = "fMCG video";
            if (mov_) { m.u8((unsigned)std::strlen(name)); m.bytes(name, std::strlen(name)); }
            else { m.bytes(name, std::strlen(name) + 1); }
            m.close(hd);
        }
        const size_t minf = m.open("minf");
        {
            const size_t vm = m.open_full("vmhd", 0, 1);
            m.u16(0); m.u16(0); m.u16(0); m.u16(0);
            m.close(vm);
            if (mov_) {   // QuickTime data handler reference
                const size_t dh = m.open_full("hdlr", 0, 0);
                m.tag("dhlr"); m.tag("url "); m.zeros(12); m.u8(0);
                m.close(dh);
            }
            const size_t dinf = m.open("dinf");
            const size_t dref = m.open_full("dref", 0, 0);
            m.u32(1);
            const size_t url = m.open_full("url ", 0, 1);   // self-contained
            m.close(url);
            m.close(dref);
            m.close(dinf);
        }
        const size_t stbl = m.open("stbl");
        {
            const size_t sd = m.open_full("stsd", 0, 0);
            m.u32(1); m.bytes(entry.b.data(), entry.b.size());
            m.close(sd);
            const size_t ts = m.open_full("stts", 0, 0);
            m.u32(1); m.u32((uint32_t)n); m.u32(delta);
            m.close(ts);
            uint32_t nkeys = 0;
            for (bool k : keys_) nkeys += k;
            if (nkeys != n) {
                const size_t ss = m.open_full("stss", 0, 0);
                m.u32(nkeys);
                for (uint64_t i = 0; i < n; ++i) if (keys_[i]) m.u32((uint32_t)(i + 1));
                m.close(ss);
            }
            const size_t sc = m.open_full("stsc", 0, 0);
            m.u32(1); m.u32(1); m.u32(1); m.u32(1);   // every chunk holds one sample
            m.close(sc);
            const size_t sz = m.open_full("stsz", 0, 0);
            m.u32(0); m.u32((uint32_t)n);
            for (uint32_t s : sizes_) m.u32(s);
            m.close(sz);
            const size_t co = m.open_full("co64", 0, 0);
            m.u32((uint32_t)n);
            for (uint64_t o : offsets_) m.u64(o);
            m.close(co);
        }
        m.close(stbl);
        m.close(minf);
        m.close(mdia);
        m.close(trak);
        m.close(moov);
        if (!write(m.b.data(), m.b.size())) return false;
        // patch the mdat largesize
        uint8_t le[8];
        for (int i = 0; i < 8; ++i) le[i] = (uint8_t)(mdat_size >> (56 - 8 * i));
        if (fseek64(f_, mdat_at_ + 8) != 0 || std::fwrite(le, 1, 8, f_) != 8) return false;
        const bool ok = std::fclose(f_) == 0;
        f_ = nullptr;
        return ok;
    }
    void abort() {
        if (f_) { std::fclose(f_); f_ = nullptr; }
        remove_utf8(path_);
    }
    uint64_t bytes() const { return f_ ? (uint64_t)ftell64(f_) : 0; }
    ~Muxer() { if (f_) std::fclose(f_); }

private:
    bool write(const void* p, size_t n) { return std::fwrite(p, 1, n, f_) == n; }
    FILE* f_ = nullptr;
    std::string path_;
    bool mov_ = false;
    int64_t mdat_at_ = 0;
    std::vector<uint64_t> offsets_;
    std::vector<uint32_t> sizes_;
    std::vector<bool> keys_;
};

// Visual sample entry shared by 'avc1' and 'rle ' (ISO VisualSampleEntry ==
// QuickTime video sample description layout).
static size_t visual_entry(Buf& e, const char* type, int w, int h, const char* compressor, int depth) {
    const size_t at = e.open(type);
    e.zeros(6); e.u16(1);                 // reserved, data_reference_index
    e.u16(0); e.u16(0); e.u32(0);         // version, revision, vendor
    e.u32(0); e.u32(0);                   // temporal / spatial quality
    e.u16((unsigned)w); e.u16((unsigned)h);
    e.u32(0x00480000); e.u32(0x00480000); // 72 dpi
    e.u32(0); e.u16(1);                   // data size, frames per sample
    uint8_t name[32] = {0};
    const size_t nl = std::min<size_t>(31, std::strlen(compressor));
    name[0] = (uint8_t)nl; std::memcpy(name + 1, compressor, nl);
    e.bytes(name, 32);
    e.u16((unsigned)depth); e.u16(0xFFFF);
    return at;
}

static uint32_t timescale_for(double fps) { return (uint32_t)std::max(1.0, std::round(fps * 1000.0)); }

// ---- QuickTime Animation (qtrle) ------------------------------------------------------

class QtrleWriter : public Writer {
public:
    bool init(const std::string& path, const Options& o, std::string& err) {
        o_ = o;
        bpp_ = o.transparent ? 4 : 3;
        gop_ = std::max(1, (int)std::lround(o.fps * std::max(0.1, o.keyframe_seconds)));
        cur_.assign((size_t)o.width * o.height * bpp_, 0);
        prev_ = cur_;
        return mux_.open(path, true, err);
    }
    bool add_frame(const Canvas& c, Rect dirty) override {
        const int W = o_.width, H = o_.height;
        const bool key = (frame_ % gop_) == 0;
        Rect d = (frame_ == 0) ? Rect{0, 0, W, H} : dirty.clipped(W, H);
        // canvas (RGBA) -> packed RGB / ARGB for the dirty area
        for (int y = d.y; y < d.y + d.h; ++y) {
            const uint8_t* s = c.row(y) + (size_t)d.x * 4;
            uint8_t* t = cur_.data() + ((size_t)y * W + d.x) * bpp_;
            if (bpp_ == 3) for (int x = 0; x < d.w; ++x, s += 4, t += 3) { t[0] = s[0]; t[1] = s[1]; t[2] = s[2]; }
            else for (int x = 0; x < d.w; ++x, s += 4, t += 4) { t[0] = s[3]; t[1] = s[0]; t[2] = s[1]; t[3] = s[2]; }
        }
        // changed line range (key frames encode everything)
        const size_t rowb = (size_t)W * bpp_;
        int y0 = 0, y1 = H;
        if (!key) {
            y0 = H; y1 = 0;
            for (int y = d.y; y < d.y + d.h; ++y)
                if (std::memcmp(cur_.data() + y * rowb, prev_.data() + y * rowb, rowb) != 0) { y0 = std::min(y0, y); y1 = std::max(y1, y + 1); }
            if (y1 <= y0) { y0 = H; y1 = H; }
        }
        out_.clear();
        put32(0);
        if ((y0 == 0 && y1 == H) || y0 == H) put16(0);
        else { put16(8); put16((unsigned)y0); put16(0); put16((unsigned)(y1 - y0)); put16(0); }
        for (int y = y0; y < y1; ++y) encode_line(cur_.data() + y * rowb, prev_.data() + y * rowb, W, key);
        out_.push_back(0);
        const uint32_t n = (uint32_t)out_.size();
        out_[0] = (uint8_t)(n >> 24); out_[1] = (uint8_t)(n >> 16); out_[2] = (uint8_t)(n >> 8); out_[3] = (uint8_t)n;
        if (!mux_.add(out_.data(), out_.size(), key)) { err_ = "write failed (disk full?)"; return false; }
        // reference frame for the next one
        for (int y = std::min(d.y, y0); y < std::max(d.y + d.h, y1) && y < H; ++y)
            std::memcpy(prev_.data() + y * rowb, cur_.data() + y * rowb, rowb);
        ++frame_;
        return true;
    }
    bool finish() override {
        Buf e;
        const size_t at = visual_entry(e, "rle ", o_.width, o_.height, "Animation", bpp_ == 4 ? 32 : 24);
        e.close(at);
        if (!mux_.finish(e, o_.width, o_.height, timescale_for(o_.fps), 1000)) { err_ = "write failed"; return false; }
        return true;
    }
    void abort() override { mux_.abort(); }
    const std::string& error() const override { return err_; }
    uint64_t bytes_written() const override { return mux_.bytes(); }

private:
    void put16(unsigned v) { out_.push_back((uint8_t)(v >> 8)); out_.push_back((uint8_t)v); }
    void put32(uint32_t v) { put16(v >> 16); put16(v & 0xFFFF); }
    void pix(const uint8_t* p) { out_.insert(out_.end(), p, p + bpp_); }

    // One line: leading skip byte (+1), then 0=skip / -n=repeat / +n=literal codes, -1 = end.
    void encode_line(const uint8_t* cur, const uint8_t* prev, int W, bool key) {
        auto same_prev = [&](int x) { return !key && std::memcmp(cur + x * bpp_, prev + x * bpp_, bpp_) == 0; };
        auto eq = [&](int a, int b) { return std::memcmp(cur + a * bpp_, cur + b * bpp_, bpp_) == 0; };
        int x = 0;
        int skip = 0;
        while (x < W && same_prev(x) && skip < 254) { ++x; ++skip; }
        out_.push_back((uint8_t)(skip + 1));
        while (x < W) {
            if (same_prev(x)) {
                int s = 0;
                while (x + s < W && same_prev(x + s) && s < 254) ++s;
                if (x + s >= W) break;               // trailing unchanged pixels: just end the line
                out_.push_back(0); out_.push_back((uint8_t)(s + 1));
                x += s;
                continue;
            }
            int run = 1;
            while (x + run < W && run < 128 && eq(x, x + run)) ++run;
            if (run >= 2) {
                out_.push_back((uint8_t)(int8_t)(-run)); pix(cur + x * bpp_);
                x += run;
                continue;
            }
            // literal: until a repeat of >= 2 or an unchanged pixel starts
            int lit = 1;
            while (x + lit < W && lit < 127 && !same_prev(x + lit) &&
                   !(x + lit + 1 < W && eq(x + lit, x + lit + 1))) ++lit;
            out_.push_back((uint8_t)lit);
            out_.insert(out_.end(), cur + x * bpp_, cur + (x + lit) * bpp_);
            x += lit;
        }
        out_.push_back(0xFF);
    }

    Options o_;
    Muxer mux_;
    int bpp_ = 3, gop_ = 60;
    uint64_t frame_ = 0;
    std::vector<uint8_t> cur_, prev_, out_;
    std::string err_;
};

// ---- H.264 via x264 ----------------------------------------------------------------

class X264Writer : public Writer {
public:
    ~X264Writer() override { if (enc_) x264_encoder_close(enc_); if (pic_ok_) x264_picture_clean(&pic_); }

    bool init(const std::string& path, const Options& o, std::string& err) {
        o_ = o;
        if ((o.width & 1) || (o.height & 1)) { err = "H.264 (4:2:0) needs an even width and height"; return false; }
        x264_param_t p;
        if (o.crf < 0 || o.crf > 51) { err = "CRF must be 0..51"; return false; }
        if (x264_param_default_preset(&p, o.preset.c_str(), o.tune.empty() ? nullptr : o.tune.c_str()) < 0) {
            err = "unknown x264 preset/tune " + o.preset + (o.tune.empty() ? "" : "/" + o.tune);
            return false;
        }
        p.i_width = o.width; p.i_height = o.height;
        p.i_csp = X264_CSP_I420;
        p.i_threads = o.threads > 0 ? o.threads : X264_THREADS_AUTO;
        ts_ = timescale_for(o.fps);
        p.i_fps_num = ts_; p.i_fps_den = 1000;
        p.i_timebase_num = 1000; p.i_timebase_den = ts_;
        p.b_vfr_input = 0;
        p.i_bframe = 0;                     // display order == decode order: no ctts needed
        p.i_keyint_max = std::max(1, (int)std::lround(o.fps * std::max(0.1, o.keyframe_seconds)));
        p.rc.i_rc_method = X264_RC_CRF;
        p.rc.f_rf_constant = (float)o.crf;
        p.b_annexb = 0;                     // 4-byte length prefixes, as MP4 stores them
        p.b_repeat_headers = 0;             // SPS/PPS go into avcC
        p.vui.i_colorprim = 1; p.vui.i_transfer = 1; p.vui.i_colmatrix = 1;   // BT.709
        p.vui.b_fullrange = 0;
        p.i_log_level = X264_LOG_WARNING;
        if (o.crf > 0) x264_param_apply_profile(&p, "high");   // CRF 0 is lossless: High 4:4:4 Predictive
        enc_ = x264_encoder_open(&p);
        if (!enc_) { err = "x264 encoder failed to open"; return false; }
        if (x264_picture_alloc(&pic_, X264_CSP_I420, o.width, o.height) < 0) { err = "out of memory"; return false; }
        pic_ok_ = true;
        // black-equivalent init; the first frame is converted whole anyway
        std::memset(pic_.img.plane[0], 16, (size_t)pic_.img.i_stride[0] * o.height);
        std::memset(pic_.img.plane[1], 128, (size_t)pic_.img.i_stride[1] * (o.height / 2));
        std::memset(pic_.img.plane[2], 128, (size_t)pic_.img.i_stride[2] * (o.height / 2));
        x264_nal_t* nals = nullptr;
        int nn = 0;
        if (x264_encoder_headers(enc_, &nals, &nn) < 0) { err = "x264 headers failed"; return false; }
        for (int i = 0; i < nn; ++i) {      // payload without the 4-byte length prefix
            if (nals[i].i_type == NAL_SPS) sps_.assign(nals[i].p_payload + 4, nals[i].p_payload + nals[i].i_payload);
            if (nals[i].i_type == NAL_PPS) pps_.assign(nals[i].p_payload + 4, nals[i].p_payload + nals[i].i_payload);
        }
        if (sps_.size() < 4 || pps_.empty()) { err = "x264 produced no SPS/PPS"; return false; }
        std::string e;
        if (!mux_.open(path, false, e)) { err = e; return false; }
        return true;
    }

    bool add_frame(const Canvas& c, Rect dirty) override {
        Rect d = (frame_ == 0) ? Rect{0, 0, o_.width, o_.height} : dirty.clipped(o_.width, o_.height);
        // expand to even coordinates (4:2:0 chroma blocks)
        if (!d.empty()) {
            const int x0 = d.x & ~1, y0 = d.y & ~1;
            const int x1 = std::min(o_.width, (d.x + d.w + 1) & ~1), y1 = std::min(o_.height, (d.y + d.h + 1) & ~1);
            d = {x0, y0, x1 - x0, y1 - y0};
            convert(c, d);
        }
        pic_.i_pts = (int64_t)frame_;
        ++frame_;
        return encode(&pic_);
    }

    bool finish() override {
        while (x264_encoder_delayed_frames(enc_) > 0)
            if (!encode(nullptr)) return false;
        Buf e;
        const size_t at = visual_entry(e, "avc1", o_.width, o_.height, "x264", 0x18);
        {
            const size_t ac = e.open("avcC");
            e.u8(1); e.u8(sps_[1]); e.u8(sps_[2]); e.u8(sps_[3]);
            e.u8(0xFF);                              // 4-byte NAL lengths
            e.u8(0xE1); e.u16((unsigned)sps_.size()); e.bytes(sps_.data(), sps_.size());
            e.u8(1); e.u16((unsigned)pps_.size()); e.bytes(pps_.data(), pps_.size());
            if (sps_[1] == 100 || sps_[1] == 110 || sps_[1] == 122 || sps_[1] == 144) {
                e.u8(0xFC | 1); e.u8(0xF8); e.u8(0xF8); e.u8(0);   // 4:2:0, 8-bit, no SPS ext
            }
            e.close(ac);
        }
        e.close(at);
        if (!mux_.finish(e, o_.width, o_.height, ts_, 1000)) { err_ = "write failed"; return false; }
        return true;
    }
    void abort() override { mux_.abort(); }
    const std::string& error() const override { return err_; }
    uint64_t bytes_written() const override { return mux_.bytes(); }

private:
    // RGBA -> BT.709 limited-range I420 for rect d (even-aligned).
    void convert(const Canvas& c, Rect d) {
        uint8_t* Y = pic_.img.plane[0]; uint8_t* U = pic_.img.plane[1]; uint8_t* V = pic_.img.plane[2];
        const int ys = pic_.img.i_stride[0], us = pic_.img.i_stride[1], vs = pic_.img.i_stride[2];
        for (int y = d.y; y < d.y + d.h; y += 2) {
            const uint8_t* r0 = c.row(y);
            const uint8_t* r1 = c.row(std::min(y + 1, o_.height - 1));
            for (int x = d.x; x < d.x + d.w; x += 2) {
                int sr = 0, sg = 0, sb = 0;
                const uint8_t* q[4] = {r0 + x * 4, r0 + (x + 1) * 4, r1 + x * 4, r1 + (x + 1) * 4};
                const int yy[4] = {y, y, y + 1, y + 1}, xx[4] = {x, x + 1, x, x + 1};
                for (int k = 0; k < 4; ++k) {
                    const int R = q[k][0], G = q[k][1], B = q[k][2];
                    if (yy[k] < o_.height && xx[k] < o_.width)
                        Y[yy[k] * ys + xx[k]] = (uint8_t)(((47 * R + 157 * G + 16 * B + 128) >> 8) + 16);
                    sr += R; sg += G; sb += B;
                }
                sr = (sr + 2) >> 2; sg = (sg + 2) >> 2; sb = (sb + 2) >> 2;
                U[(y / 2) * us + x / 2] = (uint8_t)(((-26 * sr - 87 * sg + 112 * sb + 128) >> 8) + 128);
                V[(y / 2) * vs + x / 2] = (uint8_t)(((112 * sr - 102 * sg - 10 * sb + 128) >> 8) + 128);
            }
        }
    }
    bool encode(x264_picture_t* in) {
        x264_nal_t* nals = nullptr;
        int nn = 0;
        x264_picture_t out;
        const int sz = x264_encoder_encode(enc_, &nals, &nn, in, &out);
        if (sz < 0) { err_ = "x264 encode failed"; return false; }
        if (sz == 0) return true;
        // all NALs of one picture are contiguous in memory
        if (!mux_.add(nals[0].p_payload, (size_t)sz, out.b_keyframe != 0)) { err_ = "write failed (disk full?)"; return false; }
        return true;
    }

    Options o_;
    Muxer mux_;
    x264_t* enc_ = nullptr;
    x264_picture_t pic_{};
    bool pic_ok_ = false;
    uint32_t ts_ = 60000;
    uint64_t frame_ = 0;
    std::vector<uint8_t> sps_, pps_;
    std::string err_;
};

std::unique_ptr<Writer> create_writer(const std::string& path, const Options& opt, std::string* err) {
    std::string e;
    if (opt.width <= 0 || opt.height <= 0 || opt.width > 16384 || opt.height > 16384) {
        if (err) *err = "invalid resolution";
        return nullptr;
    }
    if (opt.format == Format::MOV_QTRLE) {
        std::unique_ptr<QtrleWriter> w(new QtrleWriter);
        if (!w->init(path, opt, e)) { if (err) *err = e.empty() ? w->error() : e; return nullptr; }
        return std::unique_ptr<Writer>(w.release());
    }
    std::unique_ptr<X264Writer> w(new X264Writer);
    if (!w->init(path, opt, e)) { if (err) *err = e; return nullptr; }
    return std::unique_ptr<Writer>(w.release());
}

int render_video(const std::string& path, const Options& opt, const FrameTextSource& text,
                 const fmcg_text::TextStyle& style, const fmcg_text::Placement& placement,
                 fmcg_text::RGBA background, const std::atomic<bool>* cancel,
                 const std::function<void(size_t, size_t)>& progress, std::string* err,
                 double* text_scale) {
    const size_t total = text.frame_count();
    if (total == 0) { if (err) *err = "nothing to render"; return -1; }
    fmcg_text::FrameRenderer fr;
    std::string e;
    fmcg_text::Placement pl = placement;
    pl.frame_w = opt.width; pl.frame_h = opt.height;
    if (!fr.init(style, pl, background, text.block_spec(), &e)) { if (err) *err = e; return -1; }
    if (text_scale) *text_scale = fr.scale();
    auto writer = create_writer(path, opt, &e);
    if (!writer) { if (err) *err = e; return -1; }
    auto last = std::chrono::steady_clock::now();
    for (size_t k = 0; k < total; ++k) {
        if (cancel && cancel->load(std::memory_order_relaxed)) { writer->abort(); return -2; }
        fr.render_pieces(text.pieces(k));
        const Rect dirty = (k == 0) ? Rect{0, 0, opt.width, opt.height} : fr.dirty();
        if (!writer->add_frame(fr.canvas(), dirty)) {
            if (err) *err = writer->error();
            writer->abort();
            return -1;
        }
        const auto now = std::chrono::steady_clock::now();
        if (progress && (now - last > std::chrono::milliseconds(100) || k + 1 == total)) {
            last = now;
            progress(k + 1, total);
        }
    }
    if (!writer->finish()) { if (err) *err = writer->error(); writer->abort(); return -1; }
    return 0;
}

} // namespace fmcg_video
