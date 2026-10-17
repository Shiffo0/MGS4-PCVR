/* Additional CALL observers, private to route 2. Never writes renderer state.
 * Sites stay original (E8) and are armed (CC) only while the capturing thread
 * is inside a selected part (site 0) or a recorded subdraw (site 1). The hot
 * generic registration site therefore costs nothing outside a capture. */
#define SUBMIT_LIMIT 256
#define SUB_CONTEXT_BYTES 0x2a2
#define SUB_POSE_BYTES (64+115*64)
typedef struct {unsigned ok,start,count,vertices,accepted,dropped;unsigned short buffer;BYTE discard;BYTE raw[SUB_CONTEXT_BYTES];} SUB_STATE;
typedef struct {LONG part_sequence;DWORD tid;unsigned topology,requested,start,entered,returned,overflow,armed;ULONG64 context;unsigned view,program,occlusion,depth,flags,pose_ok;BYTE pose[SUB_POSE_BYTES];SUB_STATE before,after;} SUB_RECORD;
static SUB_RECORD submissions[SUBMIT_LIMIT];
static volatile LONG submit_count,submit_busy,submit_active,submit_lost,submit_busy_skips,submit_limit_skips;
static volatile LONG submit_enabled,submit_arms,submit_arm_failures;
static SRWLOCK submit_lock=SRWLOCK_INIT;
#include "mgs4vr_submit_uniform.inl"
static __declspec(thread) LONG submit_part=-1;
static __declspec(thread) SUB_RECORD *submit_current;
static BYTE *submit_sites[2];
static BYTE submit_sigs[2][5];
static int submit_restored[2];
typedef void (__fastcall *DRAW_FN)(unsigned,unsigned,unsigned);
typedef void (__fastcall *SUB_FN)(void *,unsigned short,unsigned short,unsigned short,unsigned,BYTE);
static DRAW_FN submit_draw_native;
static SUB_FN submit_native;
static int submit_patch(int i,char expected,char desired){
 DWORD old,tmp;char before;if(!submit_sites[i] || !VirtualProtect(submit_sites[i],1,PAGE_EXECUTE_READWRITE,&old))return 0;
 before=_InterlockedCompareExchange8((volatile char *)submit_sites[i],desired,expected);
 FlushInstructionCache(GetCurrentProcess(),submit_sites[i],1);VirtualProtect(submit_sites[i],1,old,&tmp);return before==expected;
}
/* 1 = armed CC, 0 = original CALL, -1 = unreadable/foreign bytes. */
static int submit_integrity(int i){
 __try{if(!submit_sites[i] || memcmp(submit_sites[i]+1,submit_sigs[i]+1,4))return -1;return *submit_sites[i]==0xcc?1:(*submit_sites[i]==0xe8?0:-1);}
 __except(EXCEPTION_EXECUTE_HANDLER){return -1;}
}
/* Only the single capture owner (busy lock) arms a site. The lock orders arming
 * against submit_stop, so a late arm can never leave a CC after restoration. */
static int submit_arm(int i){
 int ok=0,wanted;AcquireSRWLockExclusive(&submit_lock);wanted=submit_enabled;if(wanted)ok=submit_patch(i,(char)0xe8,(char)0xcc);ReleaseSRWLockExclusive(&submit_lock);
 if(ok)InterlockedIncrement(&submit_arms);else if(wanted)InterlockedIncrement(&submit_arm_failures);return ok;
}
static void submit_disarm(int i){AcquireSRWLockExclusive(&submit_lock);submit_patch(i,(char)0xcc,(char)0xe8);ReleaseSRWLockExclusive(&submit_lock);}
static void submit_state(SUB_STATE *s,void *p){
 BYTE *b=p;memset(s,0,sizeof(*s));
 __try{memcpy(s->raw,b,SUB_CONTEXT_BYTES);b=s->raw;s->start=*(unsigned *)(b+0x80);s->count=*(unsigned *)(b+0x84);s->vertices=*(unsigned *)(b+0x88);s->buffer=*(unsigned short *)(b+0xa8);s->accepted=*(unsigned *)(b+0x280);s->dropped=*(unsigned *)(b+0x284);s->discard=b[0x2a1];s->ok=1;}
 __except(EXCEPTION_EXECUTE_HANDLER){memset(s,0,sizeof(*s));}
}
static void __fastcall submit_observe(void *ctx,unsigned short view,unsigned short program,unsigned short occlusion,unsigned depth,BYTE flags){
 SUB_RECORD *r=submit_current;InterlockedIncrement(&submit_active);
 /* A second nested submit is explicitly ambiguous; never replace first data. */
 if(r){if(r->entered)r->overflow=1;else{r->entered=1;r->context=(ULONG64)ctx;r->view=view;r->program=program;r->occlusion=occlusion;r->depth=depth;r->flags=flags;submit_state(&r->before,ctx);submit_read_uniforms(&submit_uniforms[r-submissions],ctx);}}
 __try{submit_native(ctx,view,program,occlusion,depth,flags);if(r && !r->overflow){submit_state(&r->after,ctx);r->returned=1;}}
 __finally{InterlockedDecrement(&submit_active);}
}
static void __fastcall submit_draw(unsigned topology,unsigned requested,unsigned start){
 SUB_RECORD *r=NULL,*prior=submit_current;int locked=0,armed=0;InterlockedIncrement(&submit_active);
 /* Null scope suppresses unrelated/reentrant draws, also while saturated. */
 submit_current=NULL;
 if(submit_part>=0 && submit_count<SUBMIT_LIMIT && InterlockedCompareExchange(&submit_busy,1,0)==0){
  locked=1;r=&submissions[submit_count];memset(r,0,sizeof(*r));memset(&submit_uniforms[submit_count],0,sizeof(SUB_UNIFORMS));r->part_sequence=submit_part;r->tid=GetCurrentThreadId();r->topology=topology;r->requested=requested;r->start=start;submit_current=r;
  if(submit_part<LIMIT){DATA *d=&records[submit_part].before;
   __try{if(d->model && d->input){memcpy(r->pose,(BYTE *)d->model+0x20,64);memcpy(r->pose+64,(void *)d->input,115*64);r->pose_ok=1;}}
   __except(EXCEPTION_EXECUTE_HANDLER){memset(r->pose,0,sizeof(r->pose));r->pose_ok=0;}
  }
  armed=r->armed=submit_arm(1);
 }
 else if(submit_part>=0){if(submit_count>=SUBMIT_LIMIT)InterlockedIncrement(&submit_limit_skips);else InterlockedIncrement(&submit_busy_skips);}
 __try{submit_draw_native(topology,requested,start);if(r){if(!submit_enabled || submit_lost)r->overflow=1;InterlockedIncrement(&submit_count);}}
 __finally{if(armed)submit_disarm(1);submit_current=prior;if(locked)InterlockedExchange(&submit_busy,0);InterlockedDecrement(&submit_active);}
}
static void submit_stop(void){
 int i;AcquireSRWLockExclusive(&submit_lock);InterlockedExchange(&submit_enabled,0);
 for(i=0;i<2;++i)if(submit_sites[i]){if(submit_integrity(i)==1)submit_patch(i,(char)0xcc,(char)0xe8);submit_restored[i]=submit_integrity(i)==0;}
 ReleaseSRWLockExclusive(&submit_lock);
}
static int submit_prepare(BYTE *base){
 static const unsigned offsets[2]={0xf1751,0x76e0a1};
 static const unsigned targets[2]={0xcaa40,0x76dcb0};int i;
 for(i=0;i<2;++i){int displacement=(int)(targets[i]-offsets[i]-5);submit_sites[i]=base+offsets[i];submit_sigs[i][0]=0xe8;memcpy(submit_sigs[i]+1,&displacement,4);
  __try{if(memcmp(submit_sites[i],submit_sigs[i],5))return 0;}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
 submit_draw_native=(DRAW_FN)(base+targets[0]);submit_native=(SUB_FN)(base+targets[1]);return 1;
}
/* Nothing is patched here: arming happens per capture. Both sites must be original. */
static int submit_start(void){int i;for(i=0;i<2;++i)if(submit_integrity(i)!=0)return 0;InterlockedExchange(&submit_enabled,1);return 1;}
static int submit_check(void){int i;for(i=0;i<2;++i)if(submit_enabled && submit_integrity(i)<0){InterlockedIncrement(&submit_lost);return 0;}return 1;}
static LONG submit_trap(EXCEPTION_POINTERS *e){int i;
 if(e->ExceptionRecord->ExceptionCode!=EXCEPTION_BREAKPOINT)return EXCEPTION_CONTINUE_SEARCH;
 for(i=0;i<2;++i)if(submit_sites[i] && e->ExceptionRecord->ExceptionAddress==submit_sites[i]){
  __try{*(ULONG64 *)(e->ContextRecord->Rsp-8)=(ULONG64)(submit_sites[i]+5);}__except(EXCEPTION_EXECUTE_HANDLER){return EXCEPTION_CONTINUE_SEARCH;}
  e->ContextRecord->Rsp-=8;e->ContextRecord->Rip=(ULONG64)(i?(void *)submit_observe:(void *)submit_draw);return EXCEPTION_CONTINUE_EXECUTION;
 }return EXCEPTION_CONTINUE_SEARCH;
}
static void submit_dump(const char *exe,FILE *status){
 char path[MAX_PATH],*leaf;FILE *f;LONG i;unsigned j;
 fprintf(status,"submit_version=3 submit_records=%ld submit_integrity_lost=%ld submit_restored0=%d submit_restored1=%d submit_busy_skips=%ld submit_limit_skips=%ld submit_arms=%ld submit_arm_failures=%ld\n",submit_count,submit_lost,submit_restored[0],submit_restored[1],submit_busy_skips,submit_limit_skips,submit_arms,submit_arm_failures);
 strcpy_s(path,sizeof(path),exe);leaf=strrchr(path,'\\');if(!leaf)return;
 sprintf_s(leaf+1,sizeof(path)-(size_t)(leaf+1-path),"mgs4vr_submit_%lu.csv",GetCurrentProcessId());
 if(fopen_s(&f,path,"w"))return;
 fputs("part_sequence,tid,topology,requested,start,entered,returned,ambiguous,armed,pre_ok,post_ok,effective_start,effective_count,vertices,buffer,discard,accepted_before,accepted_after,dropped_before,dropped_after,context,view,program,occlusion,depth,flags,pose_ok,pose_hex,context_pre_hex,context_post_hex\n",f);
 for(i=0;i<submit_count;++i){SUB_RECORD *r=&submissions[i];fprintf(f,"%ld,%lu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%llx,%u,%u,%u,%u,%u,%u,",r->part_sequence,r->tid,r->topology,r->requested,r->start,r->entered,r->returned,r->overflow,r->armed,r->before.ok,r->after.ok,r->before.start,r->before.count,r->before.vertices,r->before.buffer,r->before.discard,r->before.accepted,r->after.accepted,r->before.dropped,r->after.dropped,r->context,r->view,r->program,r->occlusion,r->depth,r->flags,r->pose_ok);
  if(r->pose_ok)for(j=0;j<SUB_POSE_BYTES;++j)fprintf(f,"%02x",r->pose[j]);fputc(',',f);
  if(r->before.ok)for(j=0;j<SUB_CONTEXT_BYTES;++j)fprintf(f,"%02x",r->before.raw[j]);fputc(',',f);
  if(r->after.ok)for(j=0;j<SUB_CONTEXT_BYTES;++j)fprintf(f,"%02x",r->after.raw[j]);fputc('\n',f);
 }
 fprintf(status,"submit_csv_written=%d\n",!ferror(f));fclose(f);submit_dump_uniforms(exe,status);
}
