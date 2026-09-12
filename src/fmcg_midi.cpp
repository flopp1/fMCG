#include "fmcg_midi.h"

#include <cstring>
#include <cstdio>
#include <algorithm>
#include "fmcg_engine.h"

// ---------------------------------------------------------------------------
// Compressed archive magic detection
// ---------------------------------------------------------------------------

bool is_7z_magic(const uint8_t* m) {
    return m[0]==0x37 && m[1]==0x7A && m[2]==0xBC && m[3]==0xAF && m[4]==0x27 && m[5]==0x1C;
}
bool is_xz_magic(const uint8_t* m) {
    return m[0]==0xFD && m[1]==0x37 && m[2]==0x7A && m[3]==0x58 && m[4]==0x5A && m[5]==0x00;
}
bool is_rar_magic(const uint8_t* m) {
    return m[0]==0x52 && m[1]==0x61 && m[2]==0x72 && m[3]==0x21 && m[4]==0x1A && m[5]==0x07;
}
bool is_compressed_magic(const uint8_t* m, size_t n) {
    if (n < 6) return false;
    if (is_7z_magic(m) || is_xz_magic(m) || is_rar_magic(m)) return true;
    // gzip: 1f 8b
    if (m[0]==0x1F && m[1]==0x8B) return true;
    // bzip2: 'BZh' + level digit ('1'..'9') + pi digits 31 41 59 26
    if (m[0]=='B' && m[1]=='Z' && m[2]=='h' && m[3]>='1' && m[3]<='9'
        && n >= 10 && m[4]==0x31 && m[5]==0x41) return true;
    // zstd: 28 b5 2f fd (frame magic, little-endian 0xFD2FB528)
    if (m[0]==0x28 && m[1]==0xB5 && m[2]==0x2F && m[3]==0xFD) return true;
    // lz4 frame: 04 22 4D 18
    if (m[0]==0x04 && m[1]==0x22 && m[2]==0x4D && m[3]==0x18) return true;
    return false;
}
static size_t read_file_header(const std::string& path, uint8_t* out, size_t n) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    size_t r = fread(out, 1, n, f);
    fclose(f);
    return r;
}
bool is_compressed_file(const std::string& path) {
    uint8_t magic[10];
    size_t n = read_file_header(path, magic, sizeof(magic));
    return is_compressed_magic(magic, n);
}

// ---------------------------------------------------------------------------
// BinaryReader
// ---------------------------------------------------------------------------

void BinaryReader::ensure_buf() {
    if (!buf) buf.reset(new uint8_t[BUFSIZE]);
}

void BinaryReader::refill() {
    buf_pos = pos;
    buf_len = 0;
    if (pos >= file_size) return;
    ensure_buf();
    while (buf_len < BUFSIZE && buf_pos + buf_len < file_size) {
#if defined(_WIN32)
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)((buf_pos + buf_len) & 0xFFFFFFFF);
        ov.OffsetHigh = (DWORD)(((buf_pos + buf_len) >> 32) & 0xFFFFFFFF);
        DWORD n = 0;
        if (!ReadFile(hFile, buf.get() + buf_len, (DWORD)(BUFSIZE - buf_len), &n, &ov) || n == 0) break;
        buf_len += n;
#else
        ssize_t n = pread(fd, buf.get() + buf_len, BUFSIZE - buf_len, buf_pos + buf_len);
        if (n <= 0) break;
        buf_len += n;
#endif
    }
}

BinaryReader::BinaryReader(const std::string& path, size_t start, bool want_mmap)
    : use_mmap(want_mmap), pos(start), buf_pos(0), buf_len(0) {

#if defined(_WIN32)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) { file_size = 0; return; }
    std::wstring wpath(static_cast<size_t>(wlen) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);
    hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING,
                        use_mmap ? FILE_ATTRIBUTE_NORMAL
                                 : (FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN), nullptr);
    if (hFile == INVALID_HANDLE_VALUE) { file_size = 0; return; }
    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li) || li.QuadPart <= 0) {
        CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; file_size = 0; return;
    }
    file_size = static_cast<size_t>(li.QuadPart);

    if (use_mmap && file_size > 0) {
        hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (hMap) {
            mmap_ptr = (uint8_t*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
            if (!mmap_ptr) { CloseHandle(hMap); hMap = nullptr; use_mmap = false; }
            else { mmap_len = file_size; }
        } else {
            use_mmap = false;
        }
    }
#else
    fd = open(path.c_str(), O_RDONLY);
    if (fd == -1) { file_size = 0; return; }
    struct stat st;
    fstat(fd, &st);
    file_size = st.st_size;
#if defined(__linux__)
    posix_fadvise(fd, 0, 0, use_mmap ? POSIX_FADV_RANDOM : POSIX_FADV_SEQUENTIAL);
#endif

    if (use_mmap && file_size > 0) {
        mmap_ptr = (uint8_t*)mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mmap_ptr == MAP_FAILED) { mmap_ptr = nullptr; use_mmap = false; }
        else { mmap_len = file_size; }
    }
#endif
}

BinaryReader::~BinaryReader() {
    if (use_mmap) {
#if defined(_WIN32)
        if (mmap_ptr) UnmapViewOfFile(mmap_ptr);
        if (hMap) CloseHandle(hMap);
#else
        if (mmap_ptr) munmap(mmap_ptr, mmap_len);
#endif
    }
#if defined(_WIN32)
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
#else
    if (fd != -1) close(fd);
#endif
}

BinaryReader::BinaryReader(BinaryReader&& o) noexcept
    : use_mmap(o.use_mmap),
      mmap_ptr(o.mmap_ptr), mmap_len(o.mmap_len),
      file_size(o.file_size), pos(o.pos), buf_pos(o.buf_pos), buf_len(o.buf_len) {
#if defined(_WIN32)
    hFile = o.hFile; o.hFile = INVALID_HANDLE_VALUE;
    hMap = o.hMap; o.hMap = nullptr;
#else
    fd = o.fd; o.fd = -1;
#endif
    o.mmap_ptr = nullptr;
    o.buf = std::move(buf); o.buf_pos = 0; o.buf_len = 0;
}

size_t BinaryReader::read_raw(void* dst, size_t n) {
    if (use_mmap) {
        size_t avail = file_size - pos;
        if (n > avail) n = avail;
        std::memcpy(dst, mmap_ptr + pos, n);
        pos += n;
        return n;
    }
    ensure_buf();
    size_t out = 0;
    uint8_t* d = (uint8_t*)dst;
    while (out < n && pos < file_size) {
        if (pos < buf_pos || pos >= buf_pos + buf_len) {
            refill();
            if (buf_len == 0) break;
        }
        size_t take = std::min(n - out, buf_pos + buf_len - pos);
        std::memcpy(d + out, buf.get() + (pos - buf_pos), take);
        pos += take;
        out += take;
    }
    return out;
}

// ---------------------------------------------------------------------------
// ScaleMidiProcessor -- facade over the streaming engine
// ---------------------------------------------------------------------------

std::vector<FrameStats> ScaleMidiProcessor::process_midi(
    const std::string& filename, double fps, uint16_t& out_division,
    uint64_t& out_total_notes, bool vel0_as_note_off, const ProgressCallbacks& cb)
{
    return fmcg_stream::process_streaming(filename, fps, out_division,
                                          out_total_notes, vel0_as_note_off, cb);
}
