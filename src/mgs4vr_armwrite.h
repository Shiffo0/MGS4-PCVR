#ifndef MGS4VR_ARMWRITE_H
#define MGS4VR_ARMWRITE_H
/* Phase 2 constant-replay write test: rotate the player's right arm (bone 10,
   upper arm, and its whole subtree) rigidly about the shoulder by a FIXED angle,
   every time the game recomputes the skinning palette.

   Measured (evidence arms runs 39396/41580, spread <= 4e-3):
       palette_b = B_b * W_b * inverse(Root)      (row vectors)
   B = constant inverse bind, W = world bone matrix (model+0x100), Root =
   model+0x20. A world-space rigid change W' = W * G is therefore exactly
       palette_b' = palette_b * H,  H = Root * G * inverse(Root)
   i.e. a rotation about the shoulder expressed in model space. It is applied
   right after the native palette build (0x117950), to that call's own palette
   (model+0x108+8*index), so the shared bone input is never modified.

   Marker mgs4vr_armwrite.on: "x 45" / "y -60" / "z 30" (model axis, degrees),
   "off" = installed but no writes. Deleting the marker restores the CALL.
   Heartbeat mgs4vr_armwrite_<pid>.txt every second (prove the
   writes reach the game before any solver exists). */
#if defined(MGS4VR_ARMWRITE) && MGS4VR_ARMWRITE
void mgs4vr_armwrite_poll(void *base, const char *exe);
int mgs4vr_armwrite_active(void);
/* Pure math (desk test): H for a rotation of `deg` about model axis `axis`
   through the shoulder at world position shoulder_w, given the model Root. */
void mgs4vr_armwrite_offset(const float root[16], const float shoulder_w[3], int axis, float deg, float h[16]);
#else
#define mgs4vr_armwrite_poll(base, exe) ((void)0)
#define mgs4vr_armwrite_active() 0
#endif
#endif
