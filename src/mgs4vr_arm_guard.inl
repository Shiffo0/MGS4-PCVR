/* Bounded try-lock registration for this observed callsite only.
 * Range intersection is conservative, including read/read sharing.
 * Unknown keys, contention and exhaustion poison overlapping observations.
 * No game synchronization or permission to mutate poses is implied. */
#define GUARD_SLOTS 64
typedef struct {ULONG64 begin,end;} ARM_RANGE;
typedef struct {ARM_RANGE range[7];} ARM_KEY;
typedef struct {ARM_KEY key;LONG epoch;int conflict;volatile LONG state;} ARM_GUARD;
static ARM_GUARD guards[GUARD_SLOTS];
static SRWLOCK guard_lock=SRWLOCK_INIT;
#define GUARD_LOCK_ATTEMPTS 64
static volatile LONG guard_retry_recovered[2],guard_retry_exhausted[2];
/* No Sleep, scheduler yield or blocking lock acquisition. A finite retry
 * budget can still add latency and needs live measurement. Existing slots
 * remain published throughout close retries. Exhaustion stays unknown. */
static int guard_acquire(int closing){
 int attempt;
 for(attempt=0;attempt<GUARD_LOCK_ATTEMPTS;++attempt){
  if(TryAcquireSRWLockExclusive(&guard_lock)){
   if(attempt)InterlockedIncrement(&guard_retry_recovered[closing]);
   return 1;
  }
#ifdef ARM_GUARD_RETRY_TEST
  ARM_GUARD_RETRY_TEST(attempt+1);
#endif
  if(attempt+1<GUARD_LOCK_ATTEMPTS)YieldProcessor();
 }
 InterlockedIncrement(&guard_retry_exhausted[closing]);return 0;
}
static volatile LONG guard_epoch,guard_unknown_active,guard_unknown_total,guard_conflicts;
enum {ARM_KEY_OK,ARM_KEY_ARGS,ARM_KEY_COUNT,ARM_KEY_RANGE,ARM_KEY_READ,ARM_KEY_REASONS};
enum {OVER_SHARED=1,OVER_UNTRACKED=2,OVER_CLOSE_LOCK=4,OVER_CHANGED=8,OVER_EPOCH=16,OVER_UNKNOWN_ACTIVE=32,OVER_END_KEY=64};
static volatile LONG guard_enter_key[ARM_KEY_REASONS],guard_exit_key[ARM_KEY_REASONS];
static volatile LONG guard_enter_lock,guard_exit_lock,guard_full,guard_changed;
static int arm_range(ARM_RANGE *r,ULONG64 address,ULONG64 bytes){
 if(!address || !bytes || address>=0x800000000000ull || bytes>0x800000000000ull-address)return 0;
 r->begin=address;r->end=address+bytes;return 1;
}
static int arm_key_status(ARM_KEY *k,void *context,void *model,int index){
 BYTE *m=model;unsigned n;ULONG64 c=(ULONG64)context;
 memset(k,0,sizeof(*k));
 __try{
  if(!m || !context || index<0 || index>1)return ARM_KEY_ARGS;
  n=*(unsigned *)(m+0xcc);if(!n || n>256)return ARM_KEY_COUNT;
  if(c>=0x800000000000ull-0x10000)return ARM_KEY_RANGE;
  return (arm_range(&k->range[0],(ULONG64)m,0x1b0) &&
   arm_range(&k->range[1],*(ULONG64 *)(m+0x100),(ULONG64)n*64) &&
   arm_range(&k->range[2],*(ULONG64 *)(m+0xf8),(ULONG64)n*80) &&
   arm_range(&k->range[3],*(ULONG64 *)(m+0x108+8*index),(ULONG64)n*64) &&
   arm_range(&k->range[4],c+0x230,0x80) &&
   arm_range(&k->range[5],c+0x1540,(ULONG64)n*80) &&
   arm_range(&k->range[6],c+0x6980,(ULONG64)n*64))?ARM_KEY_OK:ARM_KEY_RANGE;
 }__except(EXCEPTION_EXECUTE_HANDLER){return ARM_KEY_READ;}
}
static int arm_key(ARM_KEY *k,void *context,void *model,int index){return arm_key_status(k,context,model,index)==ARM_KEY_OK;}
static int arm_intersects(const ARM_KEY *a,const ARM_KEY *b){
 int i,j;for(i=0;i<7;++i)for(j=0;j<7;++j)
  if(a->range[i].begin<b->range[j].end && b->range[j].begin<a->range[i].end)return 1;
 return 0;
}
static int guard_unknown(void){
 InterlockedIncrement(&guard_unknown_active);InterlockedIncrement(&guard_epoch);InterlockedIncrement(&guard_unknown_total);return -1;
}
static int guard_enter(void *context,void *model,int index){
 ARM_KEY key;int i,slot=-1,why=arm_key_status(&key,context,model,index);
 if(why!=ARM_KEY_OK){InterlockedIncrement(&guard_enter_key[why]);return guard_unknown();}
 if(!guard_acquire(0)){InterlockedIncrement(&guard_enter_lock);return guard_unknown();}
 for(i=0;i<GUARD_SLOTS;++i){if(guards[i].state!=1){guards[i].state=0;if(slot<0)slot=i;}}
 if(slot<0){InterlockedIncrement(&guard_full);guard_unknown();ReleaseSRWLockExclusive(&guard_lock);return -1;}
 guards[slot].key=key;guards[slot].epoch=guard_epoch;guards[slot].conflict=guard_unknown_active?OVER_UNKNOWN_ACTIVE:0;
 for(i=0;i<GUARD_SLOTS;++i)if(guards[i].state==1 && arm_intersects(&key,&guards[i].key)){
  guards[slot].conflict|=OVER_SHARED;guards[i].conflict|=OVER_SHARED;InterlockedIncrement(&guard_conflicts);
 }
 InterlockedExchange(&guards[slot].state,1);ReleaseSRWLockExclusive(&guard_lock);return slot;
}
/* Returns conflict/uncertainty. Only a lock-protected close can accept.
 * Retired state2 can be reclaimed by the next entrant; no field access after
 * publishing it. Only bounded try-lock retries, no allocations in this path. */
static int guard_leave(int slot,void *context,void *model,int index){
 ARM_KEY end;int why,bad,changed;
 if(slot<0){InterlockedDecrement(&guard_unknown_active);return OVER_UNTRACKED;}
 why=arm_key_status(&end,context,model,index);
 if(!guard_acquire(1)){
  InterlockedIncrement(&guard_exit_lock);InterlockedIncrement(&guard_epoch);InterlockedIncrement(&guard_unknown_total);
  InterlockedExchange(&guards[slot].state,2);return OVER_CLOSE_LOCK;
 }
 changed=memcmp(&end,&guards[slot].key,sizeof(end))!=0;
 bad=guards[slot].conflict;
 if(guards[slot].epoch!=guard_epoch)bad|=OVER_EPOCH;
 if(guard_unknown_active)bad|=OVER_UNKNOWN_ACTIVE;
 if(why!=ARM_KEY_OK || changed){
  if(why!=ARM_KEY_OK){InterlockedIncrement(&guard_exit_key[why]);bad|=OVER_END_KEY;}
  else{InterlockedIncrement(&guard_changed);bad|=OVER_CHANGED;}
  InterlockedIncrement(&guard_epoch);InterlockedIncrement(&guard_unknown_total);
 }
 InterlockedExchange(&guards[slot].state,0);ReleaseSRWLockExclusive(&guard_lock);return bad;
}

