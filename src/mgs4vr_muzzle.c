/* Experimental native shot-anchor redirect. Inert without an exact marker.
   Seam97E880: MOVAPS XMM0,[RSP+50], before native direction processing.
   Other weapon branches bypass this site and are deliberately unchanged. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include "mgs4vr_muzzle_contract.h"
#include "mgs4vr_muzzle.h"
#define MUZZLE_RVA 0x97e880
#define MUZZLE_MS 30000u
#define MUZZLE_LIMIT 32
static const BYTE muzzle_sig[]={0x0f,0x28,0x44,0x24,0x50,0x4c,0x8d,0x85,0x90,0x01,0,0};
static BYTE *mi,*ms;static PVOID mv;
static volatile LONG persistent;
static volatile LONG enabled,apply_mode,seen,eligible,applied,rejected,faulted;
static volatile LONG diag[4][12]; /* raw candidate key buckets: other,3,24,25; not validated identity */
static DWORD began,tick;static int attempted,owned;
static SRWLOCK apply_lock=SRWLOCK_INIT;
/* Scope gesture (rifle at the eye): the game shoots along its scope view then, as in MGS3; NULL = never. */
static int (*scope_open)(void);void mgs4vr_muzzle_set_gesture(int (*fn)(void)){scope_open=fn;}static volatile LONG scoped;
static char mark[MAX_PATH],status[MAX_PATH];
static int readable(uint64_t a,unsigned n,int write){
 MEMORY_BASIC_INFORMATION m;DWORD p;if(a<0x10000||a+n<a||a+n>=0x800000000000ULL||!VirtualQuery((void*)a,&m,sizeof(m)))return 0;
 p=m.Protect&0xff;if(m.State!=MEM_COMMIT||(m.Protect&PAGE_GUARD)||a+n>(uint64_t)m.BaseAddress+m.RegionSize)return 0;
 if(write)return p==PAGE_READWRITE||p==PAGE_WRITECOPY||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;
 return p==PAGE_READONLY||p==PAGE_READWRITE||p==PAGE_WRITECOPY||p==PAGE_EXECUTE_READ||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY;
}
static int live_read(void *unused,uint64_t a,void *out,unsigned n){(void)unused;if(n>64||!readable(a,n,0))return 0;__try{memcpy(out,(void*)a,n);return 1;}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
/* Preserve homogeneous lanes supplied by the native path. Normalize only xyz. */
static int prepare_vectors(const MC_RESULT *r,const float old[8],float next[8]){
 float length;int i;for(i=0;i<8;++i)if(!isfinite(old[i]))return 0;
 for(i=0;i<16;++i)if(!isfinite(r->native_shot[i]))return 0;
 length=sqrtf(r->native_shot[8]*r->native_shot[8]+r->native_shot[9]*r->native_shot[9]+r->native_shot[10]*r->native_shot[10]);
 if(!isfinite(length)||length<0.5f||length>2.0f)return 0;
 memcpy(next,old,32);for(i=0;i<3;++i){next[i]=r->native_shot[8+i]/length;next[4+i]=r->native_shot[12+i];}return 1;
}
static LONG CALLBACK muzzle_trap(EXCEPTION_POINTERS *e){
 CONTEXT *c=e->ContextRecord;float old[8],next[8];MC_RESULT r;uint64_t owner;unsigned reason=10,key=0,bucket=0;NT_TIB *t=(NT_TIB*)NtCurrentTeb();
 if(e->ExceptionRecord->ExceptionCode!=EXCEPTION_BREAKPOINT||!ms||e->ExceptionRecord->ExceptionAddress!=ms)return EXCEPTION_CONTINUE_SEARCH;
 InterlockedIncrement(&seen);
 if(live_read(0,c->Rsi+0x70,&key,4))bucket=key==3?1:key==24?2:key==25?3:0;
 InterlockedIncrement(&diag[bucket][11]);
 /* MOVAPS would also fault for a bad/misaligned stack: do not hide that error. */
 if((c->Rsp&15)||c->Rsp<(uint64_t)t->StackLimit||c->Rsp+0x70>(uint64_t)t->StackBase||!live_read(0,c->Rsp+0x50,old,32))return EXCEPTION_CONTINUE_SEARCH;
 if(enabled && (persistent || (GetTickCount()-began<MUZZLE_MS && applied<MUZZLE_LIMIT))){
  if(c->Rbp+0x7b8>=(uint64_t)t->StackLimit&&c->Rbp+0x7c0<=(uint64_t)t->StackBase &&
     live_read(0,c->Rbp+0x7b8,&owner,8)&&mc_resolve_reason(live_read,0,(uint64_t)mi,owner,c->Rsi,&r,&reason)&&(reason=10,prepare_vectors(&r,old,next))){
   InterlockedIncrement(&eligible);InterlockedIncrement(&diag[bucket][0]);
   if(scope_open&&scope_open())InterlockedIncrement(&scoped);
   else if(apply_mode && readable(c->Rsp+0x50,32,1) && TryAcquireSRWLockExclusive(&apply_lock)){
    if(persistent || applied<MUZZLE_LIMIT){
    __try {memcpy((void*)(c->Rsp+0x50),next,32);memcpy(old,next,32);InterlockedIncrement(&applied);}
    __except(EXCEPTION_EXECUTE_HANDLER){InterlockedIncrement(&faulted);InterlockedExchange(&enabled,0);}
    }
    ReleaseSRWLockExclusive(&apply_lock);
   }
  }else {InterlockedIncrement(&rejected);InterlockedIncrement(&diag[bucket][reason]);}
 }
 /* Emulate the original load, including observe/reject/removal-race paths. */
 memcpy(&c->Xmm0,old,16);c->Rip=(DWORD64)ms+5;return EXCEPTION_CONTINUE_EXECUTION;
}
static int state(void){__try{if(memcmp(ms+1,muzzle_sig+1,sizeof(muzzle_sig)-1))return -1;return ms[0]==0xcc?1:ms[0]==muzzle_sig[0]?0:-1;}__except(EXCEPTION_EXECUTE_HANDLER){return -1;}}
static int patch(BYTE from,BYTE to){DWORD old,tmp;char v;if(!VirtualProtect(ms,1,PAGE_EXECUTE_READWRITE,&old))return 0;v=_InterlockedCompareExchange8((volatile char*)ms,to,from);FlushInstructionCache(GetCurrentProcess(),ms,1);VirtualProtect(ms,1,old,&tmp);return (BYTE)v==from;}
static void report(const char *event){unsigned b,j;FILE *f;if(fopen_s(&f,status,"w"))return;fprintf(f,"muzzle_version=1 event=%s enabled=%ld mode=%s seen=%ld eligible=%ld applied=%ld rejected=%ld faults=%ld scoped=%ld restored=%d\n",event,enabled,persistent?"on":apply_mode?"apply":"observe",seen,eligible,applied,rejected,faulted,scoped,ms?state()==0:0);for(b=0;b<4;++b){fprintf(f,"candidate_key=%u",b==1?3:b==2?24:b==3?25:0);for(j=0;j<12;++j)fprintf(f," r%u=%ld",j,diag[b][j]);fputc(10,f);}fclose(f);}
static int mode(void){FILE*f;char b[32]={0};size_t n;if(fopen_s(&f,mark,"rb"))return 0;n=fread(b,1,31,f);fclose(f);while(n&&(b[n-1]=='\r'||b[n-1]=='\n'))b[--n]=0;return !strcmp(b,"on")?3:!strcmp(b,"apply")?2:!strcmp(b,"observe")?1:0;}
void mgs4vr_muzzle_poll(void *base,const char *exe){
 DWORD now=GetTickCount();char *leaf;int m;
 if(!mark[0]){strcpy_s(mark,sizeof(mark),exe);leaf=strrchr(mark,'\\');if(!leaf){mark[0]=0;return;}strcpy_s(leaf+1,sizeof(mark)-(size_t)(leaf+1-mark),"mgs4vr_muzzle.on");strcpy_s(status,sizeof(status),mark);sprintf_s(strrchr(status,'\\')+1,64,"mgs4vr_muzzle_%lu.txt",GetCurrentProcessId());}
 if(attempted){
  if(owned&&ms&&state()==1 && (!enabled||(!persistent && (now-began>=MUZZLE_MS||applied>=MUZZLE_LIMIT))||mode()!=(persistent?3:apply_mode?2:1))){InterlockedExchange(&enabled,0);if(patch(0xcc,muzzle_sig[0]))owned=0;report(state()==0?"stopped":"restore_failed");}
  if(enabled&&state()!=1){InterlockedExchange(&enabled,0);report("integrity_lost");}
  if(!enabled&&(!ms||state()==0)&&GetFileAttributesA(mark)==INVALID_FILE_ATTRIBUTES)attempted=0;
  if(enabled&&now-tick>=1000){tick=now;report("active");}return;
 }
 m=mode();if(!m)return;attempted=1;mi=(BYTE*)base;ms=mi+MUZZLE_RVA;
 if(!readable((uint64_t)ms,sizeof(muzzle_sig),0)||memcmp(ms,muzzle_sig,sizeof(muzzle_sig))){report("reject_signature");return;}
 if(!mv)mv=AddVectoredExceptionHandler(1,muzzle_trap);if(!mv){report("reject_veh");return;}
 seen=eligible=applied=rejected=faulted=0;persistent=m==3;apply_mode=m>=2;began=tick=now;InterlockedExchange(&enabled,1);
 if(!patch(muzzle_sig[0],0xcc)){InterlockedExchange(&enabled,0);report("reject_patch");return;}owned=1;report("installed");
}
