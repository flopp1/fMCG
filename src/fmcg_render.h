// fmcg_render.h -- FFmpeg invocation. Writes a batch script with a fixed bare
// ASS filename because ffmpeg's filter-argument grammar mangles user paths.
#pragma once
#include <string>

std::string prepare_ass_for_filter(const std::string& ass_filename);
std::string ffmpeg_color_spec(const std::string& ass_col);
int render_video(const std::string& ass_filename, const std::string& output_video,
                 const std::string& midi_dir, const std::string& midi_stem,
                 int width, int height, double fps, double total_duration,
                 const std::string& bg_color_ass = "&H00000000");
