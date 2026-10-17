/* Observations BEFORE native bone processing and later part classification.
 * Stable double-read is not exclusive ownership or current draw output. */
#define MASK_PARTS 32
#define MASK_BONES 64
typedef struct {unsigned id,state,bone_count;BYTE bones[MASK_BONES];} MASK_PART;
typedef struct {ULONG64 model,parts,mask;unsigned view,flags78,flagsc0,model_state,part_count;MASK_PART part[MASK_PARTS];} MASK_DATA;
typedef struct {unsigned status;MASK_DATA data;} MASK_OBS;
static MASK_OBS mask_samples[LIMIT][2];
static MASK_DATA mask_again;
static unsigned mask_read(MASK_DATA *d,BYTE *context,BYTE *model){
 unsigned i;memset(d,0,sizeof(*d));
 __try{
  d->model=(ULONG64)model;d->view=*(unsigned *)(context+0x88);
  /* Only the two view slots observed by the existing route are supported. */
  if(d->view>1)return 3;
  d->mask=*(ULONG64 *)(model+0x80+8*d->view);
  d->flags78=*(unsigned *)(model+0x78);d->flagsc0=*(unsigned *)(model+0xc0);
  d->model_state=*(unsigned *)(model+0xdc);d->part_count=*(unsigned *)(model+0xc8);
  d->parts=*(ULONG64 *)(model+0xf0);
  if(d->part_count>MASK_PARTS || (d->part_count && !d->parts))return 3;
  for(i=0;i<d->part_count;++i){
   BYTE *p=(BYTE *)d->parts+i*0x1d0;MASK_PART *v=&d->part[i];
   v->id=*(unsigned *)(p+8);v->state=*(unsigned *)(p+0x20);v->bone_count=*(unsigned *)(p+0x14);
   if(v->bone_count>MASK_BONES)return 3;
   memcpy(v->bones,p+0x30,v->bone_count);
  }
  return 1;
 }__except(EXCEPTION_EXECUTE_HANDLER){return 2;}
}
static void mask_capture_one(MASK_OBS *o,BYTE *context,BYTE *model){
 unsigned second;o->status=mask_read(&o->data,context,model);
 if(o->status!=1)return;
 second=mask_read(&mask_again,context,model);
 if(second!=1)o->status=second;
 else if(memcmp(&o->data,&mask_again,sizeof(mask_again)))o->status=4;
}
static void mask_capture(SAMPLE *s){
 MASK_OBS *o=mask_samples[s->sequence];
 mask_capture_one(&o[0],(BYTE *)s->render_context,(BYTE *)s->model);
 mask_capture_one(&o[1],(BYTE *)s->render_context,(BYTE *)s->owner_model);
}
static void mask_dump(FILE *f,LONG n){
 LONG i;unsigned role,j,k;
 fputs("sequence,role,bone_sample_valid,status,view,model,mask,flags78,flagsc0,model_state,parts,part_count,part_index,part_id,part_state,bone_count,bone_indices\n",f);
 for(i=0;i<n;++i)for(role=0;role<2;++role){
  MASK_OBS *o=&mask_samples[i][role];MASK_DATA *d=&o->data;
  unsigned rows=o->status==1 && d->part_count?d->part_count:1;
  for(j=0;j<rows;++j){
   fprintf(f,"%ld,%u,%u,%u,%u,%llx,%llx,%x,%x,%x,%llx,%u,",i,role,samples[i].valid,o->status,d->view,d->model,d->mask,d->flags78,d->flagsc0,d->model_state,d->parts,d->part_count);
   if(o->status==1 && d->part_count){MASK_PART *v=&d->part[j];fprintf(f,"%u,%u,%x,%u,",j,v->id,v->state,v->bone_count);for(k=0;k<v->bone_count;++k)fprintf(f,"%02x",v->bones[k]);}
   else fputs("-1,0,0,0,",f);
   fputc('\n',f);
  }
 }
}
