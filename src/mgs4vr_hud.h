/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
#ifndef MGS4VR_HUD_H
#define MGS4VR_HUD_H
#include <stdint.h>
/* HUD off in VR, Solid Eye overlays kept (16 Oct 2026, user: "option A, but the Solid Eye labels must stay visible").
   Marker mgs4vr_hudhide.on ("on" = hide; VR Settings > VR DISPLAY > HUD writes it).
   Sim 16 Oct: every UI layout is in one global list (rva 0x2377AB0,
   layout object from 0x435A30: +0 magic 0xF615E7, +8 next, +0x1C layout id, +0x170 node count, +0x180 node array, node =
   u32 flags, bit 0x400000 visible). Clearing the visible bit of ALL nodes of a layout hides it and the game does not put
   it back on its own (root-only does not work). The corner HUD = the layouts made by the HUD class (0x4B..-0x4D..) plus the
   life gauge (0xB66172) and the camo index (0x3E6492, 0x8B1931); Solid Eye layouts (made in 0xE2..-0xE3.., e.g. the enemy
   labels 0xFF617F / 0xDE6517, one per enemy) are never touched.
   Game thread, once per gameplay frame (camera finisher): hide the visible nodes of those layouts and remember them per
   layout; when the option goes off, show the remembered nodes again (only in layouts that are still live). */
#define MGS4VR_HUD_LIST_RVA 0x2377ab0
#define MGS4VR_HUD_MAGIC 0xf615e7u
int mgs4vr_hud_hidden_id(unsigned id);
/* One pass over the live layouts: want = hide. Returns the number of node flags changed. */
int mgs4vr_hud_frame(void *base, int want);
void mgs4vr_hud_poll(const char *exe);       /* worker: reads the marker, writes the status */
int mgs4vr_hud_wanted(void);
/* Game thread: hide now? = marker on and (first person, or token "all"). */
int mgs4vr_hud_want_now(int first_person);
void mgs4vr_hud_set_all(int all);   /* desk tests */
#endif
