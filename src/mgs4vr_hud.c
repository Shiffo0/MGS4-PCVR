/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
/* HUD off in VR with the Solid Eye overlays kept; see mgs4vr_hud.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "mgs4vr_hud.h"

/* Layout ids of the corner HUD (sim 16 Oct; creator in brackets): life 0xB66172 (0xE3BA30/0xF2F6A0/0x14D3C00), camo index
   0x3E6492 + 0x8B1931 (0xF4D820), weapon/item panels 0x59FFD5 (0x4CB380), 0x29FC73 (0x4C9340), 0x99FAA3 (0x4CD650),
   0xB6C1DB (0x4B65F0), 0xB14341 (0x4D0AA0), 0xAF2275 (0x4CEC40), 0xB679A0 (0x4D02A0), weapon picture 0x128499, 0xA324E1. */
static const unsigned hud_ids[] = { 0xb66172, 0x3e6492, 0x8b1931, 0x59ffd5, 0x29fc73, 0x99faa3, 0xb6c1db, 0xb14341, 0xaf2275, 0xb679a0, 0x128499, 0xa324e1 };
/* 17 Oct 2026 (user): hide only in first person, and only the life gauge and the weapon/item panels; the camo index
   stays. Marker "on" = that; "on all" = the old behaviour (whole corner HUD, also in third person: no item names when
   picking things up, no weapon change there). */
static volatile LONG hud_all;
static int camo_id(unsigned id) { return id == 0x3e6492 || id == 0x8b1931; }
int mgs4vr_hud_hidden_id(unsigned id) {
    size_t i; if (!hud_all && camo_id(id)) return 0;
    for (i = 0; i < sizeof(hud_ids) / sizeof(hud_ids[0]); ++i) if (hud_ids[i] == id) return 1;
    return 0;
}

void mgs4vr_hud_set_all(int all) { InterlockedExchange(&hud_all, all ? 1 : 0); }
#define HUD_SLOTS 64
#define HUD_MAX_NODES 1024
#define HUD_MAX_LAYOUTS 4000
typedef struct { uint64_t layout; unsigned id; unsigned char bits[HUD_MAX_NODES / 8]; } HUD_SLOT;
static HUD_SLOT hud_slot[HUD_SLOTS]; static int hud_held;   /* game thread */
static volatile LONG hud_want, hud_frames, hud_hidden, hud_shown, hud_faults, hud_layouts, hud_full;
static int ptr_ok(uint64_t p) { return p >= 0x10000 && p < 0x800000000000ULL; }
static HUD_SLOT *slot_for(uint64_t layout, unsigned id, int create) {
    int i; HUD_SLOT *free_slot = NULL;
    for (i = 0; i < HUD_SLOTS; ++i) { if (hud_slot[i].layout == layout && hud_slot[i].id == id) return &hud_slot[i]; if (!hud_slot[i].layout && !free_slot) free_slot = &hud_slot[i]; }
    if (!create || !free_slot) { if (create) InterlockedIncrement(&hud_full); return NULL; }
    memset(free_slot, 0, sizeof(*free_slot)); free_slot->layout = layout; free_slot->id = id; return free_slot;
}
int mgs4vr_hud_frame(void *base, int want) {
    int changed = 0, seen = 0, i; uint64_t a;
    if (!base || (!want && !hud_held)) return 0;
    __try {
        a = *(uint64_t *)((BYTE *)base + MGS4VR_HUD_LIST_RVA);
        while (ptr_ok(a) && seen < HUD_MAX_LAYOUTS && *(unsigned *)a == MGS4VR_HUD_MAGIC) {
            unsigned id = *(unsigned *)(a + 0x1c), n = *(unsigned *)(a + 0x170); uint64_t arr = *(uint64_t *)(a + 0x180);
            ++seen;
            if (mgs4vr_hud_hidden_id(id) && n && n <= 5000 && ptr_ok(arr)) {
                HUD_SLOT *s = slot_for(a, id, want);
                unsigned lim = n < HUD_MAX_NODES ? n : HUD_MAX_NODES;
                for (i = 1; s && i <= (int)lim; ++i) {
                    uint64_t node = *(uint64_t *)(arr + 8 * (uint64_t)i); unsigned f;
                    if (!ptr_ok(node)) continue;
                    f = *(unsigned *)node;
                    if (want && (f & 0x400000u)) { *(unsigned *)node = (f & ~0x400000u) | 0x40000000u; s->bits[i >> 3] |= (unsigned char)(1u << (i & 7)); ++changed; }
                    else if (!want && (s->bits[i >> 3] & (1u << (i & 7)))) { *(unsigned *)node = f | 0x40400000u; ++changed; }
                }
                if (s && !want) memset(s, 0, sizeof(*s));                     /* shown again: forget */
            }
            a = *(uint64_t *)(a + 8);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&hud_faults); memset(hud_slot, 0, sizeof(hud_slot)); hud_held = 0; return changed; }
    if (!want) memset(hud_slot, 0, sizeof(hud_slot));                         /* layouts that died keep nothing to restore */
    hud_held = 0; for (i = 0; i < HUD_SLOTS; ++i) if (hud_slot[i].layout) hud_held = 1;
    InterlockedExchange(&hud_layouts, seen); InterlockedIncrement(&hud_frames);
    if (want) InterlockedAdd(&hud_hidden, changed); else InterlockedAdd(&hud_shown, changed);
    return changed;
}
int mgs4vr_hud_wanted(void) { return hud_want != 0; }
int mgs4vr_hud_want_now(int first_person) { return hud_want && (hud_all || first_person); }
void mgs4vr_hud_poll(const char *exe) {
    static char mark[MAX_PATH], status[MAX_PATH]; static DWORD tick; DWORD now = GetTickCount(); FILE *f; char b[32] = { 0 }; int on = 0; const char *p = b;
    if (!mark[0]) { const char *leaf = strrchr(exe, '\\'); if (!leaf) return; sprintf_s(mark, sizeof(mark), "%.*smgs4vr_hudhide.on", (int)(leaf + 1 - exe), exe);
        sprintf_s(status, sizeof(status), "%.*smgs4vr_hud_%lu.txt", (int)(leaf + 1 - exe), exe, GetCurrentProcessId()); }
    if (now - tick < 250) return; tick = now;
    if (!fopen_s(&f, mark, "rb")) { size_t n = fread(b, 1, sizeof(b) - 1, f); fclose(f); b[n] = 0;
        if ((unsigned char)p[0] == 0xef) p += 3; while (*p == ' ') ++p; on = !strncmp(p, "on", 2) && (p[2] == 0 || p[2] == ' ' || p[2] == '\r' || p[2] == '\n'); }
    InterlockedExchange(&hud_want, on); InterlockedExchange(&hud_all, on && strstr(p, " all") != NULL);
    { static DWORD rep; if (now - rep >= 1000 && (on || hud_frames)) { rep = now;
        if (!fopen_s(&f, status, "w")) { fprintf(f, "hud_version=2 hide=%d all=%ld frames=%ld layouts=%ld hidden=%ld shown=%ld held=%d full=%ld faults=%ld\n", on, hud_all, hud_frames, hud_layouts, hud_hidden, hud_shown, hud_held, hud_full, hud_faults); fclose(f); } } }
}
