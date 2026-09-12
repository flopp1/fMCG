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
// name and spawn ffmpeg with that directory as its working directory, so the
// filter argument is a simple name no parser can mangle. Returns the renamed
// file's full path.
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

std::vector<std::string> ffmpeg_args(const std::string& output_video,
                                     int width, int height, double fps,
                                     double total_duration,
                                     const std::string& bg_color_ass,
                                     bool progress_to_stdout) {
    std::string out_fwd = output_video;
    std::replace(out_fwd.begin(), out_fwd.end(), '\\', '/');

    std::vector<std::string> a;
    a.push_back("-y");
    a.push_back("-f");            a.push_back("lavfi");
    a.push_back("-i");            a.push_back("color=c=" + ffmpeg_color_spec(bg_color_ass)
                                                + ":s=" + std::to_string(width) + "x" + std::to_string(height)
                                                + ":r=" + std::to_string(fps)
                                                + ":d=" + std::to_string(total_duration));
    a.push_back("-vf");           a.push_back("subtitles=temp_stats.ass");
    a.push_back("-c:v");          a.push_back("libx264");
    a.push_back("-pix_fmt");      a.push_back("yuv420p");
    if (progress_to_stdout) {
        a.push_back("-progress"); a.push_back("pipe:1");
    }
    a.push_back(out_fwd);
    return a;
}
