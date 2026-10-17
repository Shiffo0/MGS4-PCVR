#ifndef MGS4VR_WATCH_H
#define MGS4VR_WATCH_H
/* One-shot hardware data watchpoint on the player's right-hand world matrix
   (bone 12, translation x at input + 12*64 + 48). Answers: which game code
   WRITES the hand every frame (where an arm pose must be written so the game
   attaches the weapon to it) and which code READS it (palette builder, weapon
   attachment, ...). Read-only diagnostic: nothing in the game is changed.

   mgs4vr_watch.on next to mgs4.exe starts one 5 s window. Its content picks the target:
     (empty) / "hand"  right-hand translation x (default, as before)
     "camera"          main camera camera-to-world forward axis x (C2W + 32): who reads the aim
     "camera_pos"      main camera camera-to-world translation x (C2W + 48)
     "rva <hex>"       image base + rva (e.g. an aim variable found by tools/aim_scan.py)
     "abs <hex>"       absolute address (heap); refused unless committed and readable
     "exec <hex>"      image rva of an INSTRUCTION: execution breakpoint, records every call chain
                       that runs it (e.g. the bullet-hole code, to find who fires the ray) The camera hooks use
   all four debug registers, so pause_cam() is called before arming and
   resume_cam() after. Output: mgs4vr_watch_<pid>.csv (unique instruction +
   caller chain, hit counts, value-changed counts) and mgs4vr_watch_<pid>.txt. */
#if defined(MGS4VR_WATCH) && MGS4VR_WATCH
void mgs4vr_watch_poll(void *base, const char *exe, unsigned frame, void (*pause_cam)(void), void (*resume_cam)(void));
int mgs4vr_watch_active(void);
/* Desk test: watch an explicit address instead of the player's hand. */
void mgs4vr_watch_set_target_override(volatile float *address);
#else
#define mgs4vr_watch_poll(base, exe, frame, pause_cam, resume_cam) ((void)0)
#define mgs4vr_watch_active() 0
#endif
#endif
