/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
/* Action button in first person (16 Oct 2026, user: "pick up a body, lean against a wall, climb a ladder in FPS; now
   the action button sidesteps"). The mod's first person is the game's aim + first-person view (mgs4vr_fpshold: right
   mouse + F held, Snake act 0x2F); the game's context actions only work in its normal stance (act 0x2: E at a wall ->
   act 0x4A, back to the wall, sim 16 Oct). X in first person therefore: leave first person (release aim + F), wait
   until Snake is out of it, press E while X is held; while Snake's act differs from his rest act the action runs (head
   camera from mgs4vr_cqcfp keeps the view first person), back at rest -> first person again; nothing changed within
   NOTHING_MS -> straight back. Marker mgs4vr_pad.on token nofpaction switches it off (X = E as before). */
#ifndef MGS4VR_FPACTION_H
#define MGS4VR_FPACTION_H
/* Item use (17 Oct 2026, user: use items such as the ration from the item ring): the game uses the current item only
   in its normal stance, with the item window (hold 1) and the left mouse button (sim 17 Oct: life 200 -> 700 three times
   in a row; at full life a ration is refused). mgs4vr_fpaction_request_use(): leave first person (if in it), hold 1,
   click the left button at USE_CLICK_MS, let go of 1 at USE_TOTAL_MS, back to first person. */
enum { FA_IDLE, FA_LEAVE, FA_PRESS, FA_ACTIVE, FA_RETURN, FA_USE };
#define MGS4VR_FA_ACT_FP 0x2f
typedef struct { int state, base, x_was, use_req, kind, was_fp; unsigned at, rest_at; } MGS4VR_FA;   /* kind 0 action, 1 item use */
typedef struct { int suspend_fp, hold_e, head_cam, hold_1, lmb; } MGS4VR_FA_OUT;
/* Pure step (worker thread): x = X held, fp = first person held (fpshold stage 2), act = Snake's act (-1 unknown). */
void mgs4vr_fpaction_step(MGS4VR_FA *s, int enabled, int x, int fp, int act, unsigned now, MGS4VR_FA_OUT *o);
/* Glue (worker): state for the other modules. */
void mgs4vr_fpaction_tick(int enabled, int x, int fp, int act, unsigned now);
int mgs4vr_fpaction_suspend_fp(void);   /* fpshold: release first person */
int mgs4vr_fpaction_hold_e(void);       /* pad: E down (replaces X = E while the machine runs) */
int mgs4vr_fpaction_busy(void);         /* pad: the machine owns X / E now */
int mgs4vr_fpaction_head_cam(void);     /* cqcfp: head camera although the game is not in first person */
int mgs4vr_fpaction_state(void);
void mgs4vr_fpaction_request_use(void);   /* worker: use the current item (item ring) */
int mgs4vr_fpaction_hold_1(void);         /* pad: key 1 (item window) down */
int mgs4vr_fpaction_lmb(void);            /* pad: left mouse down (use) */
#endif
