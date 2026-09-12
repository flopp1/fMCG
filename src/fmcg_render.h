// fmcg_render.h -- FFmpeg invocation.
//
// ffmpeg's filter-argument grammar mangles user paths (backslash escapes,
// apostrophe quoting, colon separators), so the ASS is renamed next to its
// own directory under a fixed bare name (temp_stats.ass) and ffmpeg is
// spawned with its working directory set to that directory. The argument
// list built here contains no user paths for the filter argument, so it can
// be passed straight to exec/CreateProcess with no shell and no batch file.
#pragma once
#include <string>
#include <vector>

// Copy the generated ASS next to itself under the fixed bare name that the
// subtitles filter receives. A copy (not a rename) keeps the original in
// place so the same processed MIDI can be rendered again later. Returns the
// copy's full path.
std::string prepare_ass_for_filter(const std::string& ass_filename);

// Convert an ASS colour (&HAABBGGRR) to ffmpeg's "0xRRGGBB" colour spec for
// the lavfi color source. Falls back to black on malformed input.
std::string ffmpeg_color_spec(const std::string& ass_col);

// Build the ffmpeg argument list (argv, excluding the program name) for the
// overlay render: lavfi colour background + subtitles filter + libx264.
// `progress_to_stdout` enables "-progress pipe:1" so the caller can parse
// out_time_ms lines from the child's stdout for live progress.
std::vector<std::string> ffmpeg_args(const std::string& output_video,
                                     int width, int height, double fps,
                                     double total_duration,
                                     const std::string& bg_color_ass,
                                     bool progress_to_stdout = true);
