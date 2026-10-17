/* Explosive shot direction. Inert without marker mgs4vr_rocket.on ("observe" or "on").
   Shot routine 0x97D680 (rcx weapon, rdx owner, r8d shot type, r9 matrix): ebx = type-0x2E
   selects a jump table at 0x97DF9D. Types 0..0x1E (launchers, grenades, rail gun) never reach
   the bullet seam 0x97E880; all of them read the direction at [rsp+50] (also in xmm2) and the
   origin in xmm8 (= [rsp+60]) set before the table. Two routes reach the table: the shot-list
   route (weapon+0x390 > 0, JMP 97DF9D from 97D80B) and the single-shot route via 97DF7E; the
   sim run of 12 Oct proved that a seam at 97DF7E misses the list route. Seam97DFA6: MOVSXD
   RAX,EBX, the first instruction after JA 97E4E0, so it only runs for types 0..0x1E. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include "mgs4vr_muzzle_contract.h"
#include "mgs4vr_rocket.h"
#define ROCKET_RVA 0x97dfa6
#define ROCKET_TYPES 0x1f
static const BYTE rocket_sig[]={0x48,0x63,0xc3,0x0f,0xb6,0x84,0x02,0x4c,0xec,0x97,0x00};
static BYTE *ri,*rs;static PVOID rv;
static volatile LONG r_enabled,r_apply,r_seen,r_bullets,r_eligible,r_applied,r_rejected,r_faulted;
static volatile LONG r_types[ROCKET_TYPES],r_reason[12];
static DWORD r_tick;static int r_attempted,r_owned;static float r_last[16]; /* last applied shot: old dir, old origin, new dir, new origin (xyzw) */
static char r_mark[MAX_PATH],r_status[MAX_PATH];
static int (*r_scope_open)(void);void mgs4vr_rocket_set_gesture(int (*fn)(void)){r_scope_open=fn;} /* launcher at the eye: game aims along its scope view */
static int r_readable(uint64_t a,unsigned n,int write){
 MEMORY_BASIC_INFORMATION m;DWORD p;if(a<0x10000||a+n<a||a+n>=0x800000000000ULL||!VirtualQuery((void*)a,&m,sizeof(m)))return 0;
 p=m.Protect&0xff;if(m.State!=MEM_COMMIT||(m.Protect&PAGE_GUARD)||a+n>(uint64_t)m.BaseAddress+m.RegionSize)return 0;
 if(write)return p==PAGE_READWRITE||p==PAGE_WRITECOPY||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;
 return p==PAGE_READONLY||p==PAGE_READWRITE||p==PAGE_WRITECOPY||p==PAGE_EXECUTE_READ||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;
}
static int r_read(void *unused,uint64_t a,void *out,unsigned n){(void)unused;if(n>64||!r_readable(a,n,0))return 0;__try{memcpy(out,(void*)a,n);return 1;}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
/* Direction: unit barrel axis (row 2) with the native w lane; origin: muzzle locator (row 3), native w lane. */
static int r_vectors(const MC_RESULT *r,const float old[8],float next[8]){
 float length;int i;for(i=0;i<8;++i)if(!isfinite(old[i]))return 0;
 for(i=0;i<16;++i)if(!isfinite(r->native_shot[i]))return 0;
 length=sqrtf(r->native_shot[8]*r->native_shot[8]+r->native_shot[9]*r->native_shot[9]+r->native_shot[10]*r->native_shot[10]);
 if(!isfinite(length)||length<0.5f||length>2.0f)return 0;
 memcpy(next,old,32);for(i=0;i<3;++i){next[i]=r->native_shot[8+i]/length;next[4+i]=r->native_shot[12+i];}return 1;
}
static LONG CALLBACK rocket_trap(EXCEPTION_POINTERS *e){
 CONTEXT *c=e->ContextRecord;float old[8],next[8];MC_RESULT r;uint64_t owner;unsigned reason=10,type;NT_TIB *t=(NT_TIB*)NtCurrentTeb();
 if(e->ExceptionRecord->ExceptionCode!=EXCEPTION_BREAKPOINT||!rs||e->ExceptionRecord->ExceptionAddress!=rs)return EXCEPTION_CONTINUE_SEARCH;
 InterlockedIncrement(&r_seen);

 if((c->Rsp&15)||c->Rsp<(uint64_t)t->StackLimit||c->Rsp+0x70>(uint64_t)t->StackBase||
    c->Rbp+0x7b8<(uint64_t)t->StackLimit||c->Rbp+0x7c0>(uint64_t)t->StackBase||!r_read(0,c->Rsp+0x50,old,32))return EXCEPTION_CONTINUE_SEARCH;
 type=(unsigned)(DWORD)c->Rbx;
 if(type>=ROCKET_TYPES)InterlockedIncrement(&r_bullets);
 else{
  InterlockedIncrement(&r_types[type]);
  if(r_enabled){
   if(r_read(0,c->Rbp+0x7b8,&owner,8)&&mc_resolve_reason(r_read,0,(uint64_t)ri,owner,c->Rsi,&r,&reason)&&(reason=10,r_vectors(&r,old,next))){
    InterlockedIncrement(&r_eligible);
    if(r_apply&&!(r_scope_open&&r_scope_open())&&r_readable(c->Rsp+0x50,32,1)){
     __try{memcpy((void*)(c->Rsp+0x50),next,32);memcpy(&c->Xmm8,next+4,16);memcpy(&c->Xmm2,next,16);memcpy(r_last,old,32);memcpy(r_last+8,next,32);InterlockedIncrement(&r_applied);}
     __except(EXCEPTION_EXECUTE_HANDLER){InterlockedIncrement(&r_faulted);InterlockedExchange(&r_enabled,0);}
    }
   }else{InterlockedIncrement(&r_rejected);InterlockedIncrement(&r_reason[reason<12?reason:11]);}
  }
 }
 c->Rax=(DWORD64)(INT64)(INT32)(DWORD)c->Rbx;c->Rip=(DWORD64)rs+3;return EXCEPTION_CONTINUE_EXECUTION;
}
static int r_state(void){__try{if(memcmp(rs+1,rocket_sig+1,sizeof(rocket_sig)-1))return -1;return rs[0]==0xcc?1:rs[0]==rocket_sig[0]?0:-1;}__except(EXCEPTION_EXECUTE_HANDLER){return -1;}}
static int r_patch(BYTE from,BYTE to){DWORD old,tmp;char v;if(!VirtualProtect(rs,1,PAGE_EXECUTE_READWRITE,&old))return 0;v=_InterlockedCompareExchange8((volatile char*)rs,to,from);FlushInstructionCache(GetCurrentProcess(),rs,1);VirtualProtect(rs,1,old,&tmp);return (BYTE)v==from;}
static void r_report(const char *event){unsigned j;FILE *f;if(fopen_s(&f,r_status,"w"))return;
 fprintf(f,"rocket_version=1 event=%s enabled=%ld mode=%s seen=%ld bullets=%ld eligible=%ld applied=%ld rejected=%ld faults=%ld restored=%d\ntypes",event,r_enabled,r_apply?"on":"observe",r_seen,r_bullets,r_eligible,r_applied,r_rejected,r_faulted,rs?r_state()==0:0);
 for(j=0;j<ROCKET_TYPES;++j)if(r_types[j])fprintf(f," t%u=%ld",j,r_types[j]);fputs("\nreasons",f);for(j=0;j<12;++j)fprintf(f," r%u=%ld",j,r_reason[j]);
 if(r_applied){float d=r_last[0]*r_last[8]+r_last[1]*r_last[9]+r_last[2]*r_last[10],n=sqrtf((r_last[0]*r_last[0]+r_last[1]*r_last[1]+r_last[2]*r_last[2])*(r_last[8]*r_last[8]+r_last[9]*r_last[9]+r_last[10]*r_last[10]));
  fprintf(f,"\nlast native_dir=%.3f,%.3f,%.3f native_origin=%.0f,%.0f,%.0f barrel_dir=%.3f,%.3f,%.3f muzzle=%.0f,%.0f,%.0f angle_deg=%.1f",r_last[0],r_last[1],r_last[2],r_last[4],r_last[5],r_last[6],r_last[8],r_last[9],r_last[10],r_last[12],r_last[13],r_last[14],n>0?acosf(fmaxf(-1.f,fminf(1.f,d/n)))*57.29578f:-1.f);}
 fputc(10,f);fclose(f);}
static int r_mode(void){FILE*f;char b[32]={0};size_t n;if(fopen_s(&f,r_mark,"rb"))return 0;n=fread(b,1,31,f);fclose(f);while(n&&(b[n-1]=='\r'||b[n-1]=='\n'||b[n-1]==' '))b[--n]=0;return !strcmp(b,"on")?2:!strcmp(b,"observe")?1:0;}
void mgs4vr_rocket_poll(void *base,const char *exe){
 DWORD now=GetTickCount();char *leaf;int m;
 if(!r_mark[0]){strcpy_s(r_mark,sizeof(r_mark),exe);leaf=strrchr(r_mark,'\\');if(!leaf){r_mark[0]=0;return;}strcpy_s(leaf+1,sizeof(r_mark)-(size_t)(leaf+1-r_mark),"mgs4vr_rocket.on");strcpy_s(r_status,sizeof(r_status),r_mark);sprintf_s(strrchr(r_status,'\\')+1,64,"mgs4vr_rocket_%lu.txt",GetCurrentProcessId());}
 if(r_attempted){
  if(r_owned&&rs&&r_state()==1&&(!r_enabled||r_mode()!=(r_apply?2:1))){InterlockedExchange(&r_enabled,0);if(r_patch(0xcc,rocket_sig[0]))r_owned=0;r_report(r_state()==0?"stopped":"restore_failed");}
  if(r_enabled&&r_state()!=1){InterlockedExchange(&r_enabled,0);r_report("integrity_lost");}
  if(!r_enabled&&(!rs||r_state()==0)&&GetFileAttributesA(r_mark)==INVALID_FILE_ATTRIBUTES)r_attempted=0;
  if(r_enabled&&now-r_tick>=1000){r_tick=now;r_report("active");}return;
 }
 m=r_mode();if(!m)return;r_attempted=1;ri=(BYTE*)base;rs=ri+ROCKET_RVA;
 if(!r_readable((uint64_t)rs,sizeof(rocket_sig),0)||memcmp(rs,rocket_sig,sizeof(rocket_sig))){r_report("reject_signature");return;}
 if(!rv)rv=AddVectoredExceptionHandler(1,rocket_trap);if(!rv){r_report("reject_veh");return;}
 r_seen=r_bullets=r_eligible=r_applied=r_rejected=r_faulted=0;memset((void*)r_types,0,sizeof(r_types));memset((void*)r_reason,0,sizeof(r_reason));
 r_apply=m==2;r_tick=now;InterlockedExchange(&r_enabled,1);
 if(!r_patch(rocket_sig[0],0xcc)){InterlockedExchange(&r_enabled,0);r_report("reject_patch");return;}r_owned=1;r_report("installed");
}
