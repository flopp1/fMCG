#include <cstdio>
#include <cerrno>
#define main fmcg_program_main
#include "../fMCG.cpp"
#undef main

int main() {
    printf("font dirs:\n");
    for (auto& d : get_font_directories()) {
        DIR* dp = opendir(d.c_str());
        if (dp) {
            int n = 0;
            while (readdir(dp)) n++;
            closedir(dp);
            printf("  %s  entries via readdir: %d\n", d.c_str(), n);
        } else {
            printf("  %s  opendir FAILED (errno=%d)\n", d.c_str(), errno);
        }
    }
    std::vector<std::string> files;
    for (auto& d : get_font_directories()) collect_font_files(d, files, 0);
    printf("font files found: %zu\n", files.size());
    if (!files.empty()) {
        printf("  first: %s\n", files[0].c_str());
        std::ifstream f(files[0], std::ios::binary);
        uint8_t hdr[4];
        f.read((char*)hdr, 4);
        printf("  magic: %02X %02X %02X %02X\n", hdr[0], hdr[1], hdr[2], hdr[3]);
        printf("  family: '%s'\n", read_font_family(files[0]).c_str());
        printf("  arial family: '%s'\n", read_font_family("C:\\WINDOWS\\Fonts\\arial.ttf").c_str());
    }
    auto fonts = enumerate_system_fonts();
    printf("families: %zu\n", fonts.size());
    for (size_t i = 0; i < fonts.size() && i < 8; ++i) printf("  [%zu] %s\n", i + 1, fonts[i].c_str());
    return 0;
}
