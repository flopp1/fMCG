#include "fmcg_render.h"
#include "fmcg_util.h"

#include <fstream>
#include <algorithm>
#include <cstdio>
#include <system_error>

// ffmpeg's filter arguments are parsed by its own grammar, not the shell's:
// backslashes are consumed as escapes (even inside quotes), apostrophes
// terminate the quoting, and colons must be escaped. A user-derived path can
// therefore never be passed through the subtitles= filter safely. Like the
// original fMCG (which rendered a fixed "temp_stats.ass" from the working
// directory), we move the ASS next to its own directory under a fixed bare
// name and cd there in the batch file, so the filter argument is a simple
// name no parser can mangle. Returns the renamed file's full path.
std::string prepare_ass_for_filter(const std::string& ass_filename) {
    size_t sep = ass_filename.find_last_of("/\\");
    std::string dir = (sep != std::string::npos) ? ass_filename.substr(0, sep + 1) : std::string("./");
    std::string safe = dir + "temp_stats.ass";
    std::remove(safe.c_str());
    if (std::rename(ass_filename.c_str(), safe.c_str()) != 0) {
        // rename can fail on exotic filesystem setups; fall back to a copy
        std::ifstream src(ass_filename.c_str(), std::ios::binary);
        if (src) {
            std::ofstream dst(safe.c_str(), std::ios::binary);
            dst << src.rdbuf();
        }
        std::remove(ass_filename.c_str());
    }
    return safe;
}

// Convert an ASS colour (&HAABBGGRR) to ffmpeg's "0xRRGGBB" colour spec for
// the lavfi color source. Falls back to black on malformed input.
std::string ffmpeg_color_spec(const std::string& ass_col) {
    if (ass_col.size() >= 8 && ass_col[0] == '&' && (ass_col[1] == 'H' || ass_col[1] == 'h')) {
        std::string hex = ass_col.substr(2);
        if (hex.size() == 8) hex = hex.substr(2);   // drop alpha -> BBGGRR
        if (hex.size() == 6) {
            std::string bb = hex.substr(0, 2), gg = hex.substr(2, 2), rr = hex.substr(4, 2);
            return "0x" + rr + gg + bb;
        }
    }
    return "0x000000";
}

int render_video(const std::string& ass_filename, const std::string& output_video,
                         const std::string& midi_dir, const std::string& midi_stem,
                         int width, int height, double fps, double total_duration,
                         const std::string& bg_color_ass) {
    std::string safe_ass = prepare_ass_for_filter(ass_filename);
    size_t sep = safe_ass.find_last_of("/\\");
    std::string dir_cd = (sep != std::string::npos) ? safe_ass.substr(0, sep) : std::string(".");
    std::string out_fwd = output_video;
    std::replace(out_fwd.begin(), out_fwd.end(), '\\', '/');

    std::string bat_file = midi_dir + midi_stem + "_fMCG_ffmpeg.bat";
    {
        std::ofstream bat(bat_file);
        bat << "@echo off\ncd /d \"" << dir_cd << "\" || exit /b 1\n";
        bat << "ffmpeg -y -f lavfi -i \"color=c=" << ffmpeg_color_spec(bg_color_ass)
            << ":s=" << width << "x" << height
            << ":r=" << fps << ":d=" << total_duration
            << "\" -vf \"subtitles=temp_stats.ass\""
            << " -c:v libx264 -pix_fmt yuv420p \"" << out_fwd << "\"\n";
    }

    std::string cmd = "\"" + bat_file + "\"";
    int ret = std::system(cmd.c_str());

    std::remove(safe_ass.c_str());
    std::remove(ass_filename.c_str());
    std::remove(bat_file.c_str());

    return ret;
}
