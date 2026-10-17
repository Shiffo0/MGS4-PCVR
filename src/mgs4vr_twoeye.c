/* mgs4vr_twoeye.c - see mgs4vr_twoeye.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mgs4vr_twoeye.h"
#include "mgs4vr_lens.h"
#include "mgs4vr_weapon_identity.h"
#include "mgs4vr_muzzle_contract.h"
/* Game-thread reader for the identity/shot-anchor chains: SEH only (no VirtualQuery per read). */
static int te_rd(void *u, uint64_t a, void *o, unsigned n) { (void)u; __try { memcpy(o, (const void *)a, n); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static volatile LONG te_shot_ok, te_shot_fail, te_sight_ok, te_sight_fallback;
/* The weapon model's sight anchors (+0x4E0 objective, +0x560 ocular; see mgs4vr_lens.h) in world space. */
static int te_lens_sight(uint64_t model_actor, float ocular[3], float objective[3]) {
    uint64_t model, mat; float base[16], l[16], w[16];
    if (!mc_q(te_rd, 0, model_actor + 0x3b8, &model) || !mc_q(te_rd, 0, model + 0x100, &mat) || !mc_matrix(te_rd, 0, mat, base)) return 0;
    if (!mc_matrix(te_rd, 0, model_actor + 0x560, l)) return 0; mc_mul(w, l, base); memcpy(ocular, w + 12, 12);
    if (!mc_matrix(te_rd, 0, model_actor + 0x4e0, l)) return 0; mc_mul(w, l, base); memcpy(objective, w + 12, 12);
    return 1;
}
/* The held weapon's shot anchor (world, game units): row 2 = barrel direction, row 3 = muzzle. */
static uint64_t te_last_model;
static int te_lens_shot(void *image, float shot[16]) {
    WI_IDENTITY id; MC_RESULT r;
    if (!wi_resolve(te_rd, 0, (uint64_t)image, &id) || !mc_resolve(te_rd, 0, (uint64_t)image, id.owner, id.weapon, &r)) { InterlockedIncrement(&te_shot_fail); return 0; }
    memcpy(shot, r.native_shot, 64); te_last_model = r.model_actor; InterlockedIncrement(&te_shot_ok); return 1;
}
#define TE_MAIN_RVA 0x1DDDC70u
#define TE_VIEW_STRIDE 0x1590u
#define TE_VP 0x350        /* x0, y0, x1, y1, w, h (int32) */
#define TE_FLAGS 0x394
static volatile LONG te_wanted, te_frames, te_restores, te_rejects, te_faults, te_lens_frames;
static volatile LONG te_orflags;                  /* marker token orflags=<hex>: extra view-1 flag bits (colour investigation) */
/* Pass bit 0x80000 (colour/NV pass) of view 1 samples the MAIN view's picture (sim 14 Oct: right half = the left camera's picture,
   shift 0 px instead of ~470 px, i.e. the double vision of 13 Oct; with the bit cleared the right eye shows its own camera, shift
   474-498 px, vertical 0). Cleared on both views by default so both eyes look alike (overlap brightness right/left 1.14 vs 1.65
   with only view 1 cleared); the cost: no colour grading / NV filter while two-eye is on. Tokens passclear=/mainclear=<hex>. */
#define TE_COLOUR_PASS 0x80000
/* 15 Oct 2026 (sim, D3D probe 5496/24796): WHY the colour pass copies the left eye. Both views execute the same 2D command list
   (render context +0x490, list 3 = pass bit 0x80000, run by 0xBE070). Its full-screen copy is opcode 05 (handler 0xBC600:
   +8 colour, +0xC/+0xE u0/v0, +0x10/+0x12 x/y, +0x14/+0x16 u1/v1, +0x18/+0x1A w/h, all s16) with source rect 0,0-1920,2160 in
   texture pixels; 0xBE070 only moves the VIEWPORT to the view (globals 0x1DE49A0.. = view x/y/w/h, opcode 0A adds x), so view 1
   draws into the right half but samples the left half. Fix: dispatch-table entry 5 (0x1B06400) -> te_op05, which for the right
   view (x > 0) in list 3 runs the original handler on a copy with u0/u1 moved by the view x when the source rect is exactly the
   view size. Then both eyes keep the colour pass (grading + NV filter); passclear/mainclear default to 0 while the hook is in. */
#define TE_OP_TABLE_RVA 0x1B06400u
#define TE_OP05_RVA 0xBC600u
#define TE_LIST_RVA 0x1DE49E8u     /* int: list index being executed (0xBE070 param 3) */
#define TE_RECT_RVA 0x1DE49A0u     /* int x, y, w, h of the view (0xBE070) */
#define TE_AUTO (-1)
/* Lists 2 (pass bit 0x40000) AND 3 (0x80000). Headset 15 Oct 22:20 (8DBF4FD7, list 3 only): near objects doubled in the right
   eye - list 2 also blends a view-size copy of the LEFT half in (sim: translucent left-eye hand in the right eye, ~2 opcode-05
   copies per frame in list 2); with 0xC the ghost is gone, normal and NV. */
#define TE_UV_DEFAULT 0xC
static volatile LONG te_passclear = TE_AUTO;      /* view-1 pass bits cleared during two-eye (TE_AUTO: 0 with the uv hook, else 0x80000) */
static long long te_two_pass_saved = -1;          /* game thread: view 1 passes before two-eye cleared any */
static volatile LONG te_mainclear = TE_AUTO;      /* main-view pass bits cleared during two-eye */
static volatile LONG te_uvmask = TE_UV_DEFAULT;   /* marker token uvfix=<hex list mask> (0 = off) */
/* 16 Oct 2026 experiment token skip05=<hex>: skip list-2 copies of the right view by class (bit 0 full-target source, 1 scene->scene,
   2 scene->half, 3 half->half, 4 half->scene, 5 256-noise source, 6 anything else in list 2); bit 8 = in the left view too. Measurement only. */
static volatile LONG te_skip05, te_skipped05;
/* 16 Oct 2026 18:55 (user at the ghost spot, live bisect): the ghost is in class 6, but class 6 also holds the HUD/2D sprites.
   Tokens skip6min=/skip6max=<dest width px> limit the class-6 skip to a width band; a class-6 census by size shows what is there. */
static volatile LONG te_skip6min, te_skip6max = 0x7FFF;
int mgs4vr_twoeye_skip6_band(int w, int lo, int hi) { return w >= lo && w <= hi; }
/* 16 Oct 2026 19:15 class-6 census (pid 32848): the ghost = 64x64-source screen sprites (light shafts/dust), hundreds per frame,
   at the SAME screen position in both views (dst -12,261 in view x 0 and 1920) = no parallax. Token spr64=<n>: 1 skip them in
   the right view, 2 skip in both views, 3 shift them in the right view by dst x += spr64k/100 * width (sprite size ~ 1/depth,
   so disparity ~ size; sign/scale found live). */
static volatile LONG te_spr64 = 2, te_spr64k, te_spr64_seen;   /* 0 = both eyes; default 2 = off (user choice 16 Oct 19:30) */
int mgs4vr_twoeye_spr64(const unsigned char *cmd) { short u0, v0, u1, v1; memcpy(&u0, cmd + 0xC, 2); memcpy(&v0, cmd + 0xE, 2); memcpy(&u1, cmd + 0x14, 2); memcpy(&v1, cmd + 0x16, 2); return u1 - u0 == 64 && v1 - v0 == 64; }
int mgs4vr_twoeye_spr64_shift(int x, int w, int k100) { return x + (int)((long long)w * k100 / 100); }
#define TE_C6 96
typedef struct { int vx, du, dv, w, h, u0, v0, x, y; volatile LONG n; } TE_C6_ROW;
static TE_C6_ROW te_c6[TE_C6]; static volatile LONG te_c6_n, te_c6_full;
static int te_op05_class(const unsigned char *cmd, int sw, int sh) {
    short u1, v1, w, h; memcpy(&u1, cmd + 0x14, 2); memcpy(&v1, cmd + 0x16, 2); memcpy(&w, cmd + 0x18, 2); memcpy(&h, cmd + 0x1A, 2);
    if (u1 == 256 && v1 == 256) return 5;
    if (u1 == sw * 2 && v1 == sh) return 0;
    if (u1 == sw && v1 == sh) return w == sw ? 1 : 2;
    if (u1 == sw / 2 && v1 == sh / 2) return w == sw / 2 ? 3 : 4;
    return 6;
}
static volatile LONG te_uv_on, te_uv_shifted, te_uv_installed, te_uv_failed;
static BYTE *te_uv_image;
static unsigned long long (*te_op05_orig)(unsigned long long);
int mgs4vr_twoeye_uv_shift(const unsigned char *cmd, int list, const int rect[4], unsigned mask, unsigned char out[0x20]) {
    short u0, v0, u1, v1, w, h, nu0, nu1;
    if (!cmd || !rect || list < 0 || list > 31 || !(mask & (1u << list)) || rect[0] <= 0 || rect[2] <= 0 || rect[3] <= 0) return 0;
    memcpy(&u0, cmd + 0xC, 2); memcpy(&v0, cmd + 0xE, 2); memcpy(&u1, cmd + 0x14, 2); memcpy(&v1, cmd + 0x16, 2); memcpy(&w, cmd + 0x18, 2); memcpy(&h, cmd + 0x1A, 2);
    if (u0 != 0 || v0 != 0 || u1 != rect[2] || v1 != rect[3] || rect[0] + rect[2] > 0x7FFF) return 0;
    if (list == 2 && (w < u1 || h < v1)) return 0;                                                      /* reducing copy in list 2 only (see uv_remap) */
    memcpy(out, cmd, 0x20); nu0 = (short)(u0 + rect[0]); nu1 = (short)(u1 + rect[0]);
    memcpy(out + 0xC, &nu0, 2); memcpy(out + 0x14, &nu1, 2);
    return 1;
}
/* 16 Oct 2026: general form - a copy whose source is exactly the main view's scene (0,0,scene w,scene h) reads the
   executing view's own rect instead, for every view but the main view itself: the right eye (x shift, as above) and the
   lens square (0,0,224,224: until now its colour pass had to be cleared, the square showed the whole frame squashed). */
/* 16 Oct 2026 (sim at the user's ghost spot, skip series q_*.png): a REDUCING copy (destination smaller than the scene-size
   source, e.g. 1920x2160 -> 960x1080 in list 2) reads a scratch target the view itself just wrote at x = 0 (D3D probe: those
   steps keep viewport x 0 in the right view), not the side-by-side scene. Shifting it read a stretched/stale area = the
   translucent shifted copy in the right eye. Such copies are left as they are. */
int mgs4vr_twoeye_uv_remap(const unsigned char *cmd, int list, const int rect[4], int scene_w, int scene_h, unsigned mask, unsigned char out[0x20]) {
    short u0, v0, u1, v1, w, h, n[4];
    if (!cmd || !rect || list < 0 || list > 31 || !(mask & (1u << list)) || rect[2] <= 0 || rect[3] <= 0 || scene_w <= 0 || scene_h <= 0) return 0;
    memcpy(&u0, cmd + 0xC, 2); memcpy(&v0, cmd + 0xE, 2); memcpy(&u1, cmd + 0x14, 2); memcpy(&v1, cmd + 0x16, 2); memcpy(&w, cmd + 0x18, 2); memcpy(&h, cmd + 0x1A, 2);
    if (u0 != 0 || v0 != 0 || u1 != scene_w || v1 != scene_h) return 0;
    if (list == 2 && (w < u1 || h < v1)) return 0;                                                      /* reducing copy in list 2: own scratch; list 3 NV reads the scene (16 Oct 21:05) */
    if (rect[0] == 0 && rect[1] == 0 && rect[2] == scene_w && rect[3] == scene_h) return 0;          /* the main view itself */
    if (rect[0] < 0 || rect[1] < 0 || rect[0] + rect[2] > 0x7FFF || rect[1] + rect[3] > 0x7FFF) return 0;
    n[0] = (short)rect[0]; n[1] = (short)rect[1]; n[2] = (short)(rect[0] + rect[2]); n[3] = (short)(rect[1] + rect[3]);
    memcpy(out, cmd, 0x20); memcpy(out + 0xC, &n[0], 2); memcpy(out + 0xE, &n[1], 2); memcpy(out + 0x14, &n[2], 2); memcpy(out + 0x16, &n[3], 2);
    return 1;
}
static int te_active, te_full_w, te_full_h;   /* tentative (defined below); read by the render-thread copy hook */
/* 16 Oct 2026: NOT called any more. Assumed the half-size glow area holds both eyes side by side (right 960..1920); the
   D3D probe shows both views write it at x 0..960, so the right eye read an area nobody writes (ghost stayed, video 18.15).
   Kept for the record and its desk test. */
int mgs4vr_twoeye_uv_scaled(const unsigned char *cmd, int list, const int rect[4], int scene_w, int scene_h, unsigned mask, unsigned char out[0x20]) {
    short u0, v0, u1, v1, n[4]; int d;
    if (!cmd || !rect || list < 0 || list > 31 || !(mask & (1u << list)) || rect[2] <= 0 || rect[3] <= 0 || scene_w <= 0 || scene_h <= 0) return 0;
    if (rect[0] == 0 && rect[1] == 0 && rect[2] == scene_w && rect[3] == scene_h) return 0;          /* the main view itself */
    memcpy(&u0, cmd + 0xC, 2); memcpy(&v0, cmd + 0xE, 2); memcpy(&u1, cmd + 0x14, 2); memcpy(&v1, cmd + 0x16, 2);
    if (u0 != 0 || v0 != 0) return 0;
    for (d = 2; d <= 4; d *= 2) {
        if (scene_w % d || scene_h % d || rect[0] % d || rect[1] % d || rect[2] % d || rect[3] % d) continue;
        if (u1 != scene_w / d || v1 != scene_h / d) continue;
        if (rect[0] < 0 || rect[1] < 0 || (rect[0] + rect[2]) / d > 0x7FFF || (rect[1] + rect[3]) / d > 0x7FFF) return 0;
        n[0] = (short)(rect[0] / d); n[1] = (short)(rect[1] / d); n[2] = (short)((rect[0] + rect[2]) / d); n[3] = (short)((rect[1] + rect[3]) / d);
        memcpy(out, cmd, 0x20); memcpy(out + 0xC, &n[0], 2); memcpy(out + 0xE, &n[1], 2); memcpy(out + 0x14, &n[2], 2); memcpy(out + 0x16, &n[3], 2);
        return 1;
    }
    return 0;
}
/* 16 Oct 2026 (headset video 17.32, sim census): list 2 runs a copy whose source is the WHOLE render target, 0,0-3840,2160 =
   both eyes side by side, in BOTH views every frame (an after-image/blur feedback that shows when the camera turns fast): the
   left eye blended a squeezed both-eyes image in, translucent. Such a copy now reads only the executing view's own rect. */
int mgs4vr_twoeye_uv_fullframe(const unsigned char *cmd, int list, const int rect[4], int full_w, int full_h, unsigned mask, unsigned char out[0x20]) {
    short u0, v0, u1, v1, n[4];
    if (!cmd || !rect || list < 0 || list > 31 || !(mask & (1u << list)) || full_w <= 0 || full_h <= 0 || rect[2] <= 0 || rect[3] <= 0) return 0;
    memcpy(&u0, cmd + 0xC, 2); memcpy(&v0, cmd + 0xE, 2); memcpy(&u1, cmd + 0x14, 2); memcpy(&v1, cmd + 0x16, 2);
    if (u0 != 0 || v0 != 0 || u1 != full_w || v1 != full_h) return 0;
    if (rect[0] == 0 && rect[1] == 0 && rect[2] == full_w && rect[3] == full_h) return 0;              /* a view over the whole target */
    if (rect[0] < 0 || rect[1] < 0 || rect[0] + rect[2] > 0x7FFF || rect[1] + rect[3] > 0x7FFF) return 0;
    n[0] = (short)rect[0]; n[1] = (short)rect[1]; n[2] = (short)(rect[0] + rect[2]); n[3] = (short)(rect[1] + rect[3]);
    memcpy(out, cmd, 0x20); memcpy(out + 0xC, &n[0], 2); memcpy(out + 0xE, &n[1], 2); memcpy(out + 0x14, &n[2], 2); memcpy(out + 0x16, &n[3], 2);
    return 1;
}
/* 16 Oct 2026 census (headset video 17.32: translucent other-eye content while turning fast): every opcode-05 copy while the
   hook is on, by (list, view x, source rect, shifted?) with counts; written to mgs4vr_twoeye_op05_<pid>.txt. */
#define TE_CENSUS 160
typedef struct { int list, vx, vw, u0, v0, u1, v1, w, h, x, y, shifted; volatile LONG n; } TE_OP05_ROW;
static TE_OP05_ROW te_census[TE_CENSUS]; static volatile LONG te_census_n, te_census_full;
static SRWLOCK te_census_lock = SRWLOCK_INIT;
static void te_census_add(const unsigned char *cmd, int list, const int rect[4], int shifted) {
    short u0, v0, u1, v1, w, h, x, y; int i;
    memcpy(&x, cmd + 0x10, 2); memcpy(&y, cmd + 0x12, 2); memcpy(&u0, cmd + 0xC, 2); memcpy(&v0, cmd + 0xE, 2); memcpy(&u1, cmd + 0x14, 2); memcpy(&v1, cmd + 0x16, 2); memcpy(&w, cmd + 0x18, 2); memcpy(&h, cmd + 0x1A, 2);
    AcquireSRWLockExclusive(&te_census_lock);
    for (i = 0; i < te_census_n; ++i) { TE_OP05_ROW *r = &te_census[i];
        if (r->list == list && r->vx == rect[0] && r->vw == rect[2] && r->u0 == u0 && r->v0 == v0 && r->u1 == u1 && r->v1 == v1 && r->w == w && r->h == h && r->x == x && r->y == y && r->shifted == shifted) { ++r->n; ReleaseSRWLockExclusive(&te_census_lock); return; } }
    if (te_census_n < TE_CENSUS) { TE_OP05_ROW *r = &te_census[te_census_n++]; r->list = list; r->vx = rect[0]; r->vw = rect[2]; r->u0 = u0; r->v0 = v0; r->u1 = u1; r->v1 = v1; r->w = w; r->h = h; r->x = x; r->y = y; r->shifted = shifted; r->n = 1; }
    else ++te_census_full;
    ReleaseSRWLockExclusive(&te_census_lock);
}
static void te_c6_add(const unsigned char *cmd, int vx) {
    short u0, v0, u1, v1, w, h, x, y; int i;
    memcpy(&x, cmd + 0x10, 2); memcpy(&y, cmd + 0x12, 2); memcpy(&u0, cmd + 0xC, 2); memcpy(&v0, cmd + 0xE, 2); memcpy(&u1, cmd + 0x14, 2); memcpy(&v1, cmd + 0x16, 2); memcpy(&w, cmd + 0x18, 2); memcpy(&h, cmd + 0x1A, 2);
    AcquireSRWLockExclusive(&te_census_lock);
    for (i = 0; i < te_c6_n; ++i) { TE_C6_ROW *r = &te_c6[i];
        if (r->vx == vx && r->du == u1 - u0 && r->dv == v1 - v0 && r->w == w && r->h == h) { ++r->n; ReleaseSRWLockExclusive(&te_census_lock); return; } }
    if (te_c6_n < TE_C6) { TE_C6_ROW *r = &te_c6[te_c6_n++]; r->vx = vx; r->du = u1 - u0; r->dv = v1 - v0; r->w = w; r->h = h; r->u0 = u0; r->v0 = v0; r->x = x; r->y = y; r->n = 1; }
    else ++te_c6_full;
    ReleaseSRWLockExclusive(&te_census_lock);
}
static void te_census_write(const char *path) {
    FILE *f; int i; if (fopen_s(&f, path, "w")) return;
    AcquireSRWLockShared(&te_census_lock);
    fprintf(f, "op05_census rows=%ld full=%ld\n", te_census_n, te_census_full);
    for (i = 0; i < te_census_n; ++i) { const TE_OP05_ROW *r = &te_census[i]; fprintf(f, "list=%d view_x=%d view_w=%d src=%d,%d-%d,%d dst=%d,%d size=%dx%d shifted=%d n=%ld\n", r->list, r->vx, r->vw, r->u0, r->v0, r->u1, r->v1, r->x, r->y, r->w, r->h, r->shifted, r->n); }
    fprintf(f, "class6 rows=%ld full=%ld\n", te_c6_n, te_c6_full);
    for (i = 0; i < te_c6_n; ++i) { const TE_C6_ROW *r = &te_c6[i]; fprintf(f, "c6 view_x=%d src_size=%dx%d size=%dx%d first_src=%d,%d first_dst=%d,%d n=%ld\n", r->vx, r->du, r->dv, r->w, r->h, r->u0, r->v0, r->x, r->y, r->n); }
    ReleaseSRWLockShared(&te_census_lock); fclose(f);
}
static unsigned long long te_op05(unsigned long long cmd) {
    unsigned char copy[0x20]; int list, rect[4], sw, sh;
    if (te_uv_on && te_uv_image) {
        __try { list = *(int *)(te_uv_image + TE_LIST_RVA); memcpy(rect, te_uv_image + TE_RECT_RVA, sizeof(rect)); } __except (EXCEPTION_EXECUTE_HANDLER) { list = -1; }
        __try { sw = *(int *)(te_uv_image + TE_MAIN_RVA + TE_VP + 0x10); sh = *(int *)(te_uv_image + TE_MAIN_RVA + TE_VP + 0x14); } __except (EXCEPTION_EXECUTE_HANDLER) { sw = sh = 0; }
        if (list == 2 && sw > 0) {
            int cls = te_op05_class((const unsigned char *)cmd, sw, sh);
            if (cls == 6) __try { te_c6_add((const unsigned char *)cmd, rect[0]); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (cls == 6 && te_spr64 && te_active && mgs4vr_twoeye_spr64((const unsigned char *)cmd)) {
                InterlockedIncrement(&te_spr64_seen);
                if ((te_spr64 == 1 && rect[0] > 0) || te_spr64 == 2) { InterlockedIncrement(&te_skipped05); return cmd + 0x20; }
                if (te_spr64 == 3 && rect[0] > 0) { short x, w; memcpy(&x, cmd + 0x10, 2); memcpy(&w, cmd + 0x18, 2); memcpy(copy, (const void *)cmd, 0x20);
                    x = (short)mgs4vr_twoeye_spr64_shift(x, w, te_spr64k); memcpy(copy + 0x10, &x, 2); te_op05_orig((unsigned long long)copy); return cmd + 0x20; }
            }
            if (te_skip05 && (rect[0] > 0 || (te_skip05 & 0x100)) && ((te_skip05 >> cls) & 1) &&
                (cls != 6 || mgs4vr_twoeye_skip6_band(*(short *)(cmd + 0x18), te_skip6min, te_skip6max))) { InterlockedIncrement(&te_skipped05); return cmd + 0x20; }
        }
        if (list >= 0) {
            int moved = (te_active && mgs4vr_twoeye_uv_fullframe((const unsigned char *)cmd, list, rect, te_full_w, te_full_h, (unsigned)te_uvmask, copy)) ||
                        (sw > 0 && mgs4vr_twoeye_uv_remap((const unsigned char *)cmd, list, rect, sw, sh, (unsigned)te_uvmask, copy)) ||
                        mgs4vr_twoeye_uv_shift((const unsigned char *)cmd, list, rect, (unsigned)te_uvmask, copy);
            if (list != 1 && !(*(short *)(cmd + 0x14) == 64 && *(short *)(cmd + 0x16) == 64) && !(list == 2 && sw > 0 && te_op05_class((const unsigned char *)cmd, sw, sh) == 6)) {   /* class 6 has its own table (16 Oct: it filled the census) */ __try { te_census_add((const unsigned char *)cmd, list, rect, moved); } __except (EXCEPTION_EXECUTE_HANDLER) {} }   /* list 1 = UI atlas copies of view 4 */
            if (moved) { InterlockedIncrement(&te_uv_shifted); te_op05_orig((unsigned long long)copy); return cmd + 0x20; }
        }
    }
    return te_op05_orig(cmd);
}
/* Game thread, once: point table entry 5 at te_op05 when it still holds the original handler. */
static void te_uv_install(BYTE *image) {
    void **slot = (void **)(image + TE_OP_TABLE_RVA + 5 * 8); DWORD old, tmp;
    if (te_uv_installed || te_uv_failed) return;
    __try {
        if (*slot != (void *)(image + TE_OP05_RVA) || !VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { te_uv_failed = 1; return; }
        te_uv_image = image; te_op05_orig = (unsigned long long (*)(unsigned long long))(image + TE_OP05_RVA);
        InterlockedExchangePointer(slot, (void *)te_op05); VirtualProtect(slot, 8, old, &tmp); te_uv_installed = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { te_uv_failed = 1; }
}
static unsigned te_clear_bits(LONG token) { return token == TE_AUTO ? (te_uv_installed && te_uvmask ? 0u : (unsigned)TE_COLOUR_PASS) : (unsigned)token; }
static long long te_main_pass_saved = -1;
static int te_active, te_full_w, te_full_h, te_kind_saved = -1, te_miss;      /* game thread only */
#define TE_HOLD 30
#define TE_KIND 0x398   /* 13 Oct (sim, D3D probe + eye dumps): kind 0 = the main view's post chain incl. the colour/NV pass 0x80000; view 1's own kind 1 skips it (right eye 1.5x too bright, no NV filter, no rifle) */
static char te_mark[MAX_PATH], te_status[MAX_PATH];
static DWORD te_tick;

/* Same as the camera hook's head_matrix (mgs4vr_cam.c), without the debug sweep. Row vectors. */
static void te_head(const MGS4VR_CAM_ADJUST *a, float *h) {
    const float d = 0.017453292519943295f;
    float cy = cosf(a->yaw * d), sy = sinf(a->yaw * d), cp = cosf(a->pitch * d), sp = sinf(a->pitch * d);
    float cr = cosf(a->roll * d), sr = sinf(a->roll * d);
    float ry[9] = { cy, 0, -sy, 0, 1, 0, sy, 0, cy }, rx[9] = { 1, 0, 0, 0, cp, sp, 0, -sp, cp }, rz[9] = { cr, sr, 0, -sr, cr, 0, 0, 0, 1 };
    float t1[9], t2[9]; int r, c, k;
    for (r = 0; r < 3; ++r) for (c = 0; c < 3; ++c) { t1[r * 3 + c] = 0; for (k = 0; k < 3; ++k) t1[r * 3 + c] += rz[r * 3 + k] * rx[k * 3 + c]; }
    for (r = 0; r < 3; ++r) for (c = 0; c < 3; ++c) { t2[r * 3 + c] = 0; for (k = 0; k < 3; ++k) t2[r * 3 + c] += t1[r * 3 + k] * ry[k * 3 + c]; }
    memset(h, 0, 16 * sizeof(float));
    for (r = 0; r < 3; ++r) for (c = 0; c < 3; ++c) h[r * 4 + c] = t2[r * 3 + c];
    h[12] = a->x; h[13] = -a->y; h[14] = a->z; h[15] = 1.0f;
}
static void te_mul(const float *a, const float *b, float *o) {      /* o = a * b, row-major 4x4 */
    float t[16]; int r, c, k;
    for (r = 0; r < 4; ++r) for (c = 0; c < 4; ++c) { double s = 0; for (k = 0; k < 4; ++k) s += (double)a[r * 4 + k] * b[k * 4 + c]; t[r * 4 + c] = (float)s; }
    memcpy(o, t, sizeof(t));
}
/* Inverse of an affine row-vector matrix (rows 0..2 linear part, row 3 translation). 0 = singular. */
static int te_affine_inverse(const float *m, float *o) {
    double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10], det, inv[9]; int r, k;
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!isfinite(det) || fabs(det) < 1e-12) return 0;
    inv[0] = (e * i - f * h) / det; inv[1] = (c * h - b * i) / det; inv[2] = (b * f - c * e) / det;
    inv[3] = (f * g - d * i) / det; inv[4] = (a * i - c * g) / det; inv[5] = (c * d - a * f) / det;
    inv[6] = (d * h - e * g) / det; inv[7] = (b * g - a * h) / det; inv[8] = (a * e - b * d) / det;
    for (r = 0; r < 3; ++r) { for (k = 0; k < 3; ++k) o[r * 4 + k] = (float)inv[r * 3 + k]; o[r * 4 + 3] = 0; }
    for (k = 0; k < 3; ++k) o[12 + k] = (float)-(m[12] * inv[k] + m[13] * inv[3 + k] + m[14] * inv[6 + k]);
    o[15] = 1;
    return 1;
}
int mgs4vr_twoeye_right_block(const unsigned char *left, const MGS4VR_CAM_ADJUST *al, const MGS4VR_CAM_ADJUST *ar,
                              int mirror, unsigned char *out) {
    float c2w[16], hl[16], hr[16], hli[16], m[16], w2c[16], p[16], comb[16], pl[24]; int i, j;
    static const int off_p[3] = { 0xC0, 0x140, 0x1C0 };
    if (!left || !al || !ar || !out) return 0;
    memcpy(out, left, MGS4VR_TWOEYE_BLOCK);
    memcpy(c2w, left, 64);
    for (i = 0; i < 16; ++i) if (!isfinite(c2w[i])) return 0;
    te_head(al, hl); te_head(ar, hr);
    if (!te_affine_inverse(hl, hli)) return 0;
    te_mul(hli, c2w, m); te_mul(hr, m, c2w);                      /* undo the left eye, apply the right one */
    c2w[3] = c2w[7] = c2w[11] = 0; c2w[15] = 1;
    if (!te_affine_inverse(c2w, w2c)) return 0;
    memcpy(out, c2w, 64); memcpy(out + 0x40, w2c, 64);
    for (j = 0; j < 3; ++j) {
        memcpy(p, left + off_p[j], 64);
        if (mirror) p[8] = -p[8];                                   /* [2][0]: horizontal skew */
        memcpy(out + off_p[j], p, 64);
        if (j < 2) { te_mul(w2c, p, comb); memcpy(out + 0x100 + j * 0x80, comb, 64); }
    }
    memcpy(comb, out + 0x100, 64);
    for (i = 0; i < 6; ++i) {                                      /* plane = column 3 +/- column k, xyz-normalised */
        int k = i < 2 ? 0 : i < 4 ? 1 : 2; float s = (i == 0 || i == 3 || i == 4) ? -1.0f : 1.0f, v[4], n;
        for (j = 0; j < 4; ++j) v[j] = comb[j * 4 + 3] + s * comb[j * 4 + k];
        n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!(n > 1e-12f)) return 0;
        for (j = 0; j < 4; ++j) pl[i * 4 + j] = v[j] / n;
    }
    memcpy(out + 0x2C0, pl, sizeof(pl));
    for (i = 0; i < MGS4VR_TWOEYE_BLOCK / 4; ++i) { float f; memcpy(&f, out + i * 4, 4); if (!isfinite(f) && i * 4 < 0x330) return 0; }
    return 1;
}

/* The scene setup rebuilds every view's projection from its builder inputs +0x380 (P00, aspect factor, x skew, y skew)
   when it renders (0xEAF00 -> 0xB9B70), not from the P we write. Copying the main view's inputs gave the right eye the
   LEFT eye's skew: sim 14 Oct, both eye pictures framed identically (shift 0 px instead of ~466 px) = double vision in
   the headset (13 Oct). The right eye takes the left inputs with the x skew of its own (mirrored) block. */
void mgs4vr_twoeye_right_builder(const unsigned char *left_builder, const unsigned char *right_block, unsigned char *out) {
    float b[4], p[16];
    memcpy(b, left_builder, sizeof(b)); memcpy(p, right_block + 0xC0, sizeof(p));
    b[2] = p[8];
    memcpy(out, b, sizeof(b));
}

/* Lens scope (mgs4vr_lens.h): the main camera unchanged, every projection replaced by a symmetric square frustum with
   scale k = 1/tan(fov/2) (sign of the y scale kept), then P*V and the planes recomputed like above. */
int mgs4vr_twoeye_lens_block(const unsigned char *left, float k, const float *rel, unsigned char *out) {
    float w2c[16], c2w[16], p[16], comb[16], pl[24]; int i, j;
    static const int off_p[3] = { 0xC0, 0x140, 0x1C0 };
    if (!left || !out || !(k > 0.01f) || !isfinite(k)) return 0;
    memcpy(out, left, MGS4VR_TWOEYE_BLOCK); memcpy(w2c, left + 0x40, 64);
    for (i = 0; i < 16; ++i) if (!isfinite(w2c[i])) return 0;
    if (rel) {                                                      /* barrel camera: c2w = rel * head c2w, like the eye adjust */
        for (i = 0; i < 16; ++i) if (!isfinite(rel[i])) return 0;
        memcpy(c2w, left, 64); te_mul(rel, c2w, c2w);
        c2w[3] = c2w[7] = c2w[11] = 0; c2w[15] = 1;
        if (!te_affine_inverse(c2w, w2c)) return 0;
        memcpy(out, c2w, 64); memcpy(out + 0x40, w2c, 64);
    }
    for (j = 0; j < 3; ++j) {
        memcpy(p, left + off_p[j], 64);
        p[0] = k; p[5] = p[5] < 0 ? -k : k; p[8] = p[9] = 0;
        memcpy(out + off_p[j], p, 64);
        if (j < 2) { te_mul(w2c, p, comb); memcpy(out + 0x100 + j * 0x80, comb, 64); }
    }
    memcpy(comb, out + 0x100, 64);
    for (i = 0; i < 6; ++i) {
        int kk = i < 2 ? 0 : i < 4 ? 1 : 2; float s = (i == 0 || i == 3 || i == 4) ? -1.0f : 1.0f, v[4], n;
        for (j = 0; j < 4; ++j) v[j] = comb[j * 4 + 3] + s * comb[j * 4 + kk];
        n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!(n > 1e-12f)) return 0;
        for (j = 0; j < 4; ++j) pl[i * 4 + j] = v[j] / n;
    }
    memcpy(out + 0x2C0, pl, sizeof(pl));
    return 1;
}
static float te_oc_shot[16]; static MGS4VR_LENS_CFG te_oc_cfg; static int te_oc_valid;   /* game thread */
static volatile LONG te_oc_right;
static int te_lens_on, te_lens_kind_saved = -1; static long long te_pass_saved = -1;   /* game thread */
static BYTE *te_lens_v;                                         /* the view the lens runs on (view 1, or 2/3 with lens view=) */
static void te_lens_off(BYTE *v1) {
    __try {
        *(unsigned *)(v1 + TE_FLAGS) = 0; if (te_lens_kind_saved >= 0) { *(int *)(v1 + TE_KIND) = te_lens_kind_saved; te_lens_kind_saved = -1; }
        if (te_pass_saved >= 0) { *(unsigned *)(v1 + 0x3B0) = (unsigned)te_pass_saved; te_pass_saved = -1; }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&te_faults); }
    if (!te_active) InterlockedExchange(&te_uv_on, 0);
    te_lens_on = 0; te_lens_v = NULL; te_oc_valid = 0; mgs4vr_lens_set_rect(0, 0, 0); mgs4vr_lens_set_dot(0);
}
/* Ocular disk of the lens in one eye's picture (Titanfall 2 VR style): the model's own ocular, else the configured offset.
   te_oc_* = this frame's shot anchor and config (te_lens_frame), so two-eye can add the right eye with its own camera. */
static int te_dot_on; static volatile LONG te_dot_frames;   /* game thread: this frame's disk is a red dot sight */
static void te_lens_ocular(const float c2w[16], const float vp[16], unsigned serial, int eye) {
    float clip[16], oc[3], ob[3]; int ok;
    if (!te_oc_valid) return;
    if (!te_oc_cfg.geo_set && te_lens_sight(te_last_model, oc, ob) && mgs4vr_lens_sight_ok(te_oc_shot, oc, ob)) {
        ok = mgs4vr_lens_sight_clip(oc, ob, c2w, vp, te_oc_cfg.radius_mm, clip, te_oc_shot); InterlockedIncrement(&te_sight_ok);
    } else { ok = mgs4vr_lens_ocular_clip(te_oc_shot, c2w, vp, te_oc_cfg.back_mm, te_oc_cfg.up_mm, te_oc_cfg.radius_mm, clip); InterlockedIncrement(&te_sight_fallback); }
    if (ok && te_dot_on) { float uv[2]; if (mgs4vr_lens_dot_uv(te_oc_shot, c2w, te_oc_cfg.back_mm, te_oc_cfg.up_mm, te_oc_cfg.radius_mm, uv)) mgs4vr_lens_set_ocular_dot(clip, uv, serial, eye); }
    else if (ok) mgs4vr_lens_set_ocular(clip, serial, eye);
}
/* Red dot sight (16 Oct 2026): only the glass disk on the sight and the dot on it, drawn into each eye image (mgs4vr_xr
   lens_inscope). No lens view: a dot sight is 1x, the eye sees through the glass, so no second world pass. */
static void te_dot_frame(void *image, BYTE *main, const MGS4VR_LENS_CFG *c, const MGS4VR_EYE_TICKET *t) {
    float shot[16], c2w[16], vp[16];
    te_oc_valid = 0; te_dot_on = 0;
    if (!c->inscope || !te_lens_shot(image, shot)) return;
    __try { memcpy(c2w, main, 64); memcpy(vp, main + 0x100, 64); } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&te_faults); return; }
    memcpy(te_oc_shot, shot, 64); te_oc_cfg = *c; te_oc_valid = 1; te_dot_on = 1;
    te_lens_ocular(c2w, vp, t->packet.camera_serial, t->packet.eye); InterlockedIncrement(&te_dot_frames);
}
/* Game thread, owner build of the main camera, lens wanted and two-eye off. */
static void te_lens_frame(void *image, BYTE *main, BYTE *v1, const MGS4VR_LENS_CFG *c, const MGS4VR_EYE_TICKET *t) {
    unsigned char blk[MGS4VR_TWOEYE_BLOCK]; int fw, fh, s, vp[6], barrel; float k = 1.0f / tanf(c->fov_deg * 0.5f * 0.017453293f), rel[16];
    barrel = 0; te_oc_valid = 0; if (te_uvmask) te_uv_install((BYTE *)image);
    if (!c->controller && !c->head) {                                   /* camera on the game rifle's barrel, quad on the rifle */
        float shot[16], c2w[16], w2c[16], q[4], p[3];
        if (te_lens_shot(image, shot)) {
            __try { memcpy(c2w, main, 64); memcpy(w2c, main + 0x40, 64); } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&te_faults); return; }
            barrel = mgs4vr_lens_scope_rel(shot, c2w, w2c, c->front_mm, rel);
            if (te_dot_on) { float kd = mgs4vr_lens_dot_k(shot, c2w, c->back_mm, c->up_mm, c->radius_mm); if (kd > 0) k = kd; }   /* dot sight: exactly 1x */
            if (t && mgs4vr_lens_world_quad(shot, w2c, t->packet.quat, t->packet.pos, c->back_mm, c->up_mm, q, p)) mgs4vr_lens_set_world_quad(q, p);
            if (t && c->inscope) {                                              /* ocular disk in this eye's picture */
                float vp[16];
                __try { memcpy(vp, main + 0x100, 64); } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&te_faults); return; }
                memcpy(te_oc_shot, shot, 64); te_oc_cfg = *c; te_oc_valid = 1;
                te_lens_ocular(c2w, vp, t->packet.camera_serial, t->packet.eye);
            }
        }
    }
    if (!barrel) barrel = mgs4vr_lens_barrel(rel);                      /* fallback: controller aim */
    __try {
        BYTE *hud = main + 4 * TE_VIEW_STRIDE;
        fw = *(int *)(hud + TE_VP + 0x10); fh = *(int *)(hud + TE_VP + 0x14);
        if (fw < 256 || fh < 256 || fw > 16384 || fh > 16384) { InterlockedIncrement(&te_rejects); return; }
        s = c->size < fh ? c->size : fh;
        if (!mgs4vr_twoeye_lens_block(main, k, barrel ? rel : NULL, blk)) { InterlockedIncrement(&te_rejects); return; }
        memcpy(v1, blk, MGS4VR_TWOEYE_BLOCK);
        memcpy(v1 + 0x370, main + 0x370, 0x20); memcpy(v1 + 0x390, main + 0x390, 4); memcpy(v1 + 0x39C, main + 0x39C, 4);
        memcpy(v1 + 0x400, main + 0x400, 0x80);
        vp[0] = 0; vp[1] = c->top ? 0 : fh - s; vp[2] = s; vp[3] = vp[1] + s; vp[4] = s; vp[5] = s;   /* bottom-left (or top-left) corner */
        /* the scene setup rebuilds the projection from these builder inputs (0xEAF00 -> 0xB9B70): +0x380 focal scale (= P[0][0]),
           +0x384 aspect factor (P[1][1] = -P[0][0] * w/h * f), +0x388/+0x38C skew. Sim 13 Oct: copied from the main view the
           lens only zoomed ~2x. Square viewport: f = 1, no skew. */
        { float ff[4] = { k, 1.0f, 0.0f, 0.0f }; memcpy(v1 + 0x380, ff, sizeof(ff)); }
        memcpy(v1 + TE_VP, vp, sizeof(vp));
        if (te_lens_kind_saved < 0) te_lens_kind_saved = *(int *)(v1 + TE_KIND);
        *(int *)(v1 + TE_KIND) = *(int *)(main + TE_KIND);
        /* pass bit 0x80000 (colour/NV pass) samples the FULL frame into the view rect: with it the square showed the whole
           main image squashed and no zoom at all (sim 13 Oct, pass-bit bisect: only this bit changes the lens). Without it the
           scope camera's own zoomed image survives, colours close to the main view. */
        if (te_pass_saved < 0) te_pass_saved = *(unsigned *)(v1 + 0x3B0);
        /* 16 Oct: with the source-rect hook the colour pass stays on and reads the lens square itself (grading + NV in the scope) */
        *(unsigned *)(v1 + 0x3B0) = (unsigned)te_pass_saved & ~(te_uv_installed && (te_uvmask & 8) ? 0u : 0x80000u);
        *(unsigned *)(v1 + TE_FLAGS) = *(unsigned *)(main + TE_FLAGS) | 1u;
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&te_faults); return; }
    if (te_uv_installed && te_uvmask) InterlockedExchange(&te_uv_on, 1);
    te_lens_on = 1; te_lens_v = v1; mgs4vr_lens_set_rect(0, c->top ? 0 : fh - s, s); InterlockedIncrement(&te_lens_frames);
}
/* HUD per eye (16 Oct 2026, sim): the 2D view 4 draws the HUD over the full width, so the left eye saw the left half
   (life, Solid Eye) and the right eye the right half (radar, weapon); object-bound UI ended up in one eye only. All HUD
   nodes are 2D list 2 of view 4's render context (+0x490; 81 nodes, virtual space 1280x720), while view 4 itself also
   composites the scene to the back buffer (shrinking view 4 squashed the scene). So: view 4 stays full without list 2
   (pass bit 0x40000), and the sleeping world views 3 and 2 become HUD views (view 4's context, pass 0x40000 only, kind 0,
   flags 1) with a centred 16:9 box in the left and right half. Sim: the full HUD in both eyes, scene intact. View 2 is
   also the lens view (lens view=2): while the lens runs the HUD falls back to the split one. Token hud=split = off. */
#define TE_HUD_BIT 0x40000u
typedef struct { int vp[6]; unsigned flags, pass; int kind; unsigned long long ctx; } TE_VIEWSTATE;
static volatile LONG te_hud = 1, te_hud_frames, te_hud_off_lens;
static int te_hud_on; static TE_VIEWSTATE te_hud_saved[2]; static unsigned te_hud4_pass = 0;   /* game thread */
static void te_view_save(BYTE *v, TE_VIEWSTATE *s) {
    memcpy(s->vp, v + TE_VP, sizeof(s->vp)); s->flags = *(unsigned *)(v + TE_FLAGS); s->pass = *(unsigned *)(v + 0x3B0);
    s->kind = *(int *)(v + TE_KIND); s->ctx = *(unsigned long long *)(v + 0x490);
}
static void te_view_load(BYTE *v, const TE_VIEWSTATE *s) {
    *(unsigned *)(v + TE_FLAGS) = 0; memcpy(v + TE_VP, s->vp, sizeof(s->vp)); *(unsigned *)(v + 0x3B0) = s->pass;
    *(int *)(v + TE_KIND) = s->kind; *(unsigned long long *)(v + 0x490) = s->ctx; *(unsigned *)(v + TE_FLAGS) = s->flags;
}
/* 16:9 box centred in one eye half (x0 = 0 left, half width right). */
void mgs4vr_twoeye_hud_box(int full_w, int full_h, int right, int vp[6]) {
    int hw = (full_w / 2) & ~1, bh = (hw * 9 / 16) & ~1;
    if (bh > full_h) bh = full_h & ~1;
    vp[0] = right ? full_w - hw : 0; vp[1] = (full_h - bh) / 2; vp[2] = vp[0] + hw; vp[3] = vp[1] + bh; vp[4] = hw; vp[5] = bh;
}
/* 17 Oct 2026 (headset: Solid Eye labels double in VR): the eye images are asymmetric (the left eye's optical axis lies
   right of its half's centre, the right eye's left of it), so the same HUD pixel in both halves pointed in two directions.
   The box is now centred on each eye's optical axis (fov = OpenXR angles left,right,up,down) and shrunk where needed to
   stay inside its own half; same size in both eyes. Invalid fov -> the plain centred box. */
void mgs4vr_twoeye_hud_box_fov(int full_w, int full_h, int right, const float fov[4], int vp[6]) {
    int hw = (full_w / 2) & ~1, x0 = right ? full_w - hw : 0, bw, bh; float tl, tr, tu, td, cx, cy, half_w, half_h;
    if (!fov || !(fov[0] < 0 && fov[1] > 0 && fov[2] > 0 && fov[3] < 0) || fov[1] - fov[0] > 3.0f || fov[2] - fov[3] > 3.0f) { mgs4vr_twoeye_hud_box(full_w, full_h, right, vp); return; }
    tl = tanf(fov[0]); tr = tanf(fov[1]); tu = tanf(fov[2]); td = tanf(fov[3]);
    cx = x0 + hw * (-tl / (tr - tl)); cy = full_h * (tu / (tu - td));
    half_w = cx - x0 < x0 + hw - cx ? cx - x0 : x0 + hw - cx; half_h = cy < full_h - cy ? cy : full_h - cy;
    if (half_w * 9.f / 16.f > half_h) half_w = half_h * 16.f / 9.f;                 /* 16:9 within both limits */
    bw = ((int)(2 * half_w)) & ~1; bh = (bw * 9 / 16) & ~1;
    vp[0] = (int)(cx - bw / 2.f + 0.5f); vp[1] = (int)(cy - bh / 2.f + 0.5f); vp[2] = vp[0] + bw; vp[3] = vp[1] + bh; vp[4] = bw; vp[5] = bh;
    if (vp[0] < x0) { vp[2] += x0 - vp[0]; vp[0] = x0; } if (vp[2] > x0 + hw) { vp[0] -= vp[2] - x0 - hw; vp[2] = x0 + hw; }
    if (vp[1] < 0) { vp[3] -= vp[1]; vp[1] = 0; } if (vp[3] > full_h) { vp[1] -= vp[3] - full_h; vp[3] = full_h; }
}
static void te_hud_restore(BYTE *main) {
    BYTE *v4 = main + 4 * TE_VIEW_STRIDE; int i;
    if (!te_hud_on) return;
    __try {
        for (i = 0; i < 2; ++i) te_view_load(main + (3 - i) * TE_VIEW_STRIDE, &te_hud_saved[i]);   /* [0] view 3 left, [1] view 2 right */
        *(unsigned *)(v4 + 0x3B0) = te_hud4_pass;
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&te_faults); }
    te_hud_on = 0;
}
static const float *te_hud_fov[2];   /* game thread: this frame's eye fov (left, right), NULL = centred box */
static void te_hud_apply(BYTE *main) {
    BYTE *v4 = main + 4 * TE_VIEW_STRIDE; int i, vp[6];
    __try {
        if (!te_hud_on) {
            if (!(*(unsigned *)(v4 + 0x3B0) & TE_HUD_BIT)) return;                 /* nothing to move (no HUD list on view 4) */
            for (i = 0; i < 2; ++i) te_view_save(main + (3 - i) * TE_VIEW_STRIDE, &te_hud_saved[i]);
            te_hud4_pass = *(unsigned *)(v4 + 0x3B0); te_hud_on = 1;
        }
        for (i = 0; i < 2; ++i) {
            BYTE *v = main + (3 - i) * TE_VIEW_STRIDE;
            mgs4vr_twoeye_hud_box_fov(te_full_w, te_full_h, i, te_hud_fov[i], vp);
            memcpy(v + TE_VP, vp, sizeof(vp)); *(unsigned *)(v + 0x3B0) = TE_HUD_BIT; *(int *)(v + TE_KIND) = 0;
            *(unsigned long long *)(v + 0x490) = *(unsigned long long *)(v4 + 0x490); *(unsigned *)(v + TE_FLAGS) = *(unsigned *)(v4 + TE_FLAGS) | 1u;
        }
        *(unsigned *)(v4 + 0x3B0) = te_hud4_pass & ~TE_HUD_BIT;
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&te_faults); return; }
    InterlockedIncrement(&te_hud_frames);
}
static void te_restore(BYTE *main, BYTE *v1) {
    int vp[6] = { 0, 0, te_full_w, te_full_h, te_full_w, te_full_h };
    __try {
        if (te_full_w > 0 && te_full_h > 0) memcpy(main + TE_VP, vp, sizeof(vp));
        *(unsigned *)(v1 + TE_FLAGS) = 0;
        if (te_kind_saved >= 0) { *(int *)(v1 + TE_KIND) = te_kind_saved; te_kind_saved = -1; }
        if (te_two_pass_saved >= 0) { *(unsigned *)(v1 + 0x3B0) = (unsigned)te_two_pass_saved; te_two_pass_saved = -1; }
        if (te_main_pass_saved >= 0) { *(unsigned *)(main + 0x3B0) = (unsigned)te_main_pass_saved; te_main_pass_saved = -1; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&te_faults); }
    te_hud_restore(main);
    InterlockedExchange(&te_uv_on, te_lens_on && te_uv_installed && te_uvmask);
    te_active = 0; te_miss = 0; InterlockedIncrement(&te_restores);
}
/* 15 Oct 2026 (sim): switching two-eye off sometimes left the main view half wide (AFR picture with a black right half),
   although te_restore ran (3 of 9 toggles). Not overlapping camera builds (overlaps stayed 0): the view render 0xB84E0 with
   pass bit 0x100000 reads +0x350..+0x364 at its start, scales them for dynamic resolution and writes the values it READ back
   at its end - on the render thread, so a restore that lands in between is undone with the old halves. While two-eye is off,
   a main viewport that is still exactly our left half is therefore put back to full every owner build (te_heals; sim after
   the fix: 9 of 9 toggles full, heals 3). The frame is also serialised (cheap; te_overlaps counts overlapping entries). */
static SRWLOCK te_lock = SRWLOCK_INIT;
static volatile LONG te_inside, te_overlaps, te_heals;
static void te_frame(void *image_base, unsigned long long camera, int gameplay, int applied, const MGS4VR_EYE_TICKET *t);
void mgs4vr_twoeye_frame(void *image_base, unsigned long long camera, int gameplay, int applied, const MGS4VR_EYE_TICKET *t) {
    if (InterlockedIncrement(&te_inside) > 1) InterlockedIncrement(&te_overlaps);
    AcquireSRWLockExclusive(&te_lock);
    te_frame(image_base, camera, gameplay, applied, t);
    ReleaseSRWLockExclusive(&te_lock);
    InterlockedDecrement(&te_inside);
}
/* Game thread, two-eye inactive: our half layout still on the main view -> full again. */
static void te_heal(BYTE *main) {
    int vp[6], half[6], full[6]; int hw = (te_full_w / 2) & ~1;
    if (te_full_w <= 0 || te_full_h <= 0) return;
    half[0] = 0; half[1] = 0; half[2] = hw; half[3] = te_full_h; half[4] = hw; half[5] = te_full_h;
    full[0] = 0; full[1] = 0; full[2] = te_full_w; full[3] = te_full_h; full[4] = te_full_w; full[5] = te_full_h;
    __try { memcpy(vp, main + TE_VP, sizeof(vp)); if (!memcmp(vp, half, sizeof(vp))) { memcpy(main + TE_VP, full, sizeof(full)); InterlockedIncrement(&te_heals); } }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&te_faults); }
}
static void te_frame(void *image_base, unsigned long long camera, int gameplay, int applied, const MGS4VR_EYE_TICKET *t) {
    BYTE *main = (BYTE *)image_base + TE_MAIN_RVA, *v1 = main + TE_VIEW_STRIDE;
    unsigned char right[MGS4VR_TWOEYE_BLOCK]; int hw, vp[6];
    if (!image_base || camera != (unsigned long long)main) return;
    if (!te_active && !te_wanted && gameplay) te_heal(main);
    {   /* lens scope on view 1 only while two-eye is off (both use view 1); on view 2/3 (lens token view=) also with two-eye */
        MGS4VR_LENS_CFG lc; int own; BYTE *lv;
        mgs4vr_lens_config(&lc); own = lc.view >= 2 && lc.view <= 3; lv = main + (own ? lc.view : 1) * TE_VIEW_STRIDE;
        if (te_lens_on && te_lens_v && te_lens_v != lv) te_lens_off(te_lens_v);   /* view token changed */
        if (lc.wanted && (own || (!te_wanted && !te_active))) {
            if (gameplay && t && t->packet.valid) {
                int code = -1, ok, dot = 0;
                __try {
                    const BYTE *eq = (const BYTE *)image_base + MGS4VR_LENS_EQUIP_RVA;
                    code = *(unsigned short *)((BYTE *)image_base + MGS4VR_LENS_WEAPON_RVA) & 0x1ff;
                    if (lc.dot) dot = mgs4vr_lens_dot_attached(); (void)eq;   /* attachment part code (was the slot mask: a first-person flag) */
                } __except (EXCEPTION_EXECUTE_HANDLER) { code = -1; dot = 0; }
                ok = mgs4vr_lens_weapon_ok(&lc, code); if (ok) dot = 0;   /* a scope weapon keeps the scope */
                mgs4vr_lens_note_weapon(code, ok || dot); mgs4vr_lens_set_dot(dot);
                if (dot && !lc.dotpic) { MGS4VR_LENS_CFG wc = lc; if (te_lens_on) { te_lens_off(lv); mgs4vr_lens_set_dot(1); }   /* dot=glass token: disk only, no lens view, HUD stays per eye */
                    mgs4vr_lens_apply_dot(&wc); te_dot_frame(image_base, main, &wc, t); }
                else if (dot) { MGS4VR_LENS_CFG wc = lc; te_dot_on = 1; if (own && te_hud_on) { te_hud_restore(main); InterlockedIncrement(&te_hud_off_lens); }
                    /* the game's sight glass is an opaque mirror (sim 16 Oct): the disk shows the barrel camera at 1x (te_lens_frame) + the dot */
                    mgs4vr_lens_apply_dot(&wc); te_lens_frame(image_base, main, lv, &wc, t); InterlockedIncrement(&te_dot_frames); }
                else if (ok) { MGS4VR_LENS_CFG wc = lc; te_dot_on = 0; if (own && te_hud_on) { te_hud_restore(main); InterlockedIncrement(&te_hud_off_lens); }   /* the lens needs view 2: HUD back to split */
                    mgs4vr_lens_apply_weapon(&wc, code); te_lens_frame(image_base, main, lv, &wc, t); }
                else { te_dot_on = 0; if (te_lens_on) te_lens_off(lv); else te_oc_valid = 0; }   /* no scoped weapon: lens view off */
            }
            if (!own) return;
        } else { if (te_lens_on) te_lens_off(te_lens_v ? te_lens_v : v1); if (te_dot_on) { te_dot_on = 0; te_oc_valid = 0; mgs4vr_lens_set_dot(0); } }
    }
    if (!gameplay) { if (te_active && !te_wanted) te_restore(main, v1); return; }   /* the wrapper build of the same frame: wait for the owner build */
    if (!te_wanted || !applied || !t || !t->twoeye || !t->packet.valid) {
        /* A single frame without a two-eye ticket (the eye module skips a frame now and then: sim 13 Oct, 20 restores in
           30 s) must not flip the screen layout: hold it, restore after TE_HOLD missed owner builds or at once when the
           mode is switched off. */
        if (te_active && (!te_wanted || ++te_miss > TE_HOLD)) te_restore(main, v1);
        return;
    }
    te_miss = 0;
    if (te_uvmask) te_uv_install((BYTE *)image_base);
    __try {
        if (!te_active) {                                          /* remember the real full size once */
            /* the full size comes from the 2D/HUD view (static view 4, never touched here): learning it from the main view
               once picked up our own half width (sim 13 Oct: full=1920 after toggling) */
            BYTE *hud = main + 4 * TE_VIEW_STRIDE;
            te_full_w = *(int *)(hud + TE_VP + 0x10); te_full_h = *(int *)(hud + TE_VP + 0x14);
            if (te_full_w < 64 || te_full_h < 64 || te_full_w > 16384 || te_full_h > 16384) { InterlockedIncrement(&te_rejects); return; }
        }
        if (!mgs4vr_twoeye_right_block(main, &t->adjust, &t->adjust_right, 1, right)) { InterlockedIncrement(&te_rejects); if (te_active) te_restore(main, v1); return; }
        hw = (te_full_w / 2) & ~1;
        memcpy(v1, right, MGS4VR_TWOEYE_BLOCK);
        memcpy(v1 + 0x370, main + 0x370, 0x20);                    /* near/far, clip ranges (as the game's RT camera copies) */
        memcpy(v1 + 0x390, main + 0x390, 4); memcpy(v1 + 0x39C, main + 0x39C, 4);
        memcpy(v1 + 0x400, main + 0x400, 0x80);
        mgs4vr_twoeye_right_builder(main + 0x380, right, v1 + 0x380);
        if ((te_lens_on || te_dot_on) && te_oc_valid && te_oc_cfg.inscope && t->packet_right.valid) {   /* lens on its own view: ocular disk for the right eye too */
            float c2w_r[16], vp_r[16]; memcpy(c2w_r, right, 64); memcpy(vp_r, right + 0x100, 64);
            te_lens_ocular(c2w_r, vp_r, t->packet_right.camera_serial, t->packet_right.eye); InterlockedIncrement(&te_oc_right);
        }
        vp[0] = hw; vp[1] = 0; vp[2] = te_full_w; vp[3] = te_full_h; vp[4] = te_full_w - hw; vp[5] = te_full_h;
        memcpy(v1 + TE_VP, vp, sizeof(vp));
        vp[0] = 0; vp[1] = 0; vp[2] = hw; vp[3] = te_full_h; vp[4] = hw; vp[5] = te_full_h;
        memcpy(main + TE_VP, vp, sizeof(vp));
        if (te_kind_saved < 0) te_kind_saved = *(int *)(v1 + TE_KIND);
        *(int *)(v1 + TE_KIND) = *(int *)(main + TE_KIND);
        if (te_two_pass_saved < 0) te_two_pass_saved = *(unsigned *)(v1 + 0x3B0);
        *(unsigned *)(v1 + 0x3B0) = (unsigned)te_two_pass_saved & ~te_clear_bits(te_passclear);
        if (te_clear_bits(te_mainclear)) {
            if (te_main_pass_saved < 0) te_main_pass_saved = *(unsigned *)(main + 0x3B0);
            *(unsigned *)(main + 0x3B0) = (unsigned)te_main_pass_saved & ~te_clear_bits(te_mainclear);
        } else if (te_main_pass_saved >= 0) { *(unsigned *)(main + 0x3B0) = (unsigned)te_main_pass_saved; te_main_pass_saved = -1; }
        *(unsigned *)(v1 + TE_FLAGS) = *(unsigned *)(main + TE_FLAGS) | 1u | (unsigned)te_orflags;
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&te_faults); return; }
    te_hud_fov[0] = t ? t->packet.fov : NULL; te_hud_fov[1] = t && t->twoeye ? t->packet_right.fov : NULL;
    if (te_hud && !te_lens_on) te_hud_apply(main); else te_hud_restore(main);
    InterlockedExchange(&te_uv_on, te_uv_installed && te_uvmask);
    te_active = 1; InterlockedIncrement(&te_frames);
}
static void te_report(const char *event) {
    FILE *f; if (!te_status[0] || fopen_s(&f, te_status, "w")) return;
    fprintf(f, "twoeye_version=1 event=%s wanted=%ld frames=%ld restores=%ld rejects=%ld faults=%ld full=%dx%d passclear=%x mainclear=%x v1pass=%llx uvfix=%lx uv_installed=%ld uv_failed=%ld uv_on=%ld uv_shifted=%ld overlaps=%ld heals=%ld oc_right=%ld hud=%ld hud_frames=%ld hud_off_lens=%ld lens_frames=%ld lens_shot_ok=%ld lens_shot_fail=%ld lens_sight_ok=%ld lens_sight_fallback=%ld dot_frames=%ld skip05=%lx skipped05=%ld\n",
            event, te_wanted, te_frames, te_restores, te_rejects, te_faults, te_full_w, te_full_h, te_clear_bits(te_passclear), te_clear_bits(te_mainclear), te_two_pass_saved, (unsigned long)te_uvmask, te_uv_installed, te_uv_failed, te_uv_on, te_uv_shifted, te_overlaps, te_heals, te_oc_right, te_hud, te_hud_frames, te_hud_off_lens, te_lens_frames, te_shot_ok, te_shot_fail, te_sight_ok, te_sight_fallback, te_dot_frames, (unsigned long)te_skip05, te_skipped05);
    fclose(f);
#if !defined(DG_ENABLE_DIAGNOSTICS) || DG_ENABLE_DIAGNOSTICS != 0   /* measurement census: development builds only */
    if (te_census_n) { char cp[MAX_PATH]; const char *leaf = strrchr(te_status, '\\');
        if (leaf) { sprintf_s(cp, sizeof(cp), "%.*smgs4vr_twoeye_op05_%lu.txt", (int)(leaf + 1 - te_status), te_status, GetCurrentProcessId()); te_census_write(cp); } }
#endif
}
int mgs4vr_twoeye_poll(const char *exe) {
    DWORD now = GetTickCount(); char b[160] = { 0 }; FILE *f; int on = 0; char *leaf;
    if (!te_mark[0]) {
        if (!exe) return 0;
        strcpy_s(te_mark, sizeof(te_mark), exe); leaf = strrchr(te_mark, '\\'); if (!leaf) { te_mark[0] = 0; return 0; }
        strcpy_s(leaf + 1, sizeof(te_mark) - (size_t)(leaf + 1 - te_mark), "mgs4vr_twoeye.on");
        strcpy_s(te_status, sizeof(te_status), te_mark);
        sprintf_s(strrchr(te_status, '\\') + 1, 64, "mgs4vr_twoeye_%lu.txt", GetCurrentProcessId());
    }
    if (now - te_tick < 500 && te_tick) return te_wanted;
    te_tick = now;
    if (!fopen_s(&f, te_mark, "rb")) { size_t n = fread(b, 1, sizeof(b) - 1, f); fclose(f); b[n] = 0;
        { char *p = b; if ((unsigned char)p[0] == 0xef) p += 3; while (*p == ' ') ++p; on = !strncmp(p, "on", 2); { const char *q = strstr(p, "orflags="); unsigned v = 0; if (q && sscanf_s(q + 8, "%x", &v) == 1) InterlockedExchange(&te_orflags, (LONG)v); else InterlockedExchange(&te_orflags, 0); }
          { const char *q = strstr(p, "passclear="); unsigned v = 0; if (q && sscanf_s(q + 10, "%x", &v) == 1) InterlockedExchange(&te_passclear, (LONG)(v & 0x7FFFFFFF)); else InterlockedExchange(&te_passclear, TE_AUTO); }
          { const char *q = strstr(p, "mainclear="); unsigned v = 0; if (q && sscanf_s(q + 10, "%x", &v) == 1) InterlockedExchange(&te_mainclear, (LONG)(v & 0x7FFFFFFF)); else InterlockedExchange(&te_mainclear, TE_AUTO); }
          InterlockedExchange(&te_hud, strstr(p, "hud=split") == NULL);
          { const char *q = strstr(p, "skip05="); unsigned v = 0; InterlockedExchange(&te_skip05, q && sscanf_s(q + 7, "%x", &v) == 1 ? (LONG)v : 0); }
          { const char *q = strstr(p, "spr64="); int v = 0; InterlockedExchange(&te_spr64, q && sscanf_s(q + 6, "%d", &v) == 1 ? v : 2); }   /* default OFF (user 16 Oct 19:30) */
          { const char *q = strstr(p, "spr64k="); int v = 0; InterlockedExchange(&te_spr64k, q && sscanf_s(q + 7, "%d", &v) == 1 ? v : 0); }
          { const char *q = strstr(p, "skip6min="); int v = 0; InterlockedExchange(&te_skip6min, q && sscanf_s(q + 9, "%d", &v) == 1 ? v : 0); }
          { const char *q = strstr(p, "skip6max="); int v = 0; InterlockedExchange(&te_skip6max, q && sscanf_s(q + 9, "%d", &v) == 1 ? v : 0x7FFF); }
          { const char *q = strstr(p, "uvfix="); unsigned v = 0; if (q && sscanf_s(q + 6, "%x", &v) == 1) InterlockedExchange(&te_uvmask, (LONG)v); else InterlockedExchange(&te_uvmask, TE_UV_DEFAULT); } } }
    if (InterlockedExchange(&te_wanted, on) != on || on) te_report(on ? "active" : "off");
    return on;
}
