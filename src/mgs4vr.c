#include "mgs4vr_parts.h"
#include "mgs4vr_arms.h"
#include "mgs4vr_gpu_timer.h"
#include "mgs4vr_d3dprobe.h"
#include "mgs4vr_scope.h"
#include "mgs4vr_rec.h"
#include "mgs4vr_armwrite.h"
#include "mgs4vr_watch.h"
#include "mgs4vr_armsrc.h"
#include "mgs4vr_pad.h"
#include "mgs4vr_shot.h"
#include "mgs4vr_muzzle.h"
#include "mgs4vr_rocket.h"
#include "mgs4vr_overlay.h"
#include "mgs4vr_vrmenu.h"
#include "mgs4vr_hud.h"
#include "mgs4vr_wrist_api.h"
/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "mgs4vr_gfx.h"
#include "mgs4vr_cam.h"
#include "mgs4vr_head.h"
#include "mgs4vr_xr.h"
#include "mgs4vr_profile.h"
#include "mgs4vr_menu.h"
#include "mgs4vr_eye.h"
#include "mgs4vr_twoeye.h"
#include "mgs4vr_lens.h"
#include "mgs4vr_cqc.h"
#include "mgs4vr_camo.h"
#include "mgs4vr_cqcfp.h"
#include "mgs4vr_fpaction.h"
#include "mgs4vr_poses.h"
#include "mgs4vr_render.h"
#include "mgs4vr_scan.h"
#include "mgs4vr_fpshold.h"
#if (!defined(DG_ENABLE_DIAGNOSTICS) || DG_ENABLE_DIAGNOSTICS != 0) && !defined(MGS4VR_GPU_TIMER)
#error Player release requires DG_ENABLE_DIAGNOSTICS=0
#endif
static volatile LONG stereo_on;
/* Pistol gestures (16 Oct 2026): the game thread reports a rack (1) or a magazine insert (2); the worker reloads (R)
   when it makes sense: insert with a magazine that is not full, rack with an empty one. */
static volatile LONG pistol_reload_kind, pistol_reloads;
static void pistol_reload_req(int kind) { InterlockedExchange(&pistol_reload_kind, kind); }
/* Camera seam: the pose tool's camera first (17 Oct 2026), else CQC / first-person-action head camera. */
static int cam_source_hook(float *m) {
#if MGS4VR_ARMSRC
    if (mgs4vr_armsrc_pose_camera(m)) return 1;
#endif
    return mgs4vr_cqcfp_seam(m);
}
#if MGS4VR_ARMSRC
#define PISTOL_SET_RELOAD() mgs4vr_armsrc_set_reload(pistol_reload_req)
#else
#define PISTOL_SET_RELOAD() ((void)pistol_reload_req)
#endif
static int stereo_recenter;
static char exe[MAX_PATH],ini[MAX_PATH];
static SRWLOCK pose_lock=SRWLOCK_INIT;
static int follow,have_reference,reference_token,pose_token;
static DWORD last_pose;
static float ref_q[4],ref_p[3];
static int sha256_file(const char *path, char out_hex[65]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE h = INVALID_HANDLE_VALUE;
    static BYTE buf[1 << 20];
    BYTE digest[32];
    DWORD n;
    int ok = 0, i;
    out_hex[0] = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return 0;
    if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) != 0) goto done;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) goto done;
    for (;;) {
        if(!ReadFile(h,buf,sizeof(buf),&n,NULL))goto done;
        if(!n)break;
        if(BCryptHashData(hash,buf,n,0)!=0)goto done;
    }
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) goto done;
    for (i = 0; i < 32; ++i) sprintf_s(out_hex + i * 2, 3, "%02x", digest[i]);
    ok = 1;
done:
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static void clear_pose(void){MGS4VR_CAM_ADJUST a={0};a.cine_auto=1;a.blur_off=1;mgs4vr_cam_set_adjust(&a);}
static void head_pose(const float q[4],const float p[3]){
 MGS4VR_HEAD_CFG cfg;MGS4VR_CAM_ADJUST a={0};int i;
 AcquireSRWLockExclusive(&pose_lock);
 if(!follow){ReleaseSRWLockExclusive(&pose_lock);return;}
 for(i=0;i<4;++i)if(!isfinite(q[i])){clear_pose();ReleaseSRWLockExclusive(&pose_lock);return;}
 if(!have_reference || reference_token!=pose_token){memcpy(ref_q,q,16);memcpy(ref_p,p,12);have_reference=1;reference_token=pose_token;}
 mgs4vr_head_cfg_defaults(&cfg);cfg.position=0;a.enabled=1;a.cine_auto=1;a.blur_off=1;
 mgs4vr_head_to_offset(&cfg,ref_q,ref_p,q,p,&a);mgs4vr_cam_set_adjust(&a);last_pose=GetTickCount();
 ReleaseSRWLockExclusive(&pose_lock);
}
static int supported(void){
 BYTE *b=(BYTE *)GetModuleHandleW(NULL);IMAGE_DOS_HEADER *d=(IMAGE_DOS_HEADER *)b;
 IMAGE_NT_HEADERS64 *n=(IMAGE_NT_HEADERS64 *)(b+d->e_lfanew);char hash[65];
 return n->FileHeader.TimeDateStamp==MGS4VR_PE_TIMESTAMP && n->OptionalHeader.SizeOfImage==MGS4VR_SIZE_OF_IMAGE && sha256_file(exe,hash) && !_stricmp(hash,MGS4VR_EXE_SHA256);
}
static void views(const MGS4VR_XR_VIEWS *v){mgs4vr_eye_views(v,GetTickCount64());}
static int prepare_eye(unsigned long long camera,int cine,MGS4VR_EYE_TICKET *ticket){
 unsigned serial;int w,h;if(!stereo_on)return 0;
 if(!mgs4vr_render_serial(&serial))return -1;
 __try{w=*(int *)(camera+0x360);h=*(int *)(camera+0x364);}__except(EXCEPTION_EXECUTE_HANDLER){return -1;}
 return mgs4vr_eye_prepare(serial,cine,w,h,GetTickCount64(),ticket);
}
static volatile LONG gameplay_builds;
static void finish_eye(unsigned long long camera,unsigned long long caller,const MGS4VR_EYE_TICKET *ticket,int applied){
 float combined[16]={0};BYTE *base=(BYTE *)GetModuleHandleW(NULL);
 /* Two eyes per frame (mgs4vr_twoeye): the second native view gets the right eye of this same ticket. */
 mgs4vr_twoeye_frame(base,camera,caller==(unsigned long long)(base+MGS4VR_RVA_RET_GAMEPLAY),applied,stereo_on?ticket:NULL);
 if(caller==(unsigned long long)(base+MGS4VR_RVA_RET_GAMEPLAY))InterlockedIncrement(&gameplay_builds);
 /* HUD off in VR (mgs4vr_hud.h): once per gameplay frame on the game thread; Solid Eye overlays are kept */
 if(caller==(unsigned long long)(base+MGS4VR_RVA_RET_GAMEPLAY))mgs4vr_hud_frame(base,mgs4vr_hud_want_now(mgs4vr_fpshold_stage()==2||mgs4vr_fpaction_busy()));
 /* Recorder: one consistent game-side sample per gameplay camera build. */
 if(caller==(unsigned long long)(base+MGS4VR_RVA_RET_GAMEPLAY))mgs4vr_rec_game(base,camera,(unsigned)mgs4vr_gfx_frame());
 if(!stereo_on || caller!=(unsigned long long)(base+MGS4VR_RVA_RET_GAMEPLAY))return;
 __try{memcpy(combined,(void *)(camera+0x100),64);}__except(EXCEPTION_EXECUTE_HANDLER){applied=0;}
 mgs4vr_eye_commit(ticket,applied,combined);
 /* Arm IK: this camera together with the eye pose it was built from. */
 if(applied && ticket && ticket->packet.valid){
  float c2w[16];int ok=1;
  __try{memcpy(c2w,(void *)camera,64);}__except(EXCEPTION_EXECUTE_HANDLER){ok=0;}
  if(ok)mgs4vr_armsrc_eye_anchor(ticket->packet.quat,ticket->packet.pos,c2w,ticket->packet.display_time);
 }
}
static unsigned probe_frame(void){return (unsigned)mgs4vr_gfx_frame();}
/* The watchpoint diagnostic needs the debug registers the camera hooks use. */
static MGS4VR_CAM_CONFIG cam_cfg;static int cam_armed;
static void pause_cam(void){if(cam_armed)mgs4vr_cam_disarm();}
static void resume_cam(void){if(cam_armed)cam_armed=mgs4vr_cam_arm(&cam_cfg,NULL)>0;}
static void hands(const MGS4VR_XR_HANDS *h){
 mgs4vr_rec_hands(h);
 /* in a game menu A selects, so the first-person toggle must not see it */
 if(h && mgs4vr_pad_menu()){MGS4VR_XR_HANDS m=*h;m.hand[1].buttons&=~2u;mgs4vr_fpshold_hands(&m);}else mgs4vr_fpshold_hands(h);
 mgs4vr_armsrc_hands(h);mgs4vr_lens_hands(h);mgs4vr_cqc_hands(h);mgs4vr_camo_hands(h);mgs4vr_pad_hands(h);mgs4vr_overlay_hands(h);mgs4vr_wrist_hands(h);}
static void present(void *sc,unsigned flags){MGS4VR_EYE_PACKET p={0},r;if(stereo_on)mgs4vr_render_take(&p);
 if(p.valid&&p.twoeye){if(mgs4vr_eye_right(p.camera_serial,&r))mgs4vr_xr_on_present_two(sc,flags,&p,&r);else mgs4vr_xr_on_present_eye(sc,flags,NULL);return;}
 mgs4vr_xr_on_present_eye(sc,flags,&p);}
static void configure(const MGS4VR_MENU *m){
 MGS4VR_XR_CONFIG c={0};c.enabled=1;c.dist_m=m->distance_cm/100.f;c.width_m=3.2f;c.recenter=m->recenter;
 c.stereo=m->stereo && m->stereo_available && !m->open;
 if(stereo_on!=c.stereo || stereo_recenter!=m->recenter){
  InterlockedExchange(&stereo_on,0);mgs4vr_eye_enable(c.stereo);stereo_recenter=m->recenter;InterlockedExchange(&stereo_on,c.stereo);
 }
 c.observer=m->observer;c.menu_stereo=m->stereo;c.stereo_available=m->stereo_available;
 c.menu_open=m->open;c.menu_row=m->row;c.follow_head=m->follow && m->available && !m->stereo;c.head_available=m->available;
 c.twohanded=m->twohanded;c.smoothing_ms=m->smoothing_ms;
 mgs4vr_armsrc_configure(m->twohanded,m->smoothing_ms,m->recenter);
 mgs4vr_xr_configure(&c);
 AcquireSRWLockExclusive(&pose_lock);
 if(follow!=c.follow_head || pose_token!=m->recenter)have_reference=0;
 follow=c.follow_head;pose_token=m->recenter;if(!follow)clear_pose();
 ReleaseSRWLockExclusive(&pose_lock);
}
static void save(const MGS4VR_MENU *m){char v[32];sprintf_s(v,sizeof(v),"%d",m->distance_cm);WritePrivateProfileStringA("Theater","DistanceCm",v,ini);WritePrivateProfileStringA("Theater","FollowHead",m->follow?"1":"0",ini);WritePrivateProfileStringA("Theater","Stereo",m->stereo?"1":"0",ini);sprintf_s(v,sizeof(v),"%d",m->observer);WritePrivateProfileStringA("Theater","Observer",v,ini);WritePrivateProfileStringA("Weapons","TwoHandAiming",m->twohanded?"1":"0",ini);sprintf_s(v,sizeof(v),"%d",m->smoothing_ms);WritePrivateProfileStringA("Weapons","SupportSmoothingMs",v,ini);}
static void expire_pose(DWORD now){
 AcquireSRWLockExclusive(&pose_lock);
 if(follow && now-last_pose>400){clear_pose();have_reference=0;}
 ReleaseSRWLockExclusive(&pose_lock);
}
static DWORD WINAPI worker(void *unused){
 MGS4VR_MENU menu;DWORD start=GetTickCount();int checked=0,armed=0,known;char *leaf,loader[MAX_PATH];
 unsigned keys[]={VK_HOME,VK_ESCAPE,VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,VK_RETURN};int held[7]={0};
 (void)unused;GetModuleFileNameA(NULL,exe,MAX_PATH);leaf=strrchr(exe,'\\');leaf=leaf?leaf+1:exe;
 if(_stricmp(leaf,MGS4VR_HOST_EXE))return 0;
 strcpy_s(ini,sizeof(ini),exe);strcpy_s(strrchr(ini,'\\')+1,32,"mgs4vr.ini");
 strcpy_s(loader,sizeof(loader),exe);strcpy_s(strrchr(loader,'\\')+1,32,"openxr_loader.dll");
 mgs4vr_gpu_open(exe);
 known=supported();mgs4vr_menu_init(&menu,GetPrivateProfileIntA("Theater","FollowHead",0,ini),GetPrivateProfileIntA("Theater","DistanceCm",250,ini));
 menu.observer=(int)GetPrivateProfileIntA("Theater","Observer",2,ini);if(menu.observer<0||menu.observer>2)menu.observer=2;
 menu.twohanded=!!GetPrivateProfileIntA("Weapons","TwoHandAiming",1,ini);
 menu.smoothing_ms=(int)GetPrivateProfileIntA("Weapons","SupportSmoothingMs",0,ini);
 if(menu.smoothing_ms!=30&&menu.smoothing_ms!=60&&menu.smoothing_ms!=90)menu.smoothing_ms=0;
 menu.stereo=!!GetPrivateProfileIntA("Theater","Stereo",0,ini);
 configure(&menu);mgs4vr_gfx_set_options(1);mgs4vr_gfx_set_present_callback(present);
 if(!mgs4vr_gfx_start(NULL,0))return 0;
 mgs4vr_d3dprobe_set_frame_source(probe_frame);mgs4vr_scope_set_frame_source(probe_frame);
 mgs4vr_xr_set_views_callback(views);mgs4vr_xr_set_pose_callback(head_pose);mgs4vr_xr_set_hands_callback(hands);mgs4vr_xr_start(NULL,loader,NULL);
 for(;;){
  int i,changed=0;DWORD pid=0;Sleep(25);GetWindowThreadProcessId(GetForegroundWindow(),&pid);
  for(i=0;i<7;++i){int down=!!(GetAsyncKeyState(keys[i])&0x8000);if(pid==GetCurrentProcessId() && down && !held[i])changed|=mgs4vr_menu_key(&menu,keys[i]);held[i]=down;}
  if(!checked && known && mgs4vr_gfx_frame()>=120){
   MGS4VR_CAM_CONFIG c={0};static const unsigned char sig[]=MGS4VR_SIG_CAMERA_BUILDER;BYTE *b=(BYTE *)GetModuleHandleW(NULL);
   c.builder=b+MGS4VR_RVA_CAMERA_BUILDER;c.builder_sig=sig;c.builder_sig_len=sizeof(sig);c.image_base=b;c.require_call_check=1;
   c.adjust_callers[0]=b+MGS4VR_RVA_RET_WRAPPER;c.adjust_callers[1]=b+MGS4VR_RVA_RET_GAMEPLAY;c.adjust_camera=b+MGS4VR_RVA_MAIN_CAMERA;
   c.ret[0]=b+MGS4VR_RVA_RET_GAMEPLAY;c.ret[1]=b+MGS4VR_RVA_RET_CINE_REBUILD;
   c.prepare_eye=prepare_eye;c.finish_eye=finish_eye;
   {const unsigned char *text=NULL,*hit=NULL;size_t len=0;
    if(mgs4vr_image_section(b,".text",&text,&len) && mgs4vr_scan(text,len,MGS4VR_SIG_MOTION_BLUR,&hit,3)==1)c.skip_fn=(void *)hit;
   }
   {int flags=mgs4vr_cam_probe(&c,NULL);
    menu.stereo_available=c.skip_fn && (flags&MGS4VR_CAM_OK_BUILDER) && (flags&MGS4VR_CAM_OK_RET0) && mgs4vr_render_start(b);
   }
   mgs4vr_cam_set_source_hook(cam_source_hook);mgs4vr_scope_set_parts_hook(mgs4vr_cqcfp_parts);armed=mgs4vr_cam_arm(&c,NULL)>0;cam_cfg=c;cam_armed=armed;if(!armed){menu.stereo_available=0;mgs4vr_render_stop();}checked=1;menu.available=armed;changed=1;
  }
  /* VR first-person hold: only while the game is foreground and no menu is open. */
  {int fg=pid==GetCurrentProcessId(),showing=0,clipped=0,game_menu=fg && mgs4vr_pad_cursor_menu(&showing,&clipped);static DWORD traced;
   /* game menu (cursor visible): no first-person hold, A/B go to the menu */
   mgs4vr_armsrc_gameplay(fg && !menu.open && !(game_menu && mgs4vr_pad_menu()));
   /* CQC gesture (left hand): may pause the aim and press the left mouse for a moment */
   mgs4vr_cqc_set_on_weapon(mgs4vr_armsrc_left_on_weapon);
   mgs4vr_cqc_tick(GetModuleHandleW(NULL),exe,fg && !menu.open && !(game_menu && mgs4vr_pad_menu()),mgs4vr_fpshold_stage()==2,GetTickCount());
   mgs4vr_fpshold_pause_aim(mgs4vr_cqc_pause_aim());mgs4vr_pad_cqc_lmb(mgs4vr_cqc_lmb());mgs4vr_pad_cqc_dir(mgs4vr_cqc_dir());
   mgs4vr_cqcfp_tick(GetModuleHandleW(NULL),exe,GetTickCount());              /* CQC first person: head camera at the seam */
   if(mgs4vr_cqc_reapply_fp())mgs4vr_fpshold_reapply();                     /* the grab dropped first person: back in */
   /* camo reset: shake both controllers = T (not during CQC, not with the left hand on the foregrip) */
   if(mgs4vr_camo_tick(exe,fg && !menu.open && !(game_menu && mgs4vr_pad_menu()) && mgs4vr_fpshold_stage()==2 && !mgs4vr_cqc_busy() && !mgs4vr_armsrc_left_on_weapon(),GetTickCount()))mgs4vr_pad_tap('T');
   mgs4vr_fpshold_tick(fg && !menu.open && !(game_menu && mgs4vr_pad_menu()) && !mgs4vr_fpaction_suspend_fp(),GetTickCount());
   /* Quest sticks as a virtual Xbox pad (XInput IAT slot); early, so the game sees the pad from the start */
   if(known)mgs4vr_pad_poll(GetModuleHandleW(NULL),exe);
   mgs4vr_pad_tick(fg && !menu.open,mgs4vr_fpshold_stage()==2,game_menu,GetTickCount());if(mgs4vr_pad_take_recenter()){menu.recenter++;changed=1;if(mgs4vr_fpshold_stage()==2)mgs4vr_wrist_level_view();}   /* menu button held: recenter (16 Oct) */
   if(GetTickCount()-traced>=100){traced=GetTickCount();mgs4vr_pad_trace(traced,showing,clipped,mgs4vr_fpshold_stage()==2,gameplay_builds,mgs4vr_pad_menu());}}
  if(changed){configure(&menu);save(&menu);}
  if(armed)mgs4vr_cam_poll();
  /* arms probe, parts probe and armwrite trap overlapping CALLs: one at a time */
  if(known && checked){
   if(!mgs4vr_parts_active() && !mgs4vr_armwrite_active())mgs4vr_arms_poll(GetModuleHandleW(NULL),exe,mgs4vr_gfx_frame());
   if(!mgs4vr_arms_active() && !mgs4vr_armwrite_active())mgs4vr_parts_poll(GetModuleHandleW(NULL),exe,mgs4vr_gfx_frame());
   if(!mgs4vr_arms_active() && !mgs4vr_parts_active())mgs4vr_armwrite_poll(GetModuleHandleW(NULL),exe);
   if(!mgs4vr_arms_active() && !mgs4vr_parts_active() && !mgs4vr_armwrite_active())mgs4vr_watch_poll(GetModuleHandleW(NULL),exe,mgs4vr_gfx_frame(),pause_cam,resume_cam);
  }
  if(known && checked)mgs4vr_d3dprobe_poll(GetModuleHandleW(NULL),exe,mgs4vr_gfx_frame());
  /* scope mask: only while the VR first-person hold is fully applied */
  if(known && checked)mgs4vr_scope_poll(GetModuleHandleW(NULL),exe,mgs4vr_fpshold_stage()==2);
  if(known && checked)mgs4vr_rec_poll(exe);
  if(known && checked)mgs4vr_armsrc_poll(GetModuleHandleW(NULL),exe);
  if(known && checked)mgs4vr_shot_poll(GetModuleHandleW(NULL),exe);
  if(known && checked)mgs4vr_muzzle_poll(GetModuleHandleW(NULL),exe);
  if(known && checked)mgs4vr_rocket_poll(GetModuleHandleW(NULL),exe);
  if(known && checked){mgs4vr_muzzle_set_gesture(mgs4vr_overlay_gesture);mgs4vr_rocket_set_gesture(mgs4vr_overlay_gesture);mgs4vr_scope_set_gesture(mgs4vr_overlay_gesture);mgs4vr_overlay_poll(GetModuleHandleW(NULL),exe,mgs4vr_fpshold_stage()==2);{MGS4VR_VM_THEATER th={menu.follow,menu.distance_cm,menu.stereo,menu.observer,menu.available,menu.stereo_available};int r;mgs4vr_vrmenu_set_theater(&th);mgs4vr_vrmenu_poll(GetModuleHandleW(NULL),exe);mgs4vr_hud_poll(exe);   /* VR Settings in the game's Options: theater settings go through the Home menu's configure/save */
    if((r=mgs4vr_vrmenu_take_theater(&th))!=0){menu.follow=th.follow;menu.distance_cm=th.distance_cm;menu.stereo=th.stereo;menu.observer=th.observer;if(r==2)menu.recenter++;configure(&menu);save(&menu);}}mgs4vr_eye_scope(mgs4vr_overlay_gesture()?mgs4vr_scope_zoom():0.f);mgs4vr_pad_set_look_block(mgs4vr_wrist_ring_open);mgs4vr_cqcfp_set_head_source(mgs4vr_fpaction_head_cam);mgs4vr_pad_set_act_source(mgs4vr_cqcfp_last_act);mgs4vr_wrist_set_use(mgs4vr_fpaction_request_use);mgs4vr_lens_set_attach_source(mgs4vr_scope_attach_code);mgs4vr_pad_set_move_block(mgs4vr_wrist_item_ring_open);mgs4vr_pad_set_heading_source(mgs4vr_wrist_heading);mgs4vr_pad_set_yaw_source(mgs4vr_wrist_yaw);mgs4vr_eye_twoeye(mgs4vr_twoeye_poll(exe));mgs4vr_lens_poll(exe);{static DWORD dump_tick;char dm[MAX_PATH],*lf;if(GetTickCount()-dump_tick>500){dump_tick=GetTickCount();strcpy_s(dm,sizeof(dm),exe);lf=strrchr(dm,'\\');if(lf){strcpy_s(lf+1,sizeof(dm)-(size_t)(lf+1-dm),"mgs4vr_eyedump.on");if(GetFileAttributesA(dm)!=INVALID_FILE_ATTRIBUTES){DeleteFileA(dm);sprintf_s(lf+1,sizeof(dm)-(size_t)(lf+1-dm),"mgs4vr_eye_%lu_",GetCurrentProcessId());mgs4vr_xr_request_eye_dump(dm);}}}}mgs4vr_poses_poll(GetModuleHandleW(NULL),exe);PISTOL_SET_RELOAD();{LONG k=InterlockedExchange(&pistol_reload_kind,0);int mg=0,mx=0;if(k&&mgs4vr_wrist_mag(&mg,&mx)&&((k==2&&mg<mx)||(k==1&&mg==0))){mgs4vr_pad_tap('R');InterlockedIncrement(&pistol_reloads);}}mgs4vr_wrist_poll(exe,mgs4vr_fpshold_stage()==2,mgs4vr_scope_held_weapon());
   {static DWORD xr_tick;DWORD tnow=GetTickCount();if(tnow-xr_tick>=1000){MGS4VR_XR_STATS s;FILE *xf;char xp[MAX_PATH],*leaf;xr_tick=tnow;mgs4vr_xr_get_stats(&s);
    strcpy_s(xp,sizeof(xp),exe);leaf=strrchr(xp,92);if(leaf){sprintf_s(leaf+1,64,"mgs4vr_xr_%lu.txt",GetCurrentProcessId());
     if(!fopen_s(&xf,xp,"w")){fprintf(xf,"xr_status=1 frames=%ld stereo_layers=%ld theater_layers=%ld headlocked_layers=%ld wrist_layers=%ld twoeye_frames=%ld lens_captures=%ld lens_layers=%ld reticle_layers=%ld lens_inscope=%ld scope_zoom=%.2f theater_w=%ld\n",s.frames,s.stereo_layers,s.theater_layers,s.headlocked_layers,s.wrist_layers,s.twoeye_frames,s.lens_captures,s.lens_layers,s.reticle_layers,s.lens_inscope,mgs4vr_eye_scope_zoom(),s.theater_w);fclose(xf);}}}}}
  expire_pose(GetTickCount());
  if(GetTickCount()-start>1000){mgs4vr_gfx_verify();mgs4vr_gpu_drain();start=GetTickCount();}
 }
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved){
 (void)reserved;if(reason==DLL_PROCESS_ATTACH){HANDLE thread;DisableThreadLibraryCalls(instance);thread=CreateThread(NULL,0,worker,NULL,0,NULL);if(thread)CloseHandle(thread);}
 else if(reason==DLL_PROCESS_DETACH){mgs4vr_fpshold_release();mgs4vr_pad_release();} /* never leave aim/F/fire/WASD held for other programs */
 return TRUE;
}




