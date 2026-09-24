/* mgs4vr_scan.h - byte-signature search in the (decrypted, in-memory) game image.
 *
 * The Steam stub decrypts .text at startup, so signatures can only be resolved
 * at runtime. A signature is accepted only when it matches EXACTLY ONCE: zero
 * or several matches mean "unknown build" and the feature stays off.
 * Pattern syntax: hex bytes separated by spaces, "??" = any byte. */
#ifndef MGS4VR_SCAN_H
#define MGS4VR_SCAN_H
#include <stddef.h>

/* Returns the number of matches found (counting stops at max_count), or -1 on a
   bad pattern / unreadable memory. *first receives the first match. */
int mgs4vr_scan(const unsigned char *begin, size_t len, const char *pattern,
                const unsigned char **first, int max_count);

/* Locate a section of a mapped PE image by name (e.g. ".text"). 1 on success. */
int mgs4vr_image_section(const void *image_base, const char *name,
                         const unsigned char **begin, size_t *len);

#endif
