// test_archive_formats -- every compressed input format the GUI offers is
// readable by this build's libarchive (static on Windows, the system one on
// Linux): a fixture MIDI is packed with libarchive's own writer as raw
// gzip / bzip2 / xz / zstd / lz4 streams and as 7z with each compression
// method, then scanned by the engine; the counts must equal the plain file's.
// A missing codec in the build fails here instead of on a user's file.
// Multi-block .xz files (read through liblzma's multi-threaded decoder) are
// written with liblzma's multi-threaded encoder: a raw MIDI and a tar, plus
// damaged copies that must fail cleanly.
#include "src/fmcg_engine.h"

#include <archive.h>
#include <archive_entry.h>
#include <lzma.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static int fails = 0;
static void check(const std::string& what, bool ok) {
    std::printf("%-60s [%s]\n", what.c_str(), ok ? "OK" : "FAIL");
    if (!ok) ++fails;
}

static std::vector<char> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// filter: raw stream with that filter; else format "7zip" with option `seven`.
static bool write_archive(const std::string& out, const std::vector<char>& data, int filter,
                          const char* seven) {
    archive* a = archive_write_new();
    if (seven) {
        archive_write_set_format_7zip(a);
        if (archive_write_set_format_option(a, "7zip", "compression", seven) != ARCHIVE_OK) {
            archive_write_free(a);
            return false;
        }
    } else {
        archive_write_set_format_raw(a);
        if (archive_write_add_filter(a, filter) != ARCHIVE_OK) { archive_write_free(a); return false; }
    }
    if (archive_write_open_filename(a, out.c_str()) != ARCHIVE_OK) { archive_write_free(a); return false; }
    archive_entry* e = archive_entry_new();
    archive_entry_set_pathname(e, "song.mid");
    archive_entry_set_size(e, (la_int64_t)data.size());
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    bool ok = archive_write_header(a, e) == ARCHIVE_OK &&
              archive_write_data(a, data.data(), data.size()) == (la_ssize_t)data.size();
    archive_entry_free(e);
    ok &= archive_write_close(a) == ARCHIVE_OK;
    archive_write_free(a);
    return ok;
}

// .xz with blocks of `block` bytes (several blocks: the engine's
// multi-threaded decode path).
static bool write_xz_blocks(const std::string& out, const std::vector<char>& data, uint64_t block) {
    lzma_mt mt{};
    mt.threads = 2;
    mt.block_size = block;
    mt.preset = 6;
    mt.check = LZMA_CHECK_CRC64;
    lzma_stream s = LZMA_STREAM_INIT;
    if (lzma_stream_encoder_mt(&s, &mt) != LZMA_OK) return false;
    std::vector<uint8_t> buf(data.size() + data.size() / 2 + (1 << 16));
    s.next_in = (const uint8_t*)data.data();
    s.avail_in = data.size();
    s.next_out = buf.data();
    s.avail_out = buf.size();
    const lzma_ret r = lzma_code(&s, LZMA_FINISH);
    const size_t n = buf.size() - s.avail_out;
    lzma_end(&s);
    if (r != LZMA_STREAM_END) return false;
    std::FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(buf.data(), 1, n, f) == n;
    std::fclose(f);
    return ok;
}

// A tar holding `data` as song.mid, in memory.
static std::vector<char> tar_of(const std::vector<char>& data) {
    std::vector<char> out(data.size() + (1 << 16));
    size_t used = 0;
    archive* a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    archive_write_open_memory(a, out.data(), out.size(), &used);
    archive_entry* e = archive_entry_new();
    archive_entry_set_pathname(e, "song.mid");
    archive_entry_set_size(e, (la_int64_t)data.size());
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    archive_write_header(a, e);
    archive_write_data(a, data.data(), data.size());
    archive_entry_free(e);
    archive_write_close(a);
    archive_write_free(a);
    out.resize(used);
    return out;
}

static bool write_file(const std::string& out, const std::vector<char>& data) {
    std::FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

static std::string counts(const std::string& path) {
    uint16_t ppqn = 0;
    uint64_t notes = 0, ticks = 0;
    ProgressCallbacks cb;
    cb.on_log = [](const char*, bool) {};
    const fmcg_stream::ScanResult scan1 = fmcg_stream::process_streaming(path, fmcg_stream::ScanOptions{60.0, true}, cb);
    auto fr = scan1.frames;
    ppqn = scan1.ppqn;
    notes = scan1.total_notes;
    ticks = scan1.total_ticks;
    if (fr.empty()) return "FAIL";
    return std::to_string(notes) + "/" + std::to_string(ticks) + "/" + std::to_string(ppqn) + "/" +
           std::to_string(fr.back().peak_polyphony);
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const std::string midi = argc > 2 ? argv[2] : "test/data/tempo2.mid";
    std::printf("%s\n", archive_version_details());
    const std::vector<char> data = slurp(midi);
    check("fixture read", !data.empty());
    const std::string want = counts(midi);
    std::printf("plain: %s\n", want.c_str());
    struct Case { const char* name; int filter; const char* seven; };
    const Case cases[] = {
        {"gzip", ARCHIVE_FILTER_GZIP, nullptr},   {"bzip2", ARCHIVE_FILTER_BZIP2, nullptr},
        {"xz", ARCHIVE_FILTER_XZ, nullptr},       {"zstd", ARCHIVE_FILTER_ZSTD, nullptr},
        {"lz4", ARCHIVE_FILTER_LZ4, nullptr},
        {"7z lzma1", 0, "lzma1"}, {"7z lzma2", 0, "lzma2"}, {"7z bzip2", 0, "bzip2"},
        {"7z deflate", 0, "deflate"}, {"7z ppmd", 0, "ppmd"}, {"7z zstd", 0, "zstd"},
    };
    int n = 0;
    for (const Case& c : cases) {
        const std::string out = dir + "/_test_archive_" + std::to_string(n++) + ".bin";
        const bool w = write_archive(out, data, c.filter, c.seven);
        check(std::string(c.name) + ": written by libarchive", w);
        if (w) check(std::string(c.name) + ": engine reads the same counts", counts(out) == want);
        std::remove(out.c_str());
    }

    // Multi-block xz. The song is the fixture repeated (several MThd overlay
    // into one tick space), so the blocks hold enough bytes to differ.
    std::vector<char> big;
    for (int i = 0; i < 400; ++i) big.insert(big.end(), data.begin(), data.end());
    const std::string plain = dir + "/_test_archive_big.mid";
    check("multi-block: plain fixture written", write_file(plain, big));
    const std::string want_big = counts(plain);
    const std::string xz = dir + "/_test_archive_mt.xz";
    for (uint64_t block : {1000u, 4096u, 1u << 20}) {
        const std::string tag = "multi-block xz, " + std::to_string(block) + " B blocks";
        const bool w = write_xz_blocks(xz, big, block);
        check(tag + ": written", w);
        if (w) check(tag + ": engine reads the same counts", counts(xz) == want_big);
    }
    const std::string tarxz = dir + "/_test_archive_mt.tar.xz";
    const bool wt = write_xz_blocks(tarxz, tar_of(big), 2048);
    check("multi-block tar.xz: written", wt);
    if (wt) check("multi-block tar.xz: engine reads the same counts", counts(tarxz) == want_big);
    // damaged copies: a flipped byte inside a block, and a truncated file
    if (write_xz_blocks(xz, big, 1000)) {
        std::vector<char> bad = slurp(xz);
        bad[bad.size() / 2] ^= 0x55;
        write_file(xz, bad);
        check("multi-block xz, corrupt block: no count", counts(xz) != want_big);
        bad = slurp(tarxz);
        bad.resize(bad.size() * 2 / 3);
        write_file(xz, bad);
        check("multi-block tar.xz, truncated: no count", counts(xz) != want_big);
    }
    std::remove(plain.c_str());
    std::remove(xz.c_str());
    std::remove(tarxz.c_str());

    std::printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL OK", fails);
    return fails ? 1 : 0;
}
