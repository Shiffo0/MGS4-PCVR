/* mgs4vr_cqc.c - see mgs4vr_cqc.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mgs4vr_cqc.h"

enum { S_IDLE, S_PAUSE_GRAB, S_GRAB, S_THROW_UP, S_THROW_DOWN, S_PAUSE_STRIKE, S_STRIKE, S_RECOVER, S_GRAB_AIM, S_VERIFY };
#define VERIFY_MS 400u        /* after strikes: aim back, then check the game is in the aim stance (0x2E) again */
#define ACT_AIM 0x2e
#define VERIFY_TRIES 3        /* first person re-applied at most this often per gesture */
#define VERIFY_MAX_MS 6000u
static int act_busy(int a) { return a >= 0xe0 && a <= 0x120; }   /* CQC actions (0xE7 start .. 0xF7 throw, 0x101 hold + aim, ...) */
#define HOLDAIM_MS 500u       /* grab established -> button up, aim back (hold + aim) */
#define AIM_OFF_MS 60u        /* the game must see the aim released before the button */
#define TAP_MS 120u
#define RECOVER_MS 350u       /* button up -> aim back */
#define COMBO_MS 600u         /* after a strike the aim stays off this long: another push = the next blow of the combo */
#define GRAB_MAX_MS 30000u
#define ON_WEAPON_M 0.25f     /* left hand this close to the right hand = on the rifle (two-hand aiming) */
#define AT_HEAD_M 0.22f       /* left hand this close to the head = night-vision gesture */

int mgs4vr_cqc_parse(const char *b, MGS4VR_CQC_CFG *c) {
    const char *q; float v;
    c->wanted = 0; c->gate_on = 1; c->holdaim = 1; c->punch_mps = 2.0f; c->throw_mps = 1.6f; c->gate_s = 1.5f;
    if (!b) return 0;
    if ((unsigned char)b[0] == 0xef && (unsigned char)b[1] == 0xbb && (unsigned char)b[2] == 0xbf) b += 3;
    while (*b == ' ' || *b == '\t') ++b;
    c->wanted = !strncmp(b, "on", 2) && (b[2] == 0 || b[2] == ' ' || b[2] == '\r' || b[2] == '\n');
    c->gate_on = strstr(b, "nogate") == NULL;
    c->holdaim = strstr(b, "noholdaim") == NULL;
    if ((q = strstr(b, "punch=")) && sscanf_s(q + 6, "%f", &v) == 1 && v >= 0.5f && v <= 10) c->punch_mps = v;
    if ((q = strstr(b, "throw=")) && sscanf_s(q + 6, "%f", &v) == 1 && v >= 0.5f && v <= 10) c->throw_mps = v;
    if ((q = strstr(b, "gate=")) && sscanf_s(q + 5, "%f", &v) == 1 && v >= 0.1f && v <= 10) c->gate_s = v;
    return c->wanted;
}

void mgs4vr_cqc_step(MGS4VR_CQC_SM *sm, const MGS4VR_CQC_CFG *cfg, const MGS4VR_CQC_IN *in, DWORD now, MGS4VR_CQC_OUT *out) {
    int hi = in->squeeze > 0.8f, free_hand = in->d_weapon_m > ON_WEAPON_M && in->d_head_m > AT_HEAD_M;
    DWORD dt = now - sm->t;
    int rise = hi && !sm->prev_squeeze_hi;
    if (in->squeeze < 0.4f) sm->prev_squeeze_hi = 0; else if (hi) sm->prev_squeeze_hi = 1;
    if (in->v_fwd < 1.0f) sm->punch_armed = 1;                        /* a push counts once: re-armed when the hand slows */
    out->pause_aim = 0; out->lmb = 0; out->dir = 0; out->reapply_fp = 0;
    if (sm->state == S_VERIFY && cfg->wanted && in->allowed) {          /* our own re-apply drops first person for a moment: keep checking */
        if (in->act == ACT_AIM || in->act == 0 || now - sm->t_verify >= VERIFY_MAX_MS) sm->state = S_IDLE;
        else if (!in->fps) { }                                          /* the hold is being applied */
        else if (act_busy(in->act)) sm->t = now;                        /* CQC animation still running: wait */
        else if (dt >= VERIFY_MS) {
            if (sm->tries < VERIFY_TRIES) { out->reapply_fp = 1; ++sm->reapplies; ++sm->tries; sm->t = now; } else sm->state = S_IDLE;
        }
        else if (free_hand && sm->punch_armed && in->v_fwd > cfg->punch_mps && in->squeeze < 0.5f) { sm->state = S_PAUSE_STRIKE; sm->t = now; ++sm->strikes; sm->punch_armed = 0; out->pause_aim = 1; }
        return;
    }
    if (!cfg->wanted || !in->fps || !in->allowed) {                    /* leaving first person / menu: let go of everything */
        if (sm->state != S_IDLE) { ++sm->aborts; sm->state = S_IDLE; }
        return;
    }
    switch (sm->state) {
    case S_IDLE:
        if (rise && free_hand) {
            if (in->enemy_near || !cfg->gate_on) { sm->state = S_PAUSE_GRAB; sm->t = now; ++sm->grabs; sm->grabbed = 1; out->pause_aim = 1; }
            else ++sm->gated;
        } else if (free_hand && sm->punch_armed && in->v_fwd > cfg->punch_mps && in->squeeze < 0.5f) { sm->state = S_PAUSE_STRIKE; sm->t = now; ++sm->strikes; sm->punch_armed = 0; sm->grabbed = 0; out->pause_aim = 1; }
        break;
    case S_PAUSE_GRAB:   out->pause_aim = 1; if (dt >= AIM_OFF_MS) { sm->state = S_GRAB; sm->t = now; out->lmb = 1; } break;
    case S_GRAB:
        out->pause_aim = 1; out->lmb = 1;
        if (fabsf(in->v_fwd) > cfg->throw_mps && dt >= 300u) { sm->state = S_THROW_UP; sm->t = now; ++sm->throws; out->lmb = 0; sm->throw_dir = in->v_fwd > 0 ? 1 : -1; out->dir = sm->throw_dir; }
        else if (in->squeeze < 0.4f || dt >= GRAB_MAX_MS) { sm->state = S_RECOVER; sm->t = now; ++sm->releases; out->lmb = 0; }
        else if (cfg->holdaim && dt >= HOLDAIM_MS && in->r_squeeze > 0.8f) { sm->state = S_GRAB_AIM; sm->t = now; out->lmb = 0; out->pause_aim = 0; }   /* right grip: hold + aim */
        break;
    case S_GRAB_AIM:                                                    /* aim held (fpshold), trigger fires via the pad */
        if (fabsf(in->v_fwd) > cfg->throw_mps) { sm->state = S_THROW_UP; sm->t = now; ++sm->throws; sm->throw_dir = in->v_fwd > 0 ? 1 : -1; out->dir = sm->throw_dir; out->pause_aim = 1; }
        else if (in->squeeze < 0.4f || dt >= GRAB_MAX_MS) { sm->state = S_RECOVER; sm->t = now; ++sm->releases; out->pause_aim = 1; }   /* aim off = let go */
        else if (in->r_squeeze < 0.4f) { sm->state = S_GRAB; sm->t = now - HOLDAIM_MS; out->pause_aim = 1; out->lmb = 1; }      /* right grip open: plain hold again */
        break;
    case S_THROW_UP:     out->pause_aim = 1; out->dir = sm->throw_dir; if (dt >= AIM_OFF_MS) { sm->state = S_THROW_DOWN; sm->t = now; out->lmb = 1; } break;
    case S_THROW_DOWN:   out->pause_aim = 1; out->dir = sm->throw_dir; out->lmb = 1; if (dt >= TAP_MS) { sm->state = S_RECOVER; sm->t = now; out->lmb = 0; } break;
    case S_PAUSE_STRIKE: out->pause_aim = 1; if (dt >= AIM_OFF_MS) { sm->state = S_STRIKE; sm->t = now; out->lmb = 1; } break;
    case S_STRIKE:       out->pause_aim = 1; out->lmb = 1; if (dt >= TAP_MS) { sm->state = S_RECOVER; sm->t = now; out->lmb = 0; } break;
    case S_RECOVER:
        out->pause_aim = 1;
        if (!sm->grabbed && free_hand && sm->punch_armed && in->v_fwd > cfg->punch_mps && in->squeeze < 0.5f) {   /* combo: next blow */
            sm->state = S_STRIKE; sm->t = now; ++sm->strikes; ++sm->combos; sm->punch_armed = 0; out->lmb = 1; break; }
        if (dt >= (sm->grabbed ? RECOVER_MS : COMBO_MS) && in->squeeze < 0.4f) {
            sm->grabbed = 0; sm->state = S_VERIFY; sm->t = now; sm->t_verify = now; sm->tries = 0;   /* wait for the CQC animation, then first person */
        }
        break;
    case S_VERIFY: break;                                               /* handled above */
    }
}

/* XR thread -> worker. */
static SRWLOCK cq_lock = SRWLOCK_INIT;
static struct { int valid; float squeeze, v_fwd, d_weapon, d_head, peak, r_squeeze; DWORD tick; } cq_hand;
static float cq_hist[8][4]; static int cq_n, cq_i;   /* XR thread: x, y, z, t(s) */

void mgs4vr_cqc_hands(const MGS4VR_XR_HANDS *h) {
    const MGS4VR_XR_HAND *l, *r; float t, f[3], v[3] = { 0, 0, 0 }, vf = 0, dw, dh, n; int i, j;
    if (!h || !h->head_valid) return;
    l = &h->hand[0]; r = &h->hand[1];
    if (!l->active) { AcquireSRWLockExclusive(&cq_lock); cq_hand.valid = 0; ReleaseSRWLockExclusive(&cq_lock); return; }
    for (i = 0; i < 3; ++i) if (!isfinite(l->grip_pos[i]) || !isfinite(h->head_pos[i])) return;
    t = (float)((double)h->display_time * 1e-9);
    cq_hist[cq_i][0] = l->grip_pos[0]; cq_hist[cq_i][1] = l->grip_pos[1]; cq_hist[cq_i][2] = l->grip_pos[2]; cq_hist[cq_i][3] = t;
    j = cq_i; cq_i = (cq_i + 1) & 7; if (cq_n < 8) ++cq_n;
    for (i = 1; i < cq_n; ++i) {                                       /* sample ~50 ms back */
        int k = (j - i + 8) & 7; float dt = t - cq_hist[k][3];
        if (dt >= 0.045f || i == cq_n - 1) { if (dt > 0.005f && dt < 0.25f) { v[0] = (cq_hist[j][0] - cq_hist[k][0]) / dt; v[1] = (cq_hist[j][1] - cq_hist[k][1]) / dt; v[2] = (cq_hist[j][2] - cq_hist[k][2]) / dt; } break; }
    }
    {   float x = h->head_quat[0], y = h->head_quat[1], z = h->head_quat[2], w = h->head_quat[3];
        f[0] = -(2 * (x * z + w * y)); f[1] = 0; f[2] = -(1 - 2 * (x * x + y * y));   /* head forward (-Z), flattened */
        n = sqrtf(f[0] * f[0] + f[2] * f[2]); if (n > 1e-3f) { f[0] /= n; f[2] /= n; vf = v[0] * f[0] + v[2] * f[2]; } }
    dw = r->active ? sqrtf((l->grip_pos[0] - r->grip_pos[0]) * (l->grip_pos[0] - r->grip_pos[0]) + (l->grip_pos[1] - r->grip_pos[1]) * (l->grip_pos[1] - r->grip_pos[1]) + (l->grip_pos[2] - r->grip_pos[2]) * (l->grip_pos[2] - r->grip_pos[2])) : 9;
    dh = sqrtf((l->grip_pos[0] - h->head_pos[0]) * (l->grip_pos[0] - h->head_pos[0]) + (l->grip_pos[1] - h->head_pos[1]) * (l->grip_pos[1] - h->head_pos[1]) + (l->grip_pos[2] - h->head_pos[2]) * (l->grip_pos[2] - h->head_pos[2]));
    AcquireSRWLockExclusive(&cq_lock);
    cq_hand.valid = 1; cq_hand.squeeze = isfinite(l->squeeze) ? l->squeeze : 0; cq_hand.r_squeeze = r->active && isfinite(r->squeeze) ? r->squeeze : 0; cq_hand.v_fwd = vf; cq_hand.d_weapon = dw; cq_hand.d_head = dh; cq_hand.tick = GetTickCount();
    if (fabsf(vf) > cq_hand.peak) cq_hand.peak = fabsf(vf);
    ReleaseSRWLockExclusive(&cq_lock);
}

static MGS4VR_CQC_CFG cq_cfg; static MGS4VR_CQC_SM cq_sm; static MGS4VR_CQC_OUT cq_out;
static char cq_mark[MAX_PATH], cq_status[MAX_PATH]; static DWORD cq_poll_tick, cq_status_tick, cq_near_tick;
static int cq_act = -1, cq_inrange = -1;
static int (*cq_on_weapon)(void);
void mgs4vr_cqc_set_on_weapon(int (*fn)(void)) { cq_on_weapon = fn; }
int mgs4vr_cqc_pause_aim(void) { return cq_out.pause_aim; }
int mgs4vr_cqc_lmb(void) { return cq_out.lmb; }
int mgs4vr_cqc_dir(void) { return cq_out.dir; }
int mgs4vr_cqc_reapply_fp(void) { return cq_out.reapply_fp; }
int mgs4vr_cqc_busy(void) { return cq_sm.state != S_IDLE; }

/* Snake's player data: owner slot -> actor -> +0x90 (= actor+0xC0): action +0x10C, enemy-in-range +0x16A8 (2). */
static int cq_read_game(void *image, int *act, int *inrange) {
    BYTE *img = (BYTE *)image; int slot; unsigned long long owner, actor, pd;
    if (!img) return 0;
    __try {
        slot = *(int *)(img + 0x23eb5150); if (slot < -1 || slot > 23) return 0;
        owner = *(unsigned long long *)(img + 0x23eb5160 + (size_t)(slot + 1) * 0x80); actor = owner & 0xfffffffffffffULL;
        if (actor < 0x10000 || *(unsigned long long *)(actor + 0x30) != owner) return 0;
        pd = *(unsigned long long *)(actor + 0x90); if (pd < 0x10000) return 0;
        *act = *(int *)(pd + 0x10c); *inrange = *(int *)(pd + 0x16a8);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

void mgs4vr_cqc_tick(void *image, const char *exe, int allowed, int fps, DWORD now) {
    MGS4VR_CQC_IN in; FILE *f; char b[256] = { 0 }, *leaf; int act, inr;
    if (!cq_mark[0] && exe) {
        strcpy_s(cq_mark, sizeof(cq_mark), exe); leaf = strrchr(cq_mark, '\\');
        if (leaf) { strcpy_s(leaf + 1, sizeof(cq_mark) - (size_t)(leaf + 1 - cq_mark), "mgs4vr_cqc.on"); strcpy_s(cq_status, sizeof(cq_status), cq_mark);
                    sprintf_s(strrchr(cq_status, '\\') + 1, 64, "mgs4vr_cqc_%lu.txt", GetCurrentProcessId()); }
        else cq_mark[0] = 0;
    }
    if (cq_mark[0] && now - cq_poll_tick >= 500u) {
        cq_poll_tick = now;
        if (!fopen_s(&f, cq_mark, "rb")) { size_t n = fread(b, 1, sizeof(b) - 1, f); fclose(f); b[n] = 0; }
        mgs4vr_cqc_parse(b, &cq_cfg);
    }
    memset(&in, 0, sizeof(in)); in.fps = fps; in.allowed = allowed;
    AcquireSRWLockShared(&cq_lock);
    if (cq_hand.valid && now - cq_hand.tick < 150u) { in.squeeze = cq_hand.squeeze; in.v_fwd = cq_hand.v_fwd; in.d_weapon_m = cq_hand.d_weapon; in.d_head_m = cq_hand.d_head; in.r_squeeze = cq_hand.r_squeeze; }
    else { in.d_weapon_m = 0; in.d_head_m = 0; }                       /* no fresh hand: never a gesture */
    ReleaseSRWLockShared(&cq_lock);
    if (cq_on_weapon && cq_on_weapon()) in.d_weapon_m = 0;              /* two-hand aiming: the left hand is busy */
    if (cq_read_game(image, &act, &inr)) { cq_act = act; cq_inrange = inr; if (inr == 2) cq_near_tick = now; in.act = act; }
    in.enemy_near = cq_near_tick && now - cq_near_tick <= (DWORD)(cq_cfg.gate_s * 1000.0f);
    mgs4vr_cqc_step(&cq_sm, &cq_cfg, &in, now, &cq_out);
    if (cq_status[0] && (cq_cfg.wanted || cq_sm.grabs) && now - cq_status_tick >= 1000u) {
        cq_status_tick = now;
        if (!fopen_s(&f, cq_status, "w")) {
            fprintf(f, "cqc_version=1 wanted=%d gate=%d punch=%.1f throw=%.1f gate_s=%.1f state=%d grabs=%ld strikes=%ld combos=%ld throws=%ld releases=%ld gated=%ld aborts=%ld reapplies=%ld act=%#x inrange=%d squeeze=%.2f v_fwd=%.2f peak=%.2f d_weapon=%.2f d_head=%.2f fps=%d\n",
                    cq_cfg.wanted, cq_cfg.gate_on, cq_cfg.punch_mps, cq_cfg.throw_mps, cq_cfg.gate_s, cq_sm.state, cq_sm.grabs, cq_sm.strikes, cq_sm.combos, cq_sm.throws, cq_sm.releases, cq_sm.gated, cq_sm.aborts, cq_sm.reapplies,
                    cq_act, cq_inrange, in.squeeze, in.v_fwd, cq_hand.peak, in.d_weapon_m, in.d_head_m, fps);
            fclose(f);
        }
    }
}
