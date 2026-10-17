/* mgs4vr_cqcfp.c - see mgs4vr_cqcfp.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mgs4vr_cqcfp.h"

#define BONES 115
#define HEAD_BONE 79          /* same as mgs4vr_armsrc.c */
#define FP_RADIUS 400.0f      /* mm: first-person camera within this of the head bone */

int mgs4vr_cqcfp_parse(const char *b, MGS4VR_CQCFP_CFG *c) {
    c->wanted = c->freeze = c->force = 0;
    if (!b) return 0;
    if ((unsigned char)b[0] == 0xef && (unsigned char)b[1] == 0xbb && (unsigned char)b[2] == 0xbf) b += 3;
    while (*b == ' ' || *b == '\t') ++b;
    c->wanted = !strncmp(b, "on", 2) && (b[2] == 0 || b[2] == ' ' || b[2] == '\r' || b[2] == '\n');
    c->freeze = strstr(b, "yaw=freeze") != NULL;
    c->force = strstr(b, " force") != NULL;
    return c->wanted;
}

static int finite_n(const float *v, int n) { int i; for (i = 0; i < n; ++i) if (!isfinite(v[i]) || fabsf(v[i]) > 1e8f) return 0; return 1; }
/* Rows of the root's 3x3, Gram-Schmidt (the model root may carry scale). */
static int ortho_rows(const float m[16], float r[9]) {
    float a[3] = { m[0], m[1], m[2] }, b[3] = { m[4], m[5], m[6] }, l, d; int i;
    l = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); if (!(l > 1e-6f)) return 0;
    for (i = 0; i < 3; ++i) a[i] /= l;
    d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; for (i = 0; i < 3; ++i) b[i] -= d * a[i];
    l = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]); if (!(l > 1e-6f)) return 0;
    for (i = 0; i < 3; ++i) b[i] /= l;
    r[0] = a[0]; r[1] = a[1]; r[2] = a[2]; r[3] = b[0]; r[4] = b[1]; r[5] = b[2];
    r[6] = a[1] * b[2] - a[2] * b[1]; r[7] = a[2] * b[0] - a[0] * b[2]; r[8] = a[0] * b[1] - a[1] * b[0];
    if (r[6] * m[8] + r[7] * m[9] + r[8] * m[10] < 0) { r[6] = -r[6]; r[7] = -r[7]; r[8] = -r[8]; }   /* keep the third axis' sense */
    return 1;
}
static void mul3t(const float a[9], const float b[9], float o[9]) {   /* a * b^T */
    int r, c, k; for (r = 0; r < 3; ++r) for (c = 0; c < 3; ++c) { float s = 0; for (k = 0; k < 3; ++k) s += a[r * 3 + k] * b[c * 3 + k]; o[r * 3 + c] = s; }
}
static void mul3(const float a[9], const float b[9], float o[9]) {
    int r, c, k; for (r = 0; r < 3; ++r) for (c = 0; c < 3; ++c) { float s = 0; for (k = 0; k < 3; ++k) s += a[r * 3 + k] * b[k * 3 + c]; o[r * 3 + c] = s; }
}

int mgs4vr_cqcfp_calibrate(const float cam[16], const float root[16], const float head[3], MGS4VR_CQCFP_CAL *cal) {
    float rr[9], rc[9], dw[3]; int i, j;
    if (!finite_n(cam, 16) || !finite_n(root, 16) || !finite_n(head, 3) || !ortho_rows(root, rr)) return 0;
    for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) rc[i * 3 + j] = cam[i * 4 + j];
    for (i = 0; i < 3; ++i) dw[i] = cam[12 + i] - head[i];
    if (sqrtf(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]) > FP_RADIUS) return 0;
    mul3t(rc, rr, cal->rrel);
    for (i = 0; i < 3; ++i) cal->d[i] = dw[0] * rr[i * 3 + 0] + dw[1] * rr[i * 3 + 1] + dw[2] * rr[i * 3 + 2];   /* dw * rr^T */
    memcpy(cal->cam_rot, rc, sizeof(rc)); memcpy(cal->root_rot, rr, sizeof(rr)); cal->valid = 1;
    return 1;
}

int mgs4vr_cqcfp_build(const MGS4VR_CQCFP_CAL *cal, const float root[16], const float head[3], int freeze, float out[16]) {
    float rr[9], rot[9]; const float *pr; int i, j;
    if (!cal || !cal->valid || !finite_n(root, 16) || !finite_n(head, 3) || !ortho_rows(root, rr)) return 0;
    if (freeze) { memcpy(rot, cal->cam_rot, sizeof(rot)); pr = cal->root_rot; }
    else { mul3(cal->rrel, rr, rot); pr = rr; }
    memset(out, 0, 16 * sizeof(float));
    for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) out[i * 4 + j] = rot[i * 3 + j];
    for (j = 0; j < 3; ++j) out[12 + j] = head[j] + cal->d[0] * pr[j] + cal->d[1] * pr[3 + j] + cal->d[2] * pr[6 + j];
    out[15] = 1.0f;
    return finite_n(out, 16);
}

/* ------------------------------------------------------------------ game --- */
static BYTE *volatile fp_image;
static MGS4VR_CQCFP_CFG fp_cfg;              /* worker writes whole ints, game thread reads */
static MGS4VR_CQCFP_CAL fp_cal;              /* game thread only */
static volatile LONG fp_calls, fp_cals, fp_overrides, fp_no_player, fp_no_cal, fp_build_fail, fp_last_act = -1;
static float fp_maxpos, fp_maxrot, fp_lastd;  /* force mode: rebuilt vs game camera in first person */
/* Head hiding (parts seam, any thread): in first person the game gives Snake's body mask 0x1E and his head/face
   models mask 3 (bits = hidden parts, mgs4vr_scope.c); in third person 0xC and 0. The parts seam sees RENDER model
   objects (not the owner-list model; sim 14 Oct: other pointer) whose world position (+0x50, matrix row 3) is Snake's
   root, also for the head/face models. Learned in first person only: the 115-bone model within BODY_NEAR of the
   root = Snake's body; every other model with a non-zero mask = a head/face piece, kept with that mask (only Snake's own
   models carry first-person masks; scope log 14 Oct: face 39 bones mask 0x1F = mouth/teeth, 14 bones mask 3; their
   +0x50 is 0,0,0 because they hang on the body, so no position test). Identity = part table (+0xF0). While the head camera is on, the body gets
   0x1E (arms shown) and each head piece its first-person mask. */
#define BODY_NEAR 300.0f
#define FRESH_MS 250u
#define MAX_HEAD_MODELS 64   /* was 4, then 12: headset 17 Oct, list full both times and hair/face still visible at a wall */
#define MASK_BODY_FP 0x1eull
#define MASK_HEAD_FP 0x3ull
/* sim 17 Oct: the learning also caught soldiers (76/71/62 bones, masks 0x1C/0xA/0x1D2) - they filled the list and their
   parts could be hidden during the head view. Snake's head pieces are small: face 39, 15 and 14 bones. */
#define HEAD_MAX_BONES 40u
/* sim 17 Oct: the face (39 bones) was learned with mask 0x8 in that first-person frame, so mouth and teeth stayed in view
   from the head camera; every part of Snake's own head pieces is hidden while the head camera is on. */
#define HEAD_ALL_PARTS 0xffffull
static volatile DWORD fp_over_tick, fp_cal_tick;
static volatile LONG fp_mode;                   /* last seam call: 1 calibrated (first person), 2 head camera (CQC) */
static float fp_root_last[3];
static const unsigned char *volatile fp_player_model;   /* owner-list model (seam) */
static volatile unsigned long long fp_body_key;         /* part table (+0xF0) of the body render model */
static volatile unsigned long long fp_head_keys[MAX_HEAD_MODELS], fp_head_masks[MAX_HEAD_MODELS];
static volatile unsigned fp_head_bones[MAX_HEAD_MODELS];   /* diagnostics: bone count per learned model */
static volatile LONG fp_head_n, fp_masked_body, fp_masked_head, fp_parts_calls, fp_parts_115, fp_parts_m3;
static DWORD (*fp_clock)(void) = GetTickCount;  /* desk tests */
static float fp_m3_pos[3]; static volatile unsigned fp_m3_bones;   /* last mask-3 model seen (diagnostics) */

static int near_root(const unsigned char *model, float r) {
    float p[3], d[3];
    __try { memcpy(p, model + 0x50, 12); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    d[0] = p[0] - fp_root_last[0]; d[1] = p[1] - fp_root_last[1]; d[2] = p[2] - fp_root_last[2];
    return isfinite(d[0]) && isfinite(d[1]) && isfinite(d[2]) && d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < r * r;
}

/* Record slots change model from frame to frame (mgs4vr_scope.c "log"): a model is identified by its part table. */
static unsigned long long part_key(const unsigned char *model) {
    unsigned long long k = 0;
    __try { k = *(const unsigned long long *)(model + 0xf0); } __except (EXCEPTION_EXECUTE_HANDLER) { k = 0; }
    return k;
}

unsigned long long mgs4vr_cqcfp_parts(const unsigned char *model, unsigned bones, unsigned long long mask) {
    DWORD now; int i, n; unsigned long long key;
    if (!fp_cfg.wanted || !model) return mask;
    now = fp_clock(); n = fp_head_n; InterlockedIncrement(&fp_parts_calls);
    if (bones == BONES) InterlockedIncrement(&fp_parts_115);
    if (mask == MASK_HEAD_FP) { InterlockedIncrement(&fp_parts_m3); __try { memcpy(fp_m3_pos, model + 0x50, 12); fp_m3_bones = bones; } __except (EXCEPTION_EXECUTE_HANDLER) {} }   /* diagnostics */
    if (!(key = part_key(model))) return mask;
    if (now - fp_cal_tick < FRESH_MS && fp_mode == 1) {            /* learn only in first person, never once CQC began */
        if (bones == BONES) { if (key != fp_body_key && near_root(model, BODY_NEAR)) fp_body_key = key; }
        else if (mask && bones <= HEAD_MAX_BONES) {                /* Snake's head/face pieces, with their own first-person mask */
            for (i = 0; i < n && fp_head_keys[i] != key; ++i) {}
            if (i == n && n < MAX_HEAD_MODELS) { fp_head_masks[n] = mask; fp_head_keys[n] = key; fp_head_bones[n] = bones; InterlockedExchange(&fp_head_n, n + 1); }
            else if (i < n) fp_head_masks[i] = mask;
        }
    }
    if (now - fp_over_tick >= FRESH_MS) return mask;
    if (key == fp_body_key && bones == BONES) { InterlockedIncrement(&fp_masked_body); return mask | MASK_BODY_FP; }
    for (i = 0; i < n; ++i) if (fp_head_keys[i] == key) { InterlockedIncrement(&fp_masked_head); return mask | fp_head_masks[i] | HEAD_ALL_PARTS; }
    return mask;
}
/* Head-bone frame (headset 17 Oct: at a wall Snake's head looks along the wall while his root faces away from it, so a
   root-relative offset put the camera beside his face). The calibrated eye offset is also kept in the head bone's own
   frame and used whenever the head matrix is sound. */
static float fp_headm[16], fp_dh[3]; static int fp_dh_valid; static volatile LONG fp_head_frame_builds;
static int read_player(BYTE *img, int *act, float root[16], float head[3]) {
    int slot; unsigned long long owner, actor, pd; BYTE *w, *model; float (*bones)[16];
    __try {
        slot = *(int *)(img + 0x23eb5150); if (slot < -1 || slot > 23) return 0;
        owner = *(unsigned long long *)(img + 0x23eb5160 + (size_t)(slot + 1) * 0x80); actor = owner & 0xfffffffffffffULL;
        if (actor < 0x10000 || *(unsigned long long *)(actor + 0x30) != owner) return 0;
        pd = *(unsigned long long *)(actor + 0x90); if (pd < 0x10000) return 0;
        *act = *(int *)(pd + 0x10c);
        w = *(BYTE **)(img + 0x23eb5160 + (size_t)(slot + 1) * 0x80 + 0x10); if (!w) return 0;   /* owner_model() of armsrc */
        model = *(BYTE **)w; if (!model || *(unsigned *)(model + 0xcc) != BONES) return 0;
        bones = *(float (**)[16])(model + 0x100); if (!bones) return 0;
        memcpy(root, model + 0x20, 64); memcpy(head, &bones[HEAD_BONE][12], 12); memcpy(fp_headm, bones[HEAD_BONE], 64);
        if (model != fp_player_model) { fp_player_model = model; fp_body_key = 0; InterlockedExchange(&fp_head_n, 0); }   /* new level/model: relearn */
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

static int (*fp_head_source)(void);
void mgs4vr_cqcfp_set_head_source(int (*fn)(void)) { fp_head_source = fn; }
/* Snake's act from the last seam call (any thread), -1 before the first. */
int mgs4vr_cqcfp_last_act(void) { return (int)InterlockedCompareExchange(&fp_last_act, 0, 0); }
int mgs4vr_cqcfp_seam(float m[16]) {
    BYTE *img = fp_image; int act, busy; float root[16], head[3], out[16];
    if (!img || !fp_cfg.wanted) return 0;
    InterlockedIncrement(&fp_calls);
    if (!read_player(img, &act, root, head)) { InterlockedIncrement(&fp_no_player); return 0; }
    InterlockedExchange(&fp_last_act, act); memcpy(fp_root_last, root + 12, sizeof(fp_root_last));
    busy = act >= 0xe0 && act <= 0x120;
    /* force: rebuild from the PREVIOUS frame's calibration and compare with the game's own first-person camera
       (= how well "head bone + fixed offset" tracks the real camera while Snake moves) */
    if (fp_cfg.force && !busy && mgs4vr_cqcfp_build(&fp_cal, root, head, fp_cfg.freeze, out)) {
        int i; float dp = 0, dr = 0;
        for (i = 0; i < 3; ++i) { float e = fabsf(out[12 + i] - m[12 + i]); if (e > dp) dp = e; }
        for (i = 0; i < 12; ++i) { float e = fabsf(out[i] - m[i]); if (e > dr) dr = e; }
        if (dp > fp_maxpos) fp_maxpos = dp; if (dr > fp_maxrot) fp_maxrot = dr;
    }
    if (!busy && mgs4vr_cqcfp_calibrate(m, root, head, &fp_cal)) {         /* any non-CQC first-person frame (sim: 0x2, live aim: 0x2E) */
        float dw[3] = { m[12] - head[0], m[13] - head[1], m[14] - head[2] };
        fp_lastd = sqrtf(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]); InterlockedIncrement(&fp_cals); fp_cal_tick = fp_clock(); fp_mode = 1;
        { float hr[9]; int i; if (finite_n(fp_headm, 16) && ortho_rows(fp_headm, hr)) { for (i = 0; i < 3; ++i) fp_dh[i] = dw[0] * hr[i * 3] + dw[1] * hr[i * 3 + 1] + dw[2] * hr[i * 3 + 2]; fp_dh_valid = 1; } }
        return 0;                                              /* first person: the game's camera stays */
    }
    if (!busy && !fp_cfg.force && !(fp_head_source && fp_head_source())) return 0;   /* + first-person action (mgs4vr_fpaction) */
    if (!fp_cal.valid) { InterlockedIncrement(&fp_no_cal); return 0; }
    if (!mgs4vr_cqcfp_build(&fp_cal, root, head, fp_cfg.freeze, out)) { InterlockedIncrement(&fp_build_fail); return 0; }
    { float hr[9]; int j; if (fp_dh_valid && finite_n(fp_headm, 16) && ortho_rows(fp_headm, hr)) {
        for (j = 0; j < 3; ++j) out[12 + j] = head[j] + fp_dh[0] * hr[j] + fp_dh[1] * hr[3 + j] + fp_dh[2] * hr[6 + j]; InterlockedIncrement(&fp_head_frame_builds); } }
    memcpy(m, out, sizeof(out)); InterlockedIncrement(&fp_overrides); fp_over_tick = fp_clock(); fp_mode = 2;
    return 1;
}

static char fp_mark[MAX_PATH], fp_status[MAX_PATH]; static DWORD fp_poll, fp_stat;
void mgs4vr_cqcfp_tick(void *image, const char *exe, DWORD now) {
    FILE *f; char b[256] = { 0 }, *leaf;
    if (!fp_mark[0] && exe) {
        strcpy_s(fp_mark, sizeof(fp_mark), exe); leaf = strrchr(fp_mark, '\\');
        if (leaf) { strcpy_s(leaf + 1, sizeof(fp_mark) - (size_t)(leaf + 1 - fp_mark), "mgs4vr_cqcfp.on"); strcpy_s(fp_status, sizeof(fp_status), fp_mark);
                    sprintf_s(strrchr(fp_status, '\\') + 1, 64, "mgs4vr_cqcfp_%lu.txt", GetCurrentProcessId()); }
        else fp_mark[0] = 0;
    }
    if (fp_mark[0] && now - fp_poll >= 500u) {
        MGS4VR_CQCFP_CFG c; fp_poll = now;
        if (!fopen_s(&f, fp_mark, "rb")) { size_t n = fread(b, 1, sizeof(b) - 1, f); fclose(f); b[n] = 0; }
        mgs4vr_cqcfp_parse(b, &c);
        fp_cfg.freeze = c.freeze; fp_cfg.force = c.force; fp_cfg.wanted = c.wanted;
        fp_image = (BYTE *)image;
    }
    if (fp_status[0] && fp_cfg.wanted && now - fp_stat >= 1000u) {
        fp_stat = now;
        if (!fopen_s(&f, fp_status, "w")) {
            fprintf(f, "cqcfp_version=1 wanted=%d freeze=%d force=%d calls=%ld cals=%ld overrides=%ld no_player=%ld no_cal=%ld build_fail=%ld act=%#lx cal_dist=%.1f cal_d=%.1f,%.1f,%.1f maxpos=%.2f maxrot=%.4f head_models=%ld masked_body=%ld masked_head=%ld parts_calls=%ld parts_115=%ld parts_m3=%ld body=%p player=%p m3_pos=%.0f,%.0f,%.0f m3_bones=%u root=%.0f,%.0f,%.0f\n",
                    fp_cfg.wanted, fp_cfg.freeze, fp_cfg.force, fp_calls, fp_cals, fp_overrides, fp_no_player, fp_no_cal, fp_build_fail, fp_last_act,
                    fp_lastd, fp_cal.d[0], fp_cal.d[1], fp_cal.d[2], fp_maxpos, fp_maxrot, fp_head_n, fp_masked_body, fp_masked_head, fp_parts_calls, fp_parts_115, fp_parts_m3, (const void *)(ULONG_PTR)fp_body_key, (const void *)fp_player_model, fp_m3_pos[0], fp_m3_pos[1], fp_m3_pos[2], fp_m3_bones, fp_root_last[0], fp_root_last[1], fp_root_last[2]);
            { int i, n = fp_head_n; fprintf(f, "head_frame=%d head_frame_builds=%ld dh=%.1f,%.1f,%.1f head_list=", fp_dh_valid, fp_head_frame_builds, fp_dh[0], fp_dh[1], fp_dh[2]);
              for (i = 0; i < n; ++i) fprintf(f, "%s%u:%#llx", i ? "," : "", fp_head_bones[i], fp_head_masks[i]); fprintf(f, "\n"); }
            fclose(f);
        }
    }
}
