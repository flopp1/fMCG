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

// Entry point: parse any supported input (plain MIDI or compressed archive)
// and produce per-frame statistics.
std::vector<FrameStats> process_streaming(
    const std::string& filename, double fps, uint16_t& out_division,
    uint64_t& out_total_notes, bool vel0_as_note_off, uint64_t& out_total_ticks,
    const ProgressCallbacks& cb);

} // namespace fmcg_stream
