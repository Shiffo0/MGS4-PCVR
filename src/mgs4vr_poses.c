/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
/* Hand poses for the desktop pose tool; see mgs4vr_poses.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "mgs4vr_poses.h"

#define PS_WTABLE_RVA 0x1d82580u   /* weapon table: + code * 80, +0 = name pointer (mgs4vr_wrist rd_name) */
#define PS_EQUIP_RVA 0x23fed43cu   /* s16 count, s16 codes[8] at +8 */
#define PS_WEAPON_CUR_RVA 0x23fed464u
#define PS_CODES 128
static const char *ps_pose_name[MGS4VR_POSE_N] = { "grip", "support", "slide_rear", "slide_mid", "mag" };
static volatile LONG ps_freeze, ps_show = -1, ps_events[4];
/* Pose camera (17 Oct 2026, user: "you see far too little; compare the TLOU2 tool"): command "cam az el dist tx ty tz"
   (degrees, degrees, mm, target in the pistol frame) or "cam off". */
static SRWLOCK ps_cam_lock = SRWLOCK_INIT; static float ps_cam[6]; static volatile LONG ps_cam_on;
int mgs4vr_poses_cam(float v[6]) {
    int on = InterlockedCompareExchange(&ps_cam_on, 0, 0) != 0;
    if (!on) return 0;
    AcquireSRWLockShared(&ps_cam_lock); memcpy(v, ps_cam, sizeof(ps_cam)); ReleaseSRWLockShared(&ps_cam_lock);
    return 1;
}
int mgs4vr_poses_freeze(void) { return InterlockedCompareExchange(&ps_freeze, 0, 0) != 0; }
int mgs4vr_poses_show(void) { return (int)InterlockedCompareExchange(&ps_show, 0, 0); }
void mgs4vr_poses_event(int kind) { if (kind >= 0 && kind < 4) InterlockedIncrement(&ps_events[kind]); }

static char *ps_tok(char **c) { char *p = *c, *s; while (*p == ' ' || *p == '\t' || *p == '\r') ++p; if (!*p) { *c = p; return NULL; }
    s = p; while (*p && *p != ' ' && *p != '\t' && *p != '\r') ++p; if (*p) *p++ = 0; *c = p; return s; }
int mgs4vr_poses_parse(char *text, MGS4VR_POSE_TABLE *t) {
    char *line, *next, *c, *tk, *end; int first = 1, i;
    t->count = 0;
    for (line = text; line && *line; line = next) {
        MGS4VR_POSE p; float v[12];
        next = strchr(line, '\n'); if (next) *next++ = 0;
        while (*line == ' ' || *line == '\t' || *line == '\r') ++line;
        if (!*line || *line == '#') continue;
        c = line; tk = ps_tok(&c);
        if (first) { if (!tk || strcmp(tk, "POSES1") || ps_tok(&c)) return 0; first = 0; continue; }
        memset(&p, 0, sizeof(p));
        if (!strcmp(tk, "any")) p.code = -1;
        else if (tk[0] == 'w' && tk[1]) { long k = strtol(tk + 1, &end, 10); if (*end || k < 0 || k >= PS_CODES) return 0; p.code = (int)k; }
        else return 0;
        tk = ps_tok(&c); if (!tk || (strcmp(tk, "R") && strcmp(tk, "L"))) return 0; p.hand = tk[0] == 'L';
        tk = ps_tok(&c); if (!tk) return 0;
        for (p.pose = 0; p.pose < MGS4VR_POSE_N && strcmp(tk, ps_pose_name[p.pose]); ++p.pose) {}
        if (p.pose == MGS4VR_POSE_N) return 0;
        for (i = 0; i < 12; ++i) { tk = ps_tok(&c); if (!tk) return 0; v[i] = strtof(tk, &end); if (*end || !isfinite(v[i])) return 0; }
        if (ps_tok(&c)) return 0;
        for (i = 0; i < 3; ++i) { if (fabsf(v[i]) > 300.f || fabsf(v[3 + i]) > 180.f) return 0; p.d[i] = v[i]; p.rot[i] = v[3 + i]; }
        for (i = 0; i < MGS4VR_FINGERS; ++i) { if (fabsf(v[6 + i]) > 120.f) return 0; p.curl[i] = v[6 + i]; }
        if (fabsf(v[11]) > 45.f) return 0; p.spread = v[11];
        if (t->count == MGS4VR_POSES_MAX) return 0;
        for (i = 0; i < t->count; ++i) if (t->row[i].code == p.code && t->row[i].hand == p.hand && t->row[i].pose == p.pose) return 0;
        t->row[t->count++] = p;
    }
    return !first;
}
const MGS4VR_POSE *mgs4vr_poses_find(const MGS4VR_POSE_TABLE *t, int code, int hand, int pose) {
    int i; const MGS4VR_POSE *any = NULL;
    for (i = 0; i < t->count; ++i) { const MGS4VR_POSE *p = &t->row[i];
        if (p->hand != hand || p->pose != pose) continue;
        if (p->code == code) return p;
        if (p->code == -1) any = p; }
    return any;
}

/* ---- live table ---- */
static SRWLOCK ps_lock = SRWLOCK_INIT; static MGS4VR_POSE_TABLE ps_pub; static volatile LONG ps_loads, ps_bad, ps_gets;
int mgs4vr_poses_get(int code, int hand, int pose, MGS4VR_POSE *out) {
    const MGS4VR_POSE *p; int ok = 0;
    if (!TryAcquireSRWLockShared(&ps_lock)) return 0;
    p = mgs4vr_poses_find(&ps_pub, code, hand, pose); if (p) { *out = *p; ok = 1; }
    ReleaseSRWLockShared(&ps_lock);
    if (ok) InterlockedIncrement(&ps_gets);
    return ok;
}
static char ps_dir[MAX_PATH]; static DWORD ps_tick; static FILETIME ps_mtime; static int ps_names_done;
static void ps_path(char *out, size_t cap, const char *leaf) { sprintf_s(out, cap, "%s%s", ps_dir, leaf); }
static int ps_name(BYTE *img, int code, char out[48]) {
    int i; out[0] = 0;
    __try { const char *n = *(const char **)(img + PS_WTABLE_RVA + (size_t)code * 80);
        if (!n) return 0; for (i = 0; i < 47 && n[i]; ++i) out[i] = (n[i] >= 32 && n[i] < 127) ? n[i] : '?'; out[i] = 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return 0; }
    return out[0] != 0;
}
static int ps_equip(BYTE *img, short codes[8]) {
    int n = 0; __try { n = *(short *)(img + PS_EQUIP_RVA); if (n < 0 || n > 8) return 0; memcpy(codes, img + PS_EQUIP_RVA + 8, 16); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return n;
}
void mgs4vr_poses_poll(void *image, const char *exe) {
    BYTE *img = (BYTE *)image; DWORD now = GetTickCount(); char p[MAX_PATH]; FILE *f; WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!ps_dir[0]) { const char *leaf = strrchr(exe, '\\'); if (!leaf) return; memcpy(ps_dir, exe, (size_t)(leaf + 1 - exe)); ps_dir[leaf + 1 - exe] = 0; }
    if (now - ps_tick < 250) return; ps_tick = now;
    /* reload on change */
    ps_path(p, sizeof(p), "mgs4vr_poses.txt");
    if (GetFileAttributesExA(p, GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &ps_mtime)) {
        static char buf[65536]; static MGS4VR_POSE_TABLE next; size_t n = 0;
        if (!fopen_s(&f, p, "rb")) { n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = 0;
            if (n < sizeof(buf) - 1 && mgs4vr_poses_parse(buf, &next)) { AcquireSRWLockExclusive(&ps_lock); ps_pub = next; ReleaseSRWLockExclusive(&ps_lock); InterlockedIncrement(&ps_loads); ps_mtime = fa.ftLastWriteTime; }
            else { InterlockedIncrement(&ps_bad); ps_mtime = fa.ftLastWriteTime; } }
    }
    if (!img) return;
    /* all weapon names, once */
    if (!ps_names_done) { int c; char nm[48]; ps_path(p, sizeof(p), "mgs4vr_weapon_names.txt");
        if (!fopen_s(&f, p, "w")) { for (c = 1; c < PS_CODES; ++c) if (ps_name(img, c, nm)) fprintf(f, "%d\t%s\n", c, nm); fclose(f); ps_names_done = 1; } }
    /* command: equip <code> (only a code in the equip list) */
    ps_path(p, sizeof(p), "mgs4vr_pose_cmd.txt");
    if (!fopen_s(&f, p, "rb")) { static char all[2048]; char *cmd, *nl; size_t got;
        got = fread(all, 1, sizeof(all) - 1, f); fclose(f); DeleteFileA(p); all[got] = 0;
        for (cmd = all; cmd && *cmd; cmd = nl) {                           /* one command per line (the tool appends) */
        int code = -1; short codes[8]; int n, i, ok = 0;
        nl = strchr(cmd, '\n'); if (nl) *nl++ = 0;
        { char nm[24] = { 0 }; int v = 0, k;
          if (sscanf_s(cmd, "freeze %d", &v) == 1) InterlockedExchange(&ps_freeze, v != 0);
          { float cv[6];
            if (!strncmp(cmd, "cam off", 7)) InterlockedExchange(&ps_cam_on, 0);
            else if (sscanf_s(cmd, "cam %f %f %f %f %f %f", &cv[0], &cv[1], &cv[2], &cv[3], &cv[4], &cv[5]) == 6 && cv[2] >= 50 && cv[2] <= 3000) {
                AcquireSRWLockExclusive(&ps_cam_lock); memcpy(ps_cam, cv, sizeof(cv)); ReleaseSRWLockExclusive(&ps_cam_lock); InterlockedExchange(&ps_cam_on, 1); } }
          if (sscanf_s(cmd, "show %23s", nm, (unsigned)sizeof(nm)) == 1) { for (k = 0; k < MGS4VR_POSE_N && strcmp(nm, ps_pose_name[k]); ++k) {} InterlockedExchange(&ps_show, k < MGS4VR_POSE_N && k >= MGS4VR_POSE_SLIDE_REAR ? k : -1); } }
        if (sscanf_s(cmd, "equip %d", &code) == 1 && code >= 0) { n = ps_equip(img, codes); for (i = 0; i < n; ++i) if ((codes[i] & 0x1ff) == code) ok = 1;
            if (ok || code == 0) { __try { *(unsigned short *)(img + PS_WEAPON_CUR_RVA) = (unsigned short)code; } __except (EXCEPTION_EXECUTE_HANDLER) {} } }
        } }
    /* status */
    { short codes[8]; int n = ps_equip(img, codes), cur = -1, i; char nm[48];
        __try { cur = *(unsigned short *)(img + PS_WEAPON_CUR_RVA) & 0x1ff; } __except (EXCEPTION_EXECUTE_HANDLER) { cur = -1; }
        sprintf_s(p, sizeof(p), "%smgs4vr_poses_status.txt", ps_dir);
        if (!fopen_s(&f, p, "w")) {
            if (cur < 0 || !ps_name(img, cur, nm)) strcpy_s(nm, sizeof(nm), cur == 0 ? "NONE" : "?");
            fprintf(f, "pid=%lu\nweapon=%d\nweapon_name=%s\nequip=", GetCurrentProcessId(), cur, nm);
            for (i = 0; i < n; ++i) fprintf(f, "%s%d", i ? "," : "", codes[i] & 0x1ff);
            fprintf(f, "\nposes=%d\nloads=%ld\nbad=%ld\ngets=%ld\nfreeze=%ld\nshow=%ld\nslide_grabs=%ld\nracks=%ld\nmag_holds=%ld\nmag_inserts=%ld\ntick=%lu\n", ps_pub.count, ps_loads, ps_bad, ps_gets, ps_freeze, ps_show, ps_events[0], ps_events[1], ps_events[2], ps_events[3], now);
            fclose(f); } }
}
