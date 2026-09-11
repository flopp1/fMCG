#include <cstdio>
#include <cstring>
#include <new>
#define main fmcg_program_main
#include "../fMCG.cpp"
#undef main

// Usage: probe_big.exe <midi> [fps] [--csv out.csv]
int main(int argc, char* argv[]) {
    printf("hardware_concurrency: %u\n", std::thread::hardware_concurrency());
    const char* path = argc > 1 ? argv[1] : "test.mid";
    double fps = 60.0;
    const char* csv_path = nullptr;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv_path = argv[++i];
        else fps = atof(argv[i]);
    }
    printf("parsing '%s' at fps %g ...\n", path, fps);
    try {
        uint16_t div = 0;
        uint64_t total = 0;
        auto frames = ScaleMidiProcessor::process_midi(path, fps, div, total, true);
        printf("frames: %zu, division: %u, total_notes: %llu\n",
               frames.size(), div, (unsigned long long)total);
        if (!frames.empty())
            printf("duration: %.3f s\n", frames.back().timestamp_sec);
        else
            printf("RESULT: process_midi returned EMPTY\n");
        if (csv_path && !frames.empty()) {
            FILE* f = fopen(csv_path, "w");
            if (!f) { printf("cannot open csv '%s'\n", csv_path); return 2; }
            fprintf(f, "frame,timestamp_sec,cumulative_notes,current_nps,peak_nps,polyphony,peak_polyphony,bpm\n");
            for (const auto& s : frames)
                fprintf(f, "%zu,%.3f,%llu,%lld,%lld,%lld,%lld,%.2f\n",
                        (size_t)s.frame_index, s.timestamp_sec,
                        (unsigned long long)s.cumulative_notes,
                        (long long)s.notes_per_second, (long long)s.peak_nps,
                        (long long)s.polyphony, (long long)s.peak_polyphony, s.bpm);
            fclose(f);
            printf("CSV written: %s\n", csv_path);
        }
    } catch (const std::exception& e) {
        printf("EXCEPTION: %s\n", e.what());
    } catch (...) {
        printf("EXCEPTION: unknown\n");
    }
    return 0;
}

