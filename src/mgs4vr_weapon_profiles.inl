/* Worker owns disk I/O. Game thread only takes a nonblocking snapshot. Manual
   selection is deliberately explicit; there is no guessed automatic weapon ID. */
#include "mgs4vr_weapon_profiles.h"
static SRWLOCK wp_lock=SRWLOCK_INIT;
static WP_TABLE wp_table,wp_published;
static LONG wp_auto_frames,wp_last_id,wp_last_source;
static WP_PROFILE wp_selected;
static LONG wp_generation,wp_manual,wp_load_ok,wp_load_bad,wp_known_frames,wp_unknown_frames,wp_busy_frames;
static char wp_path[MAX_PATH],wp_select_path[MAX_PATH];
static DWORD wp_poll_at;
static int wp_started;
static void wp_poll(const char *exe) {
    static char data[WP_BYTES+1];static WP_TABLE next;
    char selection[80]={0},key[32]={0},*leaf;FILE *f;size_t n;DWORD now=GetTickCount();WP_PROFILE picked;int manual=0;
    if(wp_started&&now-wp_poll_at<500)return;wp_started=1;wp_poll_at=now;
    if(!wp_path[0]){strcpy_s(wp_path,sizeof(wp_path),exe);leaf=strrchr(wp_path,'\\');if(!leaf){wp_path[0]=0;return;}
        strcpy_s(leaf+1,sizeof(wp_path)-(size_t)(leaf+1-wp_path),"mgs4vr_weapons.txt");
        strcpy_s(wp_select_path,sizeof(wp_select_path),wp_path);leaf=strrchr(wp_select_path,'\\');
        strcpy_s(leaf+1,sizeof(wp_select_path)-(size_t)(leaf+1-wp_select_path),"mgs4vr_weapon_select.txt");}
    if(!fopen_s(&f,wp_path,"rb")){n=fread(data,1,WP_BYTES+1,f);if(!ferror(f)&&n<=WP_BYTES){data[n]=0;
            if(!memchr(data,0,n)&&wp_parse(data,&next)){wp_table=next;++wp_load_ok;}else ++wp_load_bad;
        }else ++wp_load_bad;fclose(f);}
    if(!fopen_s(&f,wp_select_path,"rb")){n=fread(selection,1,sizeof(selection)-1,f);if(ferror(f)||!feof(f)||memchr(selection,0,n))n=0;fclose(f);selection[n]=0;
        {char *cursor=selection,*kind=wp_token(&cursor),*name=wp_token(&cursor);
         if(kind&&name&&!strcmp(kind,"manual")&&wp_key(name)&&!wp_token(&cursor)){strcpy_s(key,sizeof(key),name);manual=1;}}}
    memset(&picked,0,sizeof(picked));
    if(manual){const WP_PROFILE *p=wp_find(&wp_table,key);if(p)picked=*p;else manual=0;}
    AcquireSRWLockExclusive(&wp_lock);
    if(manual!=wp_manual||memcmp(&picked,&wp_selected,sizeof(picked))||memcmp(&wp_table,&wp_published,sizeof(wp_table))){wp_published=wp_table;wp_selected=picked;wp_manual=manual;++wp_generation;}
    ReleaseSRWLockExclusive(&wp_lock);
}
/* Source: 0 fallback, 1 explicit manual override, 2 native identity. */
static int wp_snapshot_id(WP_PROFILE *p,LONG *generation,unsigned id) {
    int source;const WP_PROFILE *found;
    if(!TryAcquireSRWLockShared(&wp_lock)){InterlockedIncrement(&wp_busy_frames);return -1;}
    *generation=wp_generation;source=wp_manual?1:0;
    memset(p,0,sizeof(*p));
    if(source)*p=wp_selected;
    else if((found=wp_native(&wp_published,id))!=NULL){*p=*found;source=2;}
    ReleaseSRWLockShared(&wp_lock);
    InterlockedExchange(&wp_last_id,(LONG)id);InterlockedExchange(&wp_last_source,source);
    InterlockedIncrement(source==1?&wp_known_frames:source==2?&wp_auto_frames:&wp_unknown_frames);
    return source;
}
static int wp_snapshot(WP_PROFILE *p,LONG *generation){return wp_snapshot_id(p,generation,0);}
