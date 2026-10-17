#ifndef MGS4VR_TWOEYE_H
#define MGS4VR_TWOEYE_H
/* Two eyes per frame with the game's own second render view (13 Oct 2026, sim-proven route, see
  ). MGS4 keeps dormant full world views next to the main view
   (rva 0x1DDDC70, stride 0x1590); view 1 renders the whole pipeline (own shadows, world, post) and is
   composited into the final target before the HUD. Side by side: main view = left eye in the left half,
   view 1 = right eye in the right half (3840x2160 internal -> 1920x2160 per eye).
   Camera block (row vectors, builder 0xB9B70): +0x00 camera-to-world, +0x40 its inverse, +0xC0/+0x140/+0x1C0
   projections, +0x100 = w2c*P1, +0x180 = w2c*P2, +0x2C0 six planes = columns (3-0,3+0,3+1,3-1,3-2,3+2) of
   +0x100, normalised by xyz (desk-fitted on a live block: relative error 1e-8).
   Right eye = H_R * H_L^-1 * left camera-to-world (H = the eye adjust the camera hook applied), projections
   with the horizontal skew (element [2][0]) negated - exact when the eye FOVs are mirror images (checked by the
   eye module per frame; otherwise the frame stays ordinary AFR).
   Marker mgs4vr_twoeye.on = "on" next to mgs4.exe; status mgs4vr_twoeye_<pid>.txt. Without the marker nothing
   is written and the views are restored. */
#include "mgs4vr_eye.h"
#define MGS4VR_TWOEYE_BLOCK 0x350
/* Pure (desk tests): right-eye camera block from the left one. mirror = negate the projection skew. 0 = invalid. */
int mgs4vr_twoeye_right_block(const unsigned char *left, const MGS4VR_CAM_ADJUST *adj_left,
                              const MGS4VR_CAM_ADJUST *adj_right, int mirror, unsigned char *out);
/* Pure (desk tests): lens camera block - same camera, symmetric square frustum with scale k = 1/tan(fov/2). */
/* View 1's projection-builder inputs (+0x380..+0x38F): the left ones with the right eye's x skew. */
void mgs4vr_twoeye_right_builder(const unsigned char *left_builder, const unsigned char *right_block, unsigned char *out);
/* Colour pass (2D list 3, opcode 05): copy of the 0x20-byte command with u0/u1 moved by the view x when the command samples
   exactly the view-size rect from 0,0 and the list is in mask; rect = view x, y, w, h. 1 = out holds the shifted copy. */
int mgs4vr_twoeye_uv_shift(const unsigned char *cmd, int list, const int rect[4], unsigned mask, unsigned char out[0x20]);
/* General form: source exactly the main scene (0,0,scene_w,scene_h) -> the executing view's own rect (not for the main view). */
int mgs4vr_twoeye_uv_remap(const unsigned char *cmd, int list, const int rect[4], int scene_w, int scene_h, unsigned mask, unsigned char out[0x20]);
/* HUD per eye: centred 16:9 box in the left (right=0) or right eye half of the full back buffer; vp = x0,y0,x1,y1,w,h. */
void mgs4vr_twoeye_hud_box(int full_w, int full_h, int right, int vp[6]);
/* Pure: the same box centred on the eye's optical axis (fov = OpenXR angles l,r,u,d); falls back to the centred box. */
void mgs4vr_twoeye_hud_box_fov(int full_w, int full_h, int right, const float fov[4], int vp[6]);
int mgs4vr_twoeye_lens_block(const unsigned char *left, float k, const float *rel, unsigned char *out);   /* rel: mgs4vr_lens_barrel, NULL = head camera */
/* Game thread, from the camera hook's finish_eye: every build of a camera. */
void mgs4vr_twoeye_frame(void *image_base, unsigned long long camera, int gameplay, int applied,
                         const MGS4VR_EYE_TICKET *ticket);
/* Worker: marker, status. Returns 1 while the mode is wanted (the caller tells the eye module). */
int mgs4vr_twoeye_poll(const char *exe);
#endif
