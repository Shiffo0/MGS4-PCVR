/* mgs4vr_lens.c - see mgs4vr_lens.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mgs4vr_lens.h"
static SRWLOCK ln_lock = SRWLOCK_INIT;
static MGS4VR_LENS_CFG ln_cfg = { 0, 224, 1, 0, 1, 0, 0, 1, 1, 1, 6.0f, 0.22f, 0.06f, 0.70f, 50, 550, 80, 20, { 0 } };
typedef struct { float clip[16], dot[2]; unsigned serial; int eye; DWORD tick; } LN_OCULAR;
static LN_OCULAR ln_oc[4]; static int ln_oc_next; static volatile LONG ln_oc_set, ln_oc_exact, ln_oc_eye, ln_oc_miss;   /* under ln_lock */
static volatile LONG ln_weapon = -1, ln_weapon_ok, ln_weapon_blocked;
static volatile LONG ln_rect_x, ln_rect_y, ln_rect_s, ln_rect_tick, ln_frames, ln_quads;
static float ln_q[4], ln_p[3]; static int ln_have; static DWORD ln_pose_tick;   /* XR thread */
static float ln_rel[16]; static DWORD ln_rel_tick; static volatile LONG ln_barrels;   /* under ln_lock */
static float ln_wq[4], ln_wp[3]; static DWORD ln_wq_tick; static volatile LONG ln_world_quads, ln_rifle_frames;   /* under ln_lock */
static char ln_mark[MAX_PATH], ln_status[MAX_PATH]; static DWORD ln_tick;

int mgs4vr_lens_parse(const char *b, MGS4VR_LENS_CFG *c) {
    const char *q; float v;
    static const int def_w[8] = { 0x09, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D };   /* D.E. (L.B.), VSS, M82A2, DSR-1, M14 EBR, Mosin, SVD, Rail Gun (names: weapon table rva 0x1D82580 + code*80) */
    int i;
    c->wanted = 0; c->size = 224; c->top = 1; c->head = 0; c->reticle = 1; c->always = 0; c->controller = 0; c->round = 1; c->inscope = 1; c->view = 1; c->radius_mm = 20; c->fov_deg = 6.0f; c->dist_m = 0.22f; c->width_m = 0.06f; c->cam_m = 0.70f;
    c->front_mm = 50; c->back_mm = 550; c->up_mm = 80;
    memset(c->weapons, 0, sizeof(c->weapons)); c->ngeo = 0; c->geo_set = 0;
    for (i = 0; i < 8; ++i) c->weapons[def_w[i] >> 6] |= 1ull << (def_w[i] & 63);
    if (!b) return 0;
    if ((unsigned char)b[0] == 0xef && (unsigned char)b[1] == 0xbb && (unsigned char)b[2] == 0xbf) b += 3;
    while (*b == ' ' || *b == '\t') ++b;
    c->wanted = !strncmp(b, "on", 2) && (b[2] == 0 || b[2] == ' ' || b[2] == '\r' || b[2] == '\n');
    c->top = strstr(b, "corner=bl") == NULL;   /* bottom-left composites tiny (sim 13 Oct); top-left is pixel exact */
    if ((q = strstr(b, "size=")) && sscanf_s(q + 5, "%f", &v) == 1 && v >= 128 && v <= 1024) c->size = ((int)v) & ~1;
    if ((q = strstr(b, "fov=")) && sscanf_s(q + 4, "%f", &v) == 1 && v >= 1 && v <= 40) c->fov_deg = v;
    if ((q = strstr(b, "dist=")) && sscanf_s(q + 5, "%f", &v) == 1 && v >= 2 && v <= 100) c->dist_m = v / 100.0f;
    c->head = strstr(b, " head") != NULL;
    c->reticle = strstr(b, "noreticle") == NULL;
    c->controller = strstr(b, " controller") != NULL;
    c->round = strstr(b, " square") == NULL;
    c->inscope = strstr(b, " overlay") == NULL;
    if ((q = strstr(b, "radius=")) && sscanf_s(q + 7, "%f", &v) == 1 && v >= 0.3f && v <= 10) c->radius_mm = v * 10;
    if ((q = strstr(b, "front=")) && sscanf_s(q + 6, "%f", &v) == 1 && v >= -100 && v <= 300) c->front_mm = v * 10;
    if ((q = strstr(b, "back=")) && sscanf_s(q + 5, "%f", &v) == 1 && v >= -50 && v <= 300) c->back_mm = v * 10;
    if ((q = strstr(b, "up=")) && sscanf_s(q + 3, "%f", &v) == 1 && v >= -50 && v <= 50) c->up_mm = v * 10;
    c->always = strstr(b, " always") != NULL;
    c->roll_weapon = strstr(b, "roll=head") == NULL; c->roll_x = 0; c->roll_y = -1;   /* sim 16 Oct (M82A2 level, head level): shot row 1 . eye down = -0.999 -> row 1 is up, row 0 is left */
    if ((q = strstr(b, "rollx=")) && sscanf_s(q + 6, "%f", &v) == 1 && v >= -1 && v <= 1) c->roll_x = v;
    if ((q = strstr(b, "rolly=")) && sscanf_s(q + 6, "%f", &v) == 1 && v >= -1 && v <= 1) c->roll_y = v;
    c->dot = strstr(b, "nodot") == NULL; c->dotpic = strstr(b, "dotglass") == NULL; c->dot_fov_deg = 12.0f; c->dot_back_mm = 480; c->dot_up_mm = 70; c->dot_radius_mm = 15;
    if ((q = strstr(b, "dot="))) {   /* dot=<fov deg>,<back cm>,<up cm>,<radius cm>: one token, "fov=" / "up=" must not see it */
        float f[4] = { -999, -999, -999, -999 }; int got = sscanf_s(q + 4, "%f,%f,%f,%f", &f[0], &f[1], &f[2], &f[3]);
        if (got >= 1 && f[0] >= 2 && f[0] <= 60) c->dot_fov_deg = f[0];
        if (got >= 2 && f[1] >= -50 && f[1] <= 300) c->dot_back_mm = f[1] * 10;
        if (got >= 3 && f[2] >= -50 && f[2] <= 50) c->dot_up_mm = f[2] * 10;
        if (got >= 4 && f[3] >= 0.3f && f[3] <= 10) c->dot_radius_mm = f[3] * 10;
    }
    if ((q = strstr(b, "view=")) && sscanf_s(q + 5, "%f", &v) == 1 && v >= 1 && v <= 3) c->view = (int)v;   /* 15 Oct: 2/3 = own world view, coexists with two-eye (view 1) */
    {   /* per-weapon geometry: w<code>=back,up[,radius[,fov]] */
        const char *s2 = b; char *e2;
        while ((s2 = strchr(s2, 'w')) != NULL) {
            long code; float f[4] = { -1e9f, -1e9f, -1e9f, -1e9f }; int got, j;
            ++s2; if (!(*s2 >= '0' && *s2 <= '9')) continue;
            code = strtol(s2, &e2, 0); if (*e2 != '=' || code < 0 || code >= 512) continue;
            got = sscanf_s(e2 + 1, "%f,%f,%f,%f", &f[0], &f[1], &f[2], &f[3]); if (got < 2) continue;
            for (j = 0; j < c->ngeo && c->geo[j].code != code; ++j) {}
            if (j == c->ngeo) { if (c->ngeo >= 16) continue; ++c->ngeo; c->geo[j].code = (int)code; c->geo[j].radius_mm = -1; c->geo[j].fov_deg = -1; }
            if (f[0] >= -50 && f[0] <= 300) c->geo[j].back_mm = f[0] * 10;
            if (f[1] >= -50 && f[1] <= 50) c->geo[j].up_mm = f[1] * 10;
            if (got >= 3 && f[2] >= 0.3f && f[2] <= 10) c->geo[j].radius_mm = f[2] * 10;
            if (got >= 4 && f[3] >= 1 && f[3] <= 40) c->geo[j].fov_deg = f[3];
        }
    }
    if ((q = strstr(b, "weapons="))) {
        unsigned long long w[8] = { 0 }; int any = 0; char *e; long v2; q += 8;
        for (;;) {
            v2 = strtol(q, &e, 0); if (e == q) break;
            if (v2 >= 0 && v2 < 512) { w[v2 >> 6] |= 1ull << (v2 & 63); any = 1; }
            q = e; if (*q != ',') break; ++q;
        }
        if (any) memcpy(c->weapons, w, sizeof(w));
    }
    if ((q = strstr(b, "cam=")) && sscanf_s(q + 4, "%f", &v) == 1 && v >= 0 && v <= 300) c->cam_m = v / 100.0f;
    if ((q = strstr(b, "width=")) && sscanf_s(q + 6, "%f", &v) == 1 && v >= 1 && v <= 40) c->width_m = v / 100.0f;
    return c->wanted;
}
static volatile LONG ln_dot;
void mgs4vr_lens_set_dot(int on) { InterlockedExchange(&ln_dot, !!on); }
int mgs4vr_lens_dot(void) { return InterlockedCompareExchange(&ln_dot, 0, 0) != 0; }
static int (*lens_attach_src)(void);
void mgs4vr_lens_set_attach_source(int (*fn)(void)) { lens_attach_src = fn; }
int mgs4vr_lens_dot_attached(void) { return lens_attach_src && lens_attach_src() == MGS4VR_LENS_DOT_PART; }
int mgs4vr_lens_dot_held(int count, const short codes[8], int current, const unsigned masks[8]) {
    int i; if (!codes || !masks || count < 1 || count > 8 || current <= 0) return 0;
    for (i = 0; i < count; ++i) if ((codes[i] & 0x1ff) == current) return (masks[i] & MGS4VR_LENS_DOT_BIT) != 0;
    return 0;
}
/* Dot sight geometry: ~1x, the disk on the sight housing, always the configured offset (the model's ocular is the scope's). */
void mgs4vr_lens_apply_dot(MGS4VR_LENS_CFG *c) {
    c->fov_deg = c->dot_fov_deg; c->back_mm = c->dot_back_mm; c->up_mm = c->dot_up_mm; c->radius_mm = c->dot_radius_mm; c->geo_set = 1;
}
void mgs4vr_lens_config(MGS4VR_LENS_CFG *c) { AcquireSRWLockShared(&ln_lock); *c = ln_cfg; ReleaseSRWLockShared(&ln_lock); }
void mgs4vr_lens_set_rect(int x, int y, int s) {
    InterlockedExchange(&ln_rect_x, x); InterlockedExchange(&ln_rect_y, y); InterlockedExchange(&ln_rect_s, s);
    InterlockedExchange(&ln_rect_tick, (LONG)GetTickCount()); InterlockedIncrement(&ln_frames);
}
int mgs4vr_lens_rect(int *x, int *y, int *s) {
    if (!ln_rect_s || GetTickCount() - (DWORD)ln_rect_tick > 1000u) return 0;
    *x = ln_rect_x; *y = ln_rect_y; *s = ln_rect_s; return 1;
}
void mgs4vr_lens_pose(const float q[4], const float p[3], float d, float o[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3], f[3];
    f[0] = -(2 * (x * z + w * y)); f[1] = -(2 * (y * z - w * x)); f[2] = -(1 - 2 * (x * x + y * y));   /* -Z axis */
    o[0] = p[0] + f[0] * d; o[1] = p[1] + f[1] * d; o[2] = p[2] + f[2] * d;
}
/* Column-vector rotation matrix of a unit quaternion (x, y, z, w). */
static void ln_rot(const float q[4], float m[9]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - w * z);     m[2] = 2 * (x * z + w * y);
    m[3] = 2 * (x * y + w * z);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - w * x);
    m[6] = 2 * (x * z - w * y);     m[7] = 2 * (y * z + w * x);     m[8] = 1 - 2 * (x * x + y * y);
}
void mgs4vr_lens_rel(const float hq[4], const float hp[3], const float aq[4], const float ap[3], float fwd, float rel[16]) {
    static const float C[3] = { 1, -1, -1 };                       /* OpenXR (x right, y up, z back) -> game camera (x right, y down, z forward) */
    float rh[9], ra[9], r[9], cam[3], d[3], t[3]; int i, j, k;
    ln_rot(hq, rh); ln_rot(aq, ra);
    for (i = 0; i < 3; ++i) cam[i] = ap[i] - ra[i * 3 + 2] * fwd;  /* fwd metres along the aim -Z */
    for (i = 0; i < 3; ++i) d[i] = cam[i] - hp[i];
    for (i = 0; i < 3; ++i) { t[i] = 0; for (k = 0; k < 3; ++k) t[i] += rh[k * 3 + i] * d[k]; }     /* Rh^T d */
    for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) { r[i * 3 + j] = 0; for (k = 0; k < 3; ++k) r[i * 3 + j] += rh[k * 3 + i] * ra[k * 3 + j]; }   /* Rh^T Ra */
    memset(rel, 0, 16 * sizeof(float));
    for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) rel[j * 4 + i] = C[i] * r[i * 3 + j] * C[j];   /* row form: rows = scope axes in head-camera axes */
    for (i = 0; i < 3; ++i) rel[12 + i] = C[i] * t[i] * 1000.0f;
    rel[15] = 1;
}
void mgs4vr_lens_apply_weapon(MGS4VR_LENS_CFG *c, int code) {
    int j;
    for (j = 0; j < c->ngeo; ++j) if (c->geo[j].code == code) {
        c->back_mm = c->geo[j].back_mm; c->up_mm = c->geo[j].up_mm; c->geo_set = 1;
        if (c->geo[j].radius_mm > 0) c->radius_mm = c->geo[j].radius_mm;
        if (c->geo[j].fov_deg > 0) c->fov_deg = c->geo[j].fov_deg;
        return;
    }
}
int mgs4vr_lens_weapon_ok(const MGS4VR_LENS_CFG *c, int code) {
    if (c->always) return 1;
    if (code < 0 || code >= 512) return 0;
    return (c->weapons[code >> 6] >> (code & 63)) & 1;
}
void mgs4vr_lens_note_weapon(int code, int ok) { InterlockedExchange(&ln_weapon, code); InterlockedExchange(&ln_weapon_ok, ok); if (!ok) InterlockedIncrement(&ln_weapon_blocked); }
/* Crosshair: thin lines with a centre gap, thicker posts in the outer third, a centre dot; round vignette from 0.44 to 0.49 of n
   and fully black outside, so the square quad reads as a round lens. Alpha-only black (premultiplied = straight). */
void mgs4vr_lens_reticle(unsigned *px, int n, int round) {
    int x, y; float c = (n - 1) * 0.5f, thin = n / 256.0f, thick = n / 64.0f;
    if (thin < 0.75f) thin = 0.75f;
    for (y = 0; y < n; ++y) for (x = 0; x < n; ++x) {
        float dx = x - c, dy = y - c, r = sqrtf(dx * dx + dy * dy) / n, a = 0, ax = fabsf(dx), ay = fabsf(dy);
        if (r > 0.49f) a = round ? 0.0f : 1.0f; else if (r > 0.44f) a = (r - 0.44f) / 0.05f;
        if (round && r > 0.49f) { px[y * n + x] = 0; continue; }                 /* outside the lens: nothing */
        if ((ay <= thin && ax >= n * 0.03f) || (ax <= thin && ay >= n * 0.03f)) a = 1;   /* thin lines with a centre gap */
        if ((ay <= thick && ax >= n * 0.30f) || (ax <= thick && ay >= n * 0.30f)) a = 1; /* thick posts, outer part */
        if (dx * dx + dy * dy <= (thin * 1.5f) * (thin * 1.5f)) a = 1;               /* centre dot */
        px[y * n + x] = (unsigned)(a * 255.0f + 0.5f) << 24;          /* rgb 0: black */
    }
}
static float ln_norm3(float *v) { float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (!(n > 1e-6f) || !isfinite(n)) return 0; v[0] /= n; v[1] /= n; v[2] /= n; return n; }
static void ln_cross(const float *a, const float *b, float *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
/* Roll of the scope picture, its disk and the red dot (16 Oct 2026, user: "the scope rolls with the head"): the "down" axis
   came from the eye camera, so the crosshair stayed level with the eyes. Now from the weapon: rx * shot row 0 + ry * shot
   row 1, made perpendicular to the barrel (lens tokens roll=head, rollx=, rolly=; default 0,-1 measured in the sim, status roll_*).
   The picture stays world-consistent either way (camera and disk share the axis); only the crosshair and the disk's up
   offset follow the weapon. */
static volatile LONG ln_roll_weapon; static float ln_roll_x, ln_roll_y = 1;
static volatile float ln_roll_cos = 2, ln_roll_r[4];   /* diagnostics: weapon vs eye down (cos), raw rows . eye x/y */
void mgs4vr_lens_set_roll(int weapon, float rx, float ry) { ln_roll_x = rx; ln_roll_y = ry; InterlockedExchange(&ln_roll_weapon, weapon != 0); }
static int ln_down(const float shot[16], const float c2w[16], float yd[3]) {
    float z[3], w[3], e[3], ex[3], d; int i;
    memcpy(yd, c2w + 4, 12); memcpy(e, c2w + 4, 12); memcpy(ex, c2w, 12); memcpy(z, shot + 8, 12);
    if (!ln_norm3(z)) return ln_norm3(yd) > 0;
    {   float r0[3], r1[3]; memcpy(r0, shot, 12); memcpy(r1, shot + 4, 12);
        if (ln_norm3(r0) && ln_norm3(r1) && ln_norm3(e) && ln_norm3(ex)) {
            ln_roll_r[0] = r0[0] * ex[0] + r0[1] * ex[1] + r0[2] * ex[2]; ln_roll_r[1] = r0[0] * e[0] + r0[1] * e[1] + r0[2] * e[2];
            ln_roll_r[2] = r1[0] * ex[0] + r1[1] * ex[1] + r1[2] * ex[2]; ln_roll_r[3] = r1[0] * e[0] + r1[1] * e[1] + r1[2] * e[2]; } }
    memcpy(e, c2w + 4, 12);
    for (i = 0; i < 3; ++i) w[i] = ln_roll_x * shot[i] + ln_roll_y * shot[4 + i];
    if (!ln_norm3(w)) return ln_norm3(yd) > 0;
    d = w[0] * z[0] + w[1] * z[1] + w[2] * z[2]; for (i = 0; i < 3; ++i) w[i] -= d * z[i];
    if (ln_norm3(w) < 0.3f) return ln_norm3(yd) > 0;                       /* the axis lies along the barrel: eye roll */
    d = e[0] * z[0] + e[1] * z[1] + e[2] * z[2]; for (i = 0; i < 3; ++i) e[i] -= d * z[i];
    if (ln_norm3(e) > 0.05f) ln_roll_cos = w[0] * e[0] + w[1] * e[1] + w[2] * e[2];
    if (ln_roll_weapon) memcpy(yd, w, 12);
    return ln_norm3(yd) > 0;
}
int mgs4vr_lens_scope_rel(const float shot[16], const float eye_c2w[16], const float eye_w2c[16], float front, float rel[16]) {
    float z[3], yd[3], x[3], y[3], c2w[16]; int i, r, k;
    for (i = 0; i < 16; ++i) if (!isfinite(shot[i]) || !isfinite(eye_c2w[i]) || !isfinite(eye_w2c[i])) return 0;
    memcpy(z, shot + 8, 12); ln_down(shot, eye_c2w, yd);               /* barrel; "down" axis (weapon roll, or the eye camera's) */
    if (!ln_norm3(z) || !ln_norm3(yd)) return 0;
    ln_cross(yd, z, x); if (ln_norm3(x) < 0.05f) return 0;             /* x = y x z (x right, y down, z forward) */
    ln_cross(z, x, y);
    memset(c2w, 0, sizeof(c2w)); memcpy(c2w, x, 12); memcpy(c2w + 4, y, 12); memcpy(c2w + 8, z, 12);
    for (i = 0; i < 3; ++i) c2w[12 + i] = shot[12 + i] + z[i] * front; c2w[15] = 1;
    for (r = 0; r < 4; ++r) for (i = 0; i < 4; ++i) { double s = 0; for (k = 0; k < 4; ++k) s += (double)c2w[r * 4 + k] * eye_w2c[k * 4 + i]; rel[r * 4 + i] = (float)s; }
    return 1;
}
int mgs4vr_lens_world_quad(const float shot[16], const float w2c[16], const float eq[4], const float ep[3], float back, float up,
                           float oq[4], float op[3]) {
    static const float C[3] = { 1, -1, -1 };
    float z[3], pw[3], pc[3], dc[3], rm[9], px[3], dx[3], X[3], Y[3], Z[3], wu[3] = { 0, 1, 0 }, tr; int i, k;
    for (i = 0; i < 16; ++i) if (!isfinite(shot[i]) || !isfinite(w2c[i])) return 0;
    memcpy(z, shot + 8, 12); if (!ln_norm3(z)) return 0;
    for (i = 0; i < 3; ++i) pw[i] = shot[12 + i] - z[i] * back;          /* along the barrel, then up in eye-camera "up" (= -row 1 of c2w) */
    {   float c2wy[3]; /* camera down axis in world = column 1 of w2c rotation */
        for (i = 0; i < 3; ++i) c2wy[i] = w2c[i * 4 + 1];
        if (!ln_norm3(c2wy)) return 0;
        for (i = 0; i < 3; ++i) pw[i] -= c2wy[i] * up; }
    for (i = 0; i < 3; ++i) { pc[i] = w2c[12 + i]; dc[i] = 0; for (k = 0; k < 3; ++k) { pc[i] += pw[k] * w2c[k * 4 + i]; dc[i] += z[k] * w2c[k * 4 + i]; } }
    ln_rot(eq, rm);
    for (i = 0; i < 3; ++i) { px[i] = 0; dx[i] = 0; for (k = 0; k < 3; ++k) { px[i] += rm[i * 3 + k] * C[k] * pc[k] / 1000.0f; dx[i] += rm[i * 3 + k] * C[k] * dc[k]; } op[i] = ep[i] + px[i]; }
    for (i = 0; i < 3; ++i) Z[i] = -dx[i];                               /* quad front (+Z) faces back along the barrel */
    if (!ln_norm3(Z)) return 0;
    ln_cross(wu, Z, X); if (ln_norm3(X) < 0.05f) return 0; ln_cross(Z, X, Y);
    tr = X[0] + Y[1] + Z[2];                                              /* columns X, Y, Z -> quaternion (x, y, z, w) */
    if (tr > 0) { float s = sqrtf(tr + 1) * 2; oq[3] = 0.25f * s; oq[0] = (Y[2] - Z[1]) / s; oq[1] = (Z[0] - X[2]) / s; oq[2] = (X[1] - Y[0]) / s; }
    else if (X[0] > Y[1] && X[0] > Z[2]) { float s = sqrtf(1 + X[0] - Y[1] - Z[2]) * 2; oq[3] = (Y[2] - Z[1]) / s; oq[0] = 0.25f * s; oq[1] = (Y[0] + X[1]) / s; oq[2] = (Z[0] + X[2]) / s; }
    else if (Y[1] > Z[2]) { float s = sqrtf(1 + Y[1] - X[0] - Z[2]) * 2; oq[3] = (Z[0] - X[2]) / s; oq[0] = (Y[0] + X[1]) / s; oq[1] = 0.25f * s; oq[2] = (Z[1] + Y[2]) / s; }
    else { float s = sqrtf(1 + Z[2] - X[0] - Y[1]) * 2; oq[3] = (X[1] - Y[0]) / s; oq[0] = (Z[0] + X[2]) / s; oq[1] = (Z[1] + Y[2]) / s; oq[2] = 0.25f * s; }
    for (i = 0; i < 4; ++i) if (!isfinite(oq[i])) return 0;
    return 1;
}
int mgs4vr_lens_sight_ok(const float shot[16], const float oc[3], const float ob[3]) {
    float z[3], d[3], a[3], along, off2, dist; int i;
    for (i = 0; i < 3; ++i) if (!isfinite(oc[i]) || !isfinite(ob[i])) return 0;
    for (i = 0; i < 16; ++i) if (!isfinite(shot[i])) return 0;
    memcpy(z, shot + 8, 12); if (!ln_norm3(z)) return 0;
    for (i = 0; i < 3; ++i) { d[i] = oc[i] - shot[12 + i]; a[i] = oc[i] - ob[i]; }
    along = d[0] * z[0] + d[1] * z[1] + d[2] * z[2];
    off2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along;
    dist = ln_norm3(a);
    if (!(along < -100) || off2 < 20.0f * 20.0f || off2 > 150.0f * 150.0f || dist < 100 || dist > 600) return 0;
    return -(a[0] * z[0] + a[1] * z[1] + a[2] * z[2]) > 0.985f;          /* ocular behind the objective, axis ~ barrel */
}
int mgs4vr_lens_sight_clip(const float oc[3], const float ob[3], const float c2w[16], const float vp[16], float rad, float clip[16], const float roll[16]) {
    float shot[16] = { 0 }; int i;
    if (roll) memcpy(shot, roll, 32);                                                          /* the weapon's roll rows (16 Oct) */
    for (i = 0; i < 3; ++i) { shot[8 + i] = ob[i] - oc[i]; shot[12 + i] = oc[i]; } shot[15] = 1;   /* "barrel" = scope axis, "muzzle" = ocular */
    return mgs4vr_lens_ocular_clip(shot, c2w, vp, 0, 0, rad, clip);
}
int mgs4vr_lens_ocular_clip(const float shot[16], const float c2w[16], const float vp[16], float back, float up, float rad, float clip[16]) {
    float z[3], yd[3], x[3], y[3], c[3]; int i, k, j;
    static const float sx[4] = { -1, 1, -1, 1 }, sy[4] = { -1, -1, 1, 1 };    /* uv 00, 10, 01, 11: -y = up (camera y is down) */
    for (i = 0; i < 16; ++i) if (!isfinite(shot[i]) || !isfinite(c2w[i]) || !isfinite(vp[i])) return 0;
    memcpy(z, shot + 8, 12); ln_down(shot, c2w, yd);
    if (!ln_norm3(z) || !ln_norm3(yd)) return 0;
    ln_cross(yd, z, x); if (ln_norm3(x) < 0.05f) return 0; ln_cross(z, x, y);
    for (i = 0; i < 3; ++i) c[i] = shot[12 + i] - z[i] * back - yd[i] * up;
    for (k = 0; k < 4; ++k) {
        float p[4]; for (i = 0; i < 3; ++i) p[i] = c[i] + rad * (sx[k] * x[i] + sy[k] * y[i]); p[3] = 1;
        for (j = 0; j < 4; ++j) { clip[k * 4 + j] = 0; for (i = 0; i < 4; ++i) clip[k * 4 + j] += p[i] * vp[i * 4 + j]; }
        if (!(clip[k * 4 + 3] > 1.0f) || !isfinite(clip[k * 4 + 0]) || !isfinite(clip[k * 4 + 1])) return 0;   /* behind / at the eye */
    }
    return 1;
}
/* Red dot on the sight glass (16 Oct 2026): the glass = the ocular disk (centre c, axes x/y, half size rad, as
   mgs4vr_lens_ocular_clip). The dot is the barrel's point at infinity: the eye ray parallel to the barrel z hits the glass at
   c + ((E-c).x) x + ((E-c).y) y, E = the eye camera position. uv (0.5, 0.5) = eye on the sight line; outside 0..1 = no dot. */
int mgs4vr_lens_dot_uv(const float shot[16], const float c2w[16], float back, float up, float rad, float uv[2]) {
    float z[3], yd[3], x[3], y[3], d[3]; int i;
    for (i = 0; i < 16; ++i) if (!isfinite(shot[i]) || !isfinite(c2w[i])) return 0;
    if (!(rad > 0)) return 0;
    memcpy(z, shot + 8, 12); ln_down(shot, c2w, yd);
    if (!ln_norm3(z) || !ln_norm3(yd)) return 0;
    ln_cross(yd, z, x); if (ln_norm3(x) < 0.05f) return 0; ln_cross(z, x, y);
    for (i = 0; i < 3; ++i) d[i] = c2w[12 + i] - (shot[12 + i] - z[i] * back - yd[i] * up);
    uv[0] = 0.5f + (d[0] * x[0] + d[1] * x[1] + d[2] * x[2]) / (2 * rad);
    uv[1] = 0.5f + (d[0] * y[0] + d[1] * y[1] + d[2] * y[2]) / (2 * rad);
    return isfinite(uv[0]) && isfinite(uv[1]);
}
/* 1x for the dot sight: the lens frustum's focal scale k = 1/tan(half angle) of the glass disk seen from the eye =
   eye-to-glass distance / radius, clamped to 1..60 deg full angle. 0 = no estimate. */
float mgs4vr_lens_dot_k(const float shot[16], const float c2w[16], float back, float up, float rad) {
    float z[3], yd[3], d[3], dist, k; int i;
    for (i = 0; i < 16; ++i) if (!isfinite(shot[i]) || !isfinite(c2w[i])) return 0;
    if (!(rad > 0)) return 0;
    memcpy(z, shot + 8, 12); ln_down(shot, c2w, yd);
    if (!ln_norm3(z) || !ln_norm3(yd)) return 0;
    for (i = 0; i < 3; ++i) d[i] = c2w[12 + i] - (shot[12 + i] - z[i] * back - yd[i] * up);
    dist = ln_norm3(d); k = dist / rad;
    if (!isfinite(k)) return 0;
    if (k < 1.0f / tanf(30.0f * 0.017453293f)) k = 1.0f / tanf(30.0f * 0.017453293f);
    if (k > 1.0f / tanf(0.5f * 0.017453293f)) k = 1.0f / tanf(0.5f * 0.017453293f);
    return k;
}
void mgs4vr_lens_set_ocular(const float clip[16], unsigned serial, int eye) { mgs4vr_lens_set_ocular_dot(clip, NULL, serial, eye); }
void mgs4vr_lens_set_ocular_dot(const float clip[16], const float dot[2], unsigned serial, int eye) {
    AcquireSRWLockExclusive(&ln_lock);
    memcpy(ln_oc[ln_oc_next].clip, clip, 64); ln_oc[ln_oc_next].dot[0] = dot ? dot[0] : 0.5f; ln_oc[ln_oc_next].dot[1] = dot ? dot[1] : 0.5f; ln_oc[ln_oc_next].serial = serial; ln_oc[ln_oc_next].eye = eye; ln_oc[ln_oc_next].tick = GetTickCount(); ln_oc_next = (ln_oc_next + 1) & 3;
    ReleaseSRWLockExclusive(&ln_lock); InterlockedIncrement(&ln_oc_set);
}
int mgs4vr_lens_ocular(float clip[16], unsigned serial, int eye) { return mgs4vr_lens_ocular_dot(clip, NULL, serial, eye); }
int mgs4vr_lens_ocular_dot(float clip[16], float dot[2], unsigned serial, int eye) {
    int i, best = -1; DWORD now = GetTickCount(), bt = 0;
    AcquireSRWLockShared(&ln_lock);
    for (i = 0; i < 4; ++i) if (ln_oc[i].tick && now - ln_oc[i].tick < 200u && ln_oc[i].serial == serial && ln_oc[i].eye == eye) { best = i; break; }
    if (best >= 0) InterlockedIncrement(&ln_oc_exact);
    else for (i = 0; i < 4; ++i) if (ln_oc[i].tick && now - ln_oc[i].tick < 200u && ln_oc[i].eye == eye && (best < 0 || (LONG)(ln_oc[i].tick - bt) > 0)) { best = i; bt = ln_oc[i].tick; }
    if (best >= 0) { memcpy(clip, ln_oc[best].clip, 64); if (dot) { dot[0] = ln_oc[best].dot[0]; dot[1] = ln_oc[best].dot[1]; } }
    ReleaseSRWLockShared(&ln_lock);
    if (best < 0) InterlockedIncrement(&ln_oc_miss); else if (ln_oc[best].serial != serial) InterlockedIncrement(&ln_oc_eye);
    return best >= 0;
}
void mgs4vr_lens_set_world_quad(const float q[4], const float p[3]) {
    AcquireSRWLockExclusive(&ln_lock); memcpy(ln_wq, q, 16); memcpy(ln_wp, p, 12); ln_wq_tick = GetTickCount(); ReleaseSRWLockExclusive(&ln_lock);
    InterlockedIncrement(&ln_rifle_frames);
}
int mgs4vr_lens_barrel(float rel[16]) {
    int ok;
    AcquireSRWLockShared(&ln_lock);
    ok = ln_cfg.wanted && !ln_cfg.head && ln_rel_tick && GetTickCount() - ln_rel_tick < 200u;
    if (ok) memcpy(rel, ln_rel, sizeof(ln_rel));
    ReleaseSRWLockShared(&ln_lock);
    if (ok) InterlockedIncrement(&ln_barrels);
    return ok;
}
void mgs4vr_lens_hands(const MGS4VR_XR_HANDS *h) {
    MGS4VR_LENS_CFG c; int i;
    if (!h || !h->hand[1].active) return;
    for (i = 0; i < 4; ++i) if (!isfinite(h->hand[1].aim_quat[i])) return;
    for (i = 0; i < 3; ++i) if (!isfinite(h->hand[1].grip_pos[i])) return;
    mgs4vr_lens_config(&c);
    memcpy(ln_q, h->hand[1].aim_quat, 16);                         /* quad front (+Z) faces back along the aim ray: towards the eye */
    mgs4vr_lens_pose(h->hand[1].aim_quat, h->hand[1].grip_pos, c.dist_m, ln_p);
    ln_have = 1; ln_pose_tick = GetTickCount();
    if (h->head_valid) {
        float rel[16]; int ok = 1;
        for (i = 0; i < 4; ++i) if (!isfinite(h->head_quat[i])) ok = 0;
        for (i = 0; i < 3; ++i) if (!isfinite(h->head_pos[i]) || !isfinite(h->hand[1].aim_pos[i])) ok = 0;
        if (ok) {
            mgs4vr_lens_rel(h->head_quat, h->head_pos, h->hand[1].aim_quat, h->hand[1].aim_pos, c.cam_m, rel);
            AcquireSRWLockExclusive(&ln_lock); memcpy(ln_rel, rel, sizeof(rel)); ln_rel_tick = GetTickCount(); ReleaseSRWLockExclusive(&ln_lock);
        }
    }
}
int mgs4vr_lens_quad(float q[4], float p[3], float *width) {
    MGS4VR_LENS_CFG c; int x, y, s;
    mgs4vr_lens_config(&c);
    if (!c.wanted || !ln_have || GetTickCount() - ln_pose_tick > 200u || !mgs4vr_lens_rect(&x, &y, &s)) return 0;
    AcquireSRWLockShared(&ln_lock);
    if (!c.controller && ln_wq_tick && GetTickCount() - ln_wq_tick < 200u) { memcpy(q, ln_wq, 16); memcpy(p, ln_wp, 12); ReleaseSRWLockShared(&ln_lock); InterlockedIncrement(&ln_world_quads); }
    else { ReleaseSRWLockShared(&ln_lock); memcpy(q, ln_q, 16); memcpy(p, ln_p, 12); }
    *width = c.width_m; InterlockedIncrement(&ln_quads); return 1;
}
void mgs4vr_lens_poll(const char *exe) {
    DWORD now = GetTickCount(); static char b[2048]; memset(b, 0, sizeof(b)); FILE *f; MGS4VR_LENS_CFG c; char *leaf;
    if (!ln_mark[0]) {
        if (!exe) return;
        strcpy_s(ln_mark, sizeof(ln_mark), exe); leaf = strrchr(ln_mark, '\\'); if (!leaf) { ln_mark[0] = 0; return; }
        strcpy_s(leaf + 1, sizeof(ln_mark) - (size_t)(leaf + 1 - ln_mark), "mgs4vr_lens.on");
        strcpy_s(ln_status, sizeof(ln_status), ln_mark);
        sprintf_s(strrchr(ln_status, '\\') + 1, 64, "mgs4vr_lens_%lu.txt", GetCurrentProcessId());
    }
    if (ln_tick && now - ln_tick < 500) return;
    ln_tick = now;
    if (!fopen_s(&f, ln_mark, "rb")) { size_t n = fread(b, 1, sizeof(b) - 1, f); fclose(f); b[n] = 0; }
    mgs4vr_lens_parse(b, &c);
    AcquireSRWLockExclusive(&ln_lock); ln_cfg = c; ReleaseSRWLockExclusive(&ln_lock); mgs4vr_lens_set_roll(c.roll_weapon, c.roll_x, c.roll_y);
    if (c.wanted || ln_frames) {
        if (!fopen_s(&f, ln_status, "w")) {
            fprintf(f, "lens_version=4 wanted=%d size=%d fov=%.1f dist_cm=%.0f width_cm=%.0f cam_cm=%.0f front_cm=%.0f back_cm=%.0f up_cm=%.0f head=%d controller=%d reticle=%d always=%d weapon=%ld weapon_ok=%ld weapon_blocked=%ld frames=%ld rifle_frames=%ld quads=%ld world_quads=%ld barrels=%ld rect=%ld,%ld,%ld inscope=%d radius_cm=%.1f ocular_set=%ld ocular_exact=%ld ocular_eye=%ld ocular_miss=%ld roll_weapon=%ld roll_xy=%.2f,%.2f roll_cos=%.3f roll_rows=%.2f,%.2f,%.2f,%.2f dot=%d dotpic=%d dot_on=%d dot_fov=%.1f dot_back_cm=%.1f dot_up_cm=%.1f dot_radius_cm=%.1f dot_uv=%.3f,%.3f\n",
                    c.wanted, c.size, c.fov_deg, c.dist_m * 100, c.width_m * 100, c.cam_m * 100, c.front_mm / 10, c.back_mm / 10, c.up_mm / 10, c.head, c.controller, c.reticle, c.always, ln_weapon, ln_weapon_ok, ln_weapon_blocked, ln_frames, ln_rifle_frames, ln_quads, ln_world_quads, ln_barrels, ln_rect_x, ln_rect_y, ln_rect_s, c.inscope, c.radius_mm / 10, ln_oc_set, ln_oc_exact, ln_oc_eye, ln_oc_miss, ln_roll_weapon, ln_roll_x, ln_roll_y, ln_roll_cos, ln_roll_r[0], ln_roll_r[1], ln_roll_r[2], ln_roll_r[3], c.dot, c.dotpic, mgs4vr_lens_dot(), c.dot_fov_deg, c.dot_back_mm / 10, c.dot_up_mm / 10, c.dot_radius_mm / 10, ln_oc[(ln_oc_next + 3) & 3].dot[0], ln_oc[(ln_oc_next + 3) & 3].dot[1]);
            fclose(f);
        }
    }
}
