#ifndef MGS4VR_REC_H
#define MGS4VR_REC_H
/* Flight recorder for the arm/controller coupling (record BOTH
   halves per frame so the coupling can be designed and replayed at the desk).
   mgs4vr_rec.on next to mgs4.exe starts a recording; its content is a label
   (re-read every 250 ms, label changes are recorded). Deleting it stops.
   Producers never block and never allocate: they copy into lock-free rings
   and count what does not fit; only the worker touches the file. */
#include "mgs4vr_xr.h"
#if defined(MGS4VR_RECORDER) && MGS4VR_RECORDER
/* XR thread: one controller/head sample per XR frame. */
void mgs4vr_rec_hands(const MGS4VR_XR_HANDS *hands);
/* Game thread, on return of the gameplay camera build: camera object and the
   player's 115 bone input matrices of that frame. */
void mgs4vr_rec_game(void *image, unsigned long long camera, unsigned frame);
/* Worker: marker, file, draining. */
void mgs4vr_rec_poll(const char *exe);
int mgs4vr_rec_active(void);
#else
#define mgs4vr_rec_hands(hands) ((void)0)
#define mgs4vr_rec_game(image, camera, frame) ((void)0)
#define mgs4vr_rec_poll(exe) ((void)0)
#define mgs4vr_rec_active() 0
#endif
#endif
