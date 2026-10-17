/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
/* VR SETTINGS row in the game's Options screen; see mgs4vr_vrmenu.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "mgs4vr_vrmenu.h"

#define VM_STATE_RVA 0x4d40a0
#define VM_TEXT_RVA 0x432790
static const BYTE vm_state_sig[] = { 0x33, 0xc0, 0x89, 0x51, 0x10, 0x48, 0x89, 0x41, 0x30, 0xc3 };   /* xor eax,eax; mov [rcx+10],edx; mov [rcx+30],rax; ret */
static const BYTE vm_text_sig[] = { 0x89, 0x4c, 0x24, 0x08, 0x4c, 0x8b, 0xdc, 0x56, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x60 };   /* mov [rsp+8],ecx; ... */

/* ---- texts (English: the game's menus are English) ---- */
enum { T_ROW = MGS4VR_VM_ID0, T_ROW_D, T_ROW_V,
       T_TWOEYE = 0x7e10, T_TWOEYE_D, T_REDDOT, T_REDDOT_D,
       T_TURN = 0x7e20, T_TURN_D, T_TURNSPEED, T_TURNSPEED_D, T_SNAP, T_SNAP_D,
       T_GRIP = 0x7e30, T_GRIP_D,
       T_CQCFP = 0x7e40, T_CQCFP_D, T_CAMO, T_CAMO_D,
       T_SMOOTH = 0x7e50, T_SNAPC, T_S15, T_S30, T_S45, T_S90, T_FAST, T_NORMAL, T_SLOW,
       T_STEREO = 0x7e60, T_STEREO_D, T_FOLLOW, T_FOLLOW_D, T_DIST, T_DIST_D, T_OBS, T_OBS_D,
       T_NO = 0x7e70, T_YES, T_OBS_OFF, T_OBS_RIGHT, T_OBS_BOTH,
       T_D0 = 0x7e80 /* .. T_D0 + 9: screen distances */,
       T_HUD = 0x7e90, T_HUD_D,
       T_SPR = 0x7ea0, T_SPR_D, T_SPR_BOTH, T_SPR_LEFT,
       T_SHOULDER = 0x7eb0, T_SHOULDER_D, T_SHOULDER_HI };
#define T_OFF 0x9f   /* the game's own OFF / ON / SLOW / FAST (sub 0x101AB9) */
#define T_ON 0xa0
#define T_GSLOW 0x8f
#define T_GFAST 0x90
static const struct { unsigned short id; const char *s; } vm_str[] = {
    { T_ROW, "VR Settings" }, { T_ROW_D, "Settings of the VR mod: two eyes, turning, weapons and actions. Back returns to the Options." }, { T_ROW_V, "" },
    { T_TWOEYE, "Two Eye Rendering" }, { T_TWOEYE_D, "Render a separate picture for each eye (real depth). Off: one picture for both eyes." },
    { T_REDDOT, "Red Dot Sight" }, { T_REDDOT_D, "Show the red dot on a mounted dot sight in VR." },
    { T_TURN, "Turning" }, { T_TURN_D, "Right stick turning: smooth, or in fixed steps (snap)." },
    { T_TURNSPEED, "Turn Speed" }, { T_TURNSPEED_D, "Speed of smooth turning (30 degrees per second per step)." },
    { T_SNAP, "Snap Angle" }, { T_SNAP_D, "Degrees per step when turning is set to snap." },
    { T_GRIP, "Two-Hand Grip Speed" }, { T_GRIP_D, "How fast the free hand moves onto the weapon and lets go again." },
    { T_CQCFP, "CQC in First Person" }, { T_CQCFP_D, "Stay in first person while grabbing an enemy (CQC)." },
    { T_CAMO, "Camo Shake" }, { T_CAMO_D, "Shake both controllers to reset the OctoCamo." },
    { T_SMOOTH, "SMOOTH" }, { T_SNAPC, "SNAP" }, { T_S15, "15" }, { T_S30, "30" }, { T_S45, "45" }, { T_S90, "90" },
    { T_FAST, "FAST" }, { T_NORMAL, "NORMAL" }, { T_SLOW, "SLOW" },
    { T_STEREO, "Stereo" }, { T_STEREO_D, "On: the game in 3D around you. Off: a flat theater screen in front of you." },
    { T_FOLLOW, "Camera Follows Headset" }, { T_FOLLOW_D, "The game camera turns with your head on the theater screen (always on in stereo)." },
    { T_DIST, "Screen Distance" }, { T_DIST_D, "Distance of the theater screen (menus, cutscenes and the flat view)." },
    { T_OBS, "Desktop Observer" }, { T_OBS_D, "What the PC monitor shows while you play in the headset." },
    { T_NO, "NO" }, { T_YES, "YES" }, { T_OBS_OFF, "OFF" }, { T_OBS_RIGHT, "RIGHT EYE" }, { T_OBS_BOTH, "BOTH EYES" },
    { T_HUD, "HUD" }, { T_HUD_D, "Life, camo, weapon and item panels. Off: Solid Eye labels stay; weapon and item are on your wrists." },
    { T_SPR, "Light Sprites" }, { T_SPR_D, "Two Eye Rendering: light/dust sprites. Both eyes can show a ghost in the right eye; left eye only or off removes it." },
    { T_SPR_BOTH, "BOTH EYES" }, { T_SPR_LEFT, "LEFT EYE" },
    { T_SHOULDER_HI, "15 cm" }, { T_SHOULDER, "Shoulder Offset" }, { T_SHOULDER_D, "First person: moves Snake's upper body back (cm) so his shoulders stay out of your view. 0 = off." },
    { T_D0, "1.0 m" }, { T_D0 + 1, "1.5 m" }, { T_D0 + 2, "2.0 m" }, { T_D0 + 3, "2.5 m" }, { T_D0 + 4, "3.0 m" },
    { T_D0 + 5, "3.5 m" }, { T_D0 + 6, "4.0 m" }, { T_D0 + 7, "5.0 m" }, { T_D0 + 8, "6.0 m" }, { T_D0 + 9, "8.0 m" },
};
static const short vm_dist_cm[10] = { 100, 150, 200, 250, 300, 350, 400, 500, 600, 800 };
static const char *vm_tabname[4] = { "VR DISPLAY", "VR TURNING", "VR WEAPONS", "VR ACTIONS" };
const char *mgs4vr_vm_text(unsigned sub, unsigned id, int vr) {
    size_t i;
    if (sub != MGS4VR_VM_SUB) return NULL;
    id &= 0xffffff;
    if (vr && id < 4) return vm_tabname[id];
    if (id < MGS4VR_VM_ID0 || id > MGS4VR_VM_ID1) return NULL;
    for (i = 0; i < sizeof(vm_str) / sizeof(vm_str[0]); ++i) if (vm_str[i].id == id) return vm_str[i].s;
    return NULL;
}

/* ---- tables ---- */
static const short vm_snap_deg[4] = { 15, 30, 45, 90 };
static MGS4VR_VM_ITEM vm_t0[8], vm_t1[3], vm_t2[2], vm_t3[2];
static void vm_choice(short *it, short label, short desc, int n, const short *ch, short v) {
    int i; memset(it, 0, sizeof(MGS4VR_VM_ITEM));
    it[0] = 3; it[1] = label; it[2] = desc; it[5] = (short)n; for (i = 0; i < n && i < 10; ++i) it[6 + i] = ch[i];
    it[16] = it[17] = (short)(v >= 0 && v < n ? v : 0);
}
static void vm_slider(short *it, short label, short desc, short lo, short hi, short v) {
    memset(it, 0, sizeof(MGS4VR_VM_ITEM));
    it[0] = 4; it[1] = label; it[2] = desc; it[3] = lo; it[4] = hi; it[6] = T_GSLOW; it[7] = T_GFAST;
    it[16] = it[17] = (short)(v < lo ? lo : v > hi ? hi : v);
}
void mgs4vr_vm_tables(MGS4VR_VM_ITEM *tabs[4], int counts[4], const short v[VM_N]) {
    static const short onoff[2] = { T_OFF, T_ON }, turn[2] = { T_SMOOTH, T_SNAPC }, snap[4] = { T_S15, T_S30, T_S45, T_S90 }, grip[3] = { T_FAST, T_NORMAL, T_SLOW };
    static const short noyes[2] = { T_NO, T_YES }, obs[3] = { T_OBS_OFF, T_OBS_RIGHT, T_OBS_BOTH };
    short dist[10]; int i; for (i = 0; i < 10; ++i) dist[i] = (short)(T_D0 + i);
    vm_choice(vm_t0[0], T_STEREO, T_STEREO_D, 2, onoff, v[VM_STEREO]);
    vm_choice(vm_t0[1], T_TWOEYE, T_TWOEYE_D, 2, onoff, v[VM_TWOEYE]);
    vm_choice(vm_t0[2], T_FOLLOW, T_FOLLOW_D, 2, noyes, v[VM_FOLLOW]);
    vm_choice(vm_t0[3], T_DIST, T_DIST_D, 10, dist, v[VM_DISTANCE]);
    vm_choice(vm_t0[4], T_OBS, T_OBS_D, 3, obs, v[VM_OBSERVER]);
    vm_choice(vm_t0[5], T_HUD, T_HUD_D, 2, onoff, v[VM_HUD]);
    { static const short spr[3] = { T_SPR_BOTH, T_SPR_LEFT, T_OFF }; vm_choice(vm_t0[6], T_SPR, T_SPR_D, 3, spr, v[VM_SPRITES]); }
    vm_slider(vm_t0[7], T_SHOULDER, T_SHOULDER_D, 0, MGS4VR_VM_SHOULDER_MAX, v[VM_SHOULDER]); vm_t0[7][6] = T_OFF; vm_t0[7][7] = T_SHOULDER_HI;
    vm_choice(vm_t1[0], T_TURN, T_TURN_D, 2, turn, v[VM_TURN]);
    vm_slider(vm_t1[1], T_TURNSPEED, T_TURNSPEED_D, 1, 10, v[VM_TURNSPEED]);
    vm_choice(vm_t1[2], T_SNAP, T_SNAP_D, 4, snap, v[VM_SNAP]);
    vm_choice(vm_t2[0], T_REDDOT, T_REDDOT_D, 2, onoff, v[VM_REDDOT]);
    vm_choice(vm_t2[1], T_GRIP, T_GRIP_D, 3, grip, v[VM_GRIP]);
    vm_choice(vm_t3[0], T_CQCFP, T_CQCFP_D, 2, onoff, v[VM_CQCFP]);
    vm_choice(vm_t3[1], T_CAMO, T_CAMO_D, 2, onoff, v[VM_CAMO]);
    tabs[0] = vm_t0; counts[0] = 8; tabs[1] = vm_t1; counts[1] = 3; tabs[2] = vm_t2; counts[2] = 2; tabs[3] = vm_t3; counts[3] = 2;
}
void mgs4vr_vm_read(short v[VM_N]) {
    v[VM_STEREO] = vm_t0[0][16]; v[VM_TWOEYE] = vm_t0[1][16]; v[VM_FOLLOW] = vm_t0[2][16]; v[VM_DISTANCE] = vm_t0[3][16]; v[VM_OBSERVER] = vm_t0[4][16]; v[VM_HUD] = vm_t0[5][16]; v[VM_SPRITES] = vm_t0[6][16]; v[VM_SHOULDER] = vm_t0[7][16];
    v[VM_TURN] = vm_t1[0][16]; v[VM_TURNSPEED] = vm_t1[1][16]; v[VM_SNAP] = vm_t1[2][16];
    v[VM_REDDOT] = vm_t2[0][16]; v[VM_GRIP] = vm_t2[1][16]; v[VM_CQCFP] = vm_t3[0][16]; v[VM_CAMO] = vm_t3[1][16];
}
/* ---- theater settings (Home menu / mgs4vr.ini) ---- */
void mgs4vr_vm_theater_values(const MGS4VR_VM_THEATER *t, short v[VM_N]) {
    int i, best = 0;
    v[VM_STEREO] = (short)(t->stereo != 0); v[VM_FOLLOW] = (short)(t->follow != 0 || t->stereo != 0);   /* the Home menu shows "Yes (stereo)" */
    for (i = 1; i < 10; ++i) if (abs(vm_dist_cm[i] - t->distance_cm) < abs(vm_dist_cm[best] - t->distance_cm)) best = i;
    v[VM_DISTANCE] = (short)best; v[VM_OBSERVER] = (short)(t->observer >= 0 && t->observer <= 2 ? t->observer : 2);
}
/* The Home menu's rules (mgs4vr_menu_key): follow only when available and not in stereo; stereo only when available;
   a follow or stereo change recenters. Distance changes only when the menu value is not the nearest of the current one. */
int mgs4vr_vm_apply_theater(const MGS4VR_VM_THEATER *t, const short v[VM_N], MGS4VR_VM_THEATER *out, int *recenter) {
    short cur[VM_N]; int changed = 0;
    *out = *t; if (recenter) *recenter = 0;
    mgs4vr_vm_theater_values(t, cur);
    if (v[VM_STEREO] != cur[VM_STEREO] && t->stereo_available && (v[VM_STEREO] == 0 || v[VM_STEREO] == 1)) { out->stereo = v[VM_STEREO]; changed = 1; if (recenter) *recenter = 1; }
    if (v[VM_FOLLOW] != cur[VM_FOLLOW] && t->available && !out->stereo && (v[VM_FOLLOW] == 0 || v[VM_FOLLOW] == 1)) { out->follow = v[VM_FOLLOW]; changed = 1; if (recenter) *recenter = 1; }
    if (v[VM_DISTANCE] != cur[VM_DISTANCE] && v[VM_DISTANCE] >= 0 && v[VM_DISTANCE] < 10) { out->distance_cm = vm_dist_cm[v[VM_DISTANCE]]; changed = 1; }
    if (v[VM_OBSERVER] != cur[VM_OBSERVER] && v[VM_OBSERVER] >= 0 && v[VM_OBSERVER] <= 2) { out->observer = v[VM_OBSERVER]; changed = 1; }
    return changed;
}
static MGS4VR_VM_THEATER vm_th, vm_th_new; static int vm_th_have, vm_th_pending, vm_th_recenter;   /* worker thread only */
void mgs4vr_vrmenu_set_theater(const MGS4VR_VM_THEATER *t) { if (t) { vm_th = *t; vm_th_have = 1; } }
int mgs4vr_vrmenu_take_theater(MGS4VR_VM_THEATER *t) { int r; if (!vm_th_pending || !t) return 0; *t = vm_th_new; vm_th_pending = 0; r = 1 + vm_th_recenter; vm_th_recenter = 0; return r; }
int mgs4vr_vm_inject(const MGS4VR_VM_ITEM *orig, int n, MGS4VR_VM_ITEM *out, int cap) {
    if (!orig || !out || n < 1 || n + 1 > cap) return 0;
    memset(out[0], 0, sizeof(MGS4VR_VM_ITEM));
    out[0][0] = MGS4VR_VM_TYPE; out[0][1] = T_ROW; out[0][2] = T_ROW_D; out[0][5] = 1; out[0][6] = T_ROW_V;   /* one empty choice: no "0" on the right */
    memcpy(out + 1, orig, (size_t)n * sizeof(MGS4VR_VM_ITEM));
    return n + 1;
}

/* ---- markers <-> values ---- */
static const char *vm_names[VM_N] = { "mgs4vr_twoeye.on", "mgs4vr_lens.on", "mgs4vr_pad.on", "mgs4vr_pad.on", "mgs4vr_pad.on", "mgs4vr_armsrc.on", "mgs4vr_cqcfp.on", "mgs4vr_camo.on", "mgs4vr_hudhide.on", "mgs4vr_twoeye.on", "mgs4vr_armsrc.on" };
const char *mgs4vr_vm_marker_name(int k) { return k >= 0 && k < VM_NMARK ? vm_names[k] : NULL; }
static const char *vm_skip(const char *p) { if (!p) return NULL; if ((unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf) p += 3; while (*p == ' ' || *p == '\t') ++p; return p; }
static int vm_on(const char *t) { t = vm_skip(t); return t && !strncmp(t, "on", 2) && (t[2] == 0 || t[2] == ' ' || t[2] == '\r' || t[2] == '\n' || t[2] == '\t'); }
static int vm_int(const char *t, const char *key, int def) { const char *q; int v; if (t && (q = strstr(t, key)) && sscanf_s(q + strlen(key), "%d", &v) == 1) return v; return def; }
void mgs4vr_vm_values(const MGS4VR_VM_MARKERS *m, short v[VM_N]) {
    int d, i, best = 1, g;
    v[VM_TWOEYE] = (short)vm_on(m->twoeye);
    v[VM_REDDOT] = (short)(m->lens && vm_on(m->lens) && !strstr(m->lens, "nodot"));
    v[VM_TURN] = (short)(m->pad && strstr(m->pad, "turn=snap") != NULL);
    d = vm_int(m->pad, "turndeg=", 120); d = (d + 15) / 30; v[VM_TURNSPEED] = (short)(d < 1 ? 1 : d > 10 ? 10 : d);
    d = vm_int(m->pad, "snapdeg=", 30);
    for (i = 0; i < 4; ++i) if (abs(vm_snap_deg[i] - d) < abs(vm_snap_deg[best] - d)) best = i;
    v[VM_SNAP] = (short)best;
    g = vm_int(m->armsrc, "grab=", 180); v[VM_GRIP] = (short)(g <= 150 ? 0 : g >= 260 ? 2 : 1);
    v[VM_CQCFP] = (short)vm_on(m->cqcfp); v[VM_CAMO] = (short)vm_on(m->camo);
    v[VM_HUD] = (short)!vm_on(m->hudhide);                                 /* 1 = HUD shown (marker hudhide off/missing) */
    d = vm_int(m->twoeye, "spr64=", 2); v[VM_SPRITES] = (short)(d == 0 || d == 1 ? d : 2);
    d = vm_int(m->armsrc, "shoulderback=", 0); v[VM_SHOULDER] = (short)(d < 0 ? 0 : d > MGS4VR_VM_SHOULDER_MAX ? MGS4VR_VM_SHOULDER_MAX : d);   /* 0 both eyes, 1 left eye only, 2 off (default, also when absent) */
}
/* Replace (or append) "key<value>" as a whole token; value NULL removes the token. */
static void vm_token(char *b, size_t cap, const char *key, const char *value) {
    char *q = b, *e; size_t kl = strlen(key);
    while ((q = strstr(q, key)) != NULL) { if (q == b || q[-1] == ' ' || q[-1] == '\t') break; q += kl; }
    if (q) {   /* cut the old token and the space before it */
        e = q; while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') ++e;
        if (q > b && q[-1] == ' ') --q;
        memmove(q, e, strlen(e) + 1);
    }
    if (value) { size_t n = strlen(b); while (n && (b[n - 1] == '\r' || b[n - 1] == '\n' || b[n - 1] == ' ')) b[--n] = 0; if (n + 1 + kl + strlen(value) + 1 <= cap) { if (n) strcat_s(b, cap, " "); strcat_s(b, cap, key); strcat_s(b, cap, value); } }
}
/* First word on/off, the rest kept. */
static void vm_word(char *b, size_t cap, const char *old, int on) {
    const char *p = vm_skip(old ? old : ""), *rest = p;
    if (!strncmp(p, "on", 2) && (p[2] == 0 || p[2] == ' ' || p[2] == '\r' || p[2] == '\n')) rest = p + 2;
    else if (!strncmp(p, "off", 3) && (p[3] == 0 || p[3] == ' ' || p[3] == '\r' || p[3] == '\n')) rest = p + 3;
    else rest = p + strlen(p);   /* unknown text: replaced */
    while (*rest == ' ') ++rest;
    sprintf_s(b, cap, "%s%s%s", on ? "on" : "off", *rest && *rest != '\r' && *rest != '\n' ? " " : "", *rest != '\r' && *rest != '\n' ? rest : "");
    { size_t n = strlen(b); while (n && (b[n - 1] == '\r' || b[n - 1] == '\n' || b[n - 1] == ' ')) b[--n] = 0; }
}
int mgs4vr_vm_marker_text(int k, short v, const char *old, char *out, size_t cap) {
    char num[16];
    if (!out || cap < 64) return 0;
    switch (k) {
    case VM_HUD:
        if (v < 0 || v > 1) return 0; vm_word(out, cap, old, !v); return 1;   /* inverted: HUD ON = hudhide off */
    case VM_SHOULDER:
        if (v < 0 || v > MGS4VR_VM_SHOULDER_MAX) return 0;
        strcpy_s(out, cap, old && *vm_skip(old) ? vm_skip(old) : "ik");
        sprintf_s(num, sizeof(num), "%d", v); vm_token(out, cap, "shoulderback=", v ? num : NULL);
        { size_t n = strlen(out); while (n && (out[n - 1] == '\r' || out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0; }
        return 1;
    case VM_SPRITES:
        if (v < 0 || v > 2) return 0;
        strcpy_s(out, cap, old && *vm_skip(old) ? vm_skip(old) : "off");
        vm_token(out, cap, "spr64=", v == 0 ? "0" : v == 1 ? "1" : NULL); vm_token(out, cap, "spr64k=", NULL);
        { size_t n = strlen(out); while (n && (out[n - 1] == '\r' || out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0; }
        return 1;
    case VM_TWOEYE: case VM_CQCFP: case VM_CAMO:
        if (v < 0 || v > 1) return 0; vm_word(out, cap, old, v); return 1;
    case VM_REDDOT:
        if (v < 0 || v > 1) return 0;
        if (!old || !vm_on(old)) return 0;   /* the lens itself off: nothing to set */
        strcpy_s(out, cap, vm_skip(old)); vm_token(out, cap, "nodot", v ? NULL : ""); return 1;
    case VM_TURN: case VM_TURNSPEED: case VM_SNAP:
        strcpy_s(out, cap, old ? vm_skip(old) : "wasd");
        if (k == VM_TURN) { if (v < 0 || v > 1) return 0; vm_token(out, cap, "turn=", v ? "snap" : "smooth"); }
        else if (k == VM_TURNSPEED) { if (v < 1 || v > 10) return 0; sprintf_s(num, sizeof(num), "%d", v * 30); vm_token(out, cap, "turndeg=", num); }
        else { if (v < 0 || v > 3) return 0; sprintf_s(num, sizeof(num), "%d", vm_snap_deg[v]); vm_token(out, cap, "snapdeg=", num); }
        { size_t n = strlen(out); while (n && (out[n - 1] == '\r' || out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0; }
        return 1;
    case VM_GRIP:
        if (v < 0 || v > 2) return 0;
        strcpy_s(out, cap, old && *vm_skip(old) ? vm_skip(old) : "ik");
        vm_token(out, cap, "grab=", v == 0 ? "120" : v == 2 ? "300" : NULL);
        vm_token(out, cap, "release=", v == 0 ? "150" : v == 2 ? "350" : NULL);
        { size_t n = strlen(out); while (n && (out[n - 1] == '\r' || out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0; }
        return 1;
    }
    return 0;
}

/* ---- seams (game thread) ---- */
static BYTE *vm_base, *vm_ss, *vm_ts; static PVOID vm_veh;
static volatile LONG vm_enabled, vm_text_armed, vm_vr, vm_faults, vm_injects, vm_enters, vm_leaves, vm_texts, vm_states, vm_text_ours;
static BYTE *vm_screen;                                            /* the Options screen we changed */
static MGS4VR_VM_ITEM vm_tab0[24]; static int vm_tab0_n;
static uint64_t vm_saved_ptr[4]; static int vm_saved_cnt[4], vm_saved_cur[4], vm_saved_first[4];
static short vm_cache[VM_N]; static volatile LONG vm_cache_ok;     /* marker values (worker), copied in on enter */
static volatile LONG vm_dirty;                                     /* VR values may differ from the markers */
static int vm_patch(BYTE *at, BYTE from, BYTE to) { DWORD old, tmp; char v; if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) return 0; v = _InterlockedCompareExchange8((volatile char *)at, to, from); FlushInstructionCache(GetCurrentProcess(), at, 1); VirtualProtect(at, 1, old, &tmp); return (BYTE)v == from; }
static void vm_text_arm(int on) {
    if (!vm_ts) return;
    if (on && !vm_text_armed) { if (vm_patch(vm_ts, vm_text_sig[0], 0xcc)) InterlockedExchange(&vm_text_armed, 1); }
    else if (!on && vm_text_armed) { if (vm_patch(vm_ts, 0xcc, vm_text_sig[0])) InterlockedExchange(&vm_text_armed, 0); }
}
static void vm_enter(BYTE *s) {
    MGS4VR_VM_ITEM *tabs[4]; int counts[4], t; short v[VM_N];
    if (vm_cache_ok) memcpy(v, vm_cache, sizeof(v)); else { memset(v, 0, sizeof(v)); v[VM_TURNSPEED] = 4; v[VM_SNAP] = 1; v[VM_GRIP] = 1; v[VM_DISTANCE] = 3; v[VM_OBSERVER] = 2; }
    mgs4vr_vm_tables(tabs, counts, v);
    for (t = 0; t < 4; ++t) {
        vm_saved_ptr[t] = *(uint64_t *)(s + 0x290 + 8 * t); vm_saved_cnt[t] = *(int *)(s + 0x27c + 4 * t);
        vm_saved_cur[t] = *(int *)(s + 0x25c + 4 * t); vm_saved_first[t] = *(int *)(s + 0x26c + 4 * t);
    }
    for (t = 0; t < 4; ++t) {
        *(uint64_t *)(s + 0x290 + 8 * t) = (uint64_t)tabs[t]; *(int *)(s + 0x27c + 4 * t) = counts[t];
        *(int *)(s + 0x25c + 4 * t) = 0; *(int *)(s + 0x26c + 4 * t) = 0;
    }
    InterlockedExchange(&vm_vr, 1); InterlockedIncrement(&vm_enters);
}
static void vm_leave(BYTE *s) {
    int t;
    for (t = 0; t < 4; ++t) {
        *(uint64_t *)(s + 0x290 + 8 * t) = vm_saved_ptr[t]; *(int *)(s + 0x27c + 4 * t) = vm_saved_cnt[t];
        *(int *)(s + 0x25c + 4 * t) = vm_saved_cur[t]; *(int *)(s + 0x26c + 4 * t) = vm_saved_first[t];
    }
    InterlockedExchange(&vm_vr, 0); InterlockedExchange(&vm_dirty, 1); InterlockedIncrement(&vm_leaves);
}
/* State set on screen s (rcx - 0x98) to st; returns the state to really set. */
static unsigned vm_state(BYTE *s, unsigned st) {
    uint64_t t0 = (uint64_t)(vm_base + MGS4VR_VM_TAB0_RVA), t1 = (uint64_t)(vm_base + MGS4VR_VM_TAB1_RVA);
    if (st == 1 && *(uint64_t *)(s + 0x290) == t0 && *(uint64_t *)(s + 0x298) == t1) {   /* fresh Options screen */
        int n = *(int *)(s + 0x27c);
        vm_text_arm(1);
        if (vm_text_armed && (vm_tab0_n = mgs4vr_vm_inject((const MGS4VR_VM_ITEM *)t0, n, vm_tab0, 24)) > 0) {
            *(uint64_t *)(s + 0x290) = (uint64_t)vm_tab0; *(int *)(s + 0x27c) = vm_tab0_n;
            vm_screen = s; InterlockedExchange(&vm_vr, 0); InterlockedIncrement(&vm_injects);
        }
        return st;
    }
    if (s != vm_screen) return st;
    {   /* still our screen: tab 0 holds our table (or, in VR, the VR table); else it was freed and reused */
        uint64_t p0 = *(uint64_t *)(s + 0x290);
        if (p0 != (uint64_t)vm_tab0 && p0 != (uint64_t)vm_t0) { vm_screen = NULL; InterlockedExchange(&vm_vr, 0); vm_text_arm(0); return st; }
    }
    if (st == MGS4VR_VM_TYPE) { if (!vm_vr) vm_enter(s); return 1; }
    if (st == 10 && vm_vr) { vm_leave(s); return 1; }
    if (st == 0xb) { vm_screen = NULL; if (vm_vr) InterlockedExchange(&vm_vr, 0); vm_text_arm(0); }   /* screen closing */
    return st;
}
static LONG CALLBACK vm_trap(EXCEPTION_POINTERS *e) {
    CONTEXT *c = e->ContextRecord; void *at = e->ExceptionRecord->ExceptionAddress;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    if (vm_ss && at == vm_ss) {
        unsigned st = (unsigned)c->Rdx;
        InterlockedIncrement(&vm_states);
        if (vm_enabled) {
            __try { st = vm_state((BYTE *)(c->Rcx - 0x98), st); }
            __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&vm_faults); }
        }
        c->Rdx = (c->Rdx & ~0xffffffffULL) | st;
        c->Rax = 0; c->Rip = (DWORD64)vm_ss + 2; return EXCEPTION_CONTINUE_EXECUTION;   /* xor eax,eax */
    }
    if (vm_ts && at == vm_ts) {
        const char *s = NULL;
        InterlockedIncrement(&vm_texts);
        if (vm_enabled) s = mgs4vr_vm_text((unsigned)c->Rcx, (unsigned)c->Rdx, vm_vr != 0);
        if (s) { InterlockedIncrement(&vm_text_ours); c->Rax = (DWORD64)s; c->Rip = *(DWORD64 *)c->Rsp; c->Rsp += 8; return EXCEPTION_CONTINUE_EXECUTION; }
        *(DWORD *)(c->Rsp + 8) = (DWORD)c->Rcx; c->Rip = (DWORD64)vm_ts + 4; return EXCEPTION_CONTINUE_EXECUTION;   /* mov [rsp+8],ecx */
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- worker ---- */
static char vm_dir[MAX_PATH], vm_mark[MAX_PATH], vm_status[MAX_PATH]; static int vm_attempted; static DWORD vm_tick;
static short vm_written[VM_N]; static volatile LONG vm_writes, vm_write_fails;
static int vm_readfile(const char *name, char *b, size_t cap) {
    char p[MAX_PATH]; FILE *f; size_t n;
    sprintf_s(p, sizeof(p), "%s%s", vm_dir, name); if (fopen_s(&f, p, "rb")) return 0;
    n = fread(b, 1, cap - 1, f); fclose(f); b[n] = 0; return 1;
}
static int vm_writefile(const char *name, const char *text) {
    char p[MAX_PATH], tmp[MAX_PATH]; FILE *f;
    sprintf_s(p, sizeof(p), "%s%s", vm_dir, name); sprintf_s(tmp, sizeof(tmp), "%s.vrmenu.tmp", p);
    if (fopen_s(&f, tmp, "wb")) return 0; fputs(text, f); fputs("\r\n", f); fclose(f);
    return MoveFileExA(tmp, p, MOVEFILE_REPLACE_EXISTING) != 0;
}
static void vm_load_cache(void) {
    static char b[7][1024]; int ok[7]; MGS4VR_VM_MARKERS m; short v[VM_N];
    ok[0] = vm_readfile("mgs4vr_twoeye.on", b[0], 1024); ok[1] = vm_readfile("mgs4vr_lens.on", b[1], 1024); ok[2] = vm_readfile("mgs4vr_pad.on", b[2], 1024);
    ok[3] = vm_readfile("mgs4vr_armsrc.on", b[3], 1024); ok[4] = vm_readfile("mgs4vr_cqcfp.on", b[4], 1024); ok[5] = vm_readfile("mgs4vr_camo.on", b[5], 1024); ok[6] = vm_readfile("mgs4vr_hudhide.on", b[6], 1024);
    m.twoeye = ok[0] ? b[0] : NULL; m.lens = ok[1] ? b[1] : NULL; m.pad = ok[2] ? b[2] : NULL; m.armsrc = ok[3] ? b[3] : NULL; m.cqcfp = ok[4] ? b[4] : NULL; m.camo = ok[5] ? b[5] : NULL; m.hudhide = ok[6] ? b[6] : NULL;
    memcpy(v, vm_cache, sizeof(v)); mgs4vr_vm_values(&m, v); if (vm_th_have) mgs4vr_vm_theater_values(&vm_th, v);
    memcpy(vm_cache, v, sizeof(v)); InterlockedExchange(&vm_cache_ok, 1);
}
/* VR values that differ from the markers -> markers (only settings the user changed in the menu). */
static void vm_commit(void) {
    short v[VM_N]; int k; char old[1024], out[1024];
    mgs4vr_vm_read(v);
    if (vm_th_have && (v[VM_STEREO] != vm_cache[VM_STEREO] || v[VM_FOLLOW] != vm_cache[VM_FOLLOW] || v[VM_DISTANCE] != vm_cache[VM_DISTANCE] || v[VM_OBSERVER] != vm_cache[VM_OBSERVER])) {
        MGS4VR_VM_THEATER out; int rc = 0;
        if (mgs4vr_vm_apply_theater(&vm_th, v, &out, &rc)) { vm_th_new = out; vm_th = out; vm_th_pending = 1; vm_th_recenter = vm_th_recenter || rc; InterlockedIncrement(&vm_writes); }
        mgs4vr_vm_theater_values(&vm_th, vm_cache);   /* a refused change (stereo unavailable, follow in stereo) is not retried every poll */
        /* the rows show the real settings again (sim 16 Oct: stereo off left "follow YES", shown only because stereo implies it, and the next poll made it real) */
        if (v[VM_STEREO] != vm_cache[VM_STEREO]) vm_t0[0][16] = vm_cache[VM_STEREO];
        if (v[VM_FOLLOW] != vm_cache[VM_FOLLOW]) vm_t0[2][16] = vm_cache[VM_FOLLOW];
        if (v[VM_DISTANCE] != vm_cache[VM_DISTANCE]) vm_t0[3][16] = vm_cache[VM_DISTANCE];
        if (v[VM_OBSERVER] != vm_cache[VM_OBSERVER]) vm_t0[4][16] = vm_cache[VM_OBSERVER];
    }
    for (k = 0; k < VM_NMARK; ++k) if (v[k] != vm_cache[k]) {
        int have = vm_readfile(vm_names[k], old, sizeof(old));
        if (mgs4vr_vm_marker_text(k, v[k], have ? old : NULL, out, sizeof(out)) && vm_writefile(vm_names[k], out)) { vm_written[k] = v[k]; InterlockedIncrement(&vm_writes); vm_cache[k] = v[k]; }
        else { InterlockedIncrement(&vm_write_fails); vm_cache[k] = v[k]; }   /* e.g. red dot with the lens off: once, not every poll */
    }
}
static void vm_report(const char *ev) {
    FILE *f; if (fopen_s(&f, vm_status, "w")) return;
    fprintf(f, "vrmenu_version=1 event=%s enabled=%ld text_armed=%ld vr=%ld screen=%p injects=%ld enters=%ld leaves=%ld states=%ld texts=%ld text_ours=%ld writes=%ld write_fails=%ld faults=%ld values=%d,%d,%d,%d,%d,%d,%d,%d hud=%d theater=%d,%d,%d,%d have=%d\n",
            ev, vm_enabled, vm_text_armed, vm_vr, (void *)vm_screen, vm_injects, vm_enters, vm_leaves, vm_states, vm_texts, vm_text_ours, vm_writes, vm_write_fails, vm_faults,
            vm_cache[0], vm_cache[1], vm_cache[2], vm_cache[3], vm_cache[4], vm_cache[5], vm_cache[6], vm_cache[7], vm_cache[VM_HUD], vm_cache[VM_STEREO], vm_cache[VM_FOLLOW], vm_cache[VM_DISTANCE], vm_cache[VM_OBSERVER], vm_th_have);
    fclose(f);
}
static int vm_marker_on(void) { char b[64]; return vm_readfile("mgs4vr_vrmenu.on", b, sizeof(b)) && vm_on(b); }
void mgs4vr_vrmenu_poll(void *base, const char *exe) {
    DWORD now = GetTickCount();
    if (!vm_dir[0]) { const char *leaf = strrchr(exe, '\\'); if (!leaf) return; memcpy(vm_dir, exe, (size_t)(leaf + 1 - exe)); vm_dir[leaf + 1 - exe] = 0;
        sprintf_s(vm_mark, sizeof(vm_mark), "%smgs4vr_vrmenu.on", vm_dir); sprintf_s(vm_status, sizeof(vm_status), "%smgs4vr_vrmenu_%lu.txt", vm_dir, GetCurrentProcessId()); }
    if (vm_attempted) {
        if (now - vm_tick < 250) return;
        vm_tick = now;
        if (!vm_vr) { if (vm_dirty) { vm_commit(); InterlockedExchange(&vm_dirty, 0); } vm_load_cache(); }   /* while the VR tabs are shown the menu owns the values */
        else vm_commit();
        if (!vm_marker_on() && vm_enabled && !vm_screen) {   /* marker removed, no menu open: take the seams out */
            InterlockedExchange(&vm_enabled, 0); vm_text_arm(0); if (vm_patch(vm_ss, 0xcc, vm_state_sig[0])) vm_attempted = 0; vm_report("stopped"); return;
        }
        { static DWORD rep; if (now - rep >= 1000) { rep = now; vm_report(vm_screen ? "menu" : "active"); } }
        return;
    }
    if (!vm_marker_on()) return;
    vm_attempted = 1; vm_tick = now; vm_base = (BYTE *)base; vm_ss = vm_base + VM_STATE_RVA; vm_ts = vm_base + VM_TEXT_RVA;
    __try { if (memcmp(vm_ss, vm_state_sig, sizeof(vm_state_sig)) || memcmp(vm_ts, vm_text_sig, sizeof(vm_text_sig))) { vm_report("reject_signature"); return; } }
    __except (EXCEPTION_EXECUTE_HANDLER) { vm_report("reject_signature"); return; }
    vm_load_cache();
    if (!vm_veh) vm_veh = AddVectoredExceptionHandler(1, vm_trap); if (!vm_veh) { vm_report("reject_veh"); return; }
    InterlockedExchange(&vm_enabled, 1);
    if (!vm_patch(vm_ss, vm_state_sig[0], 0xcc)) { InterlockedExchange(&vm_enabled, 0); vm_report("reject_patch"); return; }
    vm_report("installed");
}
