#include <cstdio>
#include <cstring>
#include "archive.h"
#include "archive_entry.h"

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: probe_stream <archive>\n"); return 1; }
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    int r = archive_read_open_filename(a, argv[1], 10240);
    printf("open=%d err=%s\n", r, archive_error_string(a));
    struct archive_entry* entry;
    r = archive_read_next_header(a, &entry);
    printf("next_header=%d err=%s\n", r, archive_error_string(a));
    printf("entry=%s size=%lld\n", archive_entry_pathname(entry),
           (long long)archive_entry_size(entry));
    for (int blk = 0; blk < 3; ++blk) {
        const void* buf; size_t sz; long long off;
        r = archive_read_data_block(a, &buf, &sz, &off);
        printf("block%d ret=%d sz=%zu off=%lld err=%s\n",
               blk, r, sz, off, archive_error_string(a));
        if (r == ARCHIVE_OK && buf) {
            const unsigned char* p = (const unsigned char*)buf;
            printf("  first16: ");
            for (int i = 0; i < 16 && (size_t)i < sz; ++i) printf("%02X ", p[i]);
            printf("\n");
        }
        if (r != ARCHIVE_OK) break;
    }
    archive_read_free(a);
    return 0;
}
