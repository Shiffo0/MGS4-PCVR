/* mgs4vr_closegate - tear XR down before the game window closes (MGS3 recipe, 9 Oct 2026).
 *
 * Without an explicit xrDestroySession/xrDestroyInstance the Meta XR Simulator's Operator layer
 * (XrApiLayer_METAX_operator.dll) cleans up in its DLL detach, after process exit already removed its
 * threads, and the exit hangs there: the process stays as an unkillable 1-thread remnant holding ~2 GB
 * (MGS4 16 Oct: also after a clean in-game exit to the launcher). The gate subclasses the game window and
 * calls the close callback once on the first WM_CLOSE or WM_DESTROY, before the game's own handler. */
#ifndef MGS4VR_CLOSEGATE_H
#define MGS4VR_CLOSEGATE_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/* Present thread: install once on the swap chain's own window (other process / NULL ignored). 1 = installed now. */
int mgs4vr_closegate_install(HWND window, void (*on_close)(const char *how));
/* 1 after the callback ran. */
int mgs4vr_closegate_done(void);
#endif
