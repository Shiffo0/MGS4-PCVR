/* Read the pending uniform interval of THIS encoder, not a global camera. */
#define SUB_UNIFORM_LIMIT 65536
typedef struct {unsigned status,serial,begin,end,capacity,slot,sizes[6];unsigned short projection,combined;ULONG64 frame,buffer;BYTE bytes[SUB_UNIFORM_LIMIT];} SUB_UNIFORMS;
static SUB_UNIFORMS submit_uniforms[SUBMIT_LIMIT];
static void submit_read_uniforms(SUB_UNIFORMS *u,void *ctx){
 BYTE *enc=ctx,*frame,*buffer;void **buffers;unsigned again;
 memset(u,0,sizeof(*u));
 __try{
  frame=*(BYTE **)enc;if(!frame){u->status=3;return;}u->frame=(ULONG64)frame;
  u->serial=*(unsigned *)(frame+0x2503728);u->slot=enc[0x2a0];
  buffers=*(void ***)(frame+0x24d35a0);if(!buffers){u->status=3;return;}
  buffer=buffers[u->slot];if(!buffer){u->status=3;return;}u->buffer=(ULONG64)buffer;
  u->capacity=*(unsigned *)buffer;u->end=*(unsigned *)(buffer+4);u->begin=*(unsigned *)(enc+0x288);
  if(u->capacity>16*1024*1024 || u->begin>u->end || u->end>u->capacity || u->end-u->begin>SUB_UNIFORM_LIMIT){u->status=3;return;}
  memcpy(u->sizes,image+0x1883950,sizeof(u->sizes));u->projection=*(unsigned short *)(image+0x1c509a0);u->combined=*(unsigned short *)(image+0x1c5099c);
  memcpy(u->bytes,buffer+8+u->begin,u->end-u->begin);
  again=*(unsigned *)(buffer+4);
  /* Double-check identity and cursors. This is not an ownership guarantee. */
  u->status=(again==u->end && *(unsigned *)buffer==u->capacity && *(unsigned *)(enc+0x288)==u->begin && enc[0x2a0]==u->slot && *(BYTE **)enc==frame && *(void ***)(frame+0x24d35a0)==buffers && buffers[u->slot]==buffer && *(unsigned *)(frame+0x2503728)==u->serial)?1:4;
 }__except(EXCEPTION_EXECUTE_HANDLER){u->status=2;}
}
static void submit_dump_uniforms(const char *exe,FILE *status){
 char path[MAX_PATH],*leaf;FILE *f;LONG i;unsigned j;
 strcpy_s(path,sizeof(path),exe);leaf=strrchr(path,'\\');if(!leaf)return;
 sprintf_s(leaf+1,sizeof(path)-(size_t)(leaf+1-path),"mgs4vr_submit_uniforms_%lu.csv",GetCurrentProcessId());
 if(fopen_s(&f,path,"w"))return;
 fputs("sequence,status,frame,buffer,serial,slot,begin,end,capacity,projection,combined,size0,size1,size2,size3,size4,size5,hex\n",f);
 for(i=0;i<submit_count;++i){SUB_UNIFORMS *u=&submit_uniforms[i];
  fprintf(f,"%ld,%u,%llx,%llx,%u,%u,%u,%u,%u,%u,%u",i,u->status,u->frame,u->buffer,u->serial,u->slot,u->begin,u->end,u->capacity,u->projection,u->combined);
  for(j=0;j<6;++j)fprintf(f,",%u",u->sizes[j]);fputc(',',f);
  if(u->status==1)for(j=0;j<u->end-u->begin;++j)fprintf(f,"%02x",u->bytes[j]);fputc('\n',f);
 }
 fprintf(status,"submit_uniform_version=1 submit_uniform_csv_written=%d\n",!ferror(f));fclose(f);
}
