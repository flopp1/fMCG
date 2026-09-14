#pragma once
#include <string>
#include <cstdint>

std::string format_file_size(uint64_t bytes);
std::string format_file_size_rate(uint64_t bytes, double seconds);   // "X MB/s"
std::string format_with_commas(uint64_t val);
std::string pad_num(uint64_t val, int pad);   // never truncates; pad<=0 -> plain
std::string format_time(double total_sec, bool milli, int pad = 0);
std::string to_ass_time(double seconds);
