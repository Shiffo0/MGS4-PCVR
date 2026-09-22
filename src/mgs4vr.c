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

static void clear_pose(void){MGS4VR_CAM_ADJUST a={0};a.cine_auto=1;mgs4vr_cam_set_adjust(&a);}
static void head_pose(const float q[4],const float p[3]){
 MGS4VR_HEAD_CFG cfg;MGS4VR_CAM_ADJUST a={0};int i;
 AcquireSRWLockExclusive(&pose_lock);
 if(!follow){ReleaseSRWLockExclusive(&pose_lock);return;}
 for(i=0;i<4;++i)if(!isfinite(q[i])){clear_pose();ReleaseSRWLockExclusive(&pose_lock);return;}
 if(!have_reference || reference_token!=pose_token){memcpy(ref_q,q,16);memcpy(ref_p,p,12);have_reference=1;reference_token=pose_token;}
 mgs4vr_head_cfg_defaults(&cfg);cfg.position=0;a.enabled=1;a.cine_auto=1;
 mgs4vr_head_to_offset(&cfg,ref_q,ref_p,q,p,&a);mgs4vr_cam_set_adjust(&a);last_pose=GetTickCount();
 ReleaseSRWLockExclusive(&pose_lock);
}
static int supported(void){
 BYTE *b=(BYTE *)GetModuleHandleW(NULL);IMAGE_DOS_HEADER *d=(IMAGE_DOS_HEADER *)b;
 IMAGE_NT_HEADERS64 *n=(IMAGE_NT_HEADERS64 *)(b+d->e_lfanew);char hash[65];
 return n->FileHeader.TimeDateStamp==MGS4VR_PE_TIMESTAMP && n->OptionalHeader.SizeOfImage==MGS4VR_SIZE_OF_IMAGE && sha256_file(exe,hash) && !_stricmp(hash,MGS4VR_EXE_SHA256);
}
static void configure(const MGS4VR_MENU *m){
 MGS4VR_XR_CONFIG c={0};c.enabled=1;c.dist_m=m->distance_cm/100.f;c.width_m=3.2f;c.recenter=m->recenter;
 c.menu_open=m->open;c.menu_row=m->row;c.follow_head=m->follow && m->available;c.head_available=m->available;
 mgs4vr_xr_configure(&c);
 AcquireSRWLockExclusive(&pose_lock);
 if(follow!=c.follow_head || pose_token!=m->recenter)have_reference=0;
 follow=c.follow_head;pose_token=m->recenter;if(!follow)clear_pose();
 ReleaseSRWLockExclusive(&pose_lock);
}
static void save(const MGS4VR_MENU *m){char v[32];sprintf_s(v,sizeof(v),"%d",m->distance_cm);WritePrivateProfileStringA("Theater","DistanceCm",v,ini);WritePrivateProfileStringA("Theater","FollowHead",m->follow?"1":"0",ini);}
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
 known=supported();mgs4vr_menu_init(&menu,GetPrivateProfileIntA("Theater","FollowHead",0,ini),GetPrivateProfileIntA("Theater","DistanceCm",250,ini));
 configure(&menu);mgs4vr_gfx_set_options(1);mgs4vr_gfx_set_present_callback(mgs4vr_xr_on_present);
 if(!mgs4vr_gfx_start(NULL,0))return 0;
 mgs4vr_xr_set_pose_callback(head_pose);mgs4vr_xr_start(NULL,loader,NULL);
 for(;;){
  int i,changed=0;DWORD pid=0;Sleep(25);GetWindowThreadProcessId(GetForegroundWindow(),&pid);
  for(i=0;i<7;++i){int down=!!(GetAsyncKeyState(keys[i])&0x8000);if(pid==GetCurrentProcessId() && down && !held[i])changed|=mgs4vr_menu_key(&menu,keys[i]);held[i]=down;}
  if(!checked && known && mgs4vr_gfx_frame()>=120){
   MGS4VR_CAM_CONFIG c={0};static const unsigned char sig[]=MGS4VR_SIG_CAMERA_BUILDER;BYTE *b=(BYTE *)GetModuleHandleW(NULL);
   c.builder=b+MGS4VR_RVA_CAMERA_BUILDER;c.builder_sig=sig;c.builder_sig_len=sizeof(sig);c.image_base=b;c.require_call_check=1;
   c.adjust_callers[0]=b+MGS4VR_RVA_RET_WRAPPER;c.adjust_callers[1]=b+MGS4VR_RVA_RET_GAMEPLAY;c.adjust_camera=b+MGS4VR_RVA_MAIN_CAMERA;
   armed=mgs4vr_cam_arm(&c,NULL)>0;checked=1;menu.available=armed;changed=1;
  }
  if(changed){configure(&menu);save(&menu);}
  if(armed)mgs4vr_cam_poll();
  expire_pose(GetTickCount());
  if(GetTickCount()-start>1000){mgs4vr_gfx_verify();start=GetTickCount();}
 }
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved){
 (void)reserved;if(reason==DLL_PROCESS_ATTACH){HANDLE thread;DisableThreadLibraryCalls(instance);thread=CreateThread(NULL,0,worker,NULL,0,NULL);if(thread)CloseHandle(thread);}return TRUE;
}
