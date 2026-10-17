/* Private read-only part submission observer. No geometry or pose writes. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>
#ifdef PART_SUBMIT
#define LIMIT 64
#include "mgs4vr_part_phases.inl"
#define CAPTURE_LIMIT (phase_mode?64:32)
#else
#define LIMIT 32
#define CAPTURE_LIMIT 32
#endif
#ifndef PART_RIG_HASH
#define PART_RIG_HASH 0x7a53058b96cc2a6eull
#endif
#ifndef CALL_RVA
#define CALL_RVA 0xc189e
#endif
#ifndef FN_RVA
#define FN_RVA 0xc1c20
#endif
#ifndef PART_ARGS
#define PART_ARGS void *part
#define PART_FORWARD part
#define PART_ARITY 1
#endif
#ifndef PART_PREFIX
#define PART_PREFIX "mgs4vr_parts"
#define PART_POLL mgs4vr_parts_poll
#define PART_ACTIVE mgs4vr_parts_active
#define PART_ROUTE -1
#endif
typedef void (__fastcall *NATIVE)(PART_ARGS);
static BYTE *image,*site,*control_site;static NATIVE native;static void *veh;
static volatile LONG enabled,active,busy,count,calls,rejected,faults,skipped,control_hits,trap_hits;
static int attempted,installed,selftest_ok,restore_owned,restore_verified,terminal;
static DWORD started;static unsigned frame_start,frame_last,integrity_checks,integrity_lost;
static FILE *stream;
typedef struct {ULONG64 part,model,owner,wrapper,input,metadata,parts,resource,draws;unsigned slot,index,parts_count,bones_count,draw_count,id,state;BYTE bones[64];} DATA;
#define DRAW_LIMIT 32
typedef struct {unsigned status,count;BYTE bytes[DRAW_LIMIT*16];} DRAW_SNAPSHOT;
/* 1=readable,2=fault,3=unsupported length/pointer. No allocation or writes.
 * Separate from association validity; readable lists are not proof of draws. */
static void read_draws(DRAW_SNAPSHOT *out,const DATA *d){
 ULONG64 bytes=(ULONG64)d->draw_count*16;memset(out,0,sizeof(*out));out->count=d->draw_count;
 if(d->draw_count>DRAW_LIMIT || (bytes && (!d->draws || d->draws>=0x800000000000ull || bytes>0x800000000000ull-d->draws))){out->status=3;return;}
 __try{if(bytes)memcpy(out->bytes,(void *)d->draws,(size_t)bytes);out->status=1;}
 __except(EXCEPTION_EXECUTE_HANDLER){memset(out->bytes,0,sizeof(out->bytes));out->status=2;}
}
typedef struct {DATA before,after;DRAW_SNAPSHOT draws_before,draws_after;unsigned draws_stable,phase;LONGLONG begin,end;DWORD tid;unsigned returned,post_ok,stable,overlap;} RECORD;
static RECORD records[LIMIT];
#ifdef PART_SUBMIT
#include "mgs4vr_submit.inl"
#endif
static unsigned long long rig_hash(BYTE *p){unsigned i;unsigned long long h=14695981039346656037ull;for(i=0;i<115*80;++i){h^=p[i];h*=1099511628211ull;}return h;}
/* This validates association at two instants, not exclusive buffer ownership. */
static int snapshot(DATA *d,void *part){
 BYTE *p=part,*m,*o,*w,*list;ULONG64 offset;int slot;unsigned n;
 memset(d,0,sizeof(*d));
 __try{
  if(!p)return 0;
  slot=*(int *)(image+0x23eb5150);if(slot < -1 || slot>23)return 0;
  w=*(BYTE **)(image+0x23eb5160+(slot+1)*0x80+0x10);if(!w)return 0;o=*(BYTE **)w;if(!o)return 0;
  m=*(BYTE **)(p+0x80);if(!m)return 0;
  if(*(unsigned *)(m+0xcc)!=115 || *(unsigned *)(o+0xcc)!=115)return 0;
  d->input=*(ULONG64 *)(m+0x100);d->metadata=*(ULONG64 *)(m+0xf8);
  if(!d->input || !d->metadata || d->input!=*(ULONG64 *)(o+0x100) || d->metadata!=*(ULONG64 *)(o+0xf8))return 0;
  /* Known rig, including parent metadata; refuses unrelated 115-bone models. */
  if(rig_hash((BYTE *)d->metadata)!=PART_RIG_HASH)return 0;
  n=*(unsigned *)(m+0xc8);list=*(BYTE **)(m+0xf0);
  if(!list || !n || n>128 || (ULONG64)p<(ULONG64)list)return 0;
  offset=(ULONG64)p-(ULONG64)list;if(offset%0x1d0 || offset/0x1d0>=n)return 0;
  d->part=(ULONG64)p;d->model=(ULONG64)m;d->owner=(ULONG64)o;d->wrapper=(ULONG64)w;d->slot=slot;
  d->parts=(ULONG64)list;d->parts_count=n;d->index=(unsigned)(offset/0x1d0);
  d->id=*(unsigned *)(p+8);d->state=*(unsigned *)(p+0x20);
  d->bones_count=*(unsigned *)(p+0x14);d->draw_count=*(unsigned *)(p+0x10);
  if(d->bones_count>64 || d->draw_count>4096)return 0;
  memcpy(d->bones,p+0x30,d->bones_count);
  d->resource=*(ULONG64 *)(p+0x58);d->draws=*(ULONG64 *)(p+0x60);
  return 1;
 }__except(EXCEPTION_EXECUTE_HANDLER){InterlockedIncrement(&faults);return 0;}
}
static LONGLONG tick(void){LARGE_INTEGER t;QueryPerformanceCounter(&t);return t.QuadPart;}
static void __fastcall observe(PART_ARGS){
 RECORD *r=NULL;int locked=0,pre=0;
#ifdef PART_SUBMIT
 int armed=0;LONG prior_part=submit_part;SUB_RECORD *prior_submit=submit_current;submit_part=-1;submit_current=NULL;
#endif
 InterlockedIncrement(&calls);InterlockedIncrement(&active);
 if(enabled && count<CAPTURE_LIMIT
#ifdef PART_SUBMIT
 && phase_allows()
#endif
 && InterlockedCompareExchange(&busy,1,0)==0){
  locked=1;if(count<CAPTURE_LIMIT){r=&records[count];memset(r,0,sizeof(*r));r->tid=GetCurrentThreadId();r->begin=tick();pre=snapshot(&r->before,part);if(pre)read_draws(&r->draws_before,&r->before);if(!pre)InterlockedIncrement(&rejected);}
 }else if(enabled && count<CAPTURE_LIMIT)InterlockedIncrement(&skipped);

#ifdef PART_SUBMIT
 if(pre){r->phase=(unsigned)phase_gate;if(!phase_allows())pre=0;}
 if(pre && r->before.index>=9 && r->before.index<=13){submit_part=count;armed=submit_arm(0);}
#endif
 __try{
  native(PART_FORWARD);
  if(pre){r->returned=1;r->post_ok=snapshot(&r->after,part);r->stable=r->post_ok && !memcmp(&r->before,&r->after,sizeof(DATA));if(r->post_ok)read_draws(&r->draws_after,&r->after);r->draws_stable=r->stable && r->draws_before.status==1 && r->draws_after.status==1 && !memcmp(&r->draws_before,&r->draws_after,sizeof(DRAW_SNAPSHOT));r->overlap=active>1;r->end=tick();InterlockedIncrement(&count);}
 }__finally{
#ifdef PART_SUBMIT
 if(armed)submit_disarm(0);
 if(pre && r->returned && r->phase>=1 && r->phase<=2)InterlockedIncrement(&phase_counts[r->phase]);
 submit_part=prior_part;submit_current=prior_submit;
#endif
 if(locked)InterlockedExchange(&busy,0);InterlockedDecrement(&active);}
}
#ifndef PART_CALL_SIG
#define PART_CALL_SIG 0xe8,0x7d,0x3,0x0,0x0
#endif
static const BYTE call_sig[]={PART_CALL_SIG};
#ifndef PART_FN_SIG
#define PART_FN_SIG 0x48,0x89,0x5c,0x24,0x8,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20
#endif
static const BYTE fn_sig[]={PART_FN_SIG};
static const BYTE accessor_sig[]={0x8b,0x5,0xca,0xaf,0x5d,0x23,0x48,0x8d,0xd,0xd3,0xaf,0x5d,0x23,0xff,0xc0,0x48,0x98,0x48,0xc1,0xe0,0x7,0x48,0x3,0xc1,0xc3};
static LONG CALLBACK trap(EXCEPTION_POINTERS *e){
#ifdef PART_SUBMIT
 if(submit_trap(e)==EXCEPTION_CONTINUE_EXECUTION)return EXCEPTION_CONTINUE_EXECUTION;
#endif
 if(e->ExceptionRecord->ExceptionCode==EXCEPTION_BREAKPOINT && control_site && e->ExceptionRecord->ExceptionAddress==control_site){
  InterlockedIncrement(&control_hits);e->ContextRecord->Rip=(ULONG64)(control_site+1);return EXCEPTION_CONTINUE_EXECUTION;
 }
 if(e->ExceptionRecord->ExceptionCode!=EXCEPTION_BREAKPOINT || e->ExceptionRecord->ExceptionAddress!=site)return EXCEPTION_CONTINUE_SEARCH;
 InterlockedIncrement(&trap_hits);
 /* Retain handler for pending traps after the single-byte restoration. */
 __try{*(ULONG64 *)(e->ContextRecord->Rsp-8)=(ULONG64)(site+5);}
 __except(EXCEPTION_EXECUTE_HANDLER){return EXCEPTION_CONTINUE_SEARCH;}
 e->ContextRecord->Rsp-=8;e->ContextRecord->Rip=(ULONG64)observe;return EXCEPTION_CONTINUE_EXECUTION;
}
static int signatures(BYTE *b){
 __try{return !memcmp(b+CALL_RVA,call_sig,sizeof(call_sig)) && !memcmp(b+FN_RVA,fn_sig,sizeof(fn_sig)) && !memcmp(b+0x8da180,accessor_sig,sizeof(accessor_sig));}
 __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
static int patch(char expected,char desired){
 DWORD old,tmp;char prior;if(!VirtualProtect(site,1,PAGE_EXECUTE_READWRITE,&old))return 0;
 prior=_InterlockedCompareExchange8((volatile char *)site,desired,expected);
 FlushInstructionCache(GetCurrentProcess(),site,1);VirtualProtect(site,1,old,&tmp);return prior==expected;
}
/* Positive control exercises this handler in the actual process, without
 * calling any game routine. The page lives only for the synchronous call. */
static int control(void){
 BYTE *code;DWORD old;LONG before=control_hits;int returned=0;
 code=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!code)return 0;
 code[0]=0xcc;code[1]=0xc3;
 if(!VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old)){VirtualFree(code,0,MEM_RELEASE);return 0;}
 FlushInstructionCache(GetCurrentProcess(),code,2);control_site=code;MemoryBarrier();
 __try{((void (*)(void))code)();returned=1;}
 __except(GetExceptionCode()==EXCEPTION_BREAKPOINT?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){returned=0;}
 control_site=NULL;VirtualFree(code,0,MEM_RELEASE);return returned && control_hits==before+1;
}
/* 1 = complete trapped CALL, 0 = original CALL, -1 = unreadable/foreign.
 * Checking only opcode would miss a modified CALL displacement. */
static int integrity(void){
 __try{if(memcmp(site+1,call_sig+1,4))return -1;if(*site==0xcc)return 1;if(*site==0xe8)return 0;return -1;}
 __except(EXCEPTION_EXECUTE_HANDLER){return -1;}
}
static void stop(void){
 int state;InterlockedExchange(&enabled,0);
#ifdef PART_SUBMIT
 submit_stop();
#endif
 if(!installed)return;
 state=integrity();if(state==1)restore_owned=patch((char)0xcc,(char)0xe8);
 restore_verified=integrity()==0;installed=0;terminal=1;
}
static void dump(FILE *f){
 LONG i;unsigned k;fputs("sequence,tid,part,model,owner,wrapper,slot,index,id,state,post_state,part_count,bone_count,bones,draw_count,resource,draws,returned,post_ok,snapshot_stable,active_at_exit_gt1,begin,end,draw_status,draw_post_status,post_draw_count,draw_stable,draw_hex,post_draw_hex,phase\n",f);
 for(i=0;i<count;++i){RECORD *r=&records[i];DATA *d=&r->before;
  fprintf(f,"%ld,%lu,%llx,%llx,%llx,%llx,%u,%u,%u,%x,%x,%u,%u,",i,r->tid,d->part,d->model,d->owner,d->wrapper,d->slot,d->index,d->id,d->state,r->after.state,d->parts_count,d->bones_count);
  for(k=0;k<d->bones_count;++k)fprintf(f,"%02x",d->bones[k]);
  fprintf(f,",%u,%llx,%llx,%u,%u,%u,%u,%lld,%lld,",d->draw_count,d->resource,d->draws,r->returned,r->post_ok,r->stable,r->overlap,r->begin,r->end);
  fprintf(f,"%u,%u,%u,%u,",r->draws_before.status,r->draws_after.status,r->draws_after.count,r->draws_stable);
  if(r->draws_before.status==1)for(k=0;k<r->draws_before.count*16;++k)fprintf(f,"%02x",r->draws_before.bytes[k]);
  fputc(',',f);
  if(r->draws_after.status==1)for(k=0;k<r->draws_after.count*16;++k)fprintf(f,"%02x",r->draws_after.bytes[k]);
  fprintf(f,",%u\n",r->phase);
 }
}
void PART_POLL(void *base,const char *exe,unsigned frame){
 char path[MAX_PATH],*leaf;
 if(!attempted){
  strcpy_s(path,sizeof(path),exe);leaf=strrchr(path,'\\');if(!leaf)return;
  strcpy_s(leaf+1,sizeof(path)-(size_t)(leaf+1-path),"mgs4vr_parts_probe.on");
  if(GetFileAttributesA(path)==INVALID_FILE_ATTRIBUTES)return;
  /* Do not run two trap probes concurrently. */
  strcpy_s(leaf+1,sizeof(path)-(size_t)(leaf+1-path),"mgs4vr_arms_probe.on");
  if(GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES)return;
  attempted=1;image=base;site=image+CALL_RVA;native=(NATIVE)(image+FN_RVA);
  sprintf_s(leaf+1,sizeof(path)-(size_t)(leaf+1-path),PART_PREFIX "_%lu.txt",GetCurrentProcessId());
  if(fopen_s(&stream,path,"w") || !stream)return;
  if(!signatures(image)){fputs("REJECT signature\n",stream);fclose(stream);stream=NULL;return;}

#ifdef PART_SUBMIT
  if(!phase_prepare(exe)){fputs("REJECT phase marker\n",stream);fclose(stream);stream=NULL;return;}
  if(!submit_prepare(image)){fputs("REJECT submit signatures\n",stream);fclose(stream);stream=NULL;return;}
#endif
  veh=AddVectoredExceptionHandler(1,trap);
  if(!veh){fputs("REJECT VEH\n",stream);fclose(stream);stream=NULL;return;}
  selftest_ok=control();
  if(!selftest_ok){fputs("REJECT positive_control\n",stream);fclose(stream);stream=NULL;return;}

#ifdef PART_SUBMIT
  if(!submit_start()){fputs("REJECT submit patch\n",stream);fclose(stream);stream=NULL;return;}
#endif
  frame_start=frame_last=frame;InterlockedExchange(&enabled,1);installed=patch((char)0xe8,(char)0xcc);started=GetTickCount();
  if(!installed){
#ifdef PART_SUBMIT
 submit_stop();
#endif
 InterlockedExchange(&enabled,0);fputs("REJECT patch\n",stream);fclose(stream);stream=NULL;return;}
 }

#ifdef PART_SUBMIT
 if(enabled && !phase_update())stop();
 phase_progress();
 if(enabled && !submit_check()){++integrity_lost;stop();}
#endif
 if(enabled){frame_last=frame;++integrity_checks;if(integrity()!=1){++integrity_lost;stop();}}
 if(enabled && (count>=LIMIT || GetTickCount()-started>=
#ifdef PART_SUBMIT
 (phase_mode?120000u:3000u) || (!phase_mode && count>=32)
#else
 3000u
#endif
 )){
#ifdef PART_SUBMIT
  /* Time limit before a completed pair is a visible phase error. */
  if(phase_mode && phase_state!=4 && !phase_error)phase_error=6;
#endif
  stop();}
 if(stream && !enabled && !installed && !active
#ifdef PART_SUBMIT
 && !submit_active
#endif
 ){
  FILE *csv;LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
  fprintf(stream,"route_id=%d arity=%d draw_snapshot_version=1 draw_limit=%u\n",PART_ROUTE,PART_ARITY,DRAW_LIMIT);
  fprintf(stream,"part_probe=1 call_rva=%u function_rva=%u records=%ld calls=%ld rejected=%ld read_faults=%ld skipped_busy=%ld positive_control=%d traps=%ld integrity_checks=%u integrity_lost=%u restore_owned=%d restored=%d elapsed_ms=%lu frame_delta=%u qpc_frequency=%lld\n",CALL_RVA,FN_RVA,count,calls,rejected,faults,skipped,selftest_ok,trap_hits,integrity_checks,integrity_lost,restore_owned,restore_verified,GetTickCount()-started,frame_last-frame_start,frequency.QuadPart);
  strcpy_s(path,sizeof(path),exe);leaf=strrchr(path,'\\');
  if(leaf){sprintf_s(leaf+1,sizeof(path)-(size_t)(leaf+1-path),PART_PREFIX "_%lu.csv",GetCurrentProcessId());
   if(!fopen_s(&csv,path,"w") && csv){dump(csv);fprintf(stream,"csv_written=%d\n",!ferror(csv));fclose(csv);}else fputs("csv_written=0\n",stream);}

#ifdef PART_SUBMIT
  fprintf(stream,"phase_version=1 phase_mode=%d phase_state=%d phase_error=%d phase_tp=%ld phase_fps=%ld capture_limit=%u\n",phase_mode,phase_state,phase_error,phase_counts[1],phase_counts[2],phase_mode?64u:32u);
  submit_dump(exe,stream);
#endif
  fclose(stream);stream=NULL;
 }
 /* VEH and native retained for pending traps; no hot unload. */
}

int PART_ACTIVE(void){return installed || active
#ifdef PART_SUBMIT
 || submit_active
#endif
 ;}

