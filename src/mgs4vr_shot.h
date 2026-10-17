#ifndef MGS4VR_SHOT_H
#define MGS4VR_SHOT_H
/* Causal test for the bullet direction: only while
   firing, 0x5CF743.. copies the main camera (RVA 0x1DDDC70, or a second camera at +0x1590)
   camera-to-world into an object at [rdi+0x1C0..0x1FF]. Hypothesis: the bullet flies from that
   snapshot's position along its forward axis. We trap the instruction right after the copy
   (0x5CF7BE test edx,0x840: INT3 + emulated flags) and optionally rotate the snapshot.

   Marker mgs4vr_shot.on: "log" = count only, "yaw <deg>" = rotate the snapshot's axes about
   world Y (through its position), "pitch <deg>" = about its own right axis. Heartbeat
   mgs4vr_shot_<pid>.txt every second. Removing the marker restores the original byte. */
#include <windows.h>
#if defined(MGS4VR_SHOT) && MGS4VR_SHOT
void mgs4vr_shot_poll(void *base, const char *exe);
#else
#define mgs4vr_shot_poll(base, exe) ((void)0)
#endif
/* Pure helpers (desk tests). */
void mgs4vr_shot_rotate(float m[16], int axis, float deg);           /* axis 1 = world yaw, 0 = own pitch */
DWORD mgs4vr_shot_test_flags(DWORD eflags, DWORD edx, DWORD imm);    /* EFLAGS after TEST edx, imm */
#endif
