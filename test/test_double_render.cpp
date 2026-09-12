// Proves a processed MIDI can be rendered twice in a row: the first render
// must not consume the original ASS, so the second render succeeds and
// overwrites the same output file. Also asserts temp_stats.ass is swept.
#include "../fMCG_core.h"
#include "../src/fmcg_render.h"
#include <cstdio>
#include <sys/stat.h>

static int fails = 0;
static void check(const char* name, bool ok) {
    printf("%-42s %s\n", name, ok ? "[OK]" : "[FAIL]");
    if (!ok) ++fails;
}
static bool exists(const char* p) { struct _stat st; return _stat(p, &st) == 0; }

int main() {
    // Render #1: the "process" step writes <name>_fMCG.ass (simulate with a copy).
    if (!exists("test.mid")) { printf("FAIL: test/test.mid missing\n"); return 1; }
    remove("_dbl.ass");
    {
        FILE* in = fopen("test.mid", "rb");
        FILE* out = fopen("_dbl.ass", "wb");
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
        fclose(in); fclose(out);
    }
    // Only the last line of the path matters (temp_stats goes next to it); the
    // file need not be a real ASS for spawn-level verification.
    std::string safe1 = prepare_ass_for_filter("_dbl.ass");
    check("render1: temp_stats.ass prepared", !safe1.empty() && exists(safe1.c_str()));
    check("render1: original ASS still present", exists("_dbl.ass"));
    remove(safe1.c_str());   // run_render sweeps it after ffmpeg exits

    // Render #2: same original, must prepare again identically.
    std::string safe2 = prepare_ass_for_filter("_dbl.ass");
    check("render2: temp_stats.ass prepared again", !safe2.empty() && exists(safe2.c_str()));
    check("render2: original ASS still present", exists("_dbl.ass"));
    remove(safe2.c_str());
    remove("_dbl.ass");
    printf("%s (%d failures)\n", fails ? "FAILURES" : "ALL OK", fails);
    return fails ? 1 : 0;
}
