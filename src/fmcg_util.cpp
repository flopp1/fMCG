#include "fmcg_util.h"
#include "fmcg_format.h"

#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// File size formatting
// ---------------------------------------------------------------------------

std::string format_file_size(uint64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    double val = (double)bytes;
    const char* units[] = {"KiB", "MiB", "GiB", "TiB"};
    for (int i = 0; i < 4; ++i) {
        val /= 1024.0;
        if (val < 1024.0 || i == 3) {
            char buf[32];
            if (val >= 100.0) snprintf(buf, sizeof(buf), "%.0f %s", val, units[i]);
            else if (val >= 10.0) snprintf(buf, sizeof(buf), "%.1f %s", val, units[i]);
            else snprintf(buf, sizeof(buf), "%.2f %s", val, units[i]);
            return buf;
        }
    }
    return std::to_string(bytes) + " B";
}

// ---------------------------------------------------------------------------

std::string format_file_size_rate(uint64_t bytes, double seconds) {
    if (seconds <= 0.0) return "0 B/s";
    return format_file_size((uint64_t)((double)bytes / seconds)) + "/s";
}

std::string format_with_commas(uint64_t val) {
    std::string s = std::to_string(val);
    int insert_pos = static_cast<int>(s.length()) - 3;
    while (insert_pos > 0) {
        s.insert(insert_pos, ",");
        insert_pos -= 3;
    }
    return s;
}

// Zero-pad to a minimum width (never truncates longer numbers). pad<=0 -> plain.
std::string pad_num(uint64_t val, int pad) {
    std::string s = std::to_string(val);
    if (pad > 0 && static_cast<int>(s.size()) < pad)
        s.insert(0, static_cast<size_t>(pad) - s.size(), '0');
    return s;
}

// Negative-aware, truncating toward zero (standard clock behaviour):
// -2.5 -> "-00:02.500". Used for the start-delay countdown, where current-time
// fields run from negative up to 0 as the song approaches.
std::string format_time(double total_sec, bool milli, int pad) {
    (void)pad;   // padding applies to numeric fields only, not mm:ss
    bool neg = total_sec < 0;
    double av = neg ? -total_sec : total_sec;
    long long iv = static_cast<long long>(av);
    int ms = milli ? static_cast<int>(std::lround((av - (double)iv) * 1000.0)) : 0;
    if (ms >= 1000) { ms -= 1000; iv += 1; }
    unsigned long long a = (unsigned long long)iv;
    int mins = (int)(a / 60);
    int secs = (int)(a % 60);
    std::ostringstream ss;
    if (neg) ss << '-';
    ss << std::setfill('0') << std::setw(2) << mins << ":" << std::setw(2) << secs;
    if (milli) ss << "." << std::setw(3) << ms;
    return ss.str();
}


std::string to_ass_time(double seconds) {
    int hrs = static_cast<int>(seconds) / 3600;
    int mins = (static_cast<int>(seconds) % 3600) / 60;
    int secs = static_cast<int>(seconds) % 60;
    int cs = static_cast<int>((seconds - static_cast<int>(seconds)) * 100);
    std::ostringstream ss;
    ss << hrs << ":" << std::setfill('0') << std::setw(2) << mins << ":"
       << std::setw(2) << secs << "." << std::setw(2) << cs;
    return ss.str();
}
