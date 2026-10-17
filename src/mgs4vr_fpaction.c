/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
/* Action button in first person; see mgs4vr_fpaction.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mgs4vr_fpaction.h"

#define LEAVE_MAX_MS 600u     /* first person not left by then: give up */
#define SETTLE_MS 150u        /* out of first person: let the stance settle before E */
#define NOTHING_MS 600u       /* E pressed, act unchanged this long (and X let go): nothing to do here */
#define REST_MS 300u          /* back at the rest act this long: the action is over */
#define RETURN_MAX_MS 1500u   /* first person not back by then: idle anyway (fpshold keeps trying) */
#define USE_CLICK_MS 400u     /* item window open (1 held) this long before the click */
#define USE_CLICK_LEN 120u
#define USE_TOTAL_MS 900u     /* then 1 is let go */
/* Rest acts (headset 17 Oct: stuck in ACTIVE at act 1 after a wall action begun from act 2 - head view with third-person
   controls): the start act, or Snake's plain standing acts 1 and 2. */
#define AT_REST(act, base) ((act) == (base) || (act) == 1 || (act) == 2)

void mgs4vr_fpaction_step(MGS4VR_FA *s, int enabled, int x, int fp, int act, unsigned now, MGS4VR_FA_OUT *o) {
    int rise = x && !s->x_was;
    s->x_was = x;
    o->suspend_fp = o->hold_e = o->head_cam = o->hold_1 = o->lmb = 0;
    if (!enabled) { s->state = FA_IDLE; s->use_req = 0; return; }
    switch (s->state) {
    case FA_IDLE:
        if (s->use_req) {                                       /* item use: from first person via LEAVE, else at once */
            s->use_req = 0; s->kind = 1; s->was_fp = fp; s->at = now; s->rest_at = 0;
            if (fp) { s->state = FA_LEAVE; o->suspend_fp = o->head_cam = 1; } else { s->state = FA_USE; o->hold_1 = 1; }
            return;
        }
        if (rise && fp) { s->state = FA_LEAVE; s->kind = 0; s->was_fp = 1; s->at = now; s->rest_at = 0; o->suspend_fp = o->head_cam = 1; }
        return;
    case FA_LEAVE:
        o->suspend_fp = o->head_cam = 1;
        if (act != MGS4VR_FA_ACT_FP && !fp) {                    /* act unknown (-1, cqcfp off): E after the settle time, no action tracking */
            if (!s->rest_at) s->rest_at = now;
            if (now - s->rest_at >= SETTLE_MS) {
                if (s->kind == 1) { s->state = FA_USE; s->at = now; s->rest_at = 0; o->hold_1 = 1; }
                else { s->state = FA_PRESS; s->base = act; s->at = now; s->rest_at = 0; o->hold_e = 1; } }
        } else if (now - s->at >= LEAVE_MAX_MS) { s->state = FA_RETURN; s->at = now; s->rest_at = 0; o->suspend_fp = 0; }
        return;
    case FA_PRESS:
        o->suspend_fp = o->head_cam = 1; o->hold_e = x || now - s->at < SETTLE_MS;
        if (act >= 0 && !AT_REST(act, s->base) && act != MGS4VR_FA_ACT_FP) { s->state = FA_ACTIVE; s->rest_at = 0; }
        else if (!x && now - s->at >= NOTHING_MS) { s->state = FA_RETURN; s->at = now; o->suspend_fp = o->hold_e = 0; }
        return;
    case FA_ACTIVE:
        o->suspend_fp = o->head_cam = 1; o->hold_e = x;   /* holding X keeps E down (drag a body) */
        if (AT_REST(act, s->base)) { if (!s->rest_at) s->rest_at = now; if (now - s->rest_at >= REST_MS && !x) { s->state = FA_RETURN; s->at = now; o->suspend_fp = o->hold_e = 0; } }
        else s->rest_at = 0;
        return;
    case FA_USE:
        o->suspend_fp = o->head_cam = s->was_fp; o->hold_1 = now - s->at < USE_TOTAL_MS; o->lmb = now - s->at >= USE_CLICK_MS && now - s->at < USE_CLICK_MS + USE_CLICK_LEN;
        if (now - s->at >= USE_TOTAL_MS) { s->state = s->was_fp ? FA_RETURN : FA_IDLE; s->at = now; o->suspend_fp = o->hold_1 = 0; o->head_cam = s->was_fp && !fp; }
        return;
    case FA_RETURN:
        o->head_cam = !fp;                                 /* the view stays at the head until first person is back */
        if (fp || now - s->at >= RETURN_MAX_MS) { s->state = FA_IDLE; o->head_cam = 0; }
        return;
    }
    s->state = FA_IDLE;
}

static MGS4VR_FA fa; static volatile LONG fa_suspend, fa_e, fa_head, fa_state, fa_runs, fa_use_req, fa_1, fa_lmb, fa_uses;
void mgs4vr_fpaction_request_use(void) { InterlockedExchange(&fa_use_req, 1); InterlockedIncrement(&fa_uses); }
int mgs4vr_fpaction_hold_1(void) { return InterlockedCompareExchange(&fa_1, 0, 0) != 0; }
int mgs4vr_fpaction_lmb(void) { return InterlockedCompareExchange(&fa_lmb, 0, 0) != 0; }
void mgs4vr_fpaction_tick(int enabled, int x, int fp, int act, unsigned now) {
    MGS4VR_FA_OUT o; int was = fa.state;
    if (InterlockedExchange(&fa_use_req, 0)) fa.use_req = 1;
    mgs4vr_fpaction_step(&fa, enabled, x, fp, act, now, &o);
    if (was == FA_IDLE && fa.state != FA_IDLE) InterlockedIncrement(&fa_runs);
    InterlockedExchange(&fa_suspend, o.suspend_fp); InterlockedExchange(&fa_e, o.hold_e); InterlockedExchange(&fa_head, o.head_cam); InterlockedExchange(&fa_state, fa.state);
    InterlockedExchange(&fa_1, o.hold_1); InterlockedExchange(&fa_lmb, o.lmb);
}
int mgs4vr_fpaction_suspend_fp(void) { return InterlockedCompareExchange(&fa_suspend, 0, 0) != 0; }
int mgs4vr_fpaction_hold_e(void) { return InterlockedCompareExchange(&fa_e, 0, 0) != 0; }
int mgs4vr_fpaction_busy(void) { return InterlockedCompareExchange(&fa_state, 0, 0) != FA_IDLE; }
int mgs4vr_fpaction_head_cam(void) { return InterlockedCompareExchange(&fa_head, 0, 0) != 0; }
int mgs4vr_fpaction_state(void) { return (int)InterlockedCompareExchange(&fa_state, 0, 0); }
