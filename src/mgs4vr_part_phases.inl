/* Optional operator-labelled two-phase capture. Never detects game mode. */
#define PHASE_MISS_LIMIT 10
static int phase_mode,phase_state,phase_error,phase_misses;
static volatile LONG phase_gate,phase_counts[3];
static DWORD phase_poll_at;
static char phase_path[MAX_PATH];
static int phase_published=-1;
/* Republished on every state OR error change, so an abort is visible too. */
static void phase_progress(void){
 char path[MAX_PATH],*leaf;FILE *f;int key=phase_state*16+phase_error;
 if(!phase_mode || phase_published==key)return;
 strcpy_s(path,sizeof(path),phase_path);leaf=strrchr(path,'\\');if(!leaf)return;
 sprintf_s(leaf+1,sizeof(path)-(size_t)(leaf+1-path),"mgs4vr_parts_phase_progress_%lu.txt",GetCurrentProcessId());
 if(fopen_s(&f,path,"w"))return;
 fprintf(f,"state=%d third_person=%ld fps=%ld error=%d\n",phase_state,phase_counts[1],phase_counts[2],phase_error);
 if(!ferror(f))phase_published=key;fclose(f);
}
/* 1/0/2 = label, -1 = invalid content, -2 = missing/empty (possibly mid-write).
 * Accepts a UTF-8 BOM and trailing whitespace; UTF-16 stays invalid. */
static int phase_read(void){
 FILE *f;char value[32]={0},*p=value;size_t n;
 if(fopen_s(&f,phase_path,"rb"))return -2;
 n=fread(value,1,sizeof(value)-1,f);fclose(f);
 if(n==sizeof(value)-1)return -1;
 if(n>=3 && !memcmp(p,"\xef\xbb\xbf",3)){p+=3;n-=3;}
 while(n && (p[n-1]=='\n'||p[n-1]=='\r'||p[n-1]==' '||p[n-1]=='\t'))p[--n]=0;
 if(!n)return -2;
 if(strlen(p)!=n)return -1;
 if(!strcmp(p,"third_person"))return 1;
 if(!strcmp(p,"pause"))return 0;
 if(!strcmp(p,"fps"))return 2;
 return -1;
}
static int phase_prepare(const char *exe){
 char *leaf;strcpy_s(phase_path,sizeof(phase_path),exe);leaf=strrchr(phase_path,'\\');if(!leaf)return 0;
 strcpy_s(leaf+1,sizeof(phase_path)-(size_t)(leaf+1-phase_path),"mgs4vr_parts_phases.on");
 if(GetFileAttributesA(phase_path)==INVALID_FILE_ATTRIBUTES)return 1;
 phase_mode=1;if(phase_read()!=1){phase_error=1;phase_progress();return 0;}
 phase_state=1;InterlockedExchange(&phase_gate,1);return 1;
}
static int phase_allows(void){LONG gate=phase_gate;return !phase_mode || (gate>=1 && gate<=2 && phase_counts[gate]<32);}
/* Called by worker. Invalid/early/backward transition ends capture. A missing
 * or empty label is retried for PHASE_MISS_LIMIT polls (about one second). */
static int phase_update(void){
 int label;DWORD now=GetTickCount();
 if(!phase_mode)return 1;
 if(phase_counts[2]>=32){InterlockedExchange(&phase_gate,0);phase_state=4;return 0;}
 if(now-phase_poll_at<100)return 1;phase_poll_at=now;label=phase_read();
 if(label==-2){if(++phase_misses<PHASE_MISS_LIMIT)return 1;phase_error=5;InterlockedExchange(&phase_gate,0);return 0;}
 phase_misses=0;
 if(label<0){phase_error=2;InterlockedExchange(&phase_gate,0);return 0;}
 if(phase_state==1){
  if(phase_counts[1]>=32){InterlockedExchange(&phase_gate,0);phase_state=2;}
  else if(label!=1){phase_error=3;InterlockedExchange(&phase_gate,0);return 0;}
 }
 if(phase_state==2 && label==2){phase_state=3;InterlockedExchange(&phase_gate,2);}
 else if(phase_state==3 && label!=2){phase_error=4;InterlockedExchange(&phase_gate,0);return 0;}
 return 1;
}
