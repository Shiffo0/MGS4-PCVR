/* mgs4vr_fpshold.c - see mgs4vr_fpshold.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mgs4vr_fpshold.h"

#define AIM_TO_KEY_MS 150u    /* the game must see the aim before the first-person key */
#define TOGGLE_GAP_MS 300u    /* debounce between two A presses */

static volatile LONG wanted, stage, last_toggle, a_down;
static DWORD stage_at;
static int pause_req, aim_paused;   /* worker */
static MGS4VR_SEND_INPUT sender;

static UINT send_one(INPUT *in) { MGS4VR_SEND_INPUT s = sender ? sender : SendInput; return s(1, in, sizeof(*in)); }
static void mouse_right(int down) {
    INPUT in; ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE; in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    send_one(&in);
}
static void key_f(int down) {
    INPUT in; ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD; in.ki.wVk = 'F'; in.ki.wScan = (WORD)MapVirtualKeyA('F', MAPVK_VK_TO_VSC);  /* Raw Input reports the make code */
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    send_one(&in);
}

void mgs4vr_fpshold_hands(const MGS4VR_XR_HANDS *h) {
    int down; DWORD now = GetTickCount();
    if (!h) return;
    down = h->hand[1].active && (h->hand[1].buttons & 2u);
    if (down && !a_down && now - (DWORD)last_toggle >= TOGGLE_GAP_MS) {
        InterlockedExchange(&wanted, !wanted);
        InterlockedExchange(&last_toggle, (LONG)now);
    }
    InterlockedExchange(&a_down, down);
}

void mgs4vr_fpshold_tick(int allowed, DWORD now) {
    if (wanted && allowed) {
        if (stage == 0) { mouse_right(1); InterlockedExchange(&stage, 1); stage_at = now; }
        else if (stage == 1 && now - stage_at >= AIM_TO_KEY_MS) { key_f(1); InterlockedExchange(&stage, 2); }
        else if (stage == 2 && pause_req != aim_paused) { mouse_right(!pause_req); aim_paused = pause_req; }   /* CQC: aim off/on, F stays */
        return;
    }
    mgs4vr_fpshold_release();
}

void mgs4vr_fpshold_release(void) {
    if (stage == 2) key_f(0);
    if (stage >= 1 && !aim_paused) mouse_right(0);
    aim_paused = 0;
    InterlockedExchange(&stage, 0);
}

void mgs4vr_fpshold_pause_aim(int pause) { pause_req = pause ? 1 : 0; }
void mgs4vr_fpshold_reapply(void) { mgs4vr_fpshold_release(); }   /* the next tick presses aim, then F, again */
int mgs4vr_fpshold_stage(void) { return (int)stage; }
int mgs4vr_fpshold_wanted(void) { return (int)wanted; }
void mgs4vr_fpshold_set_sender(MGS4VR_SEND_INPUT send) { sender = send; }
