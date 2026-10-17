/* Portable profile parser. Keys are operator labels until a native identity is
   proven. No heap allocation, file access or game addresses in this module. */
#ifndef MGS4VR_WEAPON_PROFILES_H
#define MGS4VR_WEAPON_PROFILES_H
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#define WP_MAX 128
#define WP_BYTES 65536
typedef struct { char key[32]; int hand_mode; float O[9], P[3]; } WP_PROFILE;
typedef struct { unsigned count; WP_PROFILE row[WP_MAX]; } WP_TABLE;
static int wp_key(const char *s) {
    unsigned n=0; for(;*s;++s,++n) if(!isalnum((unsigned char)*s)&&*s!='_'&&*s!='-')return 0;
    return n>0&&n<32;
}
static int wp_valid(const WP_PROFILE *p) {
    int i,j,k; float d;
    if(!wp_key(p->key)||p->hand_mode<0||p->hand_mode>2)return 0;
    for(i=0;i<3;++i)if(!isfinite(p->P[i])||fabsf(p->P[i])>1000)return 0;
    for(i=0;i<9;++i)if(!isfinite(p->O[i]))return 0;
    for(i=0;i<3;++i)for(j=0;j<3;++j){d=0;for(k=0;k<3;++k)d+=p->O[i*3+k]*p->O[j*3+k];if(fabsf(d-(i==j))>.01f)return 0;}
    d=p->O[0]*(p->O[4]*p->O[8]-p->O[5]*p->O[7])-p->O[1]*(p->O[3]*p->O[8]-p->O[5]*p->O[6])+p->O[2]*(p->O[3]*p->O[7]-p->O[4]*p->O[6]);
    return fabsf(d-1)<.01f;
}
static const WP_PROFILE *wp_find(const WP_TABLE *t,const char *key) {
    unsigned i;for(i=0;i<t->count;++i)if(!strcmp(t->row[i].key,key))return &t->row[i];return NULL;
}

/* Only identities observed in labelled continuous traces. Missing calibration
   returns NULL: identifying a weapon does not manufacture its grip profile. */
static const WP_PROFILE *wp_native(const WP_TABLE *t,unsigned id) {
    const char *key=id==25?"ak102":id==24?"m4custom":id==3?"operator":NULL;char generic[16];
    if(key)return wp_find(t,key);
    /* any other identified weapon: optional row "w<decimal code>", e.g. w47 = XM25, w50 = RPG-7 */
    if(!id||id>511)return NULL;sprintf_s(generic,sizeof(generic),"w%u",id);return wp_find(t,generic);
}
static char *wp_token(char **cursor) {
    char *p=*cursor,*start;while(*p&&isspace((unsigned char)*p))++p;
    if(!*p){*cursor=p;return NULL;}start=p;while(*p&&!isspace((unsigned char)*p))++p;
    if(*p)*p++=0;*cursor=p;return start;
}
/* Parse into private scratch; only a complete END count may be published. */
static int wp_parse(char *text,WP_TABLE *t) {
    char *line,*next;int first=1,ended=0;WP_PROFILE p;t->count=0;
    for(line=text;line&&*line;line=next){char *cursor,*tok,*end;long v;unsigned i;
        next=strchr(line,'\n');if(next)*next++=0;
        while(*line&&isspace((unsigned char)*line))++line;
        if(!*line||*line=='#')continue;if(ended)return 0;
        cursor=line;tok=wp_token(&cursor);
        if(first){if(strcmp(tok,"WP1")||wp_token(&cursor))return 0;first=0;continue;}
        if(!strcmp(tok,"END")){tok=wp_token(&cursor);if(!tok)return 0;v=strtol(tok,&end,10);
            if(*end||v<0||v!=t->count||wp_token(&cursor))return 0;ended=1;continue;}
        memset(&p,0,sizeof(p));if(!wp_key(tok))return 0;memcpy(p.key,tok,strlen(tok)+1);
        tok=wp_token(&cursor);if(!tok)return 0;v=strtol(tok,&end,10);if(*end||v<0||v>2)return 0;p.hand_mode=(int)v;
        for(i=0;i<12;++i){float x;tok=wp_token(&cursor);if(!tok)return 0;x=strtof(tok,&end);if(*end)return 0;if(i<9)p.O[i]=x;else p.P[i-9]=x;}
        if(wp_token(&cursor)||!wp_valid(&p)||t->count==WP_MAX)return 0;
        for(i=0;i<t->count;++i)if(!strcmp(p.key,t->row[i].key))return 0;
        t->row[t->count++]=p;
    }
    return !first&&ended;
}
#endif

