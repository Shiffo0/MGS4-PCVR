#ifndef MGS4VR_SCOPE_H
#define MGS4VR_SCOPE_H
/* Scoped weapons, phase 1: hide the game's full-screen scope mask in VR first
   person. Inert without mgs4vr_scope.on containing exactly "nomask". */
/* Worker. base = mgs4.exe image; first_person = the VR first-person hold is
   fully applied (aim + F). */
void mgs4vr_scope_poll(void *base,const char *exe,int first_person);
/* Frame boundary for the render-thread seam (present counter). */
void mgs4vr_scope_set_frame_source(unsigned (*fn)(void));
/* Puts the game's original bytes back (desk tests). 1 = restored. */
int mgs4vr_scope_stop(void);
void mgs4vr_scope_set_gesture(int (*fn)(void));
/* Every model's part mask passes through fn (bits = hidden parts), NULL = untouched. Needs the parts seam (arms). */
void mgs4vr_scope_set_parts_hook(unsigned long long (*fn)(const unsigned char *model,unsigned bones,unsigned long long mask));
float mgs4vr_scope_zoom(void);
void *mgs4vr_scope_held_weapon(void);
/* Part code of the held weapon's attachment (weapon model actor +0x98 -> +0xAA): 0x55 DOT SIGHT, 0 none (17 Oct 2026). */
int mgs4vr_scope_attach_code(void);
#endif
