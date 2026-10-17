/* Wrist panels: right inner wrist = held weapon + ammo, left wrist = watch. Inert without marker
   mgs4vr_wrist.on ("on"). Worker: reads the held weapon (guarded) and renders the bitmaps with GDI;
   XR thread: hands callback opens/closes the panels and publishes their poses; xr.c submits them as quads.
   Weapon data (sim 12 Oct 2026, M4 CUSTOM): def = weapon+0x58 (exe table row, rva 0x1D82D00 for the M4):
   +0x00 name pointer, +0x30 s16 magazine, +0x32 s16 magazine max, *(def+0x18) s16 reserve, *(def+0x20) max
   (5 shots: 30 -> 25; reserve 295/300). Life / camo / item are not located yet: the watch says so. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "mgs4vr_wrist.h"
#include "mgs4vr_wrist_api.h"
#define WRIST_W 320
#define WRIST_H 160
#define RING_S 320
/* Radial weapon menu : hold the right stick click -> ring of the equip list in front
   of the head (slot 0 = NONE at the top, clockwise the equipped weapons from the live equip list, rva
   0x23FED43C: s16 count, s16 capacity 8, s16 codes[8]; sim 12 Oct 2026). Point the stick at a slot and let go:
   the worker writes the current weapon field (WEAPON_CUR_RVA); the game switches in first person, no window. */
#define EQUIP_RVA 0x23fed43c
/* Solid Eye night vision (sim 12 Oct 2026): item list = equip struct +0x18 (s16 codes, item window order after
   NONE), current item = rva 0x23FED46A (6 = SOLID EYE, 0 = NONE). The item window (hold 1, S per step) shows
   SELECT MODE for the Solid Eye: left mouse = NV, middle = BINOS, right = NORMAL; release 1 equips. A tap on 1
   (quick change) takes the item off again. Gesture: left grip pressed with the left hand at the left ear. */
#define ITEM_CUR_RVA 0x23fed46a
/* Item table (16 Oct 2026, exe dump): rva 0x1B036D8, stride 0x18: u64 name VA, u64 flags, u64 code (low byte = item id:
   1 RATION .. 6 S Eye, 7 MG MkII, 0xB iPod, 0x16 ENVG, 0x19 OCTOCAM ..). Names are the game's short internal names. */
#define ITEM_TABLE_RVA 0x1b036d8
#define ITEM_TABLE_MAX 96
/* Camo index (16 Oct 2026, sim: HUD "CAMO 12%" = 12): the camo HUD work (maker 0xF4D820, layouts 0x8B1931/0x3E6492) is
   stored in the global at rva 0x23FFAEB8 (0 while the HUD is gone); s16 at work +0x138 = the camo percentage (0xF4E8A0
   computes with 100 - value). Returns 1 and the value, 0 when not available. */
#define CAMO_WORK_RVA 0x23ffaeb8
int mgs4vr_wrist_camo(const BYTE *img, int *out) {
    int ok = 0;
    __try { uint64_t w = *(const uint64_t *)(img + CAMO_WORK_RVA); if (w >= 0x10000 && w < 0x800000000000ULL) { short v = *(const short *)(w + 0x138); if (v >= -100 && v <= 100) { *out = v; ok = 1; } } }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}
/* Life and psyche (sim 17 Oct 2026): the player's life gauge (0xF2F6A0, made by 0xF2FBF0) reads four s16 of the player
   status block: rva 0x23FED4E8 life, +2 max life, +4 psyche, +6 max psyche (poke test: psyche 100 -> lower gauge bar at 1/7).
   Returns 1 with percentages 0..100, 0 when the values are not sane. */
#define VITALS_RVA 0x23fed4e8
int mgs4vr_wrist_vitals(const BYTE *img, int *life_pct, int *psyche_pct) {
    short v[4]; int ok = 0;
    __try { memcpy(v, img + VITALS_RVA, sizeof(v)); ok = 1; } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (!ok || v[1] <= 0 || v[3] <= 0 || v[0] < 0 || v[2] < 0 || v[0] > v[1] || v[2] > v[3] || v[1] > 30000 || v[3] > 30000) return 0;
    *life_pct = (v[0] * 100 + v[1] / 2) / v[1]; *psyche_pct = (v[2] * 100 + v[3] / 2) / v[3];
    if (v[0] > 0 && !*life_pct) *life_pct = 1;
    return 1;
}
/* Watch line 3: the current item. out = "ITEM  <name>" / "ITEM  NONE". */
static void wr_item_line(const BYTE *img, size_t image_size, int cur, char *out, size_t cap) {
    (void)image_size;
    int i; const char *name = NULL;
    if (cur <= 0) { strcpy_s(out, cap, "ITEM  NONE"); return; }
    __try {
        for (i = 0; i < ITEM_TABLE_MAX && !name; ++i) {
            const BYTE *e = img + ITEM_TABLE_RVA + i * 0x18; uint64_t va = *(const uint64_t *)e, code = *(const uint64_t *)(e + 16);
            /* headset 17 Oct (#numbers): at run time the table points at heap copies of the display names (RATION, MG Mk. II,
               iPod\x8e - sim 17 Oct), never into the image; any readable pointer is taken, the copy below is guarded */
            if ((int)(code & 0xff) == cur && va >= 0x10000 && va < 0x800000000000ULL) name = (const char *)va;
        }
        if (name) { char nm[24]; int k; for (k = 0; k < 23 && name[k] >= 0x20 && name[k] < 0x7f && name[k] != '\\'; ++k) nm[k] = name[k]; nm[k] = 0; sprintf_s(out, cap, "ITEM  %s", k ? nm : "?"); }
        else sprintf_s(out, cap, "ITEM  #%d", cur);
    } __except (EXCEPTION_EXECUTE_HANDLER) { sprintf_s(out, cap, "ITEM  #%d", cur); }
}
static void wr_item_name(const BYTE *img,int code,char out[24]){char l[40];size_t isz=0;
    __try{isz=((const IMAGE_NT_HEADERS64 *)(img+((const IMAGE_DOS_HEADER *)img)->e_lfanew))->OptionalHeader.SizeOfImage;}__except(EXCEPTION_EXECUTE_HANDLER){isz=0;}
    wr_item_line(img,isz,code,l,sizeof(l));strcpy_s(out,24,strlen(l)>6?l+6:l);}
/* Direct selection (sim 12 Oct 2026, user: the third-person flash of the game windows is disturbing): the game
   follows these fields within a frame, in first person, without its windows: current weapon = equip +0x28
   (rva 0x23FED464), current item = rva 0x23FED46A, Solid Eye mode byte = rva 0x23FED482 (0 NORMAL, 1 NV). */
#define WEAPON_CUR_RVA 0x23fed464
#define SE_MODE_RVA 0x23fed482
#define ITEM_SOLID_EYE 6
#define EAR_RADIUS 0.14f
#define WTABLE_RVA 0x1d82580
#define RING_SLOTS 8
typedef struct { int armed, open, selected, deflected; unsigned long long opened; } RD_STATE;
static int rd_sector(float x,float y){double a;if(!(x>=-1&&x<=1&&y>=-1&&y<=1))return -1;if(x*x+y*y<0.35f*0.35f)return -1;
    a=atan2(x,y);if(a<0)a+=6.283185307179586;return (int)floor(a*RING_SLOTS/6.283185307179586+.5)%RING_SLOTS;}
/* One XR-thread sample: returns the slot to select (>=0) on release, -1 otherwise. */
static int rd_step(RD_STATE *s,int allowed,int click,float x,float y,unsigned long long now){
    int r=-1;
    if(!allowed){s->open=0;s->armed=0;return -1;}
    if(!s->armed){if(!click)s->armed=1;return -1;}
    if(!s->open){if(!click)return -1;if(x*x+y*y>=0.35f*0.35f){s->armed=0;return -1;}s->open=1;s->opened=now;s->selected=-1;s->deflected=0;}
    s->selected=rd_sector(x,y);if(s->selected>=0)s->deflected=1;
    if(!click){r=s->selected;s->open=0;s->armed=1;}
    return r;
}
typedef struct { char name[32]; int mag, mag_max, reserve, reserve_max, code; int valid; } WR_WEAPON;
static SRWLOCK wr_lock=SRWLOCK_INIT;
static volatile LONG wr_enabled, wr_fp, wr_opens[2], wr_frames[2];
static WR_GATE wr_g[2];
static MGS4VR_XR_QUAD wr_quad[2];               /* published to the XR thread under wr_lock */
static unsigned wr_version[2];
static unsigned char wr_px[2][WRIST_W*WRIST_H*4];
static WR_WEAPON wr_last;
static RD_STATE rd;static volatile LONG rd_request=-1,rd_open_flag,rd_picks,rd_runs,rd_presses;static int rd_ring_codes[RING_SLOTS];static unsigned char rd_px[RING_S*RING_S*4];static unsigned rd_version;
static MGS4VR_XR_QUAD rd_quad;

/* Player heading for closed-loop turning (sim 12 Oct 2026): player data (actor+0x90) +0x150 sin, +0x158 cos;
   positive mouse X lowers it (~26 px/deg slow, game mouse acceleration above that). Worker caches the pointer. */
static volatile LONG64 hd_pdata;
/* Direct turning (sim 13 Oct 2026): the first-person view yaw is a float in radians at camera controller +0x34
   (+0x30 pitch). Chain: player data +0xE8 -> +0x20 component list (next +8, type hash +0x40 == 0x8E375F = camera
   component) -> +0x90 controller (vtable exe+0x18B6D88). One write of +0x34 turns Snake and sticks; +yaw = left. */
#define HD_CAM_TYPE 0x8e375fu
#define HD_CTL_VTABLE 0x18b6d88u
static volatile LONG64 hd_yaw;
/* 14 Oct 2026: one headset session never found the yaw (yaw_found=0, fell back to the jerky mouse route). The search
   stopped at the FIRST camera-type component even when its controller had another vtable: now it keeps looking.
   hd_reason: 0 found, 1 no player data, 2 no component list, 3 no camera component, 4 camera component(s) without the
   controller vtable (hd_vt_seen = the last vtable seen, as an exe RVA when inside the exe). */
static volatile LONG hd_reason, hd_vt_seen, hd_levels;
static unsigned long long hd_find_yaw(BYTE *img,unsigned long long pd){unsigned long long cont,c,ctl,vt;int n,cams=0;
    if(!pd){hd_reason=1;return 0;}
    cont=*(unsigned long long *)(pd+0xe8);if(cont<0x10000){hd_reason=2;return 0;}
    for(c=*(unsigned long long *)(cont+0x20),n=0;c>0x10000&&n<16;c=*(unsigned long long *)(c+8),++n){
        if(*(unsigned *)(c+0x40)!=HD_CAM_TYPE)continue;
        ++cams;ctl=*(unsigned long long *)(c+0x90);if(ctl<=0x10000)continue;
        vt=*(unsigned long long *)ctl;
        if(vt==(unsigned long long)(img+HD_CTL_VTABLE)){hd_reason=0;return ctl+0x34;}
        hd_vt_seen=(LONG)(vt-(unsigned long long)img);}
    hd_reason=cams?4:3;return 0;}
static void hd_refresh(BYTE *img){int slot;unsigned long long owner,actor,q,pd=0,yw=0;
    __try{slot=*(int *)(img+0x23eb5150);if(slot>=-1&&slot<=23){owner=*(unsigned long long *)(img+0x23eb5160+(size_t)(slot+1)*0x80);actor=owner&0xfffffffffffffULL;
        if(actor>0x10000&&*(unsigned long long *)(actor+0x30)==owner){q=*(unsigned long long *)(actor+0x90);if(q>0x10000)pd=q;}}
        yw=hd_find_yaw(img,pd);}__except(EXCEPTION_EXECUTE_HANDLER){pd=0;yw=0;}
    InterlockedExchange64(&hd_pdata,(LONG64)pd);InterlockedExchange64(&hd_yaw,(LONG64)yw);}
float *mgs4vr_wrist_yaw(void){return (float *)(ULONG_PTR)hd_yaw;}
/* Held weapon's magazine (last worker read, 16 Oct 2026: pistol reload gestures). 0 = unknown. */
int mgs4vr_wrist_mag(int *mag,int *mag_max){WR_WEAPON w=wr_last;if(!w.valid)return 0;if(mag)*mag=w.mag;if(mag_max)*mag_max=w.mag_max;return 1;}
/* Recenter in first person (16 Oct 2026, user): the view pitch the right stick gave the game camera (controller +0x30,
   radians; sim: stick up -> -0.31) stays after an XR recenter, the eyes are an offset on top of it. One write of 0 levels
   it (sim: picture back to the start). Returns 1 when written. */
int mgs4vr_wrist_level_view(void){float *yw=mgs4vr_wrist_yaw();int ok=0;if(!yw)return 0;
    __try{float p=yw[-1];if(p==p&&p>-3.2f&&p<3.2f){yw[-1]=0.0f;ok=1;}}__except(EXCEPTION_EXECUTE_HANDLER){ok=0;}
    if(ok)InterlockedIncrement(&hd_levels);return ok;}
int mgs4vr_wrist_heading(float *deg){BYTE *pd=(BYTE *)(ULONG_PTR)hd_pdata;float s,c;if(!pd||!deg)return 0;
    __try{s=*(float *)(pd+0x150);c=*(float *)(pd+0x158);}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    if(!isfinite(s)||!isfinite(c)||fabsf(s*s+c*c-1)>0.1f)return 0;*deg=atan2f(s,c)*57.29578f;return 1;}
static volatile LONG nv_request,nv_on_runs,nv_off_runs,nv_gestures;static int nv_prev;static volatile float nv_ear;static int nv_armed=1;

/* Pure: the left-ear point for a head pose (8.5 cm to the left of the head centre, 2 cm behind). */
static void nv_ear_point(const WR_POSE *head,float out[3]){float l[3]={-0.085f,0,0.02f},v[3];wr_rotate(head->q,l,v);out[0]=head->p[0]+v[0];out[1]=head->p[1]+v[1];out[2]=head->p[2]+v[2];}
/* Pure: rising edge of the left grip while the grip is within EAR_RADIUS of the ear; returns 1 on a toggle. */
static int nv_gesture(int *armed,float squeeze,const float grip[3],const float ear[3],float *dist){
    float d=sqrtf((grip[0]-ear[0])*(grip[0]-ear[0])+(grip[1]-ear[1])*(grip[1]-ear[1])+(grip[2]-ear[2])*(grip[2]-ear[2]));
    if(dist)*dist=d;
    if(squeeze<0.4f){*armed=1;return 0;}
    if(*armed&&squeeze>0.8f&&d<=EAR_RADIUS){*armed=0;return 1;}
    if(squeeze>0.8f)*armed=0;   /* pressed elsewhere: no toggle until released */
    return 0;
}
/* Pure: the NV toggle. Returns the item to set and *mode (-1 = leave); prev keeps the item to go back to. */
static int nv_decide(const short *items,int current,int mode,int *prev,int *set_mode){
    int i,has=0;*set_mode=-1;for(i=0;i<8&&items[i];++i)if((items[i]&0xff)==ITEM_SOLID_EYE)has=1;
    if(current==ITEM_SOLID_EYE&&mode==1){int back=*prev;*prev=0;return back;}          /* off: back to the previous item */
    if(!has)return -1;                                                                     /* no Solid Eye equipped: nothing */
    if(current!=ITEM_SOLID_EYE)*prev=current;
    *set_mode=1;return ITEM_SOLID_EYE;
}
/* Pure: a radial pick is applied only for NONE or a weapon that is in the equip list. */
static int rd_valid_pick(const short *codes,int count,int code){int i;if(code==0)return 1;if(code<0)return 0;for(i=0;i<count&&i<RING_SLOTS;++i)if((codes[i]&0x1ff)==code)return 1;return 0;}
int mgs4vr_wrist_ring_open(void){return InterlockedCompareExchange(&rd_open_flag,0,0)!=0;}

/* Item button (16 Oct 2026, user): left stick click. A short click takes the item off / puts the last one on again (the
   game's own quick change, tap 1); holding it ITEM_HOLD_MS with the stick centred opens the ITEM ring (slot 0 = NONE, then
   the item list = equip +0x18, s16 codes until 0, max 7); point and let go = equip (current item rva 0x23FED46A). */
#define ITEM_LIST_RVA (EQUIP_RVA+0x18)
#define ITEM_HOLD_MS 400u
typedef struct { int armed, down, open, cancelled, selected; unsigned long long at; } IT_STATE;
static volatile LONG it_hold_ms=ITEM_HOLD_MS;   /* marker token itemhold=<ms> (150..5000) in mgs4vr_wrist.on */
/* One XR-thread sample: returns the ring slot on release of an open ring, -1 otherwise; *tap = 1 on a short click. */
static int it_step(IT_STATE *s,int allowed,int click,float x,float y,unsigned long long now,int *tap){
    int r=-1,centred=x*x+y*y<0.35f*0.35f;*tap=0;
    if(!allowed){s->open=s->down=s->armed=0;return -1;}
    if(!s->armed){if(!click)s->armed=1;return -1;}
    if(!s->down){if(click){s->down=1;s->at=now;s->cancelled=!centred;s->open=0;s->selected=-1;}return -1;}
    if(!s->open){
        if(!click){s->down=0;if(!s->cancelled&&now-s->at<(unsigned long long)it_hold_ms)*tap=1;return -1;}
        if(!centred&&now-s->at<(unsigned long long)it_hold_ms)s->cancelled=1;                    /* walking with the click held: nothing */
        if(!s->cancelled&&now-s->at>=(unsigned long long)it_hold_ms){s->open=1;s->selected=-1;}
        return -1;}
    s->selected=rd_sector(x,y);
    if(!click){r=s->selected;s->open=0;s->down=0;}
    return r;
}
static int it_list(const BYTE *image,short codes[RING_SLOTS]){int n=0;memset(codes,0,RING_SLOTS*2);if(!image)return 0;
    __try{for(n=0;n<RING_SLOTS-1;++n){short c=*(const short *)(image+ITEM_LIST_RVA+2*n);if(c<=0)break;codes[n]=c;}}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    return n;}
static int it_in_list(const short *codes,int n,int code){int i;for(i=0;i<n;++i)if((codes[i]&0xff)==code)return 1;return 0;}
/* Pure: the item after a short click. cur != 0 -> 0 (remember cur); cur == 0 -> the remembered item if still owned, else the first. */
int mgs4vr_wrist_item_toggle(int cur,int *last,const short *codes,int n){
    if(cur>0){*last=cur;return 0;}
    if(*last>0&&it_in_list(codes,n,*last))return *last;
    return n>0?(codes[0]&0xff):0;}
static IT_STATE it;static volatile LONG it_request=-1,it_tap_request,it_open_flag,it_picks,it_taps,it_runs;static int it_ring_codes[RING_SLOTS];
/* Use from the ring (17 Oct 2026, user: use items such as the ration): ring open + an item highlighted + left trigger =
   equip that item and use it (mgs4vr_fpaction item use); the ring closes and does not reopen while the click is held. */
static volatile LONG it_use_flag,it_uses;static int it_trig_armed;static void (*it_use_fn)(void);
void mgs4vr_wrist_set_use(void (*fn)(void)){it_use_fn=fn;}
/* Pure: rising left trigger (0.7 on, re-armed below 0.3). */
int mgs4vr_wrist_trigger_edge(int *armed,float trigger){if(trigger<0.3f){*armed=1;return 0;}if(*armed&&trigger>0.7f){*armed=0;return 1;}return 0;}
int mgs4vr_wrist_item_ring_open(void){return InterlockedCompareExchange(&it_open_flag,0,0)!=0;}

static void rd_name(BYTE *image,int code,char out[24]){out[0]=0;if(!code){strcpy_s(out,24,"NONE");return;}
    __try{const char *n=*(const char **)(image+WTABLE_RVA+(size_t)code*80);int i;if(n)for(i=0;i<23&&n[i];++i){out[i]=(n[i]>=32&&n[i]<127)?n[i]:63;out[i+1]=0;}}
    __except(EXCEPTION_EXECUTE_HANDLER){out[0]=0;}}
static int rd_list(BYTE *image,short codes[RING_SLOTS]){int n=0;memset(codes,0,RING_SLOTS*2);if(!image)return 0;
    __try{n=*(short *)(image+EQUIP_RVA);if(n<0||n>RING_SLOTS)return 0;memcpy(codes,image+EQUIP_RVA+8,RING_SLOTS*2);}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    return n;}
static void wr_item_name(const BYTE *img,int code,char out[24]);
static void rd_render_kind(BYTE *image,int kind,int count,const short *codes,int selected,int current,int *ring_codes);
static void rd_render(BYTE *image,int count,const short *codes,int selected,int current){rd_render_kind(image,0,count,codes,selected,current,rd_ring_codes);}
static void rd_render_kind(BYTE *image,int kind,int count,const short *codes,int selected,int current,int *ring_codes){
    static HDC dc;static HBITMAP bm;static unsigned char *bits;BITMAPINFO bi;RECT r;HFONT f;HGDIOBJ of;HBRUSH b;int i;char name[24];
    if(!dc){memset(&bi,0,sizeof(bi));dc=CreateCompatibleDC(NULL);bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=RING_S;bi.bmiHeader.biHeight=-RING_S;
        bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;bm=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,(void **)&bits,NULL,0);if(!bm)return;SelectObject(dc,bm);}
    r.left=r.top=0;r.right=r.bottom=RING_S;b=CreateSolidBrush(RGB(8,18,14));FillRect(dc,&r,b);DeleteObject(b);
    f=CreateFontA(-20,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,"Segoe UI");of=SelectObject(dc,f);SetBkMode(dc,TRANSPARENT);
    for(i=0;i<RING_SLOTS;++i){double a=i*6.283185307179586/RING_SLOTS;int cx=RING_S/2+(int)(sin(a)*118),cy=RING_S/2-(int)(cos(a)*118);int code=i==0?0:(i<=count?codes[i-1]&(kind?0xff:0x1ff):-1);   /* items: low byte = id (table code), upper bits flags */
        if(code<0)continue;ring_codes[i]=code;
        r.left=cx-70;r.right=cx+70;r.top=cy-18;r.bottom=cy+18;
        if(i==selected){b=CreateSolidBrush(RGB(40,120,70));FillRect(dc,&r,b);DeleteObject(b);}
        if(code==current){b=CreateSolidBrush(RGB(150,240,170));FrameRect(dc,&r,b);DeleteObject(b);}
        if(kind)wr_item_name(image,code,name);else rd_name(image,code,name);SetTextColor(dc,RGB(170,245,190));DrawTextA(dc,name,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);}
    for(i=count+1;i<RING_SLOTS;++i)ring_codes[i]=-1;
    {RECT t;t.left=0;t.right=RING_S;t.top=RING_S/2-14;t.bottom=RING_S/2+14;SetTextColor(dc,RGB(110,170,130));DrawTextA(dc,kind?"ITEMS":"WEAPONS",-1,&t,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);}
    SelectObject(dc,of);DeleteObject(f);GdiFlush();
    AcquireSRWLockExclusive(&wr_lock);for(i=0;i<RING_S*RING_S;++i){rd_px[i*4]=bits[i*4+2];rd_px[i*4+1]=bits[i*4+1];rd_px[i*4+2]=bits[i*4];rd_px[i*4+3]=255;}++rd_version;ReleaseSRWLockExclusive(&wr_lock);
}
static char wr_mark[MAX_PATH], wr_status[MAX_PATH];
static DWORD wr_tick, wr_read_tick;
static const WR_CFG wr_cfg[2]={
    /* left watch: INNER side of the wrist (grip +X, mirror of the right panel), 7 cm towards the elbow. Was the back of the
       wrist (grip +Y) until 13 Oct 2026; the user wants it when looking at the inside of the left wrist. */
    {0.07f,0.035f,1.0f,0,0.09f,35,35,50,45,120},
    /* right panel: inner side of the forearm (grip -X), 10 cm towards the elbow */
    {0.10f,0.035f,-1.0f,0,0.10f,35,35,50,45,120}};
static int wr_read_weapon(BYTE *w, WR_WEAPON *o) {
    BYTE *def; const char *name; short *res, *resmax; int i;
    memset(o,0,sizeof(*o)); if(!w)return 0;
    __try{
        def=*(BYTE **)(w+0x58); if(!def)return 0;
        o->code=*(int *)(def+0x10)&0x1ff; o->mag=*(short *)(def+0x30); o->mag_max=*(short *)(def+0x32);
        res=*(short **)(def+0x18); resmax=*(short **)(def+0x20); o->reserve=res?*res:-1; o->reserve_max=resmax?*resmax:-1;
        name=*(const char **)def;
        if(name)for(i=0;i<31&&name[i];++i)o->name[i]=(name[i]>=32&&name[i]<127)?name[i]:'?';
    }__except(EXCEPTION_EXECUTE_HANDLER){memset(o,0,sizeof(*o));return 0;}
    if(o->mag<0||o->mag>999||o->mag_max<0||o->mag_max>999)return 0;
    o->valid=1; return 1;
}
static void wr_render(int which, const char *l1, const char *l2, const char *l3) {
    static HDC dc; static HBITMAP bm; static unsigned char *bits; BITMAPINFO bi; RECT r; HFONT big, small; HGDIOBJ of; HBRUSH b; int i;
    if(!dc){memset(&bi,0,sizeof(bi));dc=CreateCompatibleDC(NULL);bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=WRIST_W;bi.bmiHeader.biHeight=-WRIST_H;
        bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;bm=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,(void **)&bits,NULL,0);if(!bm)return;SelectObject(dc,bm);}
    r.left=r.top=0;r.right=WRIST_W;r.bottom=WRIST_H;b=CreateSolidBrush(RGB(10,22,16));FillRect(dc,&r,b);DeleteObject(b);
    b=CreateSolidBrush(RGB(60,150,90));r.left=4;r.top=4;r.right=WRIST_W-4;r.bottom=WRIST_H-4;FrameRect(dc,&r,b);DeleteObject(b);
    small=CreateFontA(-26,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,"Segoe UI");
    big=CreateFontA(-54,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,"Consolas");
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(150,240,170));
    of=SelectObject(dc,small);r.left=16;r.right=WRIST_W-16;r.top=12;r.bottom=46;DrawTextA(dc,l1,-1,&r,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
    SelectObject(dc,big);r.top=46;r.bottom=112;DrawTextA(dc,l2,-1,&r,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
    SelectObject(dc,small);r.top=114;r.bottom=148;DrawTextA(dc,l3,-1,&r,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
    SelectObject(dc,of);DeleteObject(big);DeleteObject(small);GdiFlush();
    AcquireSRWLockExclusive(&wr_lock);
    for(i=0;i<WRIST_W*WRIST_H;++i){wr_px[which][i*4]=bits[i*4+2];wr_px[which][i*4+1]=bits[i*4+1];wr_px[which][i*4+2]=bits[i*4];wr_px[which][i*4+3]=255;}
    ++wr_version[which];
    ReleaseSRWLockExclusive(&wr_lock);
}
static void wr_report(const char *event) {
    FILE *f; if(fopen_s(&f,wr_status,"w"))return;
    fprintf(f,"wrist_version=1 event=%s enabled=%ld first_person=%ld ring_open=%ld ring_picks=%ld ring_runs=%ld nv_gestures=%ld nv_on=%ld nv_off=%ld ear_mm=%.0f left_open=%d right_open=%d opens=%ld,%ld frames=%ld,%ld left_face=%.0f left_gaze=%.0f right_face=%.0f right_gaze=%.0f weapon=\"%s\" code=%d mag=%d/%d reserve=%d/%d yaw_reason=%ld yaw_vt_seen=%#lx\n",
        event,wr_enabled,wr_fp,rd_open_flag,rd_picks,rd_runs,nv_gestures,nv_on_runs,nv_off_runs,nv_ear*1000,wr_g[0].open,wr_g[1].open,wr_opens[0],wr_opens[1],wr_frames[0],wr_frames[1],wr_g[0].face,wr_g[0].gaze,wr_g[1].face,wr_g[1].gaze,
        wr_last.name,wr_last.code,wr_last.mag,wr_last.mag_max,wr_last.reserve,wr_last.reserve_max,hd_reason,(unsigned long)hd_vt_seen);
    {short ic[RING_SLOTS];int k,n=it_list((const BYTE *)GetModuleHandleW(NULL),ic),cur=-1;__try{cur=*(const unsigned short *)((const BYTE *)GetModuleHandleW(NULL)+ITEM_CUR_RVA);}__except(EXCEPTION_EXECUTE_HANDLER){cur=-1;}
     fprintf(f,"item_uses=%ld ",it_uses);fprintf(f,"item_ring_open=%ld item_picks=%ld item_taps=%ld item_runs=%ld view_levels=%ld item_cur=%#x item_codes=",it_open_flag,it_picks,it_taps,it_runs,hd_levels,cur);
     for(k=0;k<n;++k)fprintf(f,"%s%#x",k?",":"",(unsigned)(unsigned short)ic[k]);fprintf(f,"\n");}
    fclose(f);
}
void mgs4vr_wrist_poll(const char *exe, int fp, void *held_weapon) {
    DWORD now=GetTickCount(); char *leaf; FILE *f; char b[128]={0}; int on=0; WR_WEAPON w;
    if(!wr_mark[0]){strcpy_s(wr_mark,sizeof(wr_mark),exe);leaf=strrchr(wr_mark,'\\');if(!leaf){wr_mark[0]=0;return;}
        strcpy_s(leaf+1,sizeof(wr_mark)-(size_t)(leaf+1-wr_mark),"mgs4vr_wrist.on");strcpy_s(wr_status,sizeof(wr_status),wr_mark);
        sprintf_s(strrchr(wr_status,'\\')+1,64,"mgs4vr_wrist_%lu.txt",GetCurrentProcessId());}
    /* direct selection: the game follows the current weapon / item fields (no window, no third-person flash) */
    if(InterlockedExchange(&nv_request,0)&&wr_enabled){BYTE *img=(BYTE *)GetModuleHandleW(NULL);int cur=-1,mode=0,set_mode,item;short items[8];
        __try{cur=*(unsigned short *)(img+ITEM_CUR_RVA)&0xff;mode=img[SE_MODE_RVA];memcpy(items,img+EQUIP_RVA+0x18,sizeof(items));}__except(EXCEPTION_EXECUTE_HANDLER){cur=-1;}
        if(cur>=0&&(item=nv_decide(items,cur,mode,&nv_prev,&set_mode))>=0){
            __try{if(set_mode>=0)img[SE_MODE_RVA]=(BYTE)set_mode;*(unsigned short *)(img+ITEM_CUR_RVA)=(unsigned short)item;}__except(EXCEPTION_EXECUTE_HANDLER){}
            InterlockedIncrement(item==ITEM_SOLID_EYE?&nv_on_runs:&nv_off_runs);}}
    {LONG req=InterlockedExchange(&rd_request,-1);
        if(req>=0&&req<RING_SLOTS&&wr_enabled){short codes[RING_SLOTS];BYTE *img=(BYTE *)GetModuleHandleW(NULL);int n=rd_list(img,codes),code=rd_ring_codes[req];
            if(rd_valid_pick(codes,n,code)){__try{*(unsigned short *)(img+WEAPON_CUR_RVA)=(unsigned short)code;}__except(EXCEPTION_EXECUTE_HANDLER){}InterlockedIncrement(&rd_runs);}}}    if(rd_open_flag&&wr_enabled){short codes[RING_SLOTS];BYTE *img=(BYTE *)GetModuleHandleW(NULL);int n=rd_list(img,codes);WR_WEAPON cur;static int last_sel=-2;static DWORD last_draw;
        wr_read_weapon((BYTE *)held_weapon,&cur);if(rd.selected!=last_sel||now-last_draw>500){last_sel=rd.selected;last_draw=now;rd_render(img,n,codes,rd.selected,cur.valid?cur.code:0);}}
    {   /* item ring pick / short click (16 Oct 2026): write the current item field, only codes from the item list (or NONE) */
        LONG req=InterlockedExchange(&it_request,-1),tap=InterlockedExchange(&it_tap_request,0),use=0;static int it_last;
        if((req>=0&&req<RING_SLOTS)||tap){short codes[RING_SLOTS];BYTE *img=(BYTE *)GetModuleHandleW(NULL);int n=it_list(img,codes),code=-1,cur=0;
            if(wr_enabled){__try{cur=*(unsigned short *)(img+ITEM_CUR_RVA)&0xff;}__except(EXCEPTION_EXECUTE_HANDLER){cur=-1;}
                if(req>=0){code=it_ring_codes[req];if(code>0&&!it_in_list(codes,n,code))code=-1;if(code>0&&cur>0)it_last=cur;}
                else if(cur>=0)code=mgs4vr_wrist_item_toggle(cur,&it_last,codes,n);
                if(code>=0&&code!=cur){__try{*(unsigned short *)(img+ITEM_CUR_RVA)=(unsigned short)code;}__except(EXCEPTION_EXECUTE_HANDLER){}InterlockedIncrement(&it_runs);}
                use=InterlockedExchange(&it_use_flag,0);if(use&&req>=0&&code>0&&it_use_fn)it_use_fn();}}}
    if(it_open_flag&&wr_enabled){short codes[RING_SLOTS];BYTE *img=(BYTE *)GetModuleHandleW(NULL);int n=it_list(img,codes),cur=0;static int last_sel=-2;static DWORD last_draw;
        __try{cur=*(unsigned short *)(img+ITEM_CUR_RVA)&0xff;}__except(EXCEPTION_EXECUTE_HANDLER){cur=0;}
        if(it.selected!=last_sel||now-last_draw>500){last_sel=it.selected;last_draw=now;rd_render_kind(img,1,n,codes,it.selected,cur,it_ring_codes);}}
    if(now-wr_read_tick<100)return; wr_read_tick=now; hd_refresh((BYTE *)GetModuleHandleW(NULL));
    if(!fopen_s(&f,wr_mark,"rb")){fread(b,1,127,f);fclose(f);on=!strncmp(b,"on",2);{const char *q=strstr(b,"itemhold=");int v=0;InterlockedExchange(&it_hold_ms,q&&sscanf_s(q+9,"%d",&v)==1&&v>=150&&v<=5000?v:ITEM_HOLD_MS);}}
    InterlockedExchange(&wr_enabled,on); InterlockedExchange(&wr_fp,fp?1:0);
    if(!on)return;
    wr_read_weapon((BYTE *)held_weapon,&w);
    if(memcmp(&w,&wr_last,sizeof(w))||!wr_version[1]){
        char l1[64],l2[32],l3[64];
        wr_last=w;
        if(w.valid){strcpy_s(l1,sizeof(l1),w.name[0]?w.name:"WEAPON");sprintf_s(l2,sizeof(l2),"%d / %d",w.mag,w.mag_max);
            if(w.reserve>=0)sprintf_s(l3,sizeof(l3),"RESERVE %d / %d",w.reserve,w.reserve_max);else strcpy_s(l3,sizeof(l3),"");}
        else{strcpy_s(l1,sizeof(l1),"NO WEAPON");strcpy_s(l2,sizeof(l2),"--");strcpy_s(l3,sizeof(l3),"");}
        wr_render(1,l1,l2,l3);
    }
    {   /* watch: name, clock, current item (16 Oct: was "LIFE / CAMO: n.b."); redrawn each second or when the item changes */
        static int last_item=-2,last_camo=-1000,last_life=-2,last_psy=-2; int cur=-1,camo=-1000,life=-1,psy=-1; BYTE *img=(BYTE *)GetModuleHandleW(NULL); size_t isz=0;
        __try{cur=*(unsigned short *)(img+ITEM_CUR_RVA)&0xff;isz=((IMAGE_NT_HEADERS64 *)(img+((IMAGE_DOS_HEADER *)img)->e_lfanew))->OptionalHeader.SizeOfImage;}__except(EXCEPTION_EXECUTE_HANDLER){cur=-1;}
        if(!mgs4vr_wrist_camo(img,&camo))camo=-1000;
        if(!mgs4vr_wrist_vitals(img,&life,&psy))life=psy=-1;
        if(!wr_version[0]||now-wr_tick>=1000||cur!=last_item||camo!=last_camo||life!=last_life||psy!=last_psy){char l1[48];SYSTEMTIME t;char clock[40],l3[40];int beat=now-wr_tick>=1000;
            if(beat)wr_tick=now;last_item=cur;last_camo=camo;last_life=life;last_psy=psy;
            if(life>=0)sprintf_s(l1,sizeof(l1),"LIFE %d%%   PSYCHE %d%%",life,psy);else strcpy_s(l1,sizeof(l1),"LIFE --   PSYCHE --");GetLocalTime(&t);
            /* headset 17 Oct (user: the time is of no use, the camo was cut off): big line = camo index only */
            (void)t;if(camo>-1000)sprintf_s(clock,sizeof(clock),"CAMO %d%%",camo);else strcpy_s(clock,sizeof(clock),"CAMO --");
            if(cur>=0)wr_item_line(img,isz,cur,l3,sizeof(l3));else strcpy_s(l3,sizeof(l3),"ITEM  ?");
            wr_render(0,l1,clock,l3);if(beat)wr_report("active");}}
}
/* XR thread, once per frame with fresh poses. */
void mgs4vr_wrist_hands(const MGS4VR_XR_HANDS *h) {
    WR_POSE head, grip, quad; int i, open; unsigned long long now=GetTickCount64();
    if(!h||!h->head_valid){AcquireSRWLockExclusive(&wr_lock);wr_quad[0].visible=wr_quad[1].visible=0;ReleaseSRWLockExclusive(&wr_lock);return;}
    memcpy(head.q,h->head_quat,16);memcpy(head.p,h->head_pos,12);
    if(h->hand[0].active&&wr_enabled&&wr_fp){float ear[3],d;nv_ear_point(&head,ear);
        if(nv_gesture(&nv_armed,h->hand[0].squeeze,h->hand[0].grip_pos,ear,&d)){InterlockedExchange(&nv_request,1);InterlockedIncrement(&nv_gestures);}nv_ear=d;}
    {int click=h->hand[1].active&&(h->hand[1].buttons&1u),pick;float mz[3]={0,0,-1},f[3];
     int lclick=h->hand[0].active&&(h->hand[0].buttons&1u),tap=0,ipick;
     pick=rd_step(&rd,wr_enabled&&wr_fp&&h->hand[1].active&&!it.open,click,h->hand[1].stick[0],h->hand[1].stick[1],now);
     if(pick>=0){InterlockedExchange(&rd_request,pick);InterlockedIncrement(&rd_picks);}
     ipick=it_step(&it,wr_enabled&&wr_fp&&h->hand[0].active&&!rd.open,lclick,h->hand[0].stick[0],h->hand[0].stick[1],now,&tap);
     if(mgs4vr_wrist_trigger_edge(&it_trig_armed,h->hand[0].active?h->hand[0].trigger:0.f)&&it.open&&it.selected>0){
         ipick=it.selected;it.open=0;it.cancelled=1;InterlockedExchange(&it_use_flag,1);InterlockedIncrement(&it_uses);}
     if(ipick>=0){InterlockedExchange(&it_request,ipick);InterlockedIncrement(&it_picks);}
     if(tap){InterlockedExchange(&it_tap_request,1);InterlockedIncrement(&it_taps);}
     InterlockedExchange(&rd_open_flag,rd.open);InterlockedExchange(&it_open_flag,it.open);
     wr_rotate(head.q,mz,f);AcquireSRWLockExclusive(&wr_lock);rd_quad.visible=(rd.open||it.open)&&rd_version;memcpy(rd_quad.q,head.q,16);
     rd_quad.p[0]=head.p[0]+f[0]*.45f;rd_quad.p[1]=head.p[1]+f[1]*.45f;rd_quad.p[2]=head.p[2]+f[2]*.45f;ReleaseSRWLockExclusive(&wr_lock);}
    for(i=0;i<2;++i){
        int was=wr_g[i].open;
        if(!h->hand[i].active){open=0;wr_g[i].open=0;}
        else{memcpy(grip.q,h->hand[i].grip_quat,16);memcpy(grip.p,h->hand[i].grip_pos,12);wr_quad_pose(&wr_cfg[i],&grip,&quad);
            open=wr_gate(&wr_g[i],&wr_cfg[i],&quad,&head,wr_enabled&&wr_fp,now);
            AcquireSRWLockExclusive(&wr_lock);memcpy(wr_quad[i].q,quad.q,16);memcpy(wr_quad[i].p,quad.p,12);ReleaseSRWLockExclusive(&wr_lock);}
        if(open&&!was)InterlockedIncrement(&wr_opens[i]);
        if(open)InterlockedIncrement(&wr_frames[i]);
        AcquireSRWLockExclusive(&wr_lock);wr_quad[i].visible=open;ReleaseSRWLockExclusive(&wr_lock);
    }
}
/* XR thread: copy out what to submit. Pixels are copied so the worker can keep rendering. */
int mgs4vr_wrist_quads(MGS4VR_XR_QUAD out[3], unsigned char *pixels[3]) {
    int i, n=0;
    AcquireSRWLockShared(&wr_lock);
    for(i=0;i<2;++i){out[i]=wr_quad[i];out[i].width=wr_cfg[i].width;out[i].height=wr_cfg[i].width*WRIST_H/WRIST_W;out[i].w=WRIST_W;out[i].h=WRIST_H;out[i].version=wr_version[i];
        if(out[i].visible&&pixels&&pixels[i])memcpy(pixels[i],wr_px[i],sizeof(wr_px[i]));n+=out[i].visible;}
    out[2]=rd_quad;out[2].width=.30f;out[2].height=.30f;out[2].w=RING_S;out[2].h=RING_S;out[2].version=rd_version;
    if(out[2].visible&&pixels&&pixels[2])memcpy(pixels[2],rd_px,sizeof(rd_px));n+=out[2].visible;
    ReleaseSRWLockShared(&wr_lock);
    return n;
}
