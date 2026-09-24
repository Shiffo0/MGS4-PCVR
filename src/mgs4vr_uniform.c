#include "mgs4vr_uniform.h"
#include <string.h>
static int walk(const unsigned char *data,size_t length,const unsigned sizes[6],
 unsigned p,unsigned c,void (*cb)(unsigned,const float *,void *),void *user){
 size_t pos=0,n;unsigned op,type,count,loc;float m[16];
 while(length-pos>=4){
  memcpy(&op,data+pos,4);pos+=4;if(op==1)return 1;
  type=op>>27;count=(op>>1)&1023;loc=(op>>11)&65535;
  if(type>=6 || !count || !sizes[type] || sizes[type]>64)return 0;
  n=(size_t)count*sizes[type];if(n>length-pos)return 0;
  if(type==4 && count==1 && (loc==p || loc==c)){
   if(!(op&1))return 0; /* do not follow external pointers */
   if(cb){memcpy(m,data+pos,64);cb(loc,m,user);}
  }
  pos+=n;
 }
 return 0;
}
int mgs4vr_uniform_walk(const unsigned char *data,size_t length,const unsigned sizes[6],
 unsigned p,unsigned c,void (*cb)(unsigned,const float *,void *),void *user){
 if(!data || !sizes || sizes[4]!=64 || length>16*1024*1024)return 0;
 if(!walk(data,length,sizes,p,c,0,0))return 0;
 return walk(data,length,sizes,p,c,cb,user);
}
