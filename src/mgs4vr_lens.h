#ifndef MGS4VR_LENS_H
#define MGS4VR_LENS_H
/* Lens scope, Titanfall 2 VR style (13 Oct 2026). The game's dormant world view 1 (see mgs4vr_twoeye.h) renders a
   narrow-FOV scope camera into a small square in the corner of the back buffer; at Present the XR module cuts that
   square out (and blanks it in the eye images) and shows it as a quad on the rifle in front of the right hand.
   Marker mgs4vr_lens.on next to mgs4.exe: "on [size=<px>] [fov=<deg>] [dist=<cm>] [width=<cm>] [cam=<cm>] [corner=bl] [head] [noreticle] [always] [weapons=<c>,<c>..]"
   (defaults 224 px top-left (left of the life bar), 6 deg, 22 cm in front of the grip, 6 cm wide). Two-eye mode (mgs4vr_twoeye.on) wins over the lens.
   The scope camera looks along the right controller's aim ray from cam= cm in front of it (default 70, past the muzzle);
   "head" keeps the head camera (debug). Only while a scoped weapon is held (current weapon u16 at rva 0x23FED464):
   default D.E. (L.B.) 9, VSS 39, M82A2 40, DSR-1 41, M14 EBR 42, Mosin 43, SVD 44, Rail Gun 45; weapons= (decimal or 0x) replaces the list,
   "always" ignores it. A crosshair + round vignette is a second, alpha-blended quad on top ("noreticle" = off).
   Status mgs4vr_lens_<pid>.txt. */
#define MGS4VR_LENS_WEAPON_RVA 0x23fed464
/* 13 Oct (headset): the camera and quad follow the GAME rifle, not the controller. The weapon model's shot anchor
   (mgs4vr_muzzle_contract.h native_shot: row 2 = barrel direction the bullets take, row 3 = muzzle, game units = mm)
   gives the scope camera front= cm past the muzzle along the barrel (default 5), roll from the eye camera; the quad sits
   on the rifle back= cm behind the muzzle (default 55) and up= cm above the barrel (default 8), mapped to XR space with
   the eye pose this frame was rendered for. "controller" = the old controller-aim route (also the fallback). */
/* Pure (desk tests): rel (as mgs4vr_lens_barrel) for a camera on the barrel; 0 = degenerate. */
int mgs4vr_lens_scope_rel(const float shot[16], const float eye_c2w[16], const float eye_w2c[16], float front_mm, float rel[16]);
/* Pure (desk tests): quad pose (XR) on the rifle: back_mm behind the muzzle, up_mm above (eye-camera up), facing back
   along the barrel, from the eye camera (w2c, game) and that eye's XR pose. 0 = degenerate. */
int mgs4vr_lens_world_quad(const float shot[16], const float eye_w2c[16], const float eye_q[4], const float eye_p[3],
                           float back_mm, float up_mm, float out_q[4], float out_p[3]);
/* Titanfall 2 VR style (default, "overlay" = the XR quad instead): the zoomed picture is drawn INTO each eye image on the
   ocular, a disk of radius= cm (default 2) at back=/up= on the rifle, perpendicular to the barrel; also on the desktop.
   Pure (desk tests): clip-space corners (uv 00, 10, 01, 11) of the disk's bounding square through vp (eye w2c*P, row
   vectors, block +0x100); 0 = degenerate or a corner behind the eye. */
/* 13 Oct (sim): every weapon model carries its sight line as anchors +0x4E0 (front: objective / front sight) and
   +0x560 (rear: the ocular / rear sight), same x/y above the barrel (M82A2 -53.8/-75.4 cm, M14 -48.1/-73.1, VSS -36.3/
   -67.9, SVD -61.9/-93.5, DSR-1 -38.4/-74.4; 5-8 cm up). Pure (desk tests): 1 when ocular/objective (world, game units)
   look like a scope on this barrel (behind the muzzle, 2-15 cm off the bore, 10-60 cm apart, axis within 10 deg). */
int mgs4vr_lens_sight_ok(const float shot[16], const float ocular[3], const float objective[3]);
/* Pure (desk tests): as mgs4vr_lens_ocular_clip, the disk centred on the ocular anchor, facing back along the scope axis. */
int mgs4vr_lens_sight_clip(const float ocular[3], const float objective[3], const float eye_c2w[16], const float vp[16], float radius_mm, float clip[16], const float roll[16]);
int mgs4vr_lens_ocular_clip(const float shot[16], const float eye_c2w[16], const float vp[16], float back_mm, float up_mm, float radius_mm, float clip[16]);
/* Game thread: corners for the frame with this camera serial/eye; Present: the matching (else same-eye, fresh) corners. */
void mgs4vr_lens_set_ocular(const float clip[16], unsigned serial, int eye);
int mgs4vr_lens_ocular(float clip[16], unsigned serial, int eye);
/* The same with the red dot's position on the disk (uv, mgs4vr_lens_dot_uv; 0.5,0.5 when stored without one). */
void mgs4vr_lens_set_ocular_dot(const float clip[16], const float dot[2], unsigned serial, int eye);
int mgs4vr_lens_ocular_dot(float clip[16], float dot[2], unsigned serial, int eye);
float mgs4vr_lens_dot_k(const float shot[16], const float c2w[16], float back, float up, float rad);
int mgs4vr_lens_dot_uv(const float shot[16], const float c2w[16], float back, float up, float rad, float uv[2]);
/* Game thread: publish the rifle-based quad pose (XR thread prefers it over the controller pose while fresh). */
void mgs4vr_lens_set_world_quad(const float q[4], const float p[3]);
#include "mgs4vr_xr.h"
typedef struct { int wanted, size, top, head, reticle, always, controller, round, inscope, view; float fov_deg, dist_m, width_m, cam_m, front_mm, back_mm, up_mm, radius_mm; unsigned long long weapons[8];
                 int ngeo, geo_set; struct { int code; float back_mm, up_mm, radius_mm, fov_deg; } geo[16];
                 int roll_weapon; float roll_x, roll_y; int dot, dotpic; float dot_fov_deg, dot_back_mm, dot_up_mm, dot_radius_mm; } MGS4VR_LENS_CFG;   /* dot: red dot sight via the lens (16 Oct 2026), tokens nodot, dotglass (no picture), dot=<fov deg>,<back cm>,<up cm>,<radius cm> */   /* top: 1 = top-left (default), corner=bl bottom-left */
/* Per weapon (each scope sits elsewhere on its rifle): built-in table, overridden/extended by marker tokens
   w<code>=<back>,<up>[,<radius>[,<fov>]] (cm, cm, cm, deg; code decimal or 0x..). Pure: cfg with that weapon's values. */
void mgs4vr_lens_apply_weapon(MGS4VR_LENS_CFG *cfg, int code);
/* Pure (desk tests): may the lens show with this weapon code? */
int mgs4vr_lens_weapon_ok(const MGS4VR_LENS_CFG *cfg, int code);
/* Pure (desk tests): n x n RGBA8 (R lowest byte) crosshair + vignette, premultiplied black; transparent elsewhere. */
void mgs4vr_lens_reticle(unsigned *rgba, int n, int round);   /* round: transparent (not black) outside the vignette ring */
/* Game thread: weapon code seen this frame (status only). */
void mgs4vr_lens_note_weapon(int code, int ok);
/* Pure (desk tests): marker text -> config (defaults, ranges). Returns cfg.wanted. */
int mgs4vr_lens_parse(const char *text, MGS4VR_LENS_CFG *cfg);
/* Worker: marker + status, every ~500 ms. */
void mgs4vr_lens_poll(const char *exe);
/* Any thread: current config. */
void mgs4vr_lens_config(MGS4VR_LENS_CFG *cfg);
/* Red dot sight (16 Oct 2026). The equip struct (rva 0x23FED43C: s16 count, s16 codes[8] at +8; current weapon 0x23FED464) has a
   u32 attachment mask per equip slot at rva 0x23FED4D4 + 4*slot; bit 0x4 = DOT SIGHT (part code 0x55): sim, M4 Custom slot 4,
   2 without / 6 with the dot sight. 1 = the held weapon carries a dot sight. */
#define MGS4VR_LENS_EQUIP_RVA 0x23fed43c
#define MGS4VR_LENS_SLOTMASK_RVA 0x23fed4d4
#define MGS4VR_LENS_DOT_BIT 0x4u
int mgs4vr_lens_dot_held(int count, const short codes[8], int current, const unsigned masks[8]);
/* 17 Oct 2026 (headset: red dot shown without a mounted sight; sim: the slot-mask bit 0x4 is a first-person flag, not the
   sight). The held weapon's attachment part code (mgs4vr_scope_attach_code: 0x55 = DOT SIGHT) decides instead. */
#define MGS4VR_LENS_DOT_PART 0x55
void mgs4vr_lens_set_attach_source(int (*fn)(void));
int mgs4vr_lens_dot_attached(void);
/* Set by the lens frame (game thread), read by the XR draw: the lens currently shows a red dot sight. */
void mgs4vr_lens_apply_dot(MGS4VR_LENS_CFG *cfg);
/* Roll source for the scope/disk/dot "down" axis (lens tokens roll=head, rollx=, rolly=; default -(weapon shot row 1), row 1 = up). */
void mgs4vr_lens_set_roll(int weapon, float rx, float ry);
void mgs4vr_lens_set_dot(int on);
int mgs4vr_lens_dot(void);
/* Game thread (twoeye module) marks the square it rendered this frame; XR/present read it (0 = none this second). */
void mgs4vr_lens_set_rect(int x, int y, int size);
int mgs4vr_lens_rect(int *x, int *y, int *size);
/* XR thread: hands -> quad pose (right grip + aim forward). */
void mgs4vr_lens_hands(const MGS4VR_XR_HANDS *hands);
/* XR thread: 1 with pose (x,y,z,w quat; position m) and width while the lens should be shown. */
int mgs4vr_lens_quad(float quat[4], float pos[3], float *width_m);
/* Game thread: 1 with the barrel camera relative to the head camera (row-vector 4x4, game camera axes x right, y down,
   z forward, game units = mm) while it is fresh and wanted. */
int mgs4vr_lens_barrel(float rel[16]);
/* Pure (desk tests): head + aim pose (OpenXR) -> rel as above, camera fwd_m along the aim -Z. */
void mgs4vr_lens_rel(const float head_q[4], const float head_p[3], const float aim_q[4], const float aim_p[3], float fwd_m, float rel[16]);
/* Pure (desk tests): quad pose in front of a grip pose along its -Z (aim) axis. */
void mgs4vr_lens_pose(const float grip_q[4], const float grip_p[3], float dist_m, float out_p[3]);
#endif
