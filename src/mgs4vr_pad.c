/* mgs4vr_pad.c - see mgs4vr_pad.h. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <share.h>
#include "mgs4vr_pad.h"
#include "mgs4vr_fpaction.h"

#define STALE_MS 100u
#define FIRE_ON 0.6f
#define FIRE_OFF 0.4f
#define WASD_ON 0.5f
#define WASD_OFF 0.35f
#define LOOK_DEAD 0.15f
#define LOOK_MAX_DT 50u
enum { MODE_OFF, MODE_ON, MODE_WASD };
typedef DWORD (WINAPI *GET_STATE)(DWORD, XINPUT_STATE *);

static volatile LONG calls, calls_pad0, virtual_answers, real_connected, stick_active, repairs, hand_samples, fire_presses, wasd_presses, stick_suppressed, space_presses;
static volatile LONG look_moves, look_allowed, look_x = 900, look_y = 600, look_invert;
/* Smooth/snap turning on the right stick (marker tokens turn=smooth|snap, turndeg=, snapdeg=, pxdeg=):
   smooth = linear turndeg deg/s at full deflection, snap = one snapdeg turn per flick (re-arms below 0.3).
   pxdeg = mouse pixels per degree of player yaw (sim 12 Oct 2026: ~48). turn=mouse keeps the old curve. */
static volatile LONG turn_mode, turn_deg = 120, snap_deg = 30, px_deg_x10 = 260, look_px_total, snaps, closed_frames; static int snap_armed = 1;
/* Closed loop (MGS4 mouse look has acceleration): a heading source steers the mouse until the target is reached. */
static int (*heading_fn)(float *deg); static float turn_target; static int turn_have; static DWORD snap_until;
/* Snap: a fixed pixel train at TURN_MAX_PX per frame (constant speed = constant game acceleration):
   sim 12 Oct 2026 204 px -> 17 deg three times = 12 px/deg at that speed (marker snappx=). */
static volatile LONG snap_px_deg = 12; static float snap_pending;
void mgs4vr_pad_set_heading_source(int (*fn)(float *deg)) { heading_fn = fn; }
static float wrap180(float a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
#define TURN_MAX_PX 80.0f
#define TURN_MOUSE 0
#define TURN_SMOOTH 1
#define TURN_SNAP 2
static float look_acc[2]; static DWORD look_tick;                  /* XR thread only */
static volatile LONG use_presses, e_presses, tab_presses, menu_ticks, menu_enters, menu_buttons, menu_ctx, menu_detect = 1;
static volatile LONG pad_fire, pad_fire_frames;
static FILE *trace; static char trace_path[MAX_PATH];
static GET_STATE real_get_state;
static void **slot;
static int installed, enabled, attempted;
static volatile LONG pad_mode, suppress_stick;                   /* suppress: WASD owns the stick */
static int fire_down, key_down[4], space_down, e_down, one_down, tab_down, cqc_req, cqc_down, cqc_dir, ctrl_down, lclick_down, sneak_toggle;   /* worker thread only */
/* Menu button (16 Oct 2026, user: "recenter by holding the menu button a few seconds"): a short press is a Tab tap on
   release (pause menu, as before), holding it recenter_ms recenters once and sends no Tab. Token recenterms=<ms>
   (500..10000, default 1000; was 2000, user 16 Oct: far too long); recenterms=0 = the old behaviour (Tab while held, no recenter). */
static MGS4VR_MENU_BTN menu_btn; static volatile LONG recenter_ms = 1000, recenter_req, recenters;
static volatile LONG sneak_enabled = 1, sneak_max_x100 = 80, sneak_presses, sneak_toggles;
static volatile LONG tap_req, taps; static WORD tap_vk, tap_down_vk; static DWORD tap_at;   /* one key tap (camo reset) */
static MGS4VR_PAD_SEND sender;
static char marker[MAX_PATH], status_path[MAX_PATH];
static DWORD poll_at, status_at, started;
/* Latest left stick + right trigger (seqlock: XR thread writes, input/worker threads read). */
typedef struct { float stick[2], trigger, rstick[2]; DWORD tick; int valid, rvalid; unsigned rbuttons, lbuttons; } STICK;
static STICK sv; static volatile LONG sv_seq;
static DWORD packet;
static UINT send_one(INPUT *in);
void mgs4vr_pad_look(const float *rstick, DWORD now);
static XINPUT_STATE last_out;

void mgs4vr_pad_hands(const MGS4VR_XR_HANDS *h) {
    STICK s; const MGS4VR_XR_HAND *l, *r;
    if (!h) return;
    l = &h->hand[0]; r = &h->hand[1];
    memset(&s, 0, sizeof(s));
    s.valid = l->active && isfinite(l->stick[0]) && isfinite(l->stick[1]);
    if (s.valid) { s.stick[0] = l->stick[0]; s.stick[1] = l->stick[1]; }
    if (l->active) s.lbuttons = l->buttons;
    s.rvalid = r->active && isfinite(r->trigger);
    if (s.rvalid) { s.trigger = r->trigger; s.rbuttons = r->buttons; }
    if (s.rvalid && isfinite(r->stick[0]) && isfinite(r->stick[1])) { s.rstick[0] = r->stick[0]; s.rstick[1] = r->stick[1]; }
    s.tick = GetTickCount();
    InterlockedIncrement(&sv_seq); MemoryBarrier();
    sv = s;
    MemoryBarrier(); InterlockedIncrement(&sv_seq);
    InterlockedIncrement(&hand_samples);
    mgs4vr_pad_look(s.rvalid ? s.rstick : NULL, s.tick);
}
static int read_stick(STICK *out) {
    int tries; LONG a, b;
    for (tries = 0; tries < 8; ++tries) {
        a = sv_seq; MemoryBarrier();
        if (a & 1) { YieldProcessor(); continue; }
        *out = sv; MemoryBarrier(); b = sv_seq;
        if (a == b) return 1;
    }
    return 0;
}
static SHORT axis(float v) {
    if (v > 1) v = 1; if (v < -1) v = -1;
    return (SHORT)(v < 0 ? v * 32768.0f : v * 32767.0f);
}
static SHORT stronger(SHORT a, SHORT b) { return abs(a) >= abs(b) ? a : b; }
DWORD mgs4vr_pad_merge(DWORD real_rc, const XINPUT_STATE *real, int vr_fresh, const float stick[2], WORD buttons, XINPUT_STATE *out) {
    SHORT x = 0, y = 0;
    if (real_rc == ERROR_SUCCESS && real) *out = *real; else memset(out, 0, sizeof(*out));
    if (vr_fresh && stick) { x = axis(stick[0]); y = axis(stick[1]); }                 /* OpenXR and XInput: +y = up */
    out->Gamepad.sThumbLX = stronger(out->Gamepad.sThumbLX, x);
    out->Gamepad.sThumbLY = stronger(out->Gamepad.sThumbLY, y);
    if (vr_fresh) out->Gamepad.wButtons |= buttons;
    return ERROR_SUCCESS;                                                              /* pad 0 is always connected while on */
}
static int (*look_block)(void);
static int (*move_block)(void);   /* item ring open (mgs4vr_wrist.c): the left stick picks an item, no walking */
static int (*act_source)(void);   /* Snake's act (mgs4vr_cqcfp), -1 unknown: the first-person action machine */
static volatile LONG fpaction_on = 1;   /* token nofpaction */
void mgs4vr_pad_set_act_source(int (*fn)(void)) { act_source = fn; }/* Direct turning (13 Oct 2026, user: no mouse emulation): the game thread's XInput poll (once per
   game frame) writes the view yaw itself. Mouse turning stuttered: the game smooths mouse deltas and the closed
   loop overshot, the yaw went 2 frames forward and 1-2 back (sim: -83.6, -87.9, -83.5, -76.9, -87.4 deg).
   Smooth: yaw -= deflection x turndeg x dt (dt = game frame time, clamped). Snap: one snapdeg step per flick.
   Default when a yaw source is set and found; marker token "turnvia=mouse" keeps the old mouse route. */
static float *(*yaw_fn)(void);
static volatile LONG turn_via_mouse, direct_frames, direct_snaps, direct_misses, direct_tick;
static LARGE_INTEGER direct_prev; static int direct_snap_armed = 1;   /* game thread only */
void mgs4vr_pad_set_yaw_source(float *(*fn)(void)) { yaw_fn = fn; }
/* Pure step (desk tests): new yaw for stick x (-1..1), seconds dt; *armed carries the snap state. */
float mgs4vr_pad_turn_step(float yaw, float x, float dt, int mode, float rate_deg, float snap_deg_v, int *armed, int *snapped) {
    float a = fabsf(x), lin;
    if (snapped) *snapped = 0;
    if (mode == TURN_SMOOTH) {
        lin = a <= LOOK_DEAD ? 0 : (a > 1 ? 1 : (a - LOOK_DEAD) / (1 - LOOK_DEAD));
        if (dt > 0.05f) dt = 0.05f; if (dt < 0) dt = 0;
        yaw -= (x < 0 ? -lin : lin) * rate_deg * 0.017453293f * dt;           /* stick right = turn right = yaw down */
    } else if (mode == TURN_SNAP) {
        if (a < 0.3f) *armed = 1;
        else if (*armed && a > 0.7f) { *armed = 0; yaw -= (x < 0 ? -1.0f : 1.0f) * snap_deg_v * 0.017453293f; if (snapped) *snapped = 1; }
    }
    while (yaw > 3.14159265f) yaw -= 6.28318531f;
    while (yaw < -3.14159265f) yaw += 6.28318531f;
    return yaw;
}
static int direct_on(void) { return yaw_fn && !turn_via_mouse && (turn_mode == TURN_SMOOTH || turn_mode == TURN_SNAP); }
/* Game thread (XInput poll). */
static void direct_turn(const STICK *s) {   /* s = NULL: no fresh controller sample */
    LARGE_INTEGER now, f; float dt = 0, *p, y, ny; int snapped = 0;
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    if (direct_prev.QuadPart) dt = (float)(now.QuadPart - direct_prev.QuadPart) / (float)f.QuadPart;
    direct_prev = now;
    if (!direct_on() || !look_allowed || !s || !s->rvalid || (look_block && look_block())) { direct_snap_armed = 1; return; }
    p = yaw_fn();
    if (!p) { InterlockedIncrement(&direct_misses); return; }
    __try {
        y = *p;
        if (!isfinite(y)) { InterlockedIncrement(&direct_misses); return; }
        ny = mgs4vr_pad_turn_step(y, s->rstick[0], dt > 0.2f ? 0 : dt, turn_mode, (float)turn_deg, (float)snap_deg, &direct_snap_armed, &snapped);
        if (ny != y) *p = ny;
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&direct_misses); return; }
    InterlockedExchange(&direct_tick, (LONG)GetTickCount());
    InterlockedIncrement(&direct_frames);
    if (snapped) InterlockedIncrement(&direct_snaps);
}
static DWORD WINAPI hook_get_state(DWORD index, XINPUT_STATE *state) {
    DWORD rc; XINPUT_STATE real, out; STICK s; int fresh; WORD buttons = 0;
    InterlockedIncrement(&calls);
    rc = real_get_state ? real_get_state(index, &real) : ERROR_DEVICE_NOT_CONNECTED;
    if (index != 0 || !enabled || !state) { if (state && rc == ERROR_SUCCESS) *state = real; return rc; }
    InterlockedIncrement(&calls_pad0);
    if (rc == ERROR_SUCCESS) InterlockedIncrement(&real_connected);
    { int got = read_stick(&s) && GetTickCount() - s.tick <= STALE_MS;
      direct_turn(got ? &s : NULL);                                                  /* once per game frame */
      fresh = got && s.valid; }
    if (fresh && suppress_stick) { fresh = 0; InterlockedIncrement(&stick_suppressed); }
    if (fresh && menu_ctx && s.rvalid) {                                             /* game menu: A select, B back */
        if (s.rbuttons & 2u) buttons |= XINPUT_GAMEPAD_A;
        if (s.rbuttons & 4u) buttons |= XINPUT_GAMEPAD_B;
        if (buttons) InterlockedIncrement(&menu_buttons);
    }
    /* "padfire" (13 Oct 2026, Virtual Range test): outside first person the right trigger also goes to the virtual pad
       as RB + RT (the game takes pad input there; CQC/attack = R1 on the PS3 pad). */
    if (pad_fire && s.rvalid && !look_allowed && GetTickCount() - s.tick <= STALE_MS && s.trigger > FIRE_ON) { buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; InterlockedIncrement(&pad_fire_frames); }
    mgs4vr_pad_merge(rc, &real, fresh || (buttons != 0), fresh ? s.stick : NULL, buttons, &out);
    if (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) out.Gamepad.bRightTrigger = 255;
    if (fresh && (s.stick[0] * s.stick[0] + s.stick[1] * s.stick[1]) > 0.04f) InterlockedIncrement(&stick_active);
    /* our own packet number, advanced whenever the merged state changes */
    if (memcmp(&out.Gamepad, &last_out.Gamepad, sizeof(out.Gamepad))) { ++packet; last_out = out; }
    out.dwPacketNumber = packet;
    *state = out;
    InterlockedIncrement(&virtual_answers);
    return ERROR_SUCCESS;
}

/* XINPUT1_4.dll ordinal 2 in the host import table. */
/* Right stick -> relative mouse move. XR thread; the worker decides whether looking is allowed. */
static float look_curve(float v) {
    float a = fabsf(v);
    if (a <= LOOK_DEAD) return 0;
    if (a > 1) a = 1;
    a = (a - LOOK_DEAD) / (1 - LOOK_DEAD);
    return v < 0 ? -a * a : a * a;
}
/* Radial menu open (mgs4vr_wrist.c): the right stick picks a slot then, it must not turn (look_block above). */
void mgs4vr_pad_set_look_block(int (*fn)(void)) { look_block = fn; }
void mgs4vr_pad_set_move_block(int (*fn)(void)) { move_block = fn; }
void mgs4vr_pad_look(const float *rstick, DWORD now) {
    DWORD dt = now - look_tick; float sx, sy; int mx, my;
    look_tick = now;
    if (!rstick || !look_allowed || dt > 1000 || (look_block && look_block())) { look_acc[0] = look_acc[1] = 0; return; }
    if (dt > LOOK_MAX_DT) dt = LOOK_MAX_DT;                                          /* a hitch never becomes a jump */
    sx = look_curve(rstick[0]); sy = look_curve(rstick[1]);
    if (direct_on() && GetTickCount() - (DWORD)direct_tick < 200u) {                /* the game thread turns: only pitch here */
        look_acc[0] = 0; turn_have = 0; snap_pending = 0;
        look_acc[1] += (look_invert ? sy : -sy) * (float)look_y * (float)dt / 1000.0f;
        my = (int)look_acc[1]; if (!my) return; look_acc[1] -= (float)my;
        { INPUT in; ZeroMemory(&in, sizeof(in)); in.type = INPUT_MOUSE; in.mi.dy = my; in.mi.dwFlags = MOUSEEVENTF_MOVE; send_one(&in); InterlockedIncrement(&look_moves); }
        return;
    }
    { float hdg; if (turn_mode == TURN_SMOOTH && heading_fn && heading_fn(&hdg)) {
        float a = fabsf(rstick[0]), lin = a <= LOOK_DEAD ? 0 : (a > 1 ? 1 : (a - LOOK_DEAD) / (1 - LOOK_DEAD)), e, px;
        if (turn_mode == TURN_SMOOTH) {
            if (lin > 0) { if (!turn_have) { turn_target = hdg; turn_have = 1; } turn_target = wrap180(turn_target - (rstick[0] < 0 ? -lin : lin) * (float)turn_deg * (float)dt / 1000.0f); }
            else turn_have = 0;                                                          /* released: stop where we are */
        } else turn_have = 0;   /* snap: open-loop pixel train below */
        if (turn_have) { e = wrap180(turn_target - hdg); px = -e * (float)px_deg_x10 / 10.0f * 0.5f;   /* +px lowers the heading */
            if (px > TURN_MAX_PX) px = TURN_MAX_PX; if (px < -TURN_MAX_PX) px = -TURN_MAX_PX; look_acc[0] += px; InterlockedIncrement(&closed_frames); }
    } }
    if (turn_mode == TURN_SNAP) {
        float chunk;
        if (fabsf(rstick[0]) < 0.3f) snap_armed = 1;
        else if (snap_armed && fabsf(rstick[0]) > 0.7f) { snap_armed = 0; InterlockedIncrement(&snaps);
            snap_pending += (rstick[0] < 0 ? -1.0f : 1.0f) * (float)snap_deg * (float)snap_px_deg; }
        chunk = snap_pending > TURN_MAX_PX ? TURN_MAX_PX : snap_pending < -TURN_MAX_PX ? -TURN_MAX_PX : snap_pending;
        snap_pending -= chunk; look_acc[0] += chunk;
    } else if (turn_mode == TURN_SMOOTH && heading_fn) {
        /* handled by the closed loop above (no heading this frame: no turn rather than a wrong one) */
    } else
    if (turn_mode == TURN_SMOOTH) {                                                  /* linear: deflection beyond the dead zone x deg/s */
        float a = fabsf(rstick[0]), lin = a <= LOOK_DEAD ? 0 : (a > 1 ? 1 : (a - LOOK_DEAD) / (1 - LOOK_DEAD));
        look_acc[0] += (rstick[0] < 0 ? -lin : lin) * (float)turn_deg * (float)px_deg_x10 / 10.0f * (float)dt / 1000.0f;
    } else
    look_acc[0] += sx * (float)look_x * (float)dt / 1000.0f;
    look_acc[1] += (look_invert ? sy : -sy) * (float)look_y * (float)dt / 1000.0f;   /* stick up = mouse up = look up */
    mx = (int)look_acc[0]; my = (int)look_acc[1];
    if (!mx && !my) return;
    look_acc[0] -= (float)mx; look_acc[1] -= (float)my;
    { INPUT in; ZeroMemory(&in, sizeof(in));
      in.type = INPUT_MOUSE; in.mi.dx = mx; in.mi.dy = my; in.mi.dwFlags = MOUSEEVENTF_MOVE;
      send_one(&in); InterlockedIncrement(&look_moves); InterlockedAdd(&look_px_total, mx); }
}
static void **find_slot(BYTE *base) {
    IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)base; IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + d->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    if (!dir.VirtualAddress) return NULL;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
        IMAGE_THUNK_DATA64 *names, *iat;
        if (_stricmp((const char *)(base + imp->Name), "XINPUT1_4.dll")) continue;
        names = (IMAGE_THUNK_DATA64 *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        iat = (IMAGE_THUNK_DATA64 *)(base + imp->FirstThunk);
        for (; names->u1.Ordinal; ++names, ++iat)
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal) && IMAGE_ORDINAL64(names->u1.Ordinal) == 2) return (void **)&iat->u1.Function;
    }
    return NULL;
}
static int write_slot(void *value) {
    DWORD old, tmp;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return 0;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(*slot), old, &tmp);
    return 1;
}
static int read_mode(void) {
    FILE *f; char v[512] = { 0 };                                                    /* mode + options (lookx=... looky=... invert nomenu) */ size_t n; char *p = v;
    if (fopen_s(&f, marker, "rb")) return MODE_OFF;
    n = fread(v, 1, sizeof(v) - 1, f); fclose(f); v[n] = 0;
    if (n >= 3 && !memcmp(p, "\xef\xbb\xbf", 3)) p += 3;
    while (*p == ' ' || *p == '\t') ++p;
    InterlockedExchange(&menu_detect, strstr(p, "nomenu") == NULL);
    { const char *q; int v;
      InterlockedExchange(&look_x, (q = strstr(p, "lookx=")) && sscanf_s(q + 6, "%d", &v) == 1 && v >= 0 && v <= 20000 ? v : 900);
      InterlockedExchange(&look_y, (q = strstr(p, "looky=")) && sscanf_s(q + 6, "%d", &v) == 1 && v >= 0 && v <= 20000 ? v : 600);
      InterlockedExchange(&look_invert, strstr(p, "invert") != NULL);
      InterlockedExchange(&turn_via_mouse, strstr(p, "turnvia=mouse") != NULL);
      InterlockedExchange(&pad_fire, strstr(p, "padfire") != NULL);
      InterlockedExchange(&sneak_enabled, strstr(p, "nosneak") == NULL);
      InterlockedExchange(&fpaction_on, strstr(p, "nofpaction") == NULL);      { float fv; InterlockedExchange(&sneak_max_x100, (q = strstr(p, "sneak=")) && sscanf_s(q + 6, "%f", &fv) == 1 && fv >= 0.3f && fv <= 0.99f ? (LONG)(fv * 100 + 0.5f) : 80); }
      InterlockedExchange(&turn_mode, strstr(p, "turn=smooth") ? TURN_SMOOTH : strstr(p, "turn=snap") ? TURN_SNAP : TURN_MOUSE);
      InterlockedExchange(&turn_deg, (q = strstr(p, "turndeg=")) && sscanf_s(q + 8, "%d", &v) == 1 && v >= 10 && v <= 720 ? v : 120);
      InterlockedExchange(&snap_deg, (q = strstr(p, "snapdeg=")) && sscanf_s(q + 8, "%d", &v) == 1 && v >= 5 && v <= 180 ? v : 30);
      InterlockedExchange(&recenter_ms, (q = strstr(p, "recenterms=")) && sscanf_s(q + 11, "%d", &v) == 1 && (v == 0 || (v >= 500 && v <= 10000)) ? v : 1000);
      InterlockedExchange(&px_deg_x10, (q = strstr(p, "pxdeg=")) && sscanf_s(q + 6, "%d", &v) == 1 && v >= 1 && v <= 1000 ? v * 10 : 260);
      InterlockedExchange(&snap_px_deg, (q = strstr(p, "snappx=")) && sscanf_s(q + 7, "%d", &v) == 1 && v >= 1 && v <= 200 ? v : 12); }
    if (!strncmp(p, "off", 3)) return MODE_OFF;
    if (!strncmp(p, "wasd", 4)) return MODE_WASD;
    return MODE_ON;
}
/* ------------------------------------------------ keyboard/mouse (worker) --- */
static UINT send_one(INPUT *in) { MGS4VR_PAD_SEND s = sender ? sender : SendInput; return s(1, in, sizeof(*in)); }
static void mouse_left(int down) {
    INPUT in; ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE; in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    send_one(&in);
}
static void key(WORD vk, int down) {
    INPUT in; ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD; in.ki.wVk = vk; in.ki.wScan = (WORD)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);   /* Raw Input reports the make code */
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    send_one(&in);
}
static const WORD wasd_vk[4] = { 'W', 'S', 'A', 'D' };
static void gameplay_release(void);
void mgs4vr_pad_release(void) {
    if (tab_down) { key(VK_TAB, 0); tab_down = 0; }
    memset(&menu_btn, 0, sizeof(menu_btn));   /* a pending Tab tap or hold does not survive focus loss */
    gameplay_release();
}
static void gameplay_release(void) {
    int i;
    if (fire_down) { mouse_left(0); fire_down = 0; }
    if (cqc_down) { mouse_left(0); cqc_down = 0; }
    if (one_down) { key('1', 0); one_down = 0; }
    if (ctrl_down) { key(VK_CONTROL, 0); ctrl_down = 0; }
    if (tap_down_vk) { key(tap_down_vk, 0); tap_down_vk = 0; }
    if (space_down) { key(VK_SPACE, 0); space_down = 0; }
    if (e_down) { key('E', 0); e_down = 0; }
    for (i = 0; i < 4; ++i) if (key_down[i]) { key(wasd_vk[i], 0); key_down[i] = 0; }
    InterlockedExchange(&suppress_stick, 0);
}
/* Pure: menu button -> Tab key state (*tab) and 1 once when the hold recenters. hold_ms 0 = Tab while held. */
int mgs4vr_pad_menu_button(MGS4VR_MENU_BTN *b, int down, DWORD now, DWORD hold_ms, int *tab) {
    int fire = 0;
    if (!hold_ms) { b->pressed = down; b->fired = 0; b->tab_until = now; *tab = down; return 0; }
    if (down && !b->pressed) { b->pressed = 1; b->since = now; b->fired = 0; }
    else if (down && !b->fired && now - b->since >= hold_ms) { b->fired = 1; fire = 1; }
    else if (!down && b->pressed) { b->pressed = 0; if (!b->fired) b->tab_until = now + 100; }   /* short press: a 100 ms Tab tap */
    *tab = (LONG)(b->tab_until - now) > 0;
    return fire;
}
int mgs4vr_pad_take_recenter(void) { return InterlockedExchange(&recenter_req, 0) != 0; }
void mgs4vr_pad_cqc_lmb(int down) { cqc_req = down ? 1 : 0; }
void mgs4vr_pad_tap(WORD vk) { tap_vk = vk; InterlockedExchange(&tap_req, 1); }
void mgs4vr_pad_cqc_dir(int dir) { cqc_dir = dir > 0 ? 1 : dir < 0 ? -1 : 0; }
void mgs4vr_pad_set_sender(MGS4VR_PAD_SEND send) { sender = send; }
int mgs4vr_pad_menu(void) { return (int)menu_ctx; }
void mgs4vr_pad_tick(int allowed, int fps, int menu, DWORD now) {
    STICK s; int fresh, i, want[4]; float v[4];
    LONG m = installed ? pad_mode : MODE_OFF;
    fresh = read_stick(&s) && now - s.tick <= STALE_MS;
    menu = menu && menu_detect;
    InterlockedExchange(&look_allowed, allowed && m != MODE_OFF && fresh && fps && !(menu && menu_detect));
    if (!allowed || m == MODE_OFF || !fresh) { mgs4vr_pad_release(); InterlockedExchange(&menu_ctx, 0); return; }
    if (menu && !menu_ctx) InterlockedIncrement(&menu_enters);
    InterlockedExchange(&menu_ctx, menu);
    /* menu button (left, bit 3) = Tab: opens and closes the pause menu, so it works in both contexts */
    { int want = 0;
      if (mgs4vr_pad_menu_button(&menu_btn, (s.lbuttons & 8u) != 0, now, (DWORD)recenter_ms, &want)) { InterlockedExchange(&recenter_req, 1); InterlockedIncrement(&recenters); }
      if (want != tab_down) { key(VK_TAB, want); tab_down = want; if (want) InterlockedIncrement(&tab_presses); } }
    if (menu) { InterlockedIncrement(&menu_ticks); gameplay_release(); return; }      /* gameplay keys off; A/B/stick go to the pad */
    /* X (left primary, bit 1) = E (action), mirrored while held. In first person the action machine (mgs4vr_fpaction.h,
       16 Oct 2026) first leaves first person; while it runs it decides E. Token nofpaction = the old mirror everywhere. */
    { int x = (s.lbuttons & 2u) != 0, want, act = act_source ? act_source() : -1;
      mgs4vr_fpaction_tick(fpaction_on, x, fps, act, (unsigned)now);
      want = mgs4vr_fpaction_busy() ? mgs4vr_fpaction_hold_e() : (x && !(fps && fpaction_on));
      if (want != e_down) { key('E', want); e_down = want; if (want) InterlockedIncrement(&e_presses); }
      /* item use (17 Oct): the machine holds 1 (item window) and clicks the left button */
      want = mgs4vr_fpaction_hold_1();
      if (want != one_down) { key('1', want); one_down = want; if (want) InterlockedIncrement(&use_presses); } }
    /* CQC gesture owns the left mouse while it wants it (mgs4vr_cqc.h) */
    { int lreq = cqc_req || mgs4vr_fpaction_lmb();                                /* item use click owns it too */
      if (lreq != cqc_down) { if (lreq && fire_down) { mouse_left(0); fire_down = 0; } mouse_left(lreq); cqc_down = lreq; } }
    if (cqc_down) { } else
    /* fire: right trigger with hysteresis */
    if (s.rvalid) {
        if (!fire_down && s.trigger > FIRE_ON) { mouse_left(1); fire_down = 1; InterlockedIncrement(&fire_presses); }
        else if (fire_down && s.trigger < FIRE_OFF) { mouse_left(0); fire_down = 0; }
    } else if (fire_down) { mouse_left(0); fire_down = 0; }
    /* B (right secondary, bit 2) = Space, mirrored while held */
    { int want = s.rvalid && (s.rbuttons & 4u) != 0;
      if (want != space_down) { key(VK_SPACE, want); space_down = want; if (want) InterlockedIncrement(&space_presses); } }
    /* WASD: only in first person, only in wasd mode; then the pad stick stays neutral */
    InterlockedExchange(&suppress_stick, m == MODE_WASD && fps);
    v[0] = s.stick[1]; v[1] = -s.stick[1]; v[2] = -s.stick[0]; v[3] = s.stick[0];   /* W S A D */
    for (i = 0; i < 4; ++i) {
        want[i] = m == MODE_WASD && fps && s.valid && (key_down[i] ? v[i] > WASD_OFF : v[i] > WASD_ON);
        if (move_block && move_block()) want[i] = 0;                              /* item ring open: the stick picks */
        if ((i == 0 && cqc_dir > 0) || (i == 1 && cqc_dir < 0)) want[i] = 1;       /* CQC throw: W forward, S back */
    }
    /* Sneak (14 Oct 2026, user): Ctrl = walk. Half-deflected stick (below sneak_max, hysteresis 0.05) or the left stick
       click toggle; Ctrl only while a move key is down, and pressed before the move keys of the same tick. */
    {   int click = s.valid && (s.lbuttons & 1u) != 0, moving = want[0] || want[1] || want[2] || want[3], slow, wctrl;
        float r = sqrtf(s.stick[0] * s.stick[0] + s.stick[1] * s.stick[1]), lim = (float)sneak_max_x100 / 100.0f;
        /* 16 Oct 2026 (user): the left stick click is the item button now (mgs4vr_wrist.c: tap = item off/on, hold = item
           ring); its sneak toggle is gone, sneaking = the half-deflected stick. */
        (void)click; lclick_down = 0; sneak_toggle = 0;
        slow = ctrl_down ? r < lim + 0.05f : r < lim - 0.05f;
        wctrl = sneak_enabled && m == MODE_WASD && fps && moving && cqc_dir == 0 && (sneak_toggle || slow);
        if (wctrl != ctrl_down) { key(VK_CONTROL, wctrl); ctrl_down = wctrl; if (wctrl) InterlockedIncrement(&sneak_presses); }
    }
    for (i = 0; i < 4; ++i)
        if (want[i] != key_down[i]) { key(wasd_vk[i], want[i]); key_down[i] = want[i]; if (want[i]) InterlockedIncrement(&wasd_presses); }
    if (ctrl_down && !(key_down[0] || key_down[1] || key_down[2] || key_down[3])) { key(VK_CONTROL, 0); ctrl_down = 0; }
    /* one tap: down now, up 100 ms later (only in first person gameplay; a request outside it waits) */
    if (tap_down_vk && now - tap_at >= 100u) { key(tap_down_vk, 0); tap_down_vk = 0; }
    else if (!tap_down_vk && fps && InterlockedExchange(&tap_req, 0)) { key(tap_vk, 1); tap_down_vk = tap_vk; tap_at = now; InterlockedIncrement(&taps); }
}
static void write_status(const char *event) {
    FILE *f;
    if (fopen_s(&f, status_path, "w")) return;
    fprintf(f, "pad_version=3 event=%s installed=%d mode=%s elapsed_ms=%lu\n", event, installed,
            pad_mode == MODE_WASD ? "wasd" : pad_mode == MODE_ON ? "on" : "off", GetTickCount() - started);
    fprintf(f, "calls=%ld calls_pad0=%ld virtual_answers=%ld real_connected=%ld stick_active=%ld hand_samples=%ld repairs=%ld\n",
            calls, calls_pad0, virtual_answers, real_connected, stick_active, hand_samples, repairs);
    fprintf(f, "fire_presses=%ld wasd_presses=%ld stick_suppressed=%ld space_presses=%ld e_presses=%ld tab_presses=%ld recenters=%ld recenter_ms=%ld\n", fire_presses, wasd_presses, stick_suppressed, space_presses, e_presses, tab_presses, recenters, recenter_ms);
    fprintf(f, "look_moves=%ld look_x=%ld look_y=%ld look_invert=%ld turn=%s turn_deg=%ld snap_deg=%ld px_deg=%.1f look_px_total=%ld snaps=%ld closed_frames=%ld heading_source=%d\n", look_moves, look_x, look_y, look_invert,
        turn_mode == TURN_SMOOTH ? "smooth" : turn_mode == TURN_SNAP ? "snap" : "mouse", turn_deg, snap_deg, px_deg_x10 / 10.0, look_px_total, snaps, closed_frames, heading_fn != NULL);
    fprintf(f, "pad_fire=%ld pad_fire_frames=%ld\n", pad_fire, pad_fire_frames);
    fprintf(f, "sneak=%ld sneak_max=%.2f sneak_toggle=%d sneak_presses=%ld sneak_toggles=%ld taps=%ld\n", sneak_enabled, sneak_max_x100 / 100.0, sneak_toggle, sneak_presses, sneak_toggles, taps);
    fprintf(f, "use_presses=%ld ", use_presses); fprintf(f, "fpaction=%ld fpaction_state=%d fpaction_suspend=%d fpaction_e=%d fpaction_head=%d act=%d\n", fpaction_on, mgs4vr_fpaction_state(), mgs4vr_fpaction_suspend_fp(), mgs4vr_fpaction_hold_e(), mgs4vr_fpaction_head_cam(), act_source ? act_source() : -1);
    fprintf(f, "turn_direct=%d yaw_found=%d direct_frames=%ld direct_snaps=%ld direct_misses=%ld\n", direct_on(), yaw_fn && yaw_fn() != NULL, direct_frames, direct_snaps, direct_misses);
    fprintf(f, "menu_detect=%ld menu_ticks=%ld menu_enters=%ld menu_buttons=%ld\n", menu_detect, menu_ticks, menu_enters, menu_buttons);
    fclose(f);
}
int mgs4vr_pad_cursor_menu(int *showing, int *clipped) {
    CURSORINFO ci; RECT clip; int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN), sh = 0, cl = 0;
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci)) sh = (ci.flags & CURSOR_SHOWING) && ci.hCursor != NULL;
    if (GetClipCursor(&clip)) cl = clip.left > vx || clip.top > vy || clip.right < vx + vw || clip.bottom < vy + vh;
    if (showing) *showing = sh;
    if (clipped) *clipped = cl;
    return sh;
}
void mgs4vr_pad_trace(DWORD now, int showing, int clipped, int fps, long gameplay_builds, int menu) {
#if defined(DG_ENABLE_DIAGNOSTICS) && DG_ENABLE_DIAGNOSTICS == 0
    (void)now; (void)showing; (void)clipped; (void)fps; (void)gameplay_builds; (void)menu; return;   /* player build: no growing trace file */
#endif
    if (!installed || !status_path[0]) return;
    if (!trace) {
        strcpy_s(trace_path, sizeof(trace_path), status_path);
        sprintf_s(strrchr(trace_path, '\\') + 1, 64, "mgs4vr_padctx_%lu.csv", GetCurrentProcessId());
        trace = _fsopen(trace_path, "w", _SH_DENYWR);                                  /* readable while the game runs */
        if (!trace) return;
        fprintf(trace, "ms,cursor_showing,cursor_clipped,fps,gameplay_builds,menu\n");
    }
    fprintf(trace, "%lu,%d,%d,%d,%ld,%d\n", now - started, showing, clipped, fps, gameplay_builds, menu);
    fflush(trace);
}
void mgs4vr_pad_poll(void *image_base, const char *exe) {
    DWORD now = GetTickCount(); char *leaf;
    if (!marker[0]) {
        strcpy_s(marker, sizeof(marker), exe); leaf = strrchr(marker, '\\'); if (!leaf) { marker[0] = 0; return; }
        strcpy_s(leaf + 1, sizeof(marker) - (size_t)(leaf + 1 - marker), "mgs4vr_pad.on");
        strcpy_s(status_path, sizeof(status_path), marker);
        sprintf_s(strrchr(status_path, '\\') + 1, 64, "mgs4vr_pad_%lu.txt", GetCurrentProcessId());
    }
    if (!attempted) {
        void *old;
        if (GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES) return;
        attempted = 1; started = poll_at = status_at = now;
        slot = find_slot((BYTE *)image_base);
        if (!slot) { write_status("reject_no_import"); return; }
        old = *slot;
        if (!old || old == (void *)hook_get_state) { write_status("reject_slot"); return; }
        real_get_state = (GET_STATE)old;
        pad_mode = read_mode(); enabled = pad_mode != MODE_OFF;
        if (!write_slot((void *)hook_get_state)) { write_status("reject_protect"); return; }
        installed = 1; write_status("installed");
        return;
    }
    if (!installed) return;
    if (now - poll_at >= 250) {
        void *now_slot = *slot;
        poll_at = now;
        if (now_slot != (void *)hook_get_state) {                                    /* import table rewritten: chain to the new one */
            if (now_slot) real_get_state = (GET_STATE)now_slot;
            if (write_slot((void *)hook_get_state)) InterlockedIncrement(&repairs);
        }
        pad_mode = GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES ? read_mode() : MODE_OFF; enabled = pad_mode != MODE_OFF;
    }
    if (now - status_at >= 1000) { status_at = now; write_status("heartbeat"); }
}
