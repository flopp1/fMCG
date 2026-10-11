// video_test_util.h -- shared by test_video / test_render: a minimal ISO-BMFF
// reader and an independent QuickTime Animation (qtrle) decoder written from
// the format description (not from the encoder), for bit-exact round trips.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- minimal ISO-BMFF reader ------------------------------------------------------
static std::vector<uint8_t> slurp(const std::string& p) {
    std::vector<uint8_t> d;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return d;
    std::fseek(f, 0, SEEK_END);
    d.resize((size_t)std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    if (!d.empty() && std::fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    std::fclose(f);
    return d;
}
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

// Finds a box by path ("moov/trak/mdia/minf/stbl/stsz"); returns payload [begin,end).
static bool find_box(const uint8_t* b, size_t n, const std::string& path, size_t& pb, size_t& pe, size_t base = 0) {
    const size_t slash = path.find('/');
    const std::string name = path.substr(0, slash);
    size_t i = 0;
    while (i + 8 <= n) {
        uint64_t sz = be32(b + i);
        size_t hdr = 8;
        if (sz == 1) { sz = be64(b + i + 8); hdr = 16; }
        if (sz < hdr || i + sz > n) return false;
        if (std::memcmp(b + i + 4, name.c_str(), 4) == 0) {
            if (slash == std::string::npos) { pb = base + i + hdr; pe = base + i + (size_t)sz; return true; }
            size_t skip = 0;
            if (name == "stsd") skip = 8;   // full box + entry count
            return find_box(b + i + hdr + skip, (size_t)sz - hdr - skip, path.substr(slash + 1), pb, pe, base + i + hdr + skip);
        }
        i += (size_t)sz;
    }
    return false;
}

struct Samples { std::vector<uint64_t> off; std::vector<uint32_t> size; std::vector<uint32_t> keys; bool ok = false; };
static Samples samples(const std::vector<uint8_t>& f) {
    Samples s;
    size_t b, e;
    const std::string st = "moov/trak/mdia/minf/stbl/";
    if (!find_box(f.data(), f.size(), st + "stsz", b, e)) return s;
    const uint32_t n = be32(f.data() + b + 8);
    for (uint32_t i = 0; i < n; ++i) s.size.push_back(be32(f.data() + b + 12 + 4 * i));
    if (!find_box(f.data(), f.size(), st + "co64", b, e)) return s;
    for (uint32_t i = 0; i < n; ++i) s.off.push_back(be64(f.data() + b + 8 + 8 * i));
    if (find_box(f.data(), f.size(), st + "stss", b, e)) {
        const uint32_t k = be32(f.data() + b + 4);
        for (uint32_t i = 0; i < k; ++i) s.keys.push_back(be32(f.data() + b + 8 + 4 * i));
    }
    s.ok = true;
    return s;
}

// ---- independent qtrle decoder (format: QuickTime Animation, 24/32 bpp) -------------
static bool qtrle_decode(const uint8_t* p, size_t n, std::vector<uint8_t>& img, int bpp, int W, int H) {
    if (n < 8) return n >= 4;                     // < 8 bytes: frame unchanged
    size_t i = 4;
    const unsigned header = (p[i] << 8) | p[i + 1]; i += 2;
    int start = 0, lines = H;
    if (header & 0x0008) {
        start = (p[i] << 8) | p[i + 1];
        lines = (p[i + 4] << 8) | p[i + 5];
        i += 8;
    }
    for (int l = 0; l < lines; ++l) {
        if (i >= n) return false;
        uint8_t* row = img.data() + (size_t)(start + l) * W * bpp;
        int x = p[i++] - 1;
        for (;;) {
            if (i >= n) return false;
            const int8_t code = (int8_t)p[i++];
            if (code == -1) break;
            if (code == 0) { x += p[i++] - 1; continue; }
            if (code < 0) {
                for (int r = 0; r < -code; ++r, ++x) std::memcpy(row + x * bpp, p + i, bpp);
                i += bpp;
            } else {
                std::memcpy(row + x * bpp, p + i, (size_t)code * bpp);
                i += (size_t)code * bpp; x += code;
            }
            if (x > W) return false;
        }
    }
    return true;
}

