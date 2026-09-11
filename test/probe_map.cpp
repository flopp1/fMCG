#include <cstdio>
#include <fstream>
#include <cstdint>
#include <cstring>

// Exact replica of ScaleMidiProcessor::map_file's Windows branch, instrumented
int main(int argc, char* argv[]) {
    const char* path = argc > 1 ? argv[1] : "test.mid";
    size_t size = 0;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    printf("is_open: %d\n", (int)file.is_open());
    size = (size_t)file.tellg();
    printf("tellg (size): %zu\n", size);
    file.seekg(0);
    printf("state after seekg: good=%d fail=%d bad=%d eof=%d\n",
           (int)file.good(), (int)file.fail(), (int)file.bad(), (int)file.eof());

    uint8_t* buffer = new (std::nothrow) uint8_t[size];
    printf("buffer allocated: %d\n", (int)(buffer != nullptr));
    if (!buffer) return 1;

    file.read(reinterpret_cast<char*>(buffer), (std::streamsize)size);
    printf("state after read: good=%d fail=%d bad=%d eof=%d\n",
           (int)file.good(), (int)file.fail(), (int)file.bad(), (int)file.eof());
    printf("gcount (bytes actually read): %lld (expected %zu)\n", (long long)file.gcount(), size);

    printf("first 12 bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
           buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5],
           buffer[6], buffer[7], buffer[8], buffer[9], buffer[10], buffer[11]);
    printf("last 4 bytes: %02X %02X %02X %02X\n",
           buffer[size-4], buffer[size-3], buffer[size-2], buffer[size-1]);
    delete[] buffer;
    return 0;
}
