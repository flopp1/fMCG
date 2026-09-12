// bench_throughput -- raw event-parsing throughput on artificial valid MIDI.
//
// Generates a synthetic-but-valid MIDI file (note on/off stream, running
// status, all deltas 0 for maximum event density: 2 bytes/event) sized to a
// target event count, then parses it twice:
//
//   single pass : the normal tick-space engine
//   two pass    : the frame-bucketed fallback (forced via a tick-1 spec limit)
//
// It reports wall-clock seconds and raw events/s for each, so you can compare
// against real-world runs: if a real compressed file lands far below these
// numbers, the bottleneck is decode/I-O, not the parser.
//
// Usage: bench_throughput [events] [outfile] [dense|spread]
//   dense : all events at tick 0 (measures pure parse ceiling)
//   spread: delta 1 per event, ticks grow with the stream (adds the
//           per-tick array cache/TLB cost of a real black MIDI)
#include "../fMCG_core.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <chrono>
#include <string>
#include <vector>

using clock_ = std::chrono::steady_clock;

static void write_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x >> 24) & 0xFF); v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 8) & 0xFF);  v.push_back(x & 0xFF);
}
static void write_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((x >> 8) & 0xFF); v.push_back(x & 0xFF);
}

int main(int argc, char** argv) {
    const uint64_t target_events = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 50000000ull;
    const char* out_path = (argc > 2) ? argv[2] : "_bench.mid";
    const bool spread = (argc > 3 && std::string(argv[3]) == "spread");

    // ---- generate a dense, valid MIDI ------------------------------------
    // One track, every event preceded by a zero delta (densest legal timing),
    // explicit status bytes, alternating note-on/off:
    //   0x00 0x9n note vel        (4 bytes)
    //   0x00 0x8n note 0x00       (4 bytes)
    // => 8 bytes per pair = 4 bytes/event. All events sit at tick 0, so
    // tick-space arrays stay trivial -- the bench measures pure parse cost.
    const uint64_t pairs = target_events / 2;
    std::vector<uint8_t> mid;
    mid.reserve((size_t)(14 + 8 + pairs * 8 + 4));
    // header
    mid.insert(mid.end(), {'M','T','h','d'});
    write_u32(mid, 6);
    write_u16(mid, 0);        // format 0
    write_u16(mid, 1);        // one track
    write_u16(mid, 480);      // division
    // track chunk placeholder
    mid.insert(mid.end(), {'M','T','r','k'});
    write_u32(mid, (uint32_t)(pairs * 8 + 4));
    const uint8_t dl_on  = spread ? 1 : 0;   // spread: each event advances one tick
    const uint8_t dl_off = spread ? 1 : 0;
    uint8_t note = 0, ch = 0;
    for (uint64_t i = 0; i < pairs; ++i) {
        mid.push_back(dl_on);
        mid.push_back(0x90 | ch); mid.push_back(note); mid.push_back(100);
        mid.push_back(dl_off);
        mid.push_back(0x80 | ch); mid.push_back(note); mid.push_back(0x00);
        note = (uint8_t)((note + 1) & 0x7F);
        if (((i + 1) & 0x7F) == 0) ch = (uint8_t)((ch + 1) & 0x0F);
    }
    mid.push_back(0x00); mid.push_back(0xFF); mid.push_back(0x2F); mid.push_back(0x00);  // end of track
    // patch actual track length (generation wrote pairs*8+4 bytes)
    const uint32_t track_len = (uint32_t)(mid.size() - 22);
    mid[18] = (track_len >> 24) & 0xFF; mid[19] = (track_len >> 16) & 0xFF;
    mid[20] = (track_len >> 8) & 0xFF;  mid[21] = track_len & 0xFF;

    { FILE* f = fopen(out_path, "wb"); fwrite(mid.data(), 1, mid.size(), f); fclose(f); }
    const uint64_t actual_events = pairs * 2;
    printf("generated %s: %zu bytes, %llu events (all at tick 0)\n\n",
           out_path, mid.size(), (unsigned long long)actual_events);

    // ---- single pass -------------------------------------------------------
    {
        ProgressCallbacks cb;
        uint16_t div = 0; uint64_t notes = 0;
        auto t0 = clock_::now();
        auto frames = ScaleMidiProcessor::process_midi(out_path, 60.0, div, notes, true, cb);
        double s = std::chrono::duration<double>(clock_::now() - t0).count();
        printf("single pass: %.2fs  ->  %.0f events/s  (%llu notes, %zu frames)\n",
               s, s > 0 ? actual_events / s : 0.0,
               (unsigned long long)notes, frames.size());
    }
    // ---- two pass (forced) ---------------------------------------------------
    {
        ProgressCallbacks cb;
        cb.spec_tick_limit = 1;
        cb.on_spec_violation = [](uint64_t, size_t) { return 1; };   // restart in two-pass
        uint16_t div = 0; uint64_t notes = 0;
        auto t0 = clock_::now();
        auto frames = ScaleMidiProcessor::process_midi(out_path, 60.0, div, notes, true, cb);
        double s = std::chrono::duration<double>(clock_::now() - t0).count();
        printf("two pass   : %.2fs  ->  %.0f events/s effective  (%llu notes, %zu frames)\n",
               s, s > 0 ? actual_events / s : 0.0,
               (unsigned long long)notes, frames.size());
    }
    std::remove(out_path);
    return 0;
}
