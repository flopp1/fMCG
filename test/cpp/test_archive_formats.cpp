// test_archive_formats -- every compressed input format the GUI offers is
// readable by this build's libarchive (static on Windows, the system one on
// Linux): a fixture MIDI is packed with libarchive's own writer as raw
// gzip / bzip2 / xz / zstd / lz4 streams and as 7z with each compression
// method, then scanned by the engine; the counts must equal the plain file's.
// A missing codec in the build fails here instead of on a user's file.
#include "src/fmcg_engine.h"

#include <archive.h>
#include <archive_entry.h>

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

static std::string counts(const std::string& path) {
    uint16_t ppqn = 0;
    uint64_t notes = 0, ticks = 0;
    ProgressCallbacks cb;
    cb.on_log = [](const char*, bool) {};
    auto fr = fmcg_stream::process_streaming(path, 60.0, ppqn, notes, true, ticks, cb);
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
    std::printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL OK", fails);
    return fails ? 1 : 0;
}
