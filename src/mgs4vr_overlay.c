/* 2D scope overlay off in VR first person. Inert without marker mgs4vr_overlay.on ("on").
   Sim proof 2026-10-12: the overlay is the UI layouts of the scope view
   actor (controller+0x98): +0xA8 crosshair/scale, +0xA0 vignette; clearing node bit 0x400000 once leaves
   a clean view. 0x14DB9D0 is the shared per-frame update of every scope actor (game thread); at
   Seam14DBA02 (MOV RCX,[RBX+A8], rbx = actor) both layouts are about to be updated by 0x43B350.
   Hide = flags & ~0x400000 | 0x40000000 (the game's own HUD idiom, 0x4CA450). Nodes we hid are
   remembered and shown again when first person ends while the same actor is still updated. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdint.h>
#include "mgs4vr_overlay.h"
#include "mgs4vr_xr.h"
#include "mgs4vr_scope_gesture.h"
#define OVERLAY_RVA 0x14dba02
#define OVERLAY_NODES 1024
#define OVERLAY_MAXCOUNT 4096
static const BYTE overlay_sig[]={0x48,0x8b,0x8b,0xa8,0,0,0,0x48,0x85,0xc9,0x74,0x0d};
static BYTE *oi,*os;static PVOID ov;
static volatile LONG o_enabled,o_fp,o_seen,o_hidden,o_shown,o_faults,o_skipped;
/* Rifle to the eye (XR thread): the overlay is the real scope then, so it stays visible (MGS3 behaviour). */
static volatile LONG o_gesture,o_opens;static volatile float o_gdist,o_gang;
/* Gesture metric (marker tokens, 13 Oct 2026): default "line" (eye on the rifle line behind the hand, see
   mgs4vr_scope_gesture.h); "gesture=near" = the old grip-within-20-cm test. line=/lineclose=/alongmin=/alongmax=/
   deg=/degclose= override the line thresholds (mm, degrees). Session minima go to the status file so one headset
   run gives the real numbers: min_line (eye-line distance with the eye behind the hand and the angle within
   degclose), min_dist (grip-head), min_angle, along_at_min. */
static SG_LINE_CFG o_cfg;static volatile LONG o_near;
static volatile float o_line,o_along,o_min_line=1e9f,o_min_dist=1e9f,o_min_ang=1e9f,o_along_at_min;static volatile LONG o_line_frames,o_cand_frames;
/* Several scope actors pass the seam each frame: one slot per actor. A slot not seen for OVERLAY_STALE seam
   calls belongs to a destroyed actor and is dropped without any write. o_count = nodes held over all slots. */
#define OVERLAY_SLOTS 8
#define OVERLAY_STALE 2000
typedef struct { uint64_t actor; LONG last; unsigned count; uint32_t *nodes[OVERLAY_NODES]; } O_SLOT;
static O_SLOT o_slot[OVERLAY_SLOTS];static volatile unsigned o_count;
static DWORD o_tick;static int o_attempted,o_owned;
static char o_mark[MAX_PATH],o_status[MAX_PATH];
/* Visit the nodes of one layout. hide=1: clear visible bits and remember in slot s; hide=0: show again only the
   remembered nodes that are still in this live layout (never write through a remembered pointer alone). */
static unsigned o_layout(O_SLOT *s,uint64_t layout,int hide){
 unsigned n,i,changed=0;uint32_t **arr,*node,f;
 if(layout<0x10000||layout>=0x800000000000ULL)return 0;
 n=*(uint32_t*)(layout+0x170);arr=*(uint32_t***)(layout+0x180);
 if(!n||n>OVERLAY_MAXCOUNT||(uint64_t)arr<0x10000||(uint64_t)arr>=0x800000000000ULL)return 0;
 for(i=1;i<=n;++i){node=arr[i];if((uint64_t)node<0x10000||(uint64_t)node>=0x800000000000ULL)continue;f=*node;
  if(hide&&(f&0x400000)){
   /* 13 Oct 2026 (sim: Javelin overlay stuck in first person, skipped=208335): a node the game shows again was
      appended again until the list was full, and a full list stopped hiding. Now: remember each node once, and
      hide even when the list is full (such a node is just not shown again later). */
   unsigned j;*node=(f&~0x400000u)|0x40000000u;++changed;
   for(j=0;j<s->count&&s->nodes[j]!=node;++j);
   if(j==s->count){if(s->count<OVERLAY_NODES)s->nodes[s->count++]=node;else InterlockedIncrement(&o_skipped);}}
  else if(!hide){unsigned j;for(j=0;j<s->count;++j)if(s->nodes[j]==node){*node=f|0x40400000u;++changed;break;}}}
 return changed;
}
static void o_total(void){unsigned i,n=0;for(i=0;i<OVERLAY_SLOTS;++i)if(o_slot[i].actor)n+=o_slot[i].count;o_count=n;}
static O_SLOT *o_find(uint64_t actor,int create){
 unsigned i;O_SLOT *free_slot=NULL;
 for(i=0;i<OVERLAY_SLOTS;++i){O_SLOT *s=&o_slot[i];
  if(s->actor==actor)return s;
  if(s->actor&&o_seen-s->last>OVERLAY_STALE){s->actor=0;s->count=0;} /* destroyed actor: forget, no writes */
  if(!s->actor&&!free_slot)free_slot=s;}
 if(create&&!free_slot){                 /* all slots busy (weapon switches): the longest unseen actor makes room, no writes */
  for(i=0;i<OVERLAY_SLOTS;++i)if(!free_slot||o_slot[i].last-free_slot->last<0)free_slot=&o_slot[i];}
 if(!create||!free_slot)return NULL;
 free_slot->actor=actor;free_slot->count=0;return free_slot;
}
static LONG CALLBACK overlay_trap(EXCEPTION_POINTERS *e){
 CONTEXT *c=e->ContextRecord;uint64_t actor,a8=0;O_SLOT *s;
 if(e->ExceptionRecord->ExceptionCode!=EXCEPTION_BREAKPOINT||!os||e->ExceptionRecord->ExceptionAddress!=os)return EXCEPTION_CONTINUE_SEARCH;
 actor=c->Rbx;
 __try{a8=*(uint64_t*)(actor+0xa8);}__except(EXCEPTION_EXECUTE_HANDLER){return EXCEPTION_CONTINUE_SEARCH;} /* the original load would fault too */
 InterlockedIncrement(&o_seen);
 __try{
  int hiding=o_enabled&&o_fp&&!o_gesture;
  s=o_find(actor,hiding);
  if(s){s->last=o_seen;
   if(hiding){unsigned k=o_layout(s,a8,1)+o_layout(s,*(uint64_t*)(actor+0xa0),1);if(k)InterlockedAdd(&o_hidden,(LONG)k);}
   else{InterlockedAdd(&o_shown,(LONG)(o_layout(s,a8,0)+o_layout(s,*(uint64_t*)(actor+0xa0),0)));s->actor=0;s->count=0;}}
  o_total();
 }__except(EXCEPTION_EXECUTE_HANDLER){InterlockedIncrement(&o_faults);InterlockedExchange(&o_enabled,0);memset(o_slot,0,sizeof(o_slot));o_count=0;}
 c->Rcx=a8;c->Rip=(DWORD64)os+7;return EXCEPTION_CONTINUE_EXECUTION;
}static int o_state(void){__try{if(memcmp(os+1,overlay_sig+1,sizeof(overlay_sig)-1))return -1;return os[0]==0xcc?1:os[0]==overlay_sig[0]?0:-1;}__except(EXCEPTION_EXECUTE_HANDLER){return -1;}}
static int o_patch(BYTE from,BYTE to){DWORD old,tmp;char v;if(!VirtualProtect(os,1,PAGE_EXECUTE_READWRITE,&old))return 0;v=_InterlockedCompareExchange8((volatile char*)os,to,from);FlushInstructionCache(GetCurrentProcess(),os,1);VirtualProtect(os,1,old,&tmp);return (BYTE)v==from;}
static void o_report(const char *event){FILE *f;if(fopen_s(&f,o_status,"w"))return;
 fprintf(f,"overlay_version=3 event=%s enabled=%ld first_person=%ld gesture=%ld opens=%ld grip_head_mm=%.0f aim_gaze_deg=%.1f seen=%ld hidden=%ld shown=%ld held=%u skipped=%ld faults=%ld restored=%d\n",event,o_enabled,o_fp,o_gesture,o_opens,o_gdist,o_gang,o_seen,o_hidden,o_shown,o_count,o_skipped,o_faults,os?o_state()==0:0);
 fprintf(f,"gesture_metric=%s line_open=%.0f line_close=%.0f along=%.0f..%.0f deg_open=%.0f deg_close=%.0f line_mm=%.0f along_mm=%.0f min_line=%.0f along_at_min=%.0f min_dist=%.0f min_angle=%.1f line_frames=%ld cand_frames=%ld\n",
  o_near?"near":"line",o_cfg.line_open,o_cfg.line_close,o_cfg.along_min,o_cfg.along_max,o_cfg.deg_open,o_cfg.deg_close,o_line,o_along,o_min_line,o_along_at_min,o_min_dist,o_min_ang,o_line_frames,o_cand_frames);fclose(f);}
/* Pure (desk tests): marker text -> on/off + gesture config. */
int mgs4vr_overlay_parse(const char *b,SG_LINE_CFG *c,int *near_mode){const char *q;float v;
 *c=SG_LINE_DEFAULT;*near_mode=strstr(b,"gesture=near")!=NULL;
#define O_TOK(name,field,lo,hi) if((q=strstr(b,name))&&sscanf_s(q+sizeof(name)-1,"%f",&v)==1&&v>=(lo)&&v<=(hi))c->field=v
 O_TOK("line=",line_open,20,600);O_TOK("lineclose=",line_close,20,800);O_TOK("alongmin=",along_min,-500,1000);O_TOK("alongmax=",along_max,0,2000);
 O_TOK("deg=",deg_open,1,90);O_TOK("degclose=",deg_close,1,120);
#undef O_TOK
 if(c->line_close<c->line_open)c->line_close=c->line_open;if(c->deg_close<c->deg_open)c->deg_close=c->deg_open;
 while(*b==' '||*b=='\t')++b;if(b[0]==(char)0xef&&b[1]==(char)0xbb&&b[2]==(char)0xbf)b+=3;
 return !strncmp(b,"on",2)&&(b[2]==0||b[2]==' '||b[2]=='\r'||b[2]=='\n'||b[2]=='\t');}
static int o_mode(void){FILE*f;char b[256]={0};size_t n;SG_LINE_CFG c;int nm,on;if(fopen_s(&f,o_mark,"rb"))return 0;n=fread(b,1,255,f);fclose(f);b[n]=0;
 on=mgs4vr_overlay_parse(b,&c,&nm);o_cfg=c;InterlockedExchange(&o_near,nm);return on;}
int mgs4vr_overlay_gesture(void){return o_enabled&&o_fp&&o_gesture;}
void mgs4vr_overlay_hands(const MGS4VR_XR_HANDS *h){
 float dist=0,ang=0,line=0,along=0;int was=o_gesture,now,lnow;SG_LINE_CFG c=o_cfg;
 if(!h||!h->head_valid||!h->hand[1].active){InterlockedExchange(&o_gesture,0);return;}
 if(c.deg_close<=0)c=SG_LINE_DEFAULT;                                      /* marker not read yet */
 now=sg_update(was,h->head_quat,h->head_pos,h->hand[1].aim_quat,h->hand[1].grip_pos,&dist,&ang);
 lnow=sg_update_line(was,&c,h->head_quat,h->head_pos,h->hand[1].aim_quat,h->hand[1].grip_pos,&line,&along,NULL);
 if(!o_near)now=lnow;
 o_gdist=dist;o_gang=ang;o_line=line;o_along=along;
 if(o_fp){if(dist<o_min_dist)o_min_dist=dist;if(ang<o_min_ang)o_min_ang=ang;
  if(along>=c.along_min&&along<=c.along_max&&ang<=c.deg_close){InterlockedIncrement(&o_cand_frames);if(line<o_min_line){o_min_line=line;o_along_at_min=along;}}
  if(lnow)InterlockedIncrement(&o_line_frames);}
 if(now&&!was)InterlockedIncrement(&o_opens);InterlockedExchange(&o_gesture,now);
}
void mgs4vr_overlay_poll(void *base,const char *exe,int fp){
 DWORD now=GetTickCount();char *leaf;
 InterlockedExchange(&o_fp,fp?1:0);
 if(!o_mark[0]){strcpy_s(o_mark,sizeof(o_mark),exe);leaf=strrchr(o_mark,'\\');if(!leaf){o_mark[0]=0;return;}strcpy_s(leaf+1,sizeof(o_mark)-(size_t)(leaf+1-o_mark),"mgs4vr_overlay.on");strcpy_s(o_status,sizeof(o_status),o_mark);sprintf_s(strrchr(o_status,'\\')+1,64,"mgs4vr_overlay_%lu.txt",GetCurrentProcessId());}
 if(o_attempted){
  /* marker gone: stop hiding first; the seam stays until the held nodes were shown again by the trap */
  if(o_enabled&&!o_mode()){InterlockedExchange(&o_enabled,0);o_report("disarming");}
  if(o_owned&&os&&o_state()==1&&!o_enabled&&!o_count){if(o_patch(0xcc,overlay_sig[0]))o_owned=0;o_report(o_state()==0?"stopped":"restore_failed");}
  if(o_enabled&&o_state()!=1){InterlockedExchange(&o_enabled,0);o_report("integrity_lost");}
  if(!o_enabled&&!o_owned&&GetFileAttributesA(o_mark)==INVALID_FILE_ATTRIBUTES)o_attempted=0;
  /* actors that never come back (scope closed) keep nothing to restore: give up after ~10 s */
  if(!o_enabled&&o_owned&&o_count&&now-o_tick>=10000){memset(o_slot,0,sizeof(o_slot));o_count=0;}
  if(o_enabled&&now-o_tick>=1000){o_tick=now;o_report("active");}return;
 }
 if(!o_mode())return;o_attempted=1;oi=(BYTE*)base;os=oi+OVERLAY_RVA;
 __try{if(memcmp(os,overlay_sig,sizeof(overlay_sig))){o_report("reject_signature");return;}}__except(EXCEPTION_EXECUTE_HANDLER){o_report("reject_signature");return;}
 if(!ov)ov=AddVectoredExceptionHandler(1,overlay_trap);if(!ov){o_report("reject_veh");return;}
 o_seen=o_hidden=o_shown=o_faults=o_skipped=0;memset(o_slot,0,sizeof(o_slot));o_count=0;o_tick=now;InterlockedExchange(&o_enabled,1);
 if(!o_patch(overlay_sig[0],0xcc)){InterlockedExchange(&o_enabled,0);o_report("reject_patch");return;}o_owned=1;o_report("installed");
}
