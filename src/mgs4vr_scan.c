#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "mgs4vr_scan.h"

#define MAX_PATTERN 64

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse(const char *p, unsigned char *bytes, unsigned char *mask) {
    int n = 0;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (n >= MAX_PATTERN) return -1;
        if (p[0] == '?' && p[1] == '?') { bytes[n] = 0; mask[n] = 0; }
        else {
            int hi = hexval(p[0]), lo = hi < 0 ? -1 : hexval(p[1]);
            if (hi < 0 || lo < 0) return -1;
            bytes[n] = (unsigned char)(hi * 16 + lo); mask[n] = 1;
        }
        ++n; p += 2;
        if (*p && *p != ' ') return -1;
    }
    return n;
}

int mgs4vr_scan(const unsigned char *begin, size_t len, const char *pattern,
                const unsigned char **first, int max_count) {
    unsigned char bytes[MAX_PATTERN], mask[MAX_PATTERN];
    int n = parse(pattern, bytes, mask), count = 0, i;
    const unsigned char *p, *end;
    if (first) *first = NULL;
    if (n <= 0 || !mask[0] || !begin || len < (size_t)n) return -1;    /* first byte must be concrete: it anchors memchr */
    end = begin + len - (size_t)n;
    __try {
        for (p = begin; p <= end; ++p) {
            p = (const unsigned char *)memchr(p, bytes[0], (size_t)(end - p) + 1);
            if (!p) break;
            for (i = 1; i < n; ++i) if (mask[i] && p[i] != bytes[i]) break;
            if (i == n) {
                if (count == 0 && first) *first = p;
                if (++count >= max_count) break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return count;
}

int mgs4vr_image_section(const void *image_base, const char *name,
                         const unsigned char **begin, size_t *len) {
    const BYTE *base = (const BYTE *)image_base;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS64 *nt;
    const IMAGE_SECTION_HEADER *sec;
    WORD i;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char n8[9] = { 0 };
        memcpy(n8, sec->Name, 8);
        if (strcmp(n8, name) == 0) {
            *begin = base + sec->VirtualAddress;
            *len = sec->Misc.VirtualSize;
            return 1;
        }
    }
    return 0;
}
