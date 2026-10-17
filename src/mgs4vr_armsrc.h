#ifndef MGS4VR_ARMSRC_H
#define MGS4VR_ARMSRC_H
/* Arm pose written AT THE SOURCE: right after the game's per-frame skeleton
   solve for Snake (CALL 0x9737B6 -> 0xD2E920, found with the hand watchpoint) and before the attachment component
   reads the hand (0x98269D). The weapon is then attached to OUR hand by the
   game itself (proved live).

   Marker mgs4vr_armsrc.on:
     "x|y|z degrees"  fixed rotation of the right arm about the shoulder (test)
     "ik"             right hand follows the right controller (phase 3)
     "off"            installed, no writes
   Heartbeat mgs4vr_armsrc_<pid>.txt every second.

   IK: absolute every frame, never integrated. Anchor = the game
   camera (the player's eyes in first person), NOT the animation hand: the
   controller relative to the head is converted to game view space (x right,
   y down, z forward, mm) and placed with the camera-to-world matrix. A fixed
   grip offset (rotation O, position in controller frame) maps the controller
   onto the hand bone; measured on recording run 13520 (rifle segment: rotation
   residual median 1.4 deg). Shoulder stays the game's, elbow from a two-bone
   solve with the game's elbow as pole hint, hand takes the target pose;
   descendants keep their local transforms. Only in first person (camera within
   400 mm of the head bone) and only with a controller sample younger than
   100 ms; otherwise the game's own pose is left untouched. */
#include "mgs4vr_xr.h"
#if defined(MGS4VR_ARMSRC) && MGS4VR_ARMSRC
void mgs4vr_armsrc_poll(void *base, const char *exe);
int mgs4vr_armsrc_active(void);
/* 1 while the left hand is snapped onto the foregrip (two-hand aiming). */
int mgs4vr_armsrc_left_on_weapon(void);
/* Worker thread: support swing toggle, optional 0/30/60/90 ms time constant,
   recenter reset token and foreground/menu gate. Never filters hand position. */
void mgs4vr_armsrc_configure(int enabled, int smoothing_ms, unsigned recenter);
/* Pure (desk tests): shoulder shift for shoulderback (mm): back along -(root row 2, horizontal) and down by half. */
void mgs4vr_armsrc_shoulder_vector(const float root[16], float back_mm, float v[3]);
/* Pistol gestures (phase 4): called on the game thread with 1 = slide racked, 2 = magazine inserted. */
void mgs4vr_armsrc_set_reload(void (*fn)(int kind));
/* Pose tool camera (game thread, camera seam): 1 = m replaced by a view of the pistol/hand (tool command "cam ..."). */
int mgs4vr_armsrc_pose_camera(float m[16]);
int mgs4vr_armsrc_pose_camera_build(const float hand[16], const float v[6], float m[16]);
void mgs4vr_armsrc_gameplay(int allowed);
/* XR thread: latest controller poses. */
void mgs4vr_armsrc_hands(const MGS4VR_XR_HANDS *hands);
/* Stereo renders one eye per game frame, and the skeleton solve reads the
   camera of the PREVIOUS frame (= the other eye; run 16796: hand exactly one
   eye distance off, 0.0 mm median with one frame camera lag). The camera
   finisher publishes each built camera together with the OpenXR eye pose it
   was built from (LOCAL space); IK places the controller relative to THAT eye
   and that camera, a consistent pair whatever the frame order. */
void mgs4vr_armsrc_eye_anchor(const float eye_q[4], const float eye_p[3], const float camera_to_world[16], int64_t display_time);   /* 16 Oct: + the packet's display time, the hand sample of that time drives the arm */
#else
#define mgs4vr_armsrc_poll(base, exe) ((void)0)
#define mgs4vr_armsrc_active() 0
#define mgs4vr_armsrc_configure(e, ms, r) ((void)0)
#define mgs4vr_armsrc_gameplay(a) ((void)0)
#define mgs4vr_armsrc_hands(hands) ((void)0)
#define mgs4vr_armsrc_eye_anchor(q, p, c, t) ((void)0)
static int mgs4vr_armsrc_left_on_weapon(void) { return 0; }
#endif
#endif
