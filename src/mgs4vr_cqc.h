#ifndef MGS4VR_CQC_H
#define MGS4VR_CQC_H
/* CQC with the LEFT hand in VR first person (13 Oct 2026, found live with the user).
   In VR first person Snake is in the aim stance (action pdata+0x10C = 0x2E, right mouse held by the first-person hold),
   where the CQC/attack button (left mouse = input bit 0x800) fires. With the aim released for a moment the same button
   starts CQC (0xE7): a tap = strike, held near an enemy = grab (0xEE/0xF0/0xF1), pressed again while holding = throw.
   Gestures (left hand, never while it is on the rifle or at the head):
     grab   = squeeze the left grip while the game reports an enemy in CQC range (pdata+0x16A8 == 2 within gate s);
              Snake holds as long as the grip is held; letting go releases.
     strike = a fast push of the left hand forward (no enemy needed).
     throw  = while holding, a fast swing forward or back.
   The first-person hold pauses the aim (right mouse up, F stays down: no third-person flash) for the gesture; the pad
   module presses the left mouse. holdaim (default; "noholdaim" = off): while holding, squeezing the RIGHT grip lets go
   of the button and brings the aim back = hold + aim (0x101), the trigger fires; opening the right grip = plain hold again
   (13 Oct: automatic aim after 0.5 s blocked throw/X/release in the headset). X = E = throat cut.
   Marker mgs4vr_cqc.on: "on [punch=<m/s>] [throw=<m/s>] [gate=<s>] [nogate] [noholdaim]"
   (defaults 2.0, 1.6, 1.5). Status mgs4vr_cqc_<pid>.txt. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mgs4vr_xr.h"
typedef struct { int wanted, gate_on, holdaim; float punch_mps, throw_mps, gate_s; } MGS4VR_CQC_CFG;
int mgs4vr_cqc_parse(const char *text, MGS4VR_CQC_CFG *cfg);
/* Pure state machine (desk tests). */
typedef struct { int fps, allowed; float squeeze, v_fwd, d_weapon_m, d_head_m; int enemy_near; float r_squeeze; int act; } MGS4VR_CQC_IN;   /* act: pdata+0x10C, 0 = unknown */
typedef struct { int pause_aim, lmb, dir, reapply_fp; } MGS4VR_CQC_OUT;   /* dir: throw W(+1)/S(-1); reapply_fp: one tick after a grab */
typedef struct { int state, prev_squeeze_hi, punch_armed, grabbed, throw_dir, tries; DWORD t, t_verify; long grabs, strikes, throws, releases, gated, aborts, combos, reapplies; } MGS4VR_CQC_SM;
void mgs4vr_cqc_step(MGS4VR_CQC_SM *sm, const MGS4VR_CQC_CFG *cfg, const MGS4VR_CQC_IN *in, DWORD now, MGS4VR_CQC_OUT *out);
/* XR thread: left-hand speed and grip, distances. */
void mgs4vr_cqc_hands(const MGS4VR_XR_HANDS *hands);
/* Worker, before the first-person hold and the pad tick. fps = first-person hold fully applied. */
void mgs4vr_cqc_tick(void *image, const char *exe, int allowed, int fps, DWORD now);
/* The arms module's "left hand snapped on the foregrip" (no grab then). */
void mgs4vr_cqc_set_on_weapon(int (*fn)(void));
int mgs4vr_cqc_pause_aim(void);
int mgs4vr_cqc_lmb(void);
int mgs4vr_cqc_dir(void);
int mgs4vr_cqc_reapply_fp(void);
/* 1 while a CQC gesture is running (not idle). */
int mgs4vr_cqc_busy(void);
#endif
