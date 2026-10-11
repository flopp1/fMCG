// fmcg_engine.h -- tick-space single-pass streaming engine.
//
// One sequential walk over the image (mmap'd file, or libarchive chunks for
// compressed inputs -- the decompressed bytes are never materialized in full),
// accumulating note events into per-tick buckets and collecting tempo events.
// A final O(ticks + frames) sweep converts tick space to frame stats using
// anchor interpolation. Memory is O(duration*ppqn + tempo events), independent
// of event count: terabyte-scale decompressed inputs need no temp file, no
// spill threshold, and no second read of the stream.
#pragma once
#include "fmcg_midi.h"

#include <deque>
#include <condition_variable>
#include <thread>
#include <mutex>

namespace fmcg_stream {

// Held-note policy. By the MIDI spec the canonical polyphony is
// (note-ons - note-offs); a note never released is a spec violation, not a
// note the counter should invent an off for. Default: OFF = do not close.
// When ON, every note still held at a track's final tick is released at that
// tick (affects peak polyphony only). Set before starting a scan; the scan
// reads the value once per track / per walk. Process-wide, thread-safe.
void set_close_held_notes(bool close);
bool close_held_notes();

// Entry point: parse any supported input (plain MIDI or compressed archive)
// and produce per-frame statistics.
//
// parse_threads: number of worker threads for parsing plain (uncompressed)
// MIDI files, whose tracks parse independently and merge commutatively into
// the per-tick arrays. 0 or 1 = the sequential walk (also used for compressed
// inputs, whose single stream cannot split). Compressed files ignore the
// setting; their decode pipeline is already threaded.
//
// track_dedup: optional content-deduplication fast path for concatenated
// files (the "base MIDI duplicated N times, then compressed" construction).
// When enabled, a track whose raw bytes match one seen earlier in the same
// scan is replayed from a cached summary instead of being parsed again --
// O(unique bytes) instead of O(total bytes). Off by default.
//
// risky_dedup (needs track_dedup; layered xz files only): in each layer, once
// the first repeated tile is seen, assume the rest of the layer is that tile
// repeated if the remaining size is an exact multiple of the tile's size, and
// stop decoding the layer. WRONG (silently) on files where the rest of a layer
// is made of different tiles with a matching total size; the log states every
// assumption applied. Off by default.
std::vector<FrameStats> process_streaming(
    const std::string& filename, double fps, uint16_t& out_division,
    uint64_t& out_total_notes, bool vel0_as_note_off, uint64_t& out_total_ticks,
    const ProgressCallbacks& cb, double end_delay = 0.0,
    int parse_threads = 0, bool track_dedup = false, bool risky_dedup = false);

} // namespace fmcg_stream
