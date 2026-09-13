// Fuzz-equivalence test for the pointer-batched fast path (#6).
//
// Generates pseudo-random but fully valid multi-track MIDIs whose event mix
// deliberately stresses every burst classification branch:
//   - dense zero-delta runs of channel events (the burst's bread and butter)
//   - nonzero deltas mid-run (burst must stop cleanly at the delta boundary)
//   - running status AND explicit status bytes
//   - note-on/off, vel-0 note-ons, CC, poly aftertouch (0xA0), pitch bend (0xE0)
//   - program change / channel pressure (0xC0/0xD0 -- 1-data-byte messages)
//   - meta events incl. tempo, sysex (0xF0/0xF7), 0xF1/0xF2/0xF3 system msgs
//   - multiple tracks, varied ppqn
// Each file is parsed by two builds of the SAME engine: this binary (burst
// enabled) and the byte-wise-only reference (compiled with -DFMCG_NO_BURST,
// invoked as a subprocess on the same temp file). All outputs must match.
//
// The reference binary path is taken from argv[1]; argv[2] = seed count
// (default 40). Requires a writable temp dir (files go next to the exe).
#include "../fMCG_core.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <cstring>
#include <string>
#include <vector>

static std::string exe_dir(const char* argv0) {
    std::string a = argv0;
    size_t p = a.find_last_of("/\\");
    return (p == std::string::npos) ? std::string(".") : a.substr(0, p);
}

static void put_vlq(std::string& s, uint32_t v) {
    uint8_t b[4]; int n = 0;
    b[n++] = v & 0x7F; v >>= 7;
    while (v) { b[n++] = (v & 0x7F) | 0x80; v >>= 7; }
    for (int i = n - 1; i >= 0; --i) s += (char)b[i];
}

static std::string gen_track(std::mt19937& rng, uint32_t tseed, int ppqn) {
    (void)ppqn;
    std::mt19937 g(tseed);
    std::string s;
    uint8_t running = 0;
    bool running_two = false;   // running status is a 2-data-byte message?
    const int events = 200 + (int)(g() % 3000);

    // Occasional tempo meta at the head (varied us_per_quarter).
    if (g() % 3 == 0) {
        put_vlq(s, 0);
        s += (char)0xFF; s += (char)0x51; s += (char)0x03;
        uint32_t us = 300000 + (g() % 900000);
        s += (char)((us >> 16) & 0xFF); s += (char)((us >> 8) & 0xFF); s += (char)(us & 0xFF);
    }

    for (int i = 0; i < events; ++i) {
        // delta: 80% zero (burst chains), else small nonzero
        uint32_t d = (g() % 5 == 0) ? (1 + g() % 40) : 0;
        put_vlq(s, d);

        const int what = (int)(g() % 100);
        if (what < 62) {                       // channel voice, 2 data bytes
            uint8_t st;
            const int kind = (int)(g() % 5);   // 80/90/B0/A0/E0 mix
            st = (uint8_t)(kind == 0 ? 0x80 : kind == 1 ? 0x90 : kind == 2 ? 0xB0
                            : kind == 3 ? 0xA0 : 0xE0);
            st |= (uint8_t)(g() % 16);
            // Running status is only legal within the same data-byte class;
            // a 0xCx/0xDx running status would make a 2-data-byte event
            // unparseable. Chaining is only allowed from a 2-data-byte status.
            // else: chain (use running status, data bytes only) -- legal only
            // when running holds a 2-data-byte status (running_two == true)
            if (!running_two || g() % 2) { s += (char)st; running = st; running_two = true; }
            uint8_t n1 = (uint8_t)(g() % 128);
            uint8_t n2 = (uint8_t)(g() % 128);
            if (st == 0x90 && (g() % 6 == 0)) n2 = 0;   // vel-0 note-ons
            s += (char)n1; s += (char)n2;
        } else if (what < 72) {                // 1-data-byte messages (0xC0/0xD0)
            uint8_t st = (uint8_t)(((g() % 2) ? 0xC0 : 0xD0) | (g() % 16));
            s += (char)st; running = st; running_two = false;
            s += (char)(g() % 128);
        } else if (what < 84) {                // meta events (incl. tempo)
            s += (char)0xFF; running = 0; running_two = false;
            if (g() % 4 == 0) {
                s += (char)0x51; s += (char)0x03;
                uint32_t us = 300000 + (g() % 900000);
                s += (char)((us >> 16) & 0xFF); s += (char)((us >> 8) & 0xFF); s += (char)(us & 0xFF);
            } else {
                uint8_t type = (uint8_t)(0x01 + g() % 9);
                uint8_t len = (uint8_t)(g() % 12);
                s += (char)type; s += (char)len;
                for (int k = 0; k < len; ++k) s += (char)(g() & 0x7F);  // text-ish
            }
        } else if (what < 90) {                // sysex
            s += (char)((g() % 2) ? 0xF0 : 0xF7); running = 0; running_two = false;
            uint8_t len = (uint8_t)(g() % 15);
            put_vlq(s, len);
            for (int k = 0; k < len; ++k) s += (char)(0x01 + g() % 0x7F);
        } else if (what < 94) {                // system common (F1/F2/F3)
            uint8_t st = (uint8_t)(0xF1 + g() % 3);
            s += (char)st; running = 0; running_two = false;
            if (st == 0xF1 || st == 0xF3) s += (char)(g() % 128);
            else { s += (char)(g() % 128); s += (char)(g() % 128); }
        } else {                               // note-off via 0x80 to balance
            uint8_t st = (uint8_t)(0x80 | (g() % 16));
            s += (char)st; running = st; running_two = true;
            s += (char)(g() % 128); s += (char)(g() % 128);
        }
    }

    // End of track meta.
    put_vlq(s, 1 + g() % 100);
    s += (char)0xFF; s += (char)0x2F; s += (char)0x00;
    return s;
}

static std::string gen_midi(std::mt19937& rng, uint32_t seed) {
    std::mt19937 g(seed);
    const uint16_t ppqn = (uint16_t)(96 << (g() % 4));      // 96..768
    const int ntracks = 1 + (int)(g() % 4);

    std::string s;
    s += "MThd";
    const uint32_t hlen = 6;
    s += (char)0; s += (char)0; s += (char)0; s += (char)hlen;
    s += (char)0; s += (char)(g() % 2);                      // format 0/1
    s += (char)0; s += (char)ntracks;
    s += (char)((ppqn >> 8) & 0x7F); s += (char)(ppqn & 0xFF);

    for (int t = 0; t < ntracks; ++t) {
        std::string body = gen_track(g, g() ^ (seed * 2654435761u), ppqn);
        s += "MTrk";
        const uint32_t len = (uint32_t)body.size();
        s += (char)((len >> 24) & 0xFF); s += (char)((len >> 16) & 0xFF);
        s += (char)((len >> 8) & 0xFF);  s += (char)(len & 0xFF);
        s += body;
    }
    return s;
}

// Parse a file; dump a compact result signature for comparison.
struct Result {
    uint16_t division = 0;
    uint64_t notes = 0;
    std::vector<FrameStats> frames;
    std::string sig;         // textual digest of frames
};

static Result parse(const char* path, double fps, bool vel0_as_note_off, bool cc) {
    Result r;
    ProgressCallbacks cb;
    cb.cc_stats = cc;
    r.frames = ScaleMidiProcessor::process_midi(path, fps, r.division, r.notes,
                                                vel0_as_note_off, cb);
    char buf[64];
    for (const auto& f : r.frames) {
        std::snprintf(buf, sizeof buf, "|%llu,%lld,%lld,%.9f,%.9f",
                      (unsigned long long)f.cumulative_notes, (long long)f.polyphony,
                      (long long)f.peak_polyphony, f.timestamp_sec, f.notes_per_second);
        r.sig += buf;
        if (cc) {
            std::snprintf(buf, sizeof buf, ",%llu", (unsigned long long)f.cumulative_cc);
            r.sig += buf;
        }
    }
    return r;
}

static int run_pair(const std::string& ref_exe, const std::string& file,
                    double fps, bool vel0, bool cc, Result& burst_out) {
#ifdef _WIN32
    // _popen runs cmd.exe, which mangles quoted program paths here. The test
    // directory layout has no spaces, so use bare backslash paths and refuse
    // anything that would need quoting.
    std::string ref_norm = ref_exe, file_norm = file;
    for (auto& c : ref_norm) if (c == '/') c = '\\';
    for (auto& c : file_norm) if (c == '/') c = '\\';
    if (ref_norm.find(' ') != std::string::npos || file_norm.find(' ') != std::string::npos) {
        std::printf("FAIL: Windows reference spawn requires space-free paths\n");
        return 1;
    }
#else
    const std::string& ref_norm = ref_exe;
    const std::string& file_norm = file;
#endif
    // Reference (byte-wise only) via subprocess; its stdout ends with a marker
    // line "SIG <division> <notes> <sig>".
    std::string cmd = ref_norm + " --single " + file_norm
                    + " " + std::to_string(fps)
                    + " " + (vel0 ? "1" : "0")
                    + " " + (cc ? "1" : "0");
    FILE* pipe;
#ifdef _WIN32
    (void)pipe;
    // popen on Windows via _popen (console flash is fine in a test binary)
    pipe = ::_popen(cmd.c_str(), "r");
#else
    pipe = ::popen(cmd.c_str(), "r");
#endif
    if (!pipe) { std::printf("FAIL: cannot spawn reference\n"); return 1; }
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, pipe)) out += buf;
    ::pclose(pipe);

    size_t p = out.rfind("SIG ");
    if (p == std::string::npos) {
        std::printf("FAIL: reference produced no SIG line. Output:\n%.400s\n", out.c_str());
        return 1;
    }
    Result ref;
    if (std::sscanf(out.c_str() + p + 4, "%hu %llu",
                    &ref.division, (unsigned long long*)&ref.notes) != 2) {
        std::printf("FAIL: bad SIG header\n"); return 1;
    }
    size_t sig_start = out.find('\n', p + 4);
    ref.sig = (sig_start == std::string::npos) ? "" : out.substr(sig_start + 1);
    while (!ref.sig.empty() && (ref.sig.back() == '\n' || ref.sig.back() == '\r'))
        ref.sig.pop_back();

    burst_out = parse(file.c_str(), fps, vel0, cc);

    int fails = 0;
    if (ref.division != burst_out.division) { std::printf("  division mismatch\n"); ++fails; }
    if (ref.notes != burst_out.notes) {
        std::printf("  notes mismatch: ref=%llu burst=%llu\n",
                    (unsigned long long)ref.notes, (unsigned long long)burst_out.notes);
        ++fails;
    }
    if (ref.sig != burst_out.sig) {
        std::printf("  frame signature mismatch (len %zu vs %zu)\n",
                    ref.sig.size(), burst_out.sig.size());
        const size_t n = std::min(ref.sig.size(), burst_out.sig.size());
        size_t i = 0;
        while (i < n && ref.sig[i] == burst_out.sig[i]) ++i;
        std::printf("  first diff at %zu: ref '%.60s' burst '%.60s'\n", i,
                    ref.sig.c_str() + i, burst_out.sig.c_str() + i);
        ++fails;
    }
    return fails;
}


// ---- byte-wise reference mode (compiled with -DFMCG_NO_BURST) --------------
// Two reference modes:
//   --single <file> <fps> <vel0> <cc>   one file, one SIG block (used by the
//                                       run_pair comparison path)
//   --batch <gen.exe> <seeds>           regenerate every seed IN THIS PROCESS
//                                       (identical mt19937 streams, since the
//                                       generator is compiled in both builds)
//                                       and print one SIG block per seed. One
//                                       process spawn for the whole run --
//                                       per-seed spawning cost ~10s each was
//                                       pure cmd.exe + AV re-scan overhead.
#ifdef FMCG_NO_BURST
static int main_ref(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--single") == 0) {
        if (argc < 6) { std::printf("reference needs: --single file fps vel0 cc\n"); return 2; }
        Result r = parse(argv[2], std::atof(argv[3]), std::atoi(argv[4]) != 0,
                         std::atoi(argv[5]) != 0);
        std::printf("SIG %u %llu\n", (unsigned)r.division, (unsigned long long)r.notes);
        std::printf("%s\n", r.sig.c_str());
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--batch") == 0) {
        if (argc < 4) { std::printf("reference needs: --batch <gen.exe> <seeds>\n"); return 2; }
        // The generator is deterministic on the seed only, so the batch child
        // regenerates byte-identical files to the driver's.
        const int seeds = std::atoi(argv[3]);
        for (int seed = 1; seed <= seeds; ++seed) {
            std::mt19937 rng(seed);
            const std::string midi = gen_midi(rng, (uint32_t)seed);
            // Write to the same temp path the driver uses (argv[2]).
            {
                FILE* f;
#ifdef _WIN32
                if (fopen_s(&f, argv[2], "wb") != 0) f = nullptr;
#else
                f = std::fopen(argv[2], "wb");
#endif
                if (!f) { std::printf("FAIL: cannot write %s\n", argv[2]); return 1; }
                std::fwrite(midi.data(), 1, midi.size(), f);
                std::fclose(f);
            }
            const double fps = (seed % 3 == 0) ? 30.0 : 60.0;
            const bool vel0  = (seed % 4 != 0);
            const bool cc    = (seed % 2 == 1);
            Result r = parse(argv[2], fps, vel0, cc);
            std::printf("SIG %u %llu\n", (unsigned)r.division, (unsigned long long)r.notes);
            std::printf("%s\n", r.sig.c_str());
            std::fflush(stdout);
        }
        return 0;
    }
    std::printf("reference needs: --single file fps vel0 cc | --batch file seeds\n");
    return 2;
}
#define main main_unused_by_ref_mode
#endif

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <reference-exe> [seeds]\n"
                    "  (reference-exe is the same test built with -DFMCG_NO_BURST)\n",
                    argv[0]);
        return 2;
    }
    // Resolve the reference binary to an ABSOLUTE path: the _popen child
    // (cmd.exe) does not reliably resolve relative program paths.
    std::string ref_exe;
    {
        char absp[1024];
#ifdef _WIN32
        if (_fullpath(absp, argv[1], sizeof absp)) ref_exe = absp;
        else { std::printf("FAIL: cannot absolutize %s\n", argv[1]); return 2; }
#else
        if (realpath(argv[1], absp)) ref_exe = absp;
        else ref_exe = argv[1];
#endif
    }
    const int seeds = (argc > 2) ? std::atoi(argv[2]) : 40;

    const std::string dir = exe_dir(argv[0]);
    std::string file = dir + "/_burst_fuzz.mid";
    char filep[1024];
#ifdef _WIN32
    if (_fullpath(filep, file.c_str(), sizeof filep)) file = filep;
#else
    if (realpath(file.c_str(), filep)) file = filep;
#endif

    // ---- ONE batch reference run: all seeds in a single subprocess --------
    // The old design spawned the reference once per seed (~10s each: cmd.exe
    // startup plus antivirus re-scans); batching removes all of it but one.
#ifdef _WIN32
    std::string ref_norm = ref_exe, file_norm = file;
    for (auto& c : ref_norm) if (c == '/') c = '\\';
    for (auto& c : file_norm) if (c == '/') c = '\\';
    if (ref_norm.find(' ') != std::string::npos || file_norm.find(' ') != std::string::npos) {
        std::printf("FAIL: Windows reference spawn requires space-free paths\n");
        return 1;
    }
#else
    const std::string& ref_norm = ref_exe;
    const std::string& file_norm = file;
#endif
    std::string batch_cmd = ref_norm + " --batch " + file_norm + " " + std::to_string(seeds);
    FILE* pipe;
#ifdef _WIN32
    pipe = ::_popen(batch_cmd.c_str(), "r");
#else
    pipe = ::popen(batch_cmd.c_str(), "r");
#endif
    if (!pipe) { std::printf("FAIL: cannot spawn reference\n"); return 1; }
    std::string batch_out;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, pipe)) batch_out += buf;
    const int ref_ret = ::pclose(pipe);

    // Split the batch output into per-seed SIG blocks. Each block is exactly
    // the SIG header line plus the one-line signature; engine log lines land
    // between seeds and must not be glued onto the previous block's sig.
    std::vector<std::string> ref_blocks;
    size_t scan = 0;
    while (true) {
        const size_t p = batch_out.find("SIG ", scan);
        if (p == std::string::npos) break;
        const size_t hdr_end = batch_out.find('\n', p);
        if (hdr_end == std::string::npos) break;                 // truncated
        const size_t sig_end = batch_out.find('\n', hdr_end + 1);
        if (sig_end == std::string::npos) break;
        ref_blocks.push_back(batch_out.substr(p, sig_end - p));
        scan = sig_end;
    }
    if ((int)ref_blocks.size() != seeds || ref_ret != 0) {
        std::printf("FAIL: reference produced %zu/%d SIG blocks (exit %d). Output:\n%.400s\n",
                    ref_blocks.size(), seeds, ref_ret, batch_out.c_str());
        std::remove(file.c_str());
        return 1;
    }

    int total_fails = 0;
    for (int seed = 1; seed <= seeds; ++seed) {
        std::mt19937 rng(seed);
        const std::string midi = gen_midi(rng, (uint32_t)seed);
        {
            FILE* f;
#ifdef _WIN32
            if (fopen_s(&f, file.c_str(), "wb") != 0) f = nullptr;
#else
            f = std::fopen(file.c_str(), "wb");
#endif
            if (!f) { std::printf("FAIL: cannot write %s\n", file.c_str()); return 1; }
            std::fwrite(midi.data(), 1, midi.size(), f);
            std::fclose(f);
        }
        // Must mirror the batch child's per-seed variation exactly.
        const double fps = (seed % 3 == 0) ? 30.0 : 60.0;
        const bool vel0  = (seed % 4 != 0);
        const bool cc    = (seed % 2 == 1);

        // Parse the reference SIG block.
        Result ref;
        const std::string& blk = ref_blocks[seed - 1];
        if (std::sscanf(blk.c_str(), "SIG %hu %llu",
                        &ref.division, (unsigned long long*)&ref.notes) != 2) {
            std::printf("seed %d: FAIL (bad SIG header)\n", seed); ++total_fails; continue;
        }
        const size_t nl = blk.find('\n');
        ref.sig = (nl == std::string::npos) ? "" : blk.substr(nl + 1);
        while (!ref.sig.empty() && (ref.sig.back() == '\n' || ref.sig.back() == '\r'))
            ref.sig.pop_back();

        Result burst = parse(file.c_str(), fps, vel0, cc);

        int fails = 0;
        if (ref.division != burst.division) { std::printf("  division mismatch\n"); ++fails; }
        if (ref.notes != burst.notes) {
            std::printf("  notes mismatch: ref=%llu burst=%llu\n",
                        (unsigned long long)ref.notes, (unsigned long long)burst.notes);
            ++fails;
        }
        if (ref.sig != burst.sig) {
            std::printf("  frame signature mismatch (len %zu vs %zu)\n",
                        ref.sig.size(), burst.sig.size());
            const size_t n = std::min(ref.sig.size(), burst.sig.size());
            size_t i = 0;
            while (i < n && ref.sig[i] == burst.sig[i]) ++i;
            std::printf("  first diff at %zu: ref '%.60s' burst '%.60s'\n", i,
                        ref.sig.c_str() + i, burst.sig.c_str() + i);
            ++fails;
        }
        if (fails) {
            std::printf("seed %d: FAIL (fps=%.0f vel0=%d cc=%d, %zu frames)\n",
                        seed, fps, (int)vel0, (int)cc, burst.frames.size());
            total_fails += fails;
        }
    }
    std::remove(file.c_str());

    if (total_fails == 0) std::printf("burst fuzz: ALL OK (%d seeds)\n", seeds);
    else std::printf("burst fuzz: %d failure(s) over %d seeds\n", total_fails, seeds);
    return total_fails ? 1 : 0;
}

#ifdef FMCG_NO_BURST
#undef main
int main(int argc, char** argv) { return main_ref(argc, argv); }
#endif
