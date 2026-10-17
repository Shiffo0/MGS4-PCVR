#ifndef MGS4VR_CQCFP_H
#define MGS4VR_CQCFP_H
/* First person during CQC (14 Oct 2026, user: "first person tijdens CQC, zodat mikken met de controllers kan").
   The game drops to its third-person camera for every CQC action (0xE0..0x120) and ignores F. At the camera seam
   (mgs4vr_cam adjust_entry, game thread, source matrix m = camera-to-world, row vectors, mm) this module swaps the
   third-person source for a head camera:
   - calibration, every non-CQC frame whose camera is within 400 mm of the head bone (first person; the action is 0x2E
     live in the aim stance, 0x2 in the simulator):
     camera rotation and offset relative to Snake's model root: Rrel = Rcam * Rroot^T, d = (cam - head) * Rroot^T;
   - override, during CQC: rotation Rrel * Rroot_now, position head_now + d * Rroot_now (follows Snake's body),
     or with "yaw=freeze" the calibrated rotation and d * Rroot_cal (no body turn).
   The VR head offset (H * m) is applied after this as usual.
   Marker mgs4vr_cqcfp.on: "on [yaw=body|freeze] [force]"; force = also override outside CQC (sim check: in first
   person the rebuilt camera must equal the game's own, status maxpos/maxrot). Status mgs4vr_cqcfp_<pid>.txt. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef struct { int wanted, freeze, force; } MGS4VR_CQCFP_CFG;
int mgs4vr_cqcfp_parse(const char *text, MGS4VR_CQCFP_CFG *cfg);
/* Pure math (desk tests). root = model root 4x4 (rows may carry scale), head = head bone position. */
typedef struct { float rrel[9], d[3], cam_rot[9], root_rot[9]; int valid; } MGS4VR_CQCFP_CAL;
int mgs4vr_cqcfp_calibrate(const float cam[16], const float root[16], const float head[3], MGS4VR_CQCFP_CAL *cal);
int mgs4vr_cqcfp_build(const MGS4VR_CQCFP_CAL *cal, const float root[16], const float head[3], int freeze, float out[16]);
/* Game thread, camera seam: may replace m (1 = replaced). */
int mgs4vr_cqcfp_seam(float m[16]);
void mgs4vr_cqcfp_set_head_source(int (*fn)(void));   /* also the head camera while fn() (first-person action, mgs4vr_fpaction.h) */
int mgs4vr_cqcfp_last_act(void);                      /* Snake's act from the last seam call, -1 before the first */
/* Parts seam (mgs4vr_scope_set_parts_hook): while the head camera is on, Snake's body gets mask 0x1E (arms shown,
   head hidden) and his head/face models, learned in first person, mask 3. */
unsigned long long mgs4vr_cqcfp_parts(const unsigned char *model, unsigned bones, unsigned long long mask);
/* Worker: marker/status. */
void mgs4vr_cqcfp_tick(void *image, const char *exe, DWORD now);
#endif
