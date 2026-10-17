/* mgs4vr_armsrc.c - see mgs4vr_armsrc.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mgs4vr_armsrc.h"
#include "mgs4vr_profile.h"
#include "mgs4vr_poses.h"

#define BONES 115
#define ARM_ROOT 10                    /* right upper arm */
#define ELBOW 11
#define HAND 12
#define HEAD_BONE 79                   /* highest bone; FPS camera sits within ~15 cm (run 13520) */
#define CALL_RVA 0x9737b6              /* call 0xd2e920 (Snake's skeleton solve) */
#define FN_RVA 0xd2e920
#define L_CLAV 5                       /* left clavicle: rotated forward when the hand is out of reach */
#define L_ROOT 6                       /* left upper arm (rig: 8<-7<-6<-5<-2, mirror of 12<-11<-10<-9<-2) */
#define L_ELBOW 7
#define L_HAND 8                       /* SKL_013_LHAND */
#define IK_CODE 5000                   /* right IK + left: controller, snapping onto the foregrip when close */
#define IK_GRIP 5001                   /* right IK + left always on the foregrip */
#define IK_FREE 5002                   /* right IK + left always on the left controller */
#define IK_RIGHT 5003                  /* right IK only (left = game) */
#define IS_IK(c) ((c) >= IK_CODE && (c) <= IK_RIGHT)
#define SNAP_ENTER 100.0f              /* mm: controller hand within this of the foregrip grabs the gun ... */
#define SNAP_EXIT 150.0f               /* ... and lets go only beyond this (hysteresis) */
#define GRAB_MS 180                    /* hand -> grip transition (16 Oct 2026: was 100 ms linear, "jarring" in the headset) */
#define RELEASE_MS 220                 /* grip -> hand transition */
#define PROTRACT_MAX_DEG 30.0f         /* clavicle forward rotation limit (160 mm clavicle: ~80 mm extra reach) */
#define POLE_OUT 0.4f                  /* left elbow: down plus this much outward (away from the right shoulder) */
#define GRIP_MAX 700.0f                /* animation left hand farther than this from the right hand: no foregrip */
#define FPS_RADIUS 400.0f
#define STALE_MS 100u
typedef void (__fastcall *NATIVE)(void *);

static const BYTE call_sig[] = { 0xe8, 0x65, 0xb1, 0x3b, 0x00 };
static const BYTE fn_sig[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x41, 0x56 };
static const BYTE accessor_sig[] = { 0x8b, 0x05, 0xca, 0xaf, 0x5d, 0x23, 0x48, 0x8d, 0x0d, 0xd3, 0xaf, 0x5d, 0x23, 0xff, 0xc0, 0x48, 0x98, 0x48, 0xc1, 0xe0, 0x07, 0x48, 0x03, 0xc1, 0xc3 };
static const unsigned rig_ids[] = { 0xa89233, 0x7011c4, 0x6c02b2, 0xf8d3cc, 0xf5d387, 0x019543, 0x027a4c, 0xfafa8b, 0xfbfa42, 0x619d43, 0x62824c, 0x5b028c, 0x5c0243 };
static const int rig_parents[] = { -1, 0, 1, 2, 3, 2, 5, 6, 7, 2, 9, 10, 11 };
/* Grip calibration, right hand:
   hand rotation = GRIP_O * controller rotation (row vectors: a fixed offset IN THE CONTROLLER FRAME),
   hand position = GRIP_P * controller rotation + controller position.
   Build 08C21A82 applied O in the VIEW frame (controller rotation * O): identical in the calibration
   pose, wrong about any other axis as soon as the wrist turns. Refitted on the same rifle frames. */
static const float GRIP_O[9] = { 0.3158f, -0.9460f, 0.0726f, 0.9421f, 0.3217f, 0.0941f, -0.1124f, 0.0387f, 0.9929f };
static const float GRIP_P[3] = { -80.6f, -35.9f, -2.9f };
/* Left hand, same model, fitted on the same rifle frames against the animation left hand (which sits on the
   foregrip): rotation spread median 3.3 deg. In that stance controller hand == foregrip, so snapping is seamless. */
static const float GRIP_O_L[9] = { 0.1297f, 0.9752f, 0.1796f, -0.3469f, -0.1250f, 0.9295f, 0.9289f, -0.1829f, 0.3221f };
static const float GRIP_P_L[3] = { -191.5f, -73.8f, 18.7f };
#include "mgs4vr_weapon_profiles.inl"
#include "mgs4vr_weapon_identity.h"
static int wp_game_read(void *u,uint64_t address,void *out,unsigned n){
    (void)u;__try {memcpy(out,(const void *)(uintptr_t)address,n);return 1;}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}

enum { R_NOT_PLAYER, R_RIG, R_FINITE, R_FAULT, R_NOT_FPS, R_STALE, R_COUNT };
static volatile LONG calls, player_calls, writes, bones_written, bones_kept, rejects[R_COUNT], active, installed, traps, mode;  /* 0 off, IK_CODE, else axis*1000 + deg + 360 */
static volatile LONG ik_clamped, hand_samples, anchor_used, anchor_fallback, anchor_samples;
static volatile LONG stale_noread, stale_invalid, stale_old, age_hist[5];   /* 16 Oct: split of reject_stale; hand sample age <16 <33 <66 <=100 >100 ms */
static volatile LONG left_writes, left_grip_frames, left_clamped, left_no_ctrl, left_no_grip, snap_enters, snap_exits, left_protract;
static BYTE *image, *site; static NATIVE native; static void *veh;
static int attempted, integrity_lost, restored, subtree_n, subtree_l;
static char marker[MAX_PATH], status_path[MAX_PATH];
static DWORD poll_at, status_at, started;
static const BYTE *mask_meta; static BYTE mask[BONES], mask_l[BONES], mask_rc[BONES]; static int parent_of[BONES];
static float shoulder_v[3];   /* game thread: this frame's shoulder shift (apply -> left_arm) */
static float last_out[BONES][16]; static BYTE have_last[BONES]; static const float *last_input;
/* Left snap state, game thread only. blend: 0 = controller, 1 = foregrip. */
static int snapped, blend_init; static float blend; static DWORD blend_tick;
/* blend stays linear in time; the poses use an ease-in-out weight so hand and weapon start and stop softly.
   Marker tokens: ik grab=<ms> release=<ms> (0..2000). */
static volatile LONG grab_ms = GRAB_MS, release_ms = RELEASE_MS;
/* Shoulder offset (16 Oct 2026, user: "Snake's shoulders clip through the camera in first person"). The shoulder pads are
   in the ARM mesh part (part mask bit 0x20 hides arm and shoulder together; no other bit touches them), so they cannot be
   hidden alone. Instead, with the IK on, the upper body from the chest (2) - without neck/head (4, hidden in first person)
   - moves BACK along the body's forward axis (root row 2) by shoulder_back_mm and DOWN by half of it, before the arms are
   solved; the left clavicle subtree only when the left arm is solved. Hands stay on the controllers, the pads leave the
   camera (sim: camera ~7 cm in front of and ~11 cm above the shoulder line). Moving only the clavicle subtrees stretched the
   pad into a sheet (it is skinned to the chest too): the stretch now sits at the waist, out of view.
   Token shoulderback=<cm> (0..SHOULDER_MAX_CM, 0 = off) in mgs4vr_armsrc.on, VR Settings "Shoulder Offset". */
#define SHOULDER_MAX_CM 15
#define CHEST 2
#define NECK 4
static volatile LONG shoulder_back_mm, shoulder_shifts;
void mgs4vr_armsrc_shoulder_vector(const float root[16], float back_mm, float v[3]) {
    float fx = root[8], fz = root[10], l = sqrtf(fx * fx + fz * fz);
    if (!(back_mm > 0) || l < 1e-4f) { v[0] = v[1] = v[2] = 0; return; }
    v[0] = -fx / l * back_mm; v[1] = -0.5f * back_mm; v[2] = -fz / l * back_mm;
}
static void shift_bones(float (*in)[16], const BYTE *m, const float v[3]) {
    int i; for (i = 0; i < BONES; ++i) if (m[i]) { in[i][12] += v[0]; in[i][13] += v[1]; in[i][14] += v[2]; }
}
static float blend_ease(float b) { if (b <= 0) return 0; if (b >= 1) return 1; return b * b * (3 - 2 * b); }
static void blend_advance(float want, DWORD now) {
    float dur = (float)(want > blend ? grab_ms : release_ms), step = dur > 0 ? (float)(now - blend_tick) / dur : 1.f;
    if (step > 1) step = 1; if (step < 0) step = 0;
    blend = want > blend ? fminf(want, blend + step) : fmaxf(want, blend - step);
}
/* Latest controllers and head, raw OpenXR LOCAL space (seqlock: XR thread writes, game thread reads). */
typedef struct { float grip_q[4], grip_p[3], head_q[4], head_p[3], lgrip_q[4], lgrip_p[3], lsqueeze; int64_t time; DWORD tick; int valid, lvalid; } HAND_VIEW;   /* time = XR display time */
static HAND_VIEW hv; static volatile LONG hv_seq;
#define HV_RING 16                                                   /* hand samples by display time (eye packets keep 16 too) */
static HAND_VIEW hv_ring[HV_RING]; static volatile LONG hv_ring_seq[HV_RING]; static volatile LONG hv_ring_next;
#define HOLD_MS 300u                                                 /* a stale sample: hold the last good one this long */
enum { PICK_NONE, PICK_PAIRED, PICK_LATEST, PICK_HOLD };
static HAND_VIEW good; static int have_good;                         /* game thread */
static volatile LONG hand_paired, hand_unpaired, hand_held;
/* Pure: which hand sample drives this frame. paired = the sample with the camera packet's display time (preferred),
   latest = the newest sample, good = the last sample that drove an arm. Usable = valid and at most STALE_MS old;
   otherwise good is held up to HOLD_MS. */
int mgs4vr_armsrc_pick_hand(const HAND_VIEW *latest, const HAND_VIEW *paired, const HAND_VIEW *good_view, DWORD now, HAND_VIEW *out) {
    if (paired && paired->valid && now - paired->tick <= STALE_MS) { *out = *paired; return PICK_PAIRED; }
    if (latest && latest->valid && now - latest->tick <= STALE_MS) { *out = *latest; return PICK_LATEST; }
    if (good_view && good_view->valid && now - good_view->tick <= HOLD_MS) { *out = *good_view; return PICK_HOLD; }
    return PICK_NONE;
}
/* Latest built stereo camera and the eye pose it was built from (seqlock). */
typedef struct { float eye_q[4], eye_p[3], C[16]; int64_t time; DWORD tick; int valid; } EYE_ANCHOR;   /* time = the eye packet's display time */
static EYE_ANCHOR ea; static volatile LONG ea_seq;
static DWORD (*left_clock)(void);             /* desk tests drive the snap blend; NULL = GetTickCount */

/* ---------------------------------------------------------------- math --- */
static void mul(const float a[16], const float b[16], float o[16]) {
    float t[16]; int r, c, k;
    for (r = 0; r < 4; ++r) for (c = 0; c < 4; ++c) { float s = 0; for (k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c]; t[r * 4 + c] = s; }
    memcpy(o, t, sizeof(t));
}
static void mul3(const float a[9], const float b[9], float o[9]) {
    float t[9]; int r, c, k;
    for (r = 0; r < 3; ++r) for (c = 0; c < 3; ++c) { float s = 0; for (k = 0; k < 3; ++k) s += a[r * 3 + k] * b[k * 3 + c]; t[r * 3 + c] = s; }
    memcpy(o, t, sizeof(t));
}
static void rigid_inverse(const float m[16], float o[16]) {
    int r, c;
    for (r = 0; r < 3; ++r) for (c = 0; c < 3; ++c) o[r * 4 + c] = m[c * 4 + r];
    o[3] = o[7] = o[11] = 0; o[15] = 1;
    for (c = 0; c < 3; ++c) o[12 + c] = -(m[12] * o[0 * 4 + c] + m[13] * o[1 * 4 + c] + m[14] * o[2 * 4 + c]);
}
/* 4x4 row-vector transform: rotation R (3x3 row form) about point P. */
static void about(const float R[9], const float P[3], float o[16]) {
    int i, j;
    memset(o, 0, 16 * sizeof(float));
    for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) o[i * 4 + j] = R[i * 3 + j];
    o[15] = 1;
    for (j = 0; j < 3; ++j) o[12 + j] = P[j] - (P[0] * R[j] + P[1] * R[3 + j] + P[2] * R[6 + j]);
}
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float len3(const float a[3]) { return sqrtf(dot3(a, a)); }
static void cross3(const float a[3], const float b[3], float o[3]) { float t[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; memcpy(o, t, sizeof(t)); }
/* Row-vector rotation R with u R = v (u, v unit). */
static void rot_between(const float u[3], const float v[3], float R[9]) {
    float k[3], s, c = dot3(u, v), K[9], K2[9]; int i;
    cross3(u, v, k); s = len3(k);
    if (s < 1e-6f) {
        if (c > 0) { static const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; memcpy(R, I, sizeof(I)); return; }
        /* opposite: 180 degrees about any axis perpendicular to u */
        { float a[3] = { 1, 0, 0 }; if (fabsf(u[0]) > 0.9f) { a[0] = 0; a[1] = 1; } cross3(u, a, k); s = len3(k); c = -1; }
        for (i = 0; i < 3; ++i) k[i] /= s; s = 0;
    } else for (i = 0; i < 3; ++i) k[i] /= s;
    /* column form Rc = I + s K + (1 - c) K^2 with K = [k]x; row form is its transpose */
    K[0] = 0; K[1] = -k[2]; K[2] = k[1]; K[3] = k[2]; K[4] = 0; K[5] = -k[0]; K[6] = -k[1]; K[7] = k[0]; K[8] = 0;
    mul3(K, K, K2);
    for (i = 0; i < 9; ++i) { int r = i / 3, cc = i % 3; float col = (r == cc ? 1.f : 0.f) + s * K[i] + (1 - c) * K2[i]; R[cc * 3 + r] = col; }
}
/* World rotation G of `deg` about model axis `axis` through the world point s: G = inv(Root) * H * Root. */
static void world_rotation(const float root[16], const float s_w[3], int axis, float deg, float g[16]) {
    float inv[16], s[3], rm[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, h[16], c = cosf(deg * 0.017453292f), n = sinf(deg * 0.017453292f);
    int j, a = (axis + 1) % 3, b = (axis + 2) % 3;
    rigid_inverse(root, inv);
    for (j = 0; j < 3; ++j) s[j] = s_w[0] * inv[j] + s_w[1] * inv[4 + j] + s_w[2] * inv[8 + j] + inv[12 + j];
    rm[a * 3 + a] = c; rm[a * 3 + b] = n; rm[b * 3 + a] = -n; rm[b * 3 + b] = c;
    about(rm, s, h);
    mul(inv, h, g); mul(g, root, g);
}
static int finite16(const float *m) { int i; for (i = 0; i < 16; ++i) if (!isfinite(m[i])) return 0; return 1; }

/* ------------------------------------------------------ controller input --- */
static void quat_col(const float q[4], float m[9]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w); m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w); m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w); m[7] = 2 * (y * z + x * w); m[8] = 1 - 2 * (x * x + y * y);
}
/* Grip relative to the head, converted to game view space (row-vector rotation, mm). */
void mgs4vr_armsrc_view_from_xr(const float head_q[4], const float head_p[3], const float grip_q[4], const float grip_p[3], float R_row[9], float p_view[3]) {
    float Rh[9], Rc[9], rel[9], d[3], rp[3]; int i, j, k; static const float B[3] = { 1, -1, -1 };
    quat_col(head_q, Rh); quat_col(grip_q, Rc);
    for (i = 0; i < 3; ++i) d[i] = grip_p[i] - head_p[i];
    for (i = 0; i < 3; ++i) rp[i] = Rh[0 * 3 + i] * d[0] + Rh[1 * 3 + i] * d[1] + Rh[2 * 3 + i] * d[2];       /* Rh^T d */
    for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) { float s = 0; for (k = 0; k < 3; ++k) s += Rh[k * 3 + i] * Rc[k * 3 + j]; rel[i * 3 + j] = s; }   /* Rh^T Rc */
    for (i = 0; i < 3; ++i) p_view[i] = B[i] * rp[i] * 1000.0f;
    for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) R_row[j * 3 + i] = B[i] * rel[i * 3 + j] * B[j];                 /* (B rel B)^T */
}
static int valid_pose(const float q[4], const float p[3]) {
    float norm = 0; int i;
    for (i = 0; i < 4; ++i) { if (!isfinite(q[i])) return 0; norm += q[i] * q[i]; }
    for (i = 0; i < 3; ++i) if (!isfinite(p[i])) return 0;
    return norm > .99f && norm < 1.01f;
}
void mgs4vr_armsrc_hands(const MGS4VR_XR_HANDS *h) {
    HAND_VIEW v; const MGS4VR_XR_HAND *r, *l;
    if (!h) return;
    r = &h->hand[1]; l = &h->hand[0];
    memset(&v, 0, sizeof(v));
    v.valid = h->head_valid && r->active && (r->grip_flags & 3u) == 3u &&
              valid_pose(h->head_quat, h->head_pos) && valid_pose(r->grip_quat, r->grip_pos);
    if (v.valid) { memcpy(v.grip_q, r->grip_quat, 16); memcpy(v.grip_p, r->grip_pos, 12); memcpy(v.head_q, h->head_quat, 16); memcpy(v.head_p, h->head_pos, 12); }
    v.lvalid = v.valid && l->active && (l->grip_flags & 3u) == 3u && valid_pose(l->grip_quat, l->grip_pos);
    if (v.lvalid) { memcpy(v.lgrip_q, l->grip_quat, 16); memcpy(v.lgrip_p, l->grip_pos, 12); v.lsqueeze = l->squeeze; }
    v.tick = GetTickCount(); v.time = h->display_time;
    InterlockedIncrement(&hv_seq); MemoryBarrier();
    hv = v;
    MemoryBarrier(); InterlockedIncrement(&hv_seq);
    {   LONG slot = hv_ring_next & (HV_RING - 1);                    /* XR thread is the only writer */
        InterlockedIncrement(&hv_ring_seq[slot]); MemoryBarrier(); hv_ring[slot] = v; MemoryBarrier(); InterlockedIncrement(&hv_ring_seq[slot]);
        InterlockedIncrement(&hv_ring_next); }
    InterlockedIncrement(&hand_samples);
}
void mgs4vr_armsrc_eye_anchor(const float eye_q[4], const float eye_p[3], const float c2w[16], int64_t display_time) {
    EYE_ANCHOR a; int i, ok = eye_q && eye_p && c2w && finite16(c2w);
    memset(&a, 0, sizeof(a));
    for (i = 0; ok && i < 4; ++i) ok = isfinite(eye_q[i]);
    for (i = 0; ok && i < 3; ++i) ok = isfinite(eye_p[i]);
    if (ok) { memcpy(a.eye_q, eye_q, 16); memcpy(a.eye_p, eye_p, 12); memcpy(a.C, c2w, 64); a.valid = 1; }
    a.tick = GetTickCount(); a.time = display_time;
    InterlockedIncrement(&ea_seq); MemoryBarrier();
    ea = a;
    MemoryBarrier(); InterlockedIncrement(&ea_seq);
    InterlockedIncrement(&anchor_samples);
}
#define SEQ_READ(seq, src, out) do { int tries_; LONG s1_, s2_; ok_ = 0; \
    for (tries_ = 0; tries_ < 8 && !ok_; ++tries_) { s1_ = seq; MemoryBarrier(); if (s1_ & 1) { YieldProcessor(); continue; } \
        *(out) = src; MemoryBarrier(); s2_ = seq; ok_ = s1_ == s2_; } } while (0)
static int read_hand(HAND_VIEW *out) { int ok_; SEQ_READ(hv_seq, hv, out); return ok_; }
static int read_anchor(EYE_ANCHOR *out) { int ok_; SEQ_READ(ea_seq, ea, out); return ok_; }
static int find_hand(int64_t time, HAND_VIEW *out) {   /* the ring sample with this display time */
    int i; for (i = 0; i < HV_RING; ++i) { HAND_VIEW t; int ok_; SEQ_READ(hv_ring_seq[i], hv_ring[i], &t); if (ok_ && t.time == time) { *out = t; return 1; } }
    return 0;
}

/* -------------------------------------------------------------- player --- */
static BYTE *owner_model(void) {
    int slot = *(int *)(image + 0x23eb5150); BYTE *w;
    if (slot < -1 || slot > 23) return NULL;
    w = *(BYTE **)(image + 0x23eb5160 + (slot + 1) * 0x80 + 0x10);
    return w ? *(BYTE **)w : NULL;
}
static int subtree(BYTE *m, int root) {
    int i, n = 1; memset(m, 0, BONES); m[root] = 1;
    for (i = root + 1; i < BONES; ++i) { int p = parent_of[i]; if (p >= 0 && p < i && m[p]) { m[i] = 1; ++n; } }
    return n;
}
static void build_mask(const BYTE *meta) {
    int i;
    for (i = 0; i < BONES; ++i) parent_of[i] = *(const int *)(meta + i * 80 + 8);
    subtree_n = subtree(mask, ARM_ROOT); subtree_l = subtree(mask_l, L_CLAV);
    {   /* shoulder shift set (sim 16 Oct: moving only the clavicle subtrees stretched the shoulder pad, which is also skinned
           to the chest): the whole upper body from the chest (2) without the neck/head (4, hidden in first person) and without
           the left arm (moved in left_arm only when it is solved) */
        BYTE neck[BONES]; int i; subtree(mask_rc, CHEST); subtree(neck, NECK);
        for (i = 0; i < BONES; ++i) if (neck[i] || mask_l[i]) mask_rc[i] = 0; }
    mask_meta = meta;
}
/* Rotation <-> quaternion (same convention both ways, so any matrix layout round-trips). */
static void mat_quat(const float m[9], float q[4]) {
    float t = m[0] + m[4] + m[8], s;
    if (t > 0) { s = sqrtf(t + 1) * 2; q[3] = s / 4; q[0] = (m[7] - m[5]) / s; q[1] = (m[2] - m[6]) / s; q[2] = (m[3] - m[1]) / s; }
    else if (m[0] > m[4] && m[0] > m[8]) { s = sqrtf(1 + m[0] - m[4] - m[8]) * 2; q[3] = (m[7] - m[5]) / s; q[0] = s / 4; q[1] = (m[1] + m[3]) / s; q[2] = (m[2] + m[6]) / s; }
    else if (m[4] > m[8]) { s = sqrtf(1 + m[4] - m[0] - m[8]) * 2; q[3] = (m[2] - m[6]) / s; q[0] = (m[1] + m[3]) / s; q[1] = s / 4; q[2] = (m[5] + m[7]) / s; }
    else { s = sqrtf(1 + m[8] - m[0] - m[4]) * 2; q[3] = (m[3] - m[1]) / s; q[0] = (m[2] + m[6]) / s; q[1] = (m[5] + m[7]) / s; q[2] = s / 4; }
}
/* Pose between a (w = 0) and b (w = 1): position lerp, rotation nlerp on the short arc. */
static void blend_pose(const float a[16], const float b[16], float w, float o[16]) {
    float qa[4], qb[4], q[4], ra[9], rb[9], d = 0, n = 0, t[16]; int i, j;
    if (w <= 0) { memcpy(o, a, 64); return; } if (w >= 1) { memcpy(o, b, 64); return; }
    for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) { ra[i * 3 + j] = a[i * 4 + j]; rb[i * 3 + j] = b[i * 4 + j]; }
    mat_quat(ra, qa); mat_quat(rb, qb);
    for (i = 0; i < 4; ++i) d += qa[i] * qb[i];
    for (i = 0; i < 4; ++i) { q[i] = (1 - w) * qa[i] + w * (d < 0 ? -qb[i] : qb[i]); n += q[i] * q[i]; }
    n = sqrtf(n); for (i = 0; i < 4; ++i) q[i] /= n;
    { float x = q[0], y = q[1], z = q[2], s = q[3];                                   /* inverse of mat_quat */
      float r[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - z * s), 2 * (x * z + y * s), 2 * (x * y + z * s), 1 - 2 * (x * x + z * z), 2 * (y * z - x * s), 2 * (x * z - y * s), 2 * (y * z + x * s), 1 - 2 * (x * x + y * y) };
      memset(t, 0, sizeof(t)); for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) t[i * 4 + j] = r[i * 3 + j]; }
    for (j = 0; j < 3; ++j) t[12 + j] = (1 - w) * a[12 + j] + w * b[12 + j];
    t[15] = 1; memcpy(o, t, 64);
}
/* Two-bone solve. Inputs: current W10, W11, W12 and target hand world matrix T.
   Outputs new W10, W11, W12. Returns 1 if the target had to be clamped. */
int mgs4vr_armsrc_solve_pole(const float w10[16], const float w11[16], const float w12[16], const float target[16], const float *pole, float n10[16], float n11[16], float n12[16]);
int mgs4vr_armsrc_solve(const float w10[16], const float w11[16], const float w12[16], const float target[16], float n10[16], float n11[16], float n12[16]) {
    return mgs4vr_armsrc_solve_pole(w10, w11, w12, target, NULL, n10, n11, n12);
}
/* pole: world direction the elbow should point to (NULL = the game's elbow). */
int mgs4vr_armsrc_solve_pole(const float w10[16], const float w11[16], const float w12[16], const float target[16], const float *pole, float n10[16], float n11[16], float n12[16]) {
    float S[3], E[3], H[3], T[3], d[3], a, b, L, Lc, cosA, sinA, n[3], nl, E2[3], u[3], v[3], Q[9], A1[16], A2[16], H1[3], E1[3], tmp[16];
    int i, clamped = 0;
    memcpy(S, w10 + 12, 12); memcpy(E, w11 + 12, 12); memcpy(H, w12 + 12, 12); memcpy(T, target + 12, 12);
    for (i = 0; i < 3; ++i) { u[i] = E[i] - S[i]; v[i] = H[i] - E[i]; d[i] = T[i] - S[i]; }
    a = len3(u); b = len3(v); L = len3(d);
    if (a < 1 || b < 1 || L < 1e-3f) { memcpy(n10, w10, 64); memcpy(n11, w11, 64); memcpy(n12, w12, 64); return 1; }
    for (i = 0; i < 3; ++i) d[i] /= L;
    Lc = L;
    if (Lc > a + b - 1.0f) { Lc = a + b - 1.0f; clamped = 1; }
    if (Lc < fabsf(a - b) + 1.0f) { Lc = fabsf(a - b) + 1.0f; clamped = 1; }
    /* pole: the given direction, else the game's elbow direction; perpendicular to the shoulder-target line */
    { const float *pp = pole ? pole : u; for (i = 0; i < 3; ++i) n[i] = pp[i] - dot3(pp, d) * d[i]; }
    nl = len3(n);
    if (nl < 1e-3f) { float down[3] = { 0, -1, 0 }; for (i = 0; i < 3; ++i) n[i] = down[i] - dot3(down, d) * d[i]; nl = len3(n); }
    for (i = 0; i < 3; ++i) n[i] /= nl;
    cosA = (a * a + Lc * Lc - b * b) / (2 * a * Lc); if (cosA > 1) cosA = 1; if (cosA < -1) cosA = -1; sinA = sqrtf(1 - cosA * cosA);
    for (i = 0; i < 3; ++i) E2[i] = S[i] + d[i] * a * cosA + n[i] * a * sinA;
    /* upper arm: rotate about the shoulder so the elbow lands on E2 */
    { float uu[3], vv[3]; for (i = 0; i < 3; ++i) { uu[i] = u[i] / a; vv[i] = (E2[i] - S[i]) / a; } rot_between(uu, vv, Q); }
    about(Q, S, A1);
    mul(w10, A1, n10);
    /* forearm: after A1, rotate about the new elbow so the hand lands on the (clamped) target */
    { float e4[4] = { E[0], E[1], E[2], 1 }, h4[4] = { H[0], H[1], H[2], 1 }; int c;
      for (c = 0; c < 3; ++c) { E1[c] = e4[0] * A1[c] + e4[1] * A1[4 + c] + e4[2] * A1[8 + c] + A1[12 + c]; H1[c] = h4[0] * A1[c] + h4[1] * A1[4 + c] + h4[2] * A1[8 + c] + A1[12 + c]; } }
    { float Tc[3], uu[3], vv[3], lu, lv; for (i = 0; i < 3; ++i) Tc[i] = S[i] + d[i] * Lc;
      for (i = 0; i < 3; ++i) { uu[i] = H1[i] - E1[i]; vv[i] = Tc[i] - E1[i]; } lu = len3(uu); lv = len3(vv);
      for (i = 0; i < 3; ++i) { uu[i] /= lu; vv[i] /= lv; } rot_between(uu, vv, Q);
      about(Q, E1, A2);
      mul(w11, A1, tmp); mul(tmp, A2, n11);
      /* hand: controller orientation, positioned at the reached point */
      memcpy(n12, target, 64); memcpy(n12 + 12, Tc, 12); }
    return clamped;
}
/* Descendants keep their local transform: new = old * inv(old parent) * new parent. */
static void follow_descendants(float (*in)[16], const float (*old)[16], const BYTE *m, int root, int s1, int s2, int s3, LONG *written, LONG *kept) {
    int i; float inv[16], t[16];
    for (i = root + 1; i < BONES; ++i) {
        int p = parent_of[i];
        if (!m[i] || i == s1 || i == s2 || i == s3) continue;
        if (have_last[i] && !memcmp(in[i], last_out[i], 64)) { ++*kept; continue; }       /* not rewritten: never twice */
        rigid_inverse(old[p], inv); mul(old[i], inv, t); mul(t, in[p], in[i]);
        memcpy(last_out[i], in[i], 64); have_last[i] = 1; ++*written;
    }
}
/* Game pose returns: forget what we wrote and the snap state. */
static void twohand_reset(void);
static void reset_follow(void) { twohand_reset(); memset(have_last, 0, sizeof(have_last)); blend_init = 0; snapped = 0; }
/* Controller grip (LOCAL space) relative to the reference eye/head -> hand world matrix via camera C. */
static void controller_target(const float ref_q[4], const float ref_p[3], const float grip_q[4], const float grip_p[3],
                              const float O[9], const float P[3], const float C[16], float out[16]) {
    float R[9], p[3], Rt[9], M[16]; int i, j;
    mgs4vr_armsrc_view_from_xr(ref_q, ref_p, grip_q, grip_p, R, p);
    mul3(O, R, Rt);
    memset(M, 0, sizeof(M));
    for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) M[i * 4 + j] = Rt[i * 3 + j];
    for (j = 0; j < 3; ++j) M[12 + j] = P[0] * R[j] + P[1] * R[3 + j] + P[2] * R[6 + j] + p[j];
    M[15] = 1;
    mul(M, C, out);                                                                     /* view -> world */
}
#include "mgs4vr_twohand.inl"
/* Rotation of `deg` about unit axis k through point c (row-vector world transform). */
static void axis_rotation(const float k[3], float deg, const float c[3], float o[16]) {
    float a = deg * 0.017453292f, cs = cosf(a), sn = sinf(a), R[9]; int i, j;
    for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) {                                 /* column form, stored transposed = row form */
        float col = (i == j ? cs : 0) + (1 - cs) * k[i] * k[j];
        if (i != j) { int m = 3 - i - j; float sg = ((i + 1) % 3 == j) ? -1.f : 1.f; col += sg * sn * k[m]; }
        R[j * 3 + i] = col;
    }
    about(R, c, o);
}
/* ---- pose tool (16 Oct 2026, mgs4vr_poses.h) ----
   Finger chains (sim 16 Oct, hand-local positions): right 37-39 thumb (+114), 40-42 index, 43-45 middle, 46 palm ->
   47-49 ring, 50-52 pinky; left 21-23 thumb (+113), 24-26, 27-29, 30 palm -> 31-33, 34-36. */
static const int finger_r[MGS4VR_FINGERS][4] = { { 37, 38, 39, 114 }, { 40, 41, 42, -1 }, { 43, 44, 45, -1 }, { 47, 48, 49, -1 }, { 50, 51, 52, -1 } };
static const int finger_l[MGS4VR_FINGERS][4] = { { 21, 22, 23, 113 }, { 24, 25, 26, -1 }, { 27, 28, 29, -1 }, { 31, 32, 33, -1 }, { 34, 35, 36, -1 } };
static volatile LONG pose_hits[2], pose_finger_frames[2], pose_frozen_frames;
/* Pure: M = L * M with L = the pose offset in M's own frame (yaw about local y, pitch about local z, roll about local x,
   then the shift d in mm). */
void mgs4vr_armsrc_pose_offset(const MGS4VR_POSE *p, float M[16]) {
    static const float X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 }, O[3] = { 0, 0, 0 };
    float gy[16], gz[16], gx[16], L[16];
    axis_rotation(Y, p->rot[0], O, gy); axis_rotation(Z, p->rot[1], O, gz); axis_rotation(X, p->rot[2], O, gx);
    mul(gx, gz, L); mul(L, gy, L); L[12] += p->d[0]; L[13] += p->d[1]; L[14] += p->d[2];
    mul(L, M, M);
}
/* Finger curls/spread on the hand's bones, after the hand and its descendants are final. sign = +1 right, -1 left
   (mirrored rig). Curl turns each joint (and the rest of its chain) about the hand's z axis (thumb: x axis) through the
   joint; spread turns the whole finger about the hand's y axis at its base (index +, ring -, pinky -1.5x). */
static void pose_fingers(float (*in)[16], int hand, const int (*ch)[4], const MGS4VR_POSE *p, float sign) {
    int f, j, m; float ax[3], ay[3], az[3], G[16], c[3];
    for (j = 0; j < 3; ++j) { ax[j] = in[hand][j]; ay[j] = in[hand][4 + j]; az[j] = in[hand][8 + j]; }
    { float lx = len3(ax), ly = len3(ay), lz = len3(az); if (lx < 1e-4f || ly < 1e-4f || lz < 1e-4f) return;
      for (j = 0; j < 3; ++j) { ax[j] /= lx; ay[j] /= ly; az[j] /= lz; } }
    for (f = 0; f < MGS4VR_FINGERS; ++f) {
        float s = f == MGS4VR_FINGER_INDEX ? 1.f : f == MGS4VR_FINGER_RING ? -1.f : f == MGS4VR_FINGER_PINKY ? -1.5f : 0.f;
        if (s != 0 && p->spread != 0) {
            memcpy(c, in[ch[f][0]] + 12, 12); axis_rotation(ay, sign * s * p->spread, c, G);
            for (m = 0; m < 4; ++m) if (ch[f][m] >= 0) mul(in[ch[f][m]], G, in[ch[f][m]]);
        }
        if (p->curl[f] == 0) continue;
        for (j = 0; j < 3; ++j) {
            memcpy(c, in[ch[f][j]] + 12, 12); axis_rotation(f == MGS4VR_FINGER_THUMB ? ax : az, sign * p->curl[f], c, G);
            for (m = j; m < 3; ++m) mul(in[ch[f][m]], G, in[ch[f][m]]);
            if (j == 0 && ch[f][3] >= 0) mul(in[ch[f][3]], G, in[ch[f][3]]);
        }
    }
}
/* Apply a hand's fingers once per fresh game pose: when the game did not rewrite the chain this frame (input = what we
   wrote last time) the curl is already in it. */
/* pose_in = the uncurled chain we last curled, pose_last = what we wrote. A chain whose input is still exactly our own
   output was not rewritten by the game: start again from pose_in (never curl a curled chain twice), with the CURRENT pose
   (16 Oct sim: skipping such frames instead froze a pose change while the game kept producing identical frames). */
static float pose_in[BONES][16], pose_last[BONES][16]; static BYTE have_pose_last[BONES];
static void pose_fingers_fresh(float (*in)[16], const float (*old)[16], int hand, const int (*ch)[4], const MGS4VR_POSE *p, float sign, int side) {
    int f, m, b = ch[1][0], stale = have_pose_last[b] && !memcmp(in[b], pose_last[b], 64);
    (void)old;
    for (f = 0; f < MGS4VR_FINGERS; ++f) for (m = 0; m < 4; ++m) if (ch[f][m] >= 0) {
        int k = ch[f][m]; if (stale && have_pose_last[k]) memcpy(in[k], pose_in[k], 64); else memcpy(pose_in[k], in[k], 64); }
    pose_fingers(in, hand, ch, p, sign); InterlockedIncrement(&pose_finger_frames[side]);
    for (f = 0; f < MGS4VR_FINGERS; ++f) for (m = 0; m < 4; ++m) if (ch[f][m] >= 0) {
        int k = ch[f][m]; memcpy(last_out[k], in[k], 64); have_last[k] = 1; memcpy(pose_last[k], in[k], 64); have_pose_last[k] = 1; }
}
/* ---- pistol interactions (16 Oct 2026, phase 4): rack the slide, insert the magazine ----
   Positions in the right hand's frame (the pistol: barrel along -x, +x = back towards the shooter). The left controller's
   wrist position lp is compared with the left-hand poses slide_rear / slide_mid / mag (their d = the wrist target).
   Slide: left grip pressed within PI_SLIDE_R of a slide pose -> the hand takes that pose and follows a pull backwards
   (+x, at most PI_PULL_MAX); PI_RACK back = racked (once until the slide went forward again). Magazine: left grip within
   PI_MAG_R of the mag pose -> the hand eases onto it as it comes closer; within PI_INSERT = inserted (once until the
   hand left the zone). Grip released = free hand again. */
#define PI_GRAB 0.6f
#define PI_DROP 0.4f
#define PI_SLIDE_R 100.f
#define PI_MAG_R 140.f
#define PI_MAG_EASE 90.f
#define PI_INSERT 35.f
#define PI_PULL_MAX 45.f
#define PI_RACK 35.f
#define PI_REARM 10.f
enum { PI_IDLE, PI_SLIDE, PI_MAG };
enum { PI_EV_GRAB = 1, PI_EV_RACK = 2, PI_EV_MAG = 4, PI_EV_INSERT = 8 };
typedef struct { int state, which, racked, inserted; float start; } PI_STATE;
static float pi_dist(const float a[3], const float b[3]) { float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; return len3(d); }
/* Pure. pos[0..2] = slide_rear, slide_mid, mag wrist targets (NULL = pose not defined). Returns the pose to show
   (MGS4VR_POSE_SLIDE_REAR/MID/MAG) or -1; *pull = slide travel (mm, +x), *w = 0..1 how far onto the mag pose. */
int mgs4vr_armsrc_interact(PI_STATE *s, const float lp[3], const float *const pos[3], float squeeze, float *pull, float *w, int *ev) {
    int k; *pull = 0; *w = 0; *ev = 0;
    if (s->state == PI_SLIDE && (squeeze < PI_DROP || !pos[s->which])) s->state = PI_IDLE;
    if (s->state == PI_MAG && (squeeze < PI_DROP || !pos[2] || pi_dist(lp, pos[2]) > PI_MAG_R * 1.3f)) { s->state = PI_IDLE; s->inserted = 0; }
    if (s->state == PI_IDLE && squeeze >= PI_GRAB) {
        int best = -1; float bd = PI_SLIDE_R;
        for (k = 0; k < 2; ++k) if (pos[k] && pi_dist(lp, pos[k]) < bd) { bd = pi_dist(lp, pos[k]); best = k; }
        if (best >= 0) { s->state = PI_SLIDE; s->which = best; s->start = lp[0]; s->racked = 0; *ev |= PI_EV_GRAB; }
        else if (pos[2] && pi_dist(lp, pos[2]) < PI_MAG_R) { s->state = PI_MAG; s->inserted = 0; *ev |= PI_EV_MAG; }
    }
    if (s->state == PI_SLIDE) {
        float p = lp[0] - s->start; if (p < 0) p = 0; if (p > PI_PULL_MAX) p = PI_PULL_MAX; *pull = p;
        if (!s->racked && p >= PI_RACK) { s->racked = 1; *ev |= PI_EV_RACK; }
        else if (s->racked && p < PI_REARM) s->racked = 0;
        return s->which ? MGS4VR_POSE_SLIDE_MID : MGS4VR_POSE_SLIDE_REAR;
    }
    if (s->state == PI_MAG) {
        float d = pi_dist(lp, pos[2]); *w = d <= PI_INSERT ? 1.f : d >= PI_INSERT + PI_MAG_EASE ? 0.f : 1.f - (d - PI_INSERT) / PI_MAG_EASE;
        if (!s->inserted && d <= PI_INSERT) { s->inserted = 1; *ev |= PI_EV_INSERT; }
        return MGS4VR_POSE_MAG;
    }
    return -1;
}
/* ---- pose camera (17 Oct 2026, tool "cam az el dist tx ty tz"): look at a point of the pistol from outside ----
   Game thread only (skeleton solve and camera seam). The right hand's final world matrix of the last IK frame is kept;
   the eye sits dist mm from the target, direction from az (about the pistol's up axis: 0 = its left side, 90 = behind,
   from the shooter, 180 = right, 270 = in front of the muzzle) and el (+ = above). Camera rows as the game's: 0 right,
   1 down, 2 forward, 3 position. */
static float pc_hand[16]; static DWORD pc_tick; static volatile LONG pc_frames;
int mgs4vr_armsrc_pose_camera_build(const float hand[16], const float v[6], float m[16]) {
    float R[3][3], l[3], t[3], dl[3], d[3], f[3], r0[3], r1[3], up[3] = { 0, 1, 0 }, az = v[0] * 0.017453292f, el = v[1] * 0.017453292f; int i, k;
    for (i = 0; i < 3; ++i) { for (k = 0; k < 3; ++k) R[i][k] = hand[i * 4 + k]; l[i] = len3(R[i]); if (l[i] < 1e-4f) return 0; for (k = 0; k < 3; ++k) R[i][k] /= l[i]; }
    dl[0] = sinf(az) * cosf(el); dl[1] = -cosf(az) * cosf(el); dl[2] = sinf(el);
    for (k = 0; k < 3; ++k) { t[k] = v[3] * R[0][k] + v[4] * R[1][k] + v[5] * R[2][k] + hand[12 + k]; d[k] = dl[0] * R[0][k] + dl[1] * R[1][k] + dl[2] * R[2][k]; f[k] = -d[k]; }
    if (fabsf(dot3(f, up)) > 0.98f) memcpy(up, R[0], sizeof(up));            /* straight up/down: the pistol's back axis */
    cross3(f, up, r0); if (len3(r0) < 1e-4f) return 0; { float n = len3(r0); for (k = 0; k < 3; ++k) r0[k] /= n; }
    cross3(f, r0, r1);
    for (k = 0; k < 3; ++k) { m[k] = r0[k]; m[4 + k] = r1[k]; m[8 + k] = f[k]; m[12 + k] = t[k] + d[k] * v[2]; }
    m[3] = m[7] = m[11] = 0; m[15] = 1;
    return 1;
}
static float pc_real[16], pc_view[16], pc_main[16]; static DWORD pc_real_tick;   /* pc_view/pc_main: diagnostics, our view vs the game's built camera */
static void pc_rel(const float *v, const float *p, float o[3]) { float d[3]; int i; for (i = 0; i < 3; ++i) d[i] = p[i] - v[12 + i]; for (i = 0; i < 3; ++i) o[i] = d[0] * v[i * 4] + d[1] * v[i * 4 + 1] + d[2] * v[i * 4 + 2]; }
static int pose_cam_live(void) { float v[6]; return mgs4vr_poses_cam(v) && pc_real_tick && GetTickCount() - pc_real_tick < 500; }
int mgs4vr_armsrc_pose_camera(float m[16]) {
    float v[6];
    if (!mgs4vr_poses_cam(v)) { pc_real_tick = 0; return 0; }
    /* the game's camera before we replace it: the IK keeps using it. Latched when the view turns on (sim 17 Oct: the
       first-person source follows the previous frame's camera, so a per-frame copy fed our view back into the IK and
       the arm moved with every camera change). */
    if (!pc_real_tick || GetTickCount() - pc_real_tick > 500) memcpy(pc_real, m, sizeof(pc_real));
    pc_real_tick = GetTickCount();
    if (!pc_tick || GetTickCount() - pc_tick > 1000) return 0;
    if (!mgs4vr_armsrc_pose_camera_build(pc_hand, v, m)) return 0;
    memcpy(pc_view, m, sizeof(pc_view));
    InterlockedIncrement(&pc_frames); return 2;                                 /* 2: the camera module skips the head offset */
}
static PI_STATE pi; static void (*pi_reload)(int kind); static float pi_lp[3], pi_sq;   /* status: last left wrist in the pistol frame, squeeze */
void mgs4vr_armsrc_set_reload(void (*fn)(int kind)) { pi_reload = fn; }
/* Slide on the model (17 Oct 2026, headset: "the slide does not move, only your hand"). Sim: the pistol classes place
   their slide bone in their model update (Mk.II 0xF77160 bone 2, Operator 0xF77B40 bone 4, 0xF7FB30) at the weapon root
   moved along the barrel by the float at weapon actor +0x1D28 (firing: 0 -> -42 mm and back); a written value stays and
   the bone follows (poke -30 -> slide -30 mm). Weapon actor = handle at held weapon +0x68 (player data +0xC8). Written on
   the game thread only while racking (-pull), and set back to 0 once afterwards. */
#define SLIDE_FIELD 0x1d28
static BYTE *held_weapon_actor(void) {
    BYTE *r = NULL;
    __try {
        int slot = *(int *)(image + 0x23eb5150); unsigned long long owner, actor, pd, w, h, wa;
        if (slot < -1 || slot > 23) return NULL;
        owner = *(unsigned long long *)(image + 0x23eb5160 + (size_t)(slot + 1) * 0x80); actor = owner & 0xfffffffffffffULL;
        if (actor < 0x10000 || *(unsigned long long *)(actor + 0x30) != owner) return NULL;
        pd = *(unsigned long long *)(actor + 0x90); if (pd < 0x10000) return NULL;
        w = *(unsigned long long *)(pd + 0xc8); if (w < 0x10000) return NULL;
        h = *(unsigned long long *)(w + 0x68); wa = h & 0xfffffffffffffULL;
        if (wa < 0x10000 || *(unsigned long long *)(wa + 0x30) != h) return NULL;
        r = (BYTE *)wa;
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; }
    return r;
}
static volatile LONG slide_writes; static float slide_last;
static void slide_set(float pull) {
    BYTE *wa; if (pull == 0 && slide_last == 0) return;
    wa = held_weapon_actor(); if (!wa) { slide_last = 0; return; }
    __try { *(float *)(wa + SLIDE_FIELD) = -pull; InterlockedIncrement(&slide_writes); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    slide_last = pull;
}
static int pose_weapon_code(void) { int c = -1; __try { c = *(unsigned short *)(image + 0x23fed464) & 0x1ff; } __except (EXCEPTION_EXECUTE_HANDLER) { c = -1; } return c; }
/* Out of reach: rotate the clavicle about its root, toward the goal, until the shoulder is within reach
   (at most PROTRACT_MAX_DEG). G = world transform for the clavicle subtree. Returns 0 when not needed. */
static int protract(float (*in)[16], const float goal[16], float G[16]) {
    float C[3], S[3], T[3], u[3], v[3], k[3], reach, want, lo = 0, hi, kl; int i, it;
    memcpy(C, in[L_CLAV] + 12, 12); memcpy(S, in[L_ROOT] + 12, 12); memcpy(T, goal + 12, 12);
    { float e[3], h[3]; for (i = 0; i < 3; ++i) { e[i] = in[L_ELBOW][12 + i] - S[i]; h[i] = in[L_HAND][12 + i] - in[L_ELBOW][12 + i]; } reach = len3(e) + len3(h); }
    want = reach - 5.0f;
    for (i = 0; i < 3; ++i) { u[i] = S[i] - C[i]; v[i] = T[i] - C[i]; }
    { float d[3]; for (i = 0; i < 3; ++i) d[i] = T[i] - S[i]; if (len3(d) <= want) return 0; }
    cross3(u, v, k); kl = len3(k); if (kl < 1e-3f) return 0;
    for (i = 0; i < 3; ++i) k[i] /= kl;
    hi = acosf(fmaxf(-1.f, fminf(1.f, dot3(u, v) / (len3(u) * len3(v))))) * 57.29578f;
    if (hi > PROTRACT_MAX_DEG) hi = PROTRACT_MAX_DEG;
    for (it = 0; it < 24; ++it) {                                                      /* distance falls monotonically up to hi */
        float mid = (lo + hi) / 2, Gm[16], s[3], d[3];
        axis_rotation(k, mid, C, Gm);
        for (i = 0; i < 3; ++i) s[i] = S[0] * Gm[i] + S[1] * Gm[4 + i] + S[2] * Gm[8 + i] + Gm[12 + i];
        for (i = 0; i < 3; ++i) d[i] = T[i] - s[i];
        if (len3(d) > want) lo = mid; else hi = mid;
    }
    axis_rotation(k, hi, C, G);
    return hi > 0.01f;
}
/* Left elbow direction from the body: down (world Y up) plus outward = away from the right shoulder, horizontal. */
static void body_pole(float (*in)[16], float pole[3]) {
    float out[3] = { in[L_ROOT][12] - in[ARM_ROOT][12], 0, in[L_ROOT][14] - in[ARM_ROOT][14] }, l = len3(out); int i;
    if (l < 1) { pole[0] = 0; pole[1] = -1; pole[2] = 0; return; }
    for (i = 0; i < 3; ++i) pole[i] = POLE_OUT * out[i] / l;
    pole[1] -= 1.0f;
}
/* Left arm, after the right hand is final. Foregrip = where the game's own animation puts the left hand
   relative to the right hand this frame (rigid, 0.0 mm spread in runs 13520/16796/28276), carried onto our
   right hand: works per weapon without calibration. Controller hand snaps onto it within SNAP_ENTER and lets
   go beyond SNAP_EXIT, blended over grab_ms / release_ms with an ease-in-out weight. No left controller: hand stays on the grip. */
static void left_arm(float (*in)[16], const float (*old)[16], LONG code, const HAND_VIEW *h, const float ref_q[4], const float ref_p[3],
                     const float C[16], int shared_blend, LONG *written, LONG *kept) {
    float inv[16], F[16], grip[16], ctrl[16], goal[16], n6[16], n7[16], n8[16], d[3], want; int i, have_grip, have_ctrl = h->lvalid, have_lpose = 0, have_ipose = 0; MGS4VR_POSE lpose, ipose; DWORD now = left_clock ? left_clock() : GetTickCount();
    if (!finite16(in[L_CLAV]) || !finite16(in[L_ROOT]) || !finite16(in[L_ELBOW]) || !finite16(in[L_HAND])) { blend_init = 0; return; }
    rigid_inverse(old[HAND], inv); mul(old[L_HAND], inv, F);
    have_grip = len3(F + 12) < GRIP_MAX;
    if (have_grip) mul(F, in[HAND], grip);
    if (have_grip && mgs4vr_poses_get(pose_weapon_code(), 1, MGS4VR_POSE_SUPPORT, &lpose)) { mgs4vr_armsrc_pose_offset(&lpose, grip); have_lpose = 1; InterlockedIncrement(&pose_hits[1]); }   /* pose tool: hand on the foregrip */
    if (have_ctrl) controller_target(ref_q, ref_p, h->lgrip_q, h->lgrip_p, GRIP_O_L, GRIP_P_L, C, ctrl);
    if (!have_grip) InterlockedIncrement(&left_no_grip);
    if (!have_ctrl) InterlockedIncrement(&left_no_ctrl);
    if (!have_grip && !have_ctrl) { blend_init = 0; return; }                          /* nothing to follow: game pose */
    if (shoulder_v[0] || shoulder_v[1] || shoulder_v[2]) shift_bones(in, mask_l, shoulder_v);   /* left shoulder back too (the arm is solved below) */
    if (!shared_blend) {
    if (!have_grip) want = 0;
    else if (!have_ctrl || code == IK_GRIP) want = 1;
    else if (code == IK_FREE) want = 0;
    else {
        for (i = 0; i < 3; ++i) d[i] = ctrl[12 + i] - grip[12 + i];
        if (!snapped && len3(d) < SNAP_ENTER) { snapped = 1; InterlockedIncrement(&snap_enters); }
        else if (snapped && len3(d) > SNAP_EXIT) { snapped = 0; InterlockedIncrement(&snap_exits); }
        want = snapped ? 1.f : 0.f;
    }
    if (!blend_init) { blend = want; blend_init = 1; }
    else blend_advance(want, now);
    blend_tick = now;
    }
    if (blend >= 1 || !have_ctrl) memcpy(goal, grip, 64);
    else if (blend <= 0 || !have_grip) memcpy(goal, ctrl, 64);
    else blend_pose(ctrl, grip, blend_ease(blend), goal);
    if (blend >= 0.5f && have_grip) InterlockedIncrement(&left_grip_frames);
    {   /* pistol interactions + the tool's preview (phases 3/4): slide_rear / slide_mid / mag relative to the right hand */
        int cw = pose_weapon_code(), k, show = mgs4vr_poses_show(), use = -1, ev = 0, have[3]; MGS4VR_POSE ip[3]; const float *pos[3];
        float hinv[16], T[3][16], lp[3], pull = 0, w = 0;
        for (k = 0; k < 3; ++k) { have[k] = mgs4vr_poses_get(cw, 1, MGS4VR_POSE_SLIDE_REAR + k, &ip[k]); pos[k] = have[k] ? ip[k].d : NULL;
            if (have[k]) { memcpy(T[k], in[HAND], 64); mgs4vr_armsrc_pose_offset(&ip[k], T[k]); } }
        if (show >= MGS4VR_POSE_SLIDE_REAR && show <= MGS4VR_POSE_MAG && have[show - MGS4VR_POSE_SLIDE_REAR]) { use = show; w = 1; pi.state = PI_IDLE; }
        else if (have_ctrl && (have[0] || have[1] || have[2])) {
            rigid_inverse(in[HAND], hinv);
            for (k = 0; k < 3; ++k) lp[k] = ctrl[12] * hinv[k] + ctrl[13] * hinv[4 + k] + ctrl[14] * hinv[8 + k] + hinv[12 + k];
            use = mgs4vr_armsrc_interact(&pi, lp, pos, h->lsqueeze, &pull, &w, &ev); memcpy(pi_lp, lp, sizeof(pi_lp)); pi_sq = h->lsqueeze;
            if (ev & PI_EV_GRAB) mgs4vr_poses_event(MGS4VR_EV_SLIDE_GRAB);
            if (ev & PI_EV_MAG) mgs4vr_poses_event(MGS4VR_EV_MAG_HOLD);
            if (ev & PI_EV_RACK) { mgs4vr_poses_event(MGS4VR_EV_RACK); if (pi_reload) pi_reload(1); }
            if (ev & PI_EV_INSERT) { mgs4vr_poses_event(MGS4VR_EV_MAG_INSERT); if (pi_reload) pi_reload(2); }
        } else pi.state = PI_IDLE;
        {   /* slide grab/release blended like the support grip (headset 17 Oct: the hand snapped onto the slide): grab_ms /
               release_ms with the same ease-in-out; release eases from the last slide pose (recomputed on the moving pistol) */
            static float sb; static DWORD sb_tick; static int sb_j = -1; static float sb_pull; int slide_now;
            DWORD now = GetTickCount(); int slide = use >= 0 && use != MGS4VR_POSE_MAG && show < 0;
            float dur = (float)(slide ? grab_ms : release_ms), step = sb_tick && dur > 0 ? (float)(now - sb_tick) / dur : 1.f;
            sb_tick = now;
            if (slide) { sb = sb + step >= 1 ? 1 : sb + step; sb_j = use - MGS4VR_POSE_SLIDE_REAR; sb_pull = pull; }
            else sb = sb - step <= 0 ? 0 : sb - step;
            slide_now = slide && pi.state == PI_SLIDE; slide_set(slide_now ? pull : 0);   /* the model's slide follows the pull */
            if (use < 0 && sb > 0 && sb_j >= 0 && have[sb_j] && have_ctrl) {   /* releasing: back to the controller */
                float t[16], ax[3] = { in[HAND][0], in[HAND][1], in[HAND][2] }, l = len3(ax); memcpy(t, T[sb_j], 64);
                if (l > 1e-4f) for (k = 0; k < 3; ++k) t[12 + k] += ax[k] / l * sb_pull;
                blend_pose(ctrl, t, blend_ease(sb), goal); ipose = ip[sb_j]; have_ipose = sb > 0.5f;
            }
            if (use >= 0) {
                int j = use - MGS4VR_POSE_SLIDE_REAR; float t[16]; memcpy(t, T[j], 64);
                if (use != MGS4VR_POSE_MAG) { float ax[3] = { in[HAND][0], in[HAND][1], in[HAND][2] }, l = len3(ax); if (l > 1e-4f) for (k = 0; k < 3; ++k) t[12 + k] += ax[k] / l * pull; w = slide ? sb : 1; }
                if (w >= 1 || !have_ctrl) memcpy(goal, t, 64); else blend_pose(ctrl, t, blend_ease(w), goal);
                ipose = ip[j]; have_ipose = w > 0.5f;
            }
        }
    }
    { float G[16], pole[3];
      if (protract(in, goal, G)) {                                                     /* shoulder forward: clavicle subtree start */
          InterlockedIncrement(&left_protract);
          for (i = L_CLAV; i <= L_HAND; ++i) mul(in[i], G, in[i]);
      }
      body_pole(in, pole);
      if (mgs4vr_armsrc_solve_pole(in[L_ROOT], in[L_ELBOW], in[L_HAND], goal, pole, n6, n7, n8)) InterlockedIncrement(&left_clamped); }
    memcpy(in[L_ROOT], n6, 64); memcpy(in[L_ELBOW], n7, 64); memcpy(in[L_HAND], n8, 64);
    for (i = L_CLAV; i <= L_HAND; ++i) { memcpy(last_out[i], in[i], 64); have_last[i] = 1; }
    *written += 4;
    follow_descendants(in, old, mask_l, L_CLAV, L_ROOT, L_ELBOW, L_HAND, written, kept);
    if (have_ipose) pose_fingers_fresh(in, old, L_HAND, finger_l, &ipose, -1.f, 1);
    else if (have_lpose && blend >= 0.5f) pose_fingers_fresh(in, old, L_HAND, finger_l, &lpose, -1.f, 1);
    InterlockedIncrement(&left_writes);
}
/* Game thread only (the skeleton solve runs there), so no locking is needed. */
static void apply(BYTE *obj, LONG code) {
    int i; float (*in)[16], root[16]; BYTE *m, *o, *meta; static float old[BONES][16]; LONG written = 0, kept = 0;
    __try {
        m = *(BYTE **)obj; o = owner_model();
        if (!m || m != o || *(unsigned *)(m + 0xcc) != BONES) { InterlockedIncrement(&rejects[R_NOT_PLAYER]); if (!o || m == o) reset_follow(); return; }
        meta = *(BYTE **)(m + 0xf8); in = *(float (**)[16])(m + 0x100);
        if (!meta || !in) { InterlockedIncrement(&rejects[R_NOT_PLAYER]); reset_follow(); return; }
        for (i = 0; i < 13; ++i) if (*(unsigned *)(meta + i * 80) != rig_ids[i] || *(int *)(meta + i * 80 + 8) != rig_parents[i]) { InterlockedIncrement(&rejects[R_RIG]); reset_follow(); return; }
        InterlockedIncrement(&player_calls);
        if (meta != mask_meta) { reset_follow(); build_mask(meta); }
        if ((const float *)in != last_input) { reset_follow(); last_input = (const float *)in; }
        memcpy(root, m + 0x20, sizeof(root));
        if (!finite16(root) || !finite16(in[ARM_ROOT]) || !finite16(in[ELBOW]) || !finite16(in[HAND])) { InterlockedIncrement(&rejects[R_FINITE]); reset_follow(); return; }
        memcpy(old, in, sizeof(old));
        if (IS_IK(code)) {
            float C[16], target[16], n10[16], n11[16], n12[16]; int shared_blend, have_rpose = 0, pcam; MGS4VR_POSE rpose; HAND_VIEW h; EYE_ANCHOR a; const float *ref_q, *ref_p;
            memcpy(C, image + MGS4VR_RVA_MAIN_CAMERA, sizeof(C));
            pcam = pose_cam_live(); if (pcam) { memcpy(pc_main, C, sizeof(pc_main)); memcpy(C, pc_real, sizeof(C)); }   /* pose camera on: the game's own camera, not the view */
            if (!finite16(C) || !finite16(in[HEAD_BONE])) { InterlockedIncrement(&rejects[R_FINITE]); reset_follow(); return; }
            { float dd[3] = { C[12] - in[HEAD_BONE][12], C[13] - in[HEAD_BONE][13], C[14] - in[HEAD_BONE][14] };
              if (len3(dd) > FPS_RADIUS) { InterlockedIncrement(&rejects[R_NOT_FPS]); reset_follow(); return; } }
            {   /* 16 Oct 2026 (camera and hands from ONE sample). Here the
                   camera comes from an eye packet (display time T1) and the hand was the newest XR sample (another
                   frame, T2): at 50 Hz game vs 65 Hz XR, T2 - T1 jumped 0..15 ms per frame. Now the hand sample with
                   the packet's own display time; and a short gap (stale sample, 4.3% of frames in the headset run of
                   15 Oct) holds the last good sample instead of dropping the arm to the animation for a frame. */
                HAND_VIEW latest, paired; int have_latest = read_hand(&latest), anchored, mode; DWORD now = GetTickCount();
                memset(&a, 0, sizeof(a));
                anchored = read_anchor(&a) && a.valid && now - a.tick <= STALE_MS;
                mode = mgs4vr_armsrc_pick_hand(have_latest ? &latest : NULL, anchored && a.time && find_hand(a.time, &paired) ? &paired : NULL,
                                               have_good ? &good : NULL, now, &h);
                if (have_latest) { DWORD age = now - latest.tick; if (age < 16) InterlockedIncrement(&age_hist[0]); else if (age < 33) InterlockedIncrement(&age_hist[1]); else if (age < 66) InterlockedIncrement(&age_hist[2]); else if (age <= STALE_MS) InterlockedIncrement(&age_hist[3]); else InterlockedIncrement(&age_hist[4]); }
                if (mode == PICK_NONE) {
                    if (!have_latest) InterlockedIncrement(&stale_noread); else if (!latest.valid) InterlockedIncrement(&stale_invalid); else InterlockedIncrement(&stale_old);
                    InterlockedIncrement(&rejects[R_STALE]); have_good = 0; reset_follow(); return;
                }
                if (mode == PICK_PAIRED) InterlockedIncrement(&hand_paired); else if (mode == PICK_LATEST) InterlockedIncrement(&hand_unpaired); else InterlockedIncrement(&hand_held);
                if (mode != PICK_HOLD) { good = h; have_good = 1; }
                {   /* pose tool "freeze" (phase 2): both controllers stay where they were (room space) while you look and tune */
                    static HAND_VIEW frozen; static int have_frozen;
                    if (mgs4vr_poses_freeze()) { if (!have_frozen) { frozen = h; have_frozen = 1; } h = frozen; InterlockedIncrement(&pose_frozen_frames); }
                    else have_frozen = 0;
                }
            }
            /* Stereo: controller relative to the eye the anchored camera was built from.
               Mono / no fresh anchor: relative to the head, placed with the live camera. */
            if (a.valid && GetTickCount() - a.tick <= STALE_MS && !pcam) {
                ref_q = a.eye_q; ref_p = a.eye_p; memcpy(C, a.C, sizeof(C)); InterlockedIncrement(&anchor_used);
            } else {
                ref_q = h.head_q; ref_p = h.head_p; InterlockedIncrement(&anchor_fallback);
            }
            { WP_PROFILE profile; LONG generation; static LONG last_generation;static unsigned last_id;static int last_source;
              WI_IDENTITY identity;unsigned id=wi_resolve(wp_game_read,NULL,(uint64_t)(uintptr_t)image,&identity)?identity.id:0;
              int selected=wp_snapshot_id(&profile,&generation,id);
              /* A contended publication skips this frame rather than mixing profiles. */
              if(selected<0){reset_follow();return;}
              if(generation!=last_generation||id!=last_id||selected!=last_source){reset_follow();last_generation=generation;last_id=id;last_source=selected;}
              if(selected&&code==IK_CODE){if(profile.hand_mode==1)code=IK_FREE;else if(profile.hand_mode==2)code=IK_RIGHT;}
              controller_target(ref_q, ref_p, h.grip_q, h.grip_p, selected?profile.O:GRIP_O, selected?profile.P:GRIP_P, C, target); }
            { MGS4VR_POSE pz; if (mgs4vr_poses_get(pose_weapon_code(), 0, MGS4VR_POSE_GRIP, &pz)) { mgs4vr_armsrc_pose_offset(&pz, target); rpose = pz; have_rpose = 1; InterlockedIncrement(&pose_hits[0]); } }   /* pose tool */
            if (!finite16(target)) { InterlockedIncrement(&rejects[R_FINITE]); reset_follow(); return; }
            mgs4vr_armsrc_shoulder_vector(root, (float)shoulder_back_mm, shoulder_v);
            if (shoulder_v[0] || shoulder_v[1] || shoulder_v[2]) { shift_bones(in, mask_rc, shoulder_v); InterlockedIncrement(&shoulder_shifts); }
            if (mgs4vr_armsrc_solve(in[ARM_ROOT], in[ELBOW], in[HAND], target, n10, n11, n12)) InterlockedIncrement(&ik_clamped);
            shared_blend = twohand_prepare(n12, (const float (*)[16])old, code, &h, ref_q, ref_p, C);
            memcpy(in[ARM_ROOT], n10, 64); memcpy(in[ELBOW], n11, 64); memcpy(in[HAND], n12, 64);
            for (i = ARM_ROOT; i <= HAND; ++i) { memcpy(last_out[i], in[i], 64); have_last[i] = 1; }
            written = 3;
            follow_descendants(in, (const float (*)[16])old, mask, ARM_ROOT, ELBOW, HAND, HAND, &written, &kept);
            if (have_rpose) pose_fingers_fresh(in, (const float (*)[16])old, HAND, finger_r, &rpose, 1.f, 0);
            memcpy(pc_hand, in[HAND], sizeof(pc_hand)); pc_tick = GetTickCount();   /* pose camera target frame */
            if (code != IK_RIGHT) left_arm(in, (const float (*)[16])old, code, &h, ref_q, ref_p, C, shared_blend, &written, &kept);
            else blend_init = 0;
        } else {
            int axis = code / 1000; float deg = (float)(code % 1000 - 360), g[16], shoulder[3];
            memcpy(shoulder, &in[ARM_ROOT][12], sizeof(shoulder));
            world_rotation(root, shoulder, axis, deg, g);
            for (i = 0; i < BONES; ++i) {
                if (!mask[i]) continue;
                if (have_last[i] && !memcmp(in[i], last_out[i], sizeof(last_out[i]))) { ++kept; continue; }
                mul(in[i], g, in[i]);
                memcpy(last_out[i], in[i], sizeof(last_out[i])); have_last[i] = 1; ++written;
            }
        }
        InterlockedExchangeAdd(&bones_written, written); InterlockedExchangeAdd(&bones_kept, kept);
        InterlockedIncrement(&writes);
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&rejects[R_FAULT]); reset_follow(); }
}
static void __fastcall wrapper(void *obj) {
    LONG code = mode; static LONG previous_code;
    if (code != previous_code) { reset_follow(); previous_code = code; }
    InterlockedIncrement(&active); InterlockedIncrement(&calls);
    __try { native(obj); if (code && installed) apply((BYTE *)obj, code); else reset_follow(); }
    __finally { InterlockedDecrement(&active); }
}

/* ------------------------------------------------------------- install --- */
static LONG CALLBACK trap(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT || !site || e->ExceptionRecord->ExceptionAddress != site) return EXCEPTION_CONTINUE_SEARCH;
    __try { *(ULONG64 *)(e->ContextRecord->Rsp - 8) = (ULONG64)(site + 5); } __except (EXCEPTION_EXECUTE_HANDLER) { return EXCEPTION_CONTINUE_SEARCH; }
    InterlockedIncrement(&traps);
    e->ContextRecord->Rsp -= 8; e->ContextRecord->Rip = (ULONG64)wrapper; return EXCEPTION_CONTINUE_EXECUTION;
}
static int patch(char expected, char desired) {
    DWORD old, tmp; char before; if (!VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &old)) return 0;
    before = _InterlockedCompareExchange8((volatile char *)site, desired, expected);
    FlushInstructionCache(GetCurrentProcess(), site, 1); VirtualProtect(site, 1, old, &tmp); return before == expected;
}
static int site_state(void) {
    __try { if (memcmp(site + 1, call_sig + 1, 4)) return -1; return site[0] == 0xcc ? 1 : site[0] == 0xe8 ? 0 : -1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static int signatures(BYTE *b) {
    __try { return !memcmp(b + CALL_RVA, call_sig, 5) && !memcmp(b + FN_RVA, fn_sig, sizeof(fn_sig)) && !memcmp(b + 0x8da180, accessor_sig, sizeof(accessor_sig)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static LONG read_mode(void) {
    FILE *f; char v[128] = { 0 }, ax = 0; int deg = 0; size_t n; char *p = v;
    if (fopen_s(&f, marker, "rb")) return 0;
    n = fread(v, 1, sizeof(v) - 1, f); fclose(f); v[n] = 0;
    if (n >= 3 && !memcmp(p, "\xef\xbb\xbf", 3)) p += 3;
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p || *p == '\r' || *p == '\n') return 45 + 360;
    if (!strncmp(p, "off", 3)) return 0;
    { const char *q; int t;   /* transition times, any mode */
      InterlockedExchange(&grab_ms, (q = strstr(p, "grab=")) && sscanf_s(q + 5, "%d", &t) == 1 && t >= 0 && t <= 2000 ? t : GRAB_MS);
      InterlockedExchange(&release_ms, (q = strstr(p, "release=")) && sscanf_s(q + 8, "%d", &t) == 1 && t >= 0 && t <= 2000 ? t : RELEASE_MS);
      InterlockedExchange(&shoulder_back_mm, (q = strstr(p, "shoulderback=")) && sscanf_s(q + 13, "%d", &t) == 1 && t >= 0 && t <= SHOULDER_MAX_CM ? t * 10 : 0); }
    if (!strncmp(p, "ik", 2) && (p[2] == 0 || p[2] == '\r' || p[2] == '\n' || p[2] == ' ')) {
        static const char *const sub[] = { "grip", "free", "right" }; int k; size_t L;
        for (p += 2; *p == ' ' || *p == '\t'; ++p) {}
        if (!*p || *p == '\r' || *p == '\n' || memchr(p, '=', strcspn(p, " \t\r\n"))) return IK_CODE;   /* plain ik, or ik followed by grab=/release= */
        for (k = 0; k < 3; ++k) { L = strlen(sub[k]); if (!strncmp(p, sub[k], L) && (p[L] == 0 || p[L] == '\r' || p[L] == '\n' || p[L] == ' ')) return IK_CODE + 1 + k; }
        return 0;
    }
    if (sscanf_s(p, "%c %d", &ax, 1, &deg) != 2 || deg < -180 || deg > 180 || !deg) return 0;
    return ax == 'x' ? deg + 360 : ax == 'y' ? 1000 + deg + 360 : ax == 'z' ? 2000 + deg + 360 : 0;
}
static void write_status(const char *event) {
    FILE *f; LONG code = mode; float pcm[3] = { 0 }, pch[3] = { 0 }, pcf[3] = { 0 }, z[3] = { 0, 0, 0 };
    if (pc_view[15] == 1.f) { float v0[16]; memcpy(v0, pc_view, sizeof(v0)); pc_rel(v0, pc_main + 12, pcm); pc_rel(v0, pc_hand + 12, pch); v0[12] = v0[13] = v0[14] = 0; pc_rel(v0, pc_main + 8, pcf); (void)z; }
    if (fopen_s(&f, status_path, "w")) return;
    if (IS_IK(code)) fprintf(f, "armsrc_version=6 event=%s installed=%ld mode=%s elapsed_ms=%lu\n", event, installed,
                             code == IK_GRIP ? "ik_grip" : code == IK_FREE ? "ik_free" : code == IK_RIGHT ? "ik_right" : "ik", GetTickCount() - started);
    else fprintf(f, "armsrc_version=6 event=%s installed=%ld mode=%s axis=%c deg=%ld elapsed_ms=%lu\n", event, installed, code ? "on" : "off",
                 code ? "xyz"[code / 1000] : '-', code ? code % 1000 - 360 : 0L, GetTickCount() - started);
    fprintf(f, "calls=%ld traps=%ld player_calls=%ld writes=%ld bones_written=%ld bones_kept=%ld subtree=%d hand_samples=%ld ik_clamped=%ld\n",
            calls, traps, player_calls, writes, bones_written, bones_kept, subtree_n, hand_samples, ik_clamped);
    fprintf(f, "shoulder_back_mm=%ld shoulder_shifts=%ld pose_hits=%ld,%ld pose_finger_frames=%ld,%ld pose_frozen_frames=%ld pi_state=%d pi_which=%d pi_lp=%.0f,%.0f,%.0f pi_squeeze=%.2f slide_writes=%ld pose_cam_frames=%ld pc_main_in_view=%.0f,%.0f,%.0f pc_hand_in_view=%.0f,%.0f,%.0f pc_main_fwd=%.3f,%.3f,%.3f\n", shoulder_back_mm, shoulder_shifts, pose_hits[0], pose_hits[1], pose_finger_frames[0], pose_finger_frames[1], pose_frozen_frames, pi.state, pi.which, pi_lp[0], pi_lp[1], pi_lp[2], pi_sq, slide_writes, pc_frames, pcm[0], pcm[1], pcm[2], pch[0], pch[1], pch[2], pcf[0], pcf[1], pcf[2]);
    fprintf(f, "anchor_samples=%ld anchor_used=%ld anchor_fallback=%ld\n", anchor_samples, anchor_used, anchor_fallback);
    fprintf(f, "stale_noread=%ld stale_invalid=%ld stale_old=%ld hand_age_ms=%ld,%ld,%ld,%ld,%ld hand_paired=%ld hand_unpaired=%ld hand_held=%ld\n", stale_noread, stale_invalid, stale_old, age_hist[0], age_hist[1], age_hist[2], age_hist[3], age_hist[4], hand_paired, hand_unpaired, hand_held);
    fprintf(f, "grab_ms=%ld release_ms=%ld left_subtree=%d left_writes=%ld left_grip_frames=%ld snap_enters=%ld snap_exits=%ld left_clamped=%ld left_protract=%ld left_no_ctrl=%ld left_no_grip=%ld\n",
            grab_ms, release_ms, subtree_l, left_writes, left_grip_frames, snap_enters, snap_exits, left_clamped, left_protract, left_no_ctrl, left_no_grip);
    fprintf(f, "twohand_options=%ld twohand_allowed=%ld twohand_frames=%ld twohand_resets=%ld\n", twohand_options, twohand_allowed, twohand_frames, twohand_resets);
    fprintf(f, "reject_not_player=%ld reject_rig=%ld reject_nonfinite=%ld reject_not_fps=%ld reject_stale=%ld faults=%ld integrity_lost=%d restored=%d\n",
            rejects[R_NOT_PLAYER], rejects[R_RIG], rejects[R_FINITE], rejects[R_NOT_FPS], rejects[R_STALE], rejects[R_FAULT], integrity_lost, restored);
    fprintf(f,"weapon_selection=source_%ld native_id=%ld generation=%ld profile_load_ok=%ld profile_load_bad=%ld manual_frames=%ld auto_frames=%ld fallback_frames=%ld busy_frames=%ld\n",
        wp_last_source,wp_last_id,wp_generation,wp_load_ok,wp_load_bad,wp_known_frames,wp_auto_frames,wp_unknown_frames,wp_busy_frames);
    fclose(f);
}
static void uninstall(const char *why) {
    InterlockedExchange(&mode, 0);
    if (installed) { if (site_state() == 1) patch((char)0xcc, (char)0xe8); restored = site_state() == 0; InterlockedExchange(&installed, 0); }
    write_status(why); attempted = 0;
}
void mgs4vr_armsrc_poll(void *base, const char *exe) {
    DWORD now = GetTickCount(); char *leaf;
    wp_poll(exe);
    if (!marker[0]) {
        strcpy_s(marker, sizeof(marker), exe); leaf = strrchr(marker, '\\'); if (!leaf) { marker[0] = 0; return; }
        strcpy_s(leaf + 1, sizeof(marker) - (size_t)(leaf + 1 - marker), "mgs4vr_armsrc.on");
        strcpy_s(status_path, sizeof(status_path), marker);
        sprintf_s(strrchr(status_path, '\\') + 1, 64, "mgs4vr_armsrc_%lu.txt", GetCurrentProcessId());
    }
    if (!attempted) {
        if (GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES) return;
        attempted = 1; started = now; image = (BYTE *)base; site = image + CALL_RVA; native = (NATIVE)(image + FN_RVA);
        if (!signatures(image)) { write_status("reject_signature"); return; }
        if (!veh) veh = AddVectoredExceptionHandler(1, trap);
        if (!veh) { write_status("reject_veh"); return; }
        InterlockedExchange(&mode, read_mode());
        InterlockedExchange(&installed, 1);
        if (!patch((char)0xe8, (char)0xcc)) { InterlockedExchange(&installed, 0); write_status("reject_patch"); return; }
        restored = 0; integrity_lost = 0; poll_at = status_at = now; write_status("installed");
        return;
    }
    if (!installed) { if (GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES) attempted = 0; return; }
    if (now - poll_at >= 250) {
        poll_at = now;
        if (site_state() != 1) { integrity_lost = 1; uninstall("integrity_lost"); return; }
        if (GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES) { uninstall("marker_removed"); return; }
        InterlockedExchange(&mode, read_mode());
    }
    if (now - status_at >= 1000) { status_at = now; write_status("heartbeat"); }
}
int mgs4vr_armsrc_active(void) { return installed || active; }
int mgs4vr_armsrc_left_on_weapon(void) { return snapped; }
