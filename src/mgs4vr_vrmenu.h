/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
#ifndef MGS4VR_VRMENU_H
#define MGS4VR_VRMENU_H
/* VR SETTINGS in the game's own Options screen (16 Oct 2026, user choice "route B, option 2"). Inert without marker
   mgs4vr_vrmenu.on ("on"). 
   - Options screen (init 0x4FF210): per tab t item table +0x290+8t, count +0x27C+4t, first visible +0x26C+4t,
     cursor +0x25C+4t, current tab +0x258, state machine +0x98 (state at +0x98+0x10). Confirm on a row sets the state
     to the row's type (0x4FEB90); back sets state 10 (exit + apply by label id, 0x4FD4D0).
   - Seam 1, state set 0x4D40A0 (xor eax,eax; mov [rcx+10],edx; ...): entering state 1 on a fresh Options screen
     (tab table still the exe's 0x1C2DC20) puts a "VR Settings" row on top of CONTROLS (type VM_TYPE, a state the
     screen never registered); state VM_TYPE = swap the four tabs to the VR tables and stay in state 1; state 10
     while in VR = swap back and stay in state 1.
   - Seam 2, text lookup 0x432790(sub, id) (armed only while that screen is open): sub 0x101AB9 + our ids -> our
     strings; in VR mode the tab names (ids 0..3) too.
   - Values: the game edits s16[16] of our VR items itself (choice / slider states 3 and 4). The worker turns them
     into the markers (twoeye, lens nodot, pad turn/turndeg/snapdeg, armsrc grab/release, cqcfp, camo). */
#define MGS4VR_VM_TYPE 8
#define MGS4VR_VM_SUB 0x101ab9
#define MGS4VR_VM_TAB0_RVA 0x1c2dc20
#define MGS4VR_VM_TAB1_RVA 0x1c2dd20
#define MGS4VR_VM_ID0 0x7e00
#define MGS4VR_VM_ID1 0x7eff
#define MGS4VR_VM_SHOULDER_MAX 15   /* cm, = SHOULDER_MAX_CM in mgs4vr_armsrc.c */
typedef short MGS4VR_VM_ITEM[18];
enum { VM_TWOEYE, VM_REDDOT, VM_TURN, VM_TURNSPEED, VM_SNAP, VM_GRIP, VM_CQCFP, VM_CAMO, VM_HUD, VM_SPRITES, VM_SHOULDER, VM_NMARK,   /* marker settings */
       VM_STEREO = VM_NMARK, VM_FOLLOW, VM_DISTANCE, VM_OBSERVER, VM_N };                                   /* theater settings (mgs4vr.ini via the worker) */
/* Theater settings of the Home menu (MGS4VR_MENU, mgs4vr.ini): the worker hands them in each loop and takes changes back. */
typedef struct { int follow, distance_cm, stereo, observer, available, stereo_available; } MGS4VR_VM_THEATER;
void mgs4vr_vrmenu_set_theater(const MGS4VR_VM_THEATER *t);
int mgs4vr_vrmenu_take_theater(MGS4VR_VM_THEATER *t);   /* 0 nothing; 1 changed, 2 changed + recenter; *t = the new settings (rules applied) */
/* Pure: theater t + menu values v -> out, with the Home menu's rules; returns 1 if anything changed, *recenter as the Home menu. */
int mgs4vr_vm_apply_theater(const MGS4VR_VM_THEATER *t, const short v[VM_N], MGS4VR_VM_THEATER *out, int *recenter);
void mgs4vr_vm_theater_values(const MGS4VR_VM_THEATER *t, short v[VM_N]);
/* Marker texts (NULL = file missing) -> setting values (the item value index; marker settings only). */
typedef struct { const char *twoeye, *lens, *pad, *armsrc, *cqcfp, *camo, *hudhide; } MGS4VR_VM_MARKERS;
void mgs4vr_vm_values(const MGS4VR_VM_MARKERS *m, short v[VM_N]);
/* New marker text for setting k with value v, from the old text (NULL = missing); 0 = invalid. */
int mgs4vr_vm_marker_text(int k, short v, const char *old, char *out, size_t cap);
const char *mgs4vr_vm_marker_name(int k);
/* Our string for (sub, id), vr = VR tabs shown; NULL = not ours. */
const char *mgs4vr_vm_text(unsigned sub, unsigned id, int vr);
/* The VR tables (4 tabs) with values v; counts out. */
void mgs4vr_vm_tables(MGS4VR_VM_ITEM *tabs[4], int counts[4], const short v[VM_N]);
/* VR values back from the tables. */
void mgs4vr_vm_read(short v[VM_N]);
/* CONTROLS with the VR row on top: out[0] = row, out[1..n] = orig[0..n-1]; returns n+1 (0 if n out of range). */
int mgs4vr_vm_inject(const MGS4VR_VM_ITEM *orig, int n, MGS4VR_VM_ITEM *out, int cap);
void mgs4vr_vrmenu_poll(void *base, const char *exe);
#endif
