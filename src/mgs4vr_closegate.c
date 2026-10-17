/* mgs4vr_closegate.c - see mgs4vr_closegate.h. */
#include "mgs4vr_closegate.h"

static WNDPROC cg_prev;
static HWND cg_window;
static void (*cg_cb)(const char *how);
static volatile LONG cg_done;

static LRESULT CALLBACK cg_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if ((msg == WM_CLOSE || msg == WM_DESTROY) && !InterlockedExchange(&cg_done, 1) && cg_cb)
        cg_cb(msg == WM_CLOSE ? "WM_CLOSE" : "WM_DESTROY");
    return CallWindowProcW(cg_prev, h, msg, w, l);
}

int mgs4vr_closegate_install(HWND window, void (*on_close)(const char *how)) {
    DWORD pid = 0; LONG_PTR prev;
    if (cg_window || !window || !on_close || !IsWindow(window)) return 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId()) return 0;
    cg_cb = on_close;
    prev = SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)cg_proc);
    if (!prev) return 0;
    cg_prev = (WNDPROC)prev; cg_window = window;
    return 1;
}

int mgs4vr_closegate_done(void) { return InterlockedCompareExchange(&cg_done, 0, 0) != 0; }
