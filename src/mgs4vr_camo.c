/* mgs4vr_camo.c - see mgs4vr_camo.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mgs4vr_camo.h"

int mgs4vr_camo_parse(const char *b, MGS4VR_CAMO_CFG *c) {
    const char *q; float v;
    c->wanted = 0; c->revs = 3; c->window_s = 0.6f; c->speed = 1.0f; c->cooldown_s = 2.0f;
    if (!b) return 0;
    if ((unsigned char)b[0] == 0xef && (unsigned char)b[1] == 0xbb && (unsigned char)b[2] == 0xbf) b += 3;
    while (*b == ' ' || *b == '\t') ++b;
    c->wanted = !strncmp(b, "on", 2) && (b[2] == 0 || b[2] == ' ' || b[2] == '\r' || b[2] == '\n');
    if ((q = strstr(b, "revs=")) && sscanf_s(q + 5, "%f", &v) == 1 && v >= 2 && v <= 8) c->revs = (int)v;
    if ((q = strstr(b, "window=")) && sscanf_s(q + 7, "%f", &v) == 1 && v >= 0.2f && v <= 3) c->window_s = v;
    if ((q = strstr(b, "speed=")) && sscanf_s(q + 6, "%f", &v) == 1 && v >= 0.3f && v <= 5) c->speed = v;
    if ((q = strstr(b, "cooldown=")) && sscanf_s(q + 9, "%f", &v) == 1 && v >= 0.5f && v <= 10) c->cooldown_s = v;
    return c->wanted;
}

void mgs4vr_camo_sample(MGS4VR_CAMO_DET *d, const MGS4VR_CAMO_CFG *c, int hand, const float v[3], double t) {
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), u[3]; int i;
    if (hand < 0 || hand > 1 || !(s > c->speed) || !isfinite(s)) return;
    for (i = 0; i < 3; ++i) u[i] = v[i] / s;
    if (d->have[hand] && u[0] * d->dir[hand][0] + u[1] * d->dir[hand][1] + u[2] * d->dir[hand][2] < -0.3f) {   /* against the last fast move */
        if (d->nrev[hand] == 8) { memmove(d->rev[hand], d->rev[hand] + 1, 7 * sizeof(double)); d->nrev[hand] = 7; }
        d->rev[hand][d->nrev[hand]++] = t; ++d->reversals[hand];
    }
    memcpy(d->dir[hand], u, sizeof(u)); d->have[hand] = 1;
}

int mgs4vr_camo_check(MGS4VR_CAMO_DET *d, const MGS4VR_CAMO_CFG *c, double t) {
    int h, i, n[2] = { 0, 0 };
    if (d->last_fire > 0 && t - d->last_fire < c->cooldown_s) return 0;
    for (h = 0; h < 2; ++h) for (i = 0; i < d->nrev[h]; ++i) if (t - d->rev[h][i] <= c->window_s) ++n[h];
    if (n[0] < c->revs || n[1] < c->revs) return 0;
    d->last_fire = t; d->nrev[0] = d->nrev[1] = 0; d->have[0] = d->have[1] = 0; ++d->shakes;
    return 1;
}

/* XR thread. */
static MGS4VR_CAMO_CFG cm_cfg; static MGS4VR_CAMO_DET cm_det;
static float cm_prev[2][3]; static double cm_prev_t[2]; static int cm_prev_ok[2];
static volatile LONG cm_fire, cm_blocked, cm_taps, cm_allowed, cm_calls, cm_inactive[2], cm_vel[2], cm_fast[2];
static float cm_vmax[2], cm_dtmin = 9, cm_dtmax;   /* diagnostics (XR thread writes, status reads) */
void mgs4vr_camo_hands(const MGS4VR_XR_HANDS *h) {
    double t; int i, k;
    if (!h || !cm_cfg.wanted) return;
    InterlockedIncrement(&cm_calls);
    t = (double)h->display_time * 1e-9;
    for (i = 0; i < 2; ++i) {
        const MGS4VR_XR_HAND *x = &h->hand[i]; float v[3]; double dt;
        if (!x->active || !isfinite(x->grip_pos[0]) || !isfinite(x->grip_pos[1]) || !isfinite(x->grip_pos[2])) { cm_prev_ok[i] = 0; InterlockedIncrement(&cm_inactive[i]); continue; }
        dt = t - cm_prev_t[i];
        if (cm_prev_ok[i]) { if ((float)dt < cm_dtmin) cm_dtmin = (float)dt; if ((float)dt > cm_dtmax && dt < 10) cm_dtmax = (float)dt; }
        if (cm_prev_ok[i] && dt > 0.005 && dt < 0.1) {
            float sp;
            for (k = 0; k < 3; ++k) v[k] = (float)((x->grip_pos[k] - cm_prev[i][k]) / dt);
            sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); InterlockedIncrement(&cm_vel[i]);
            if (sp > cm_vmax[i]) cm_vmax[i] = sp; if (sp > cm_cfg.speed) InterlockedIncrement(&cm_fast[i]);
            mgs4vr_camo_sample(&cm_det, &cm_cfg, i, v, t);
        }
        if (!cm_prev_ok[i] || dt >= 0.02) { memcpy(cm_prev[i], x->grip_pos, 12); cm_prev_t[i] = t; cm_prev_ok[i] = 1; }   /* ~20 ms baseline */
    }
    if (mgs4vr_camo_check(&cm_det, &cm_cfg, t)) { if (cm_allowed) InterlockedExchange(&cm_fire, 1); else InterlockedIncrement(&cm_blocked); }
}

static char cm_mark[MAX_PATH], cm_status[MAX_PATH]; static DWORD cm_poll, cm_stat;
int mgs4vr_camo_tick(const char *exe, int allowed, DWORD now) {
    FILE *f; char b[256] = { 0 }, *leaf; int fire;
    if (!cm_mark[0] && exe) {
        strcpy_s(cm_mark, sizeof(cm_mark), exe); leaf = strrchr(cm_mark, '\\');
        if (leaf) { strcpy_s(leaf + 1, sizeof(cm_mark) - (size_t)(leaf + 1 - cm_mark), "mgs4vr_camo.on"); strcpy_s(cm_status, sizeof(cm_status), cm_mark);
                    sprintf_s(strrchr(cm_status, '\\') + 1, 64, "mgs4vr_camo_%lu.txt", GetCurrentProcessId()); }
        else cm_mark[0] = 0;
    }
    if (cm_mark[0] && now - cm_poll >= 500u) {
        MGS4VR_CAMO_CFG c; cm_poll = now;
        if (!fopen_s(&f, cm_mark, "rb")) { size_t n = fread(b, 1, sizeof(b) - 1, f); fclose(f); b[n] = 0; }
        mgs4vr_camo_parse(b, &c); cm_cfg = c;                             /* plain copy: the XR thread only reads it */
    }
    InterlockedExchange(&cm_allowed, allowed && cm_cfg.wanted);
    fire = InterlockedExchange(&cm_fire, 0) && allowed;
    if (fire) InterlockedIncrement(&cm_taps);
    if (cm_status[0] && cm_cfg.wanted && now - cm_stat >= 1000u) {
        cm_stat = now;
        if (!fopen_s(&f, cm_status, "w")) {
            fprintf(f, "camo_version=1 wanted=%d revs=%d window=%.2f speed=%.2f cooldown=%.1f allowed=%d reversals=%ld,%ld shakes=%ld taps=%ld blocked=%ld calls=%ld inactive=%ld,%ld vel=%ld,%ld fast=%ld,%ld vmax=%.2f,%.2f dt=%.4f..%.4f\n",
                    cm_cfg.wanted, cm_cfg.revs, cm_cfg.window_s, cm_cfg.speed, cm_cfg.cooldown_s, allowed, cm_det.reversals[0], cm_det.reversals[1], cm_det.shakes, cm_taps, cm_blocked, cm_calls, cm_inactive[0], cm_inactive[1], cm_vel[0], cm_vel[1], cm_fast[0], cm_fast[1], cm_vmax[0], cm_vmax[1], cm_dtmin, cm_dtmax);
            fclose(f);
        }
    }
    return fire;
}
