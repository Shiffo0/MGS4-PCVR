/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
#ifndef MGS4VR_MENU_H
#define MGS4VR_MENU_H
typedef struct {int open,row,follow,available,distance_cm,recenter,stereo,stereo_available;} MGS4VR_MENU;
void mgs4vr_menu_init(MGS4VR_MENU *m,int follow,int distance_cm);
int mgs4vr_menu_key(MGS4VR_MENU *m,unsigned key);
void *mgs4vr_menu_bitmap(const MGS4VR_MENU *m,int w,int h);
void mgs4vr_menu_free(void);
#endif
