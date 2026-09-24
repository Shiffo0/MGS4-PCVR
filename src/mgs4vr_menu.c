/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "mgs4vr_menu.h"
void mgs4vr_menu_init(MGS4VR_MENU *m,int follow,int cm){memset(m,0,sizeof(*m));m->follow=!!follow;m->distance_cm=cm<50?50:cm>1000?1000:cm;}
int mgs4vr_menu_key(MGS4VR_MENU *m,unsigned key){
 int before;
 if(key==VK_HOME){m->open=!m->open;return 1;}
 if(!m->open)return 0;
 if(key==VK_ESCAPE){m->open=0;return 1;}
 if(key==VK_UP){m->row=(m->row+3)%4;return 1;}
 if(key==VK_DOWN){m->row=(m->row+1)%4;return 1;}
 if(key!=VK_LEFT && key!=VK_RIGHT && key!=VK_RETURN)return 0;
 if(m->row==0){if(!m->available || m->stereo)return 0;m->follow=!m->follow;m->recenter++;return 1;}
 if(m->row==1){if(key==VK_RETURN){m->recenter++;return 1;}return 0;}
 if(m->row==3){if(!m->stereo_available)return 0;m->stereo=!m->stereo;m->recenter++;return 1;}
 before=m->distance_cm;
 if(key==VK_LEFT)m->distance_cm-=10;else if(key==VK_RIGHT)m->distance_cm+=10;
 if(m->distance_cm<50)m->distance_cm=50;if(m->distance_cm>1000)m->distance_cm=1000;
 return before!=m->distance_cm;
}
static HDC dc;static HBITMAP bitmap;static HGDIOBJ old_bitmap;static unsigned char *pixels;static int bw,bh,have;
static MGS4VR_MENU previous;
void mgs4vr_menu_free(void){if(dc){if(old_bitmap)SelectObject(dc,old_bitmap);if(bitmap)DeleteObject(bitmap);DeleteDC(dc);}dc=NULL;bitmap=NULL;old_bitmap=NULL;pixels=NULL;have=0;}
void *mgs4vr_menu_bitmap(const MGS4VR_MENU *m,int w,int h){
 BITMAPINFO bi={0};RECT rect;HFONT font;HGDIOBJ old_font;HBRUSH brush;int row,i;char text[160];
 if(w<160 || h<100)return NULL;
 if(!dc || bw!=w || bh!=h){
  mgs4vr_menu_free();dc=CreateCompatibleDC(NULL);if(!dc)return NULL;
  bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;
  bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
  bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,(void **)&pixels,NULL,0);
  if(!bitmap){mgs4vr_menu_free();return NULL;}old_bitmap=SelectObject(dc,bitmap);bw=w;bh=h;
 }
 if(have && memcmp(m,&previous,sizeof(*m))==0)return pixels;
 rect.left=rect.top=0;rect.right=w;rect.bottom=h;brush=CreateSolidBrush(RGB(18,24,34));FillRect(dc,&rect,brush);DeleteObject(brush);
 font=CreateFontA(-h/15,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,"Segoe UI");
 old_font=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(235,241,250));
 for(row=0;row<7;++row){
  rect.left=w/24;rect.right=w-w/24;rect.top=h/16+row*h/8;rect.bottom=rect.top+h/10;
  if(row>=1 && row<=4 && m->row==row-1){brush=CreateSolidBrush(RGB(32,68,100));FillRect(dc,&rect,brush);DeleteObject(brush);}
  if(row==0)strcpy_s(text,sizeof(text),"MGS4-PCVR  v0.1.1");
  if(row==1)sprintf_s(text,sizeof(text),"Camera follows headset: %s",!m->available?"Unavailable":m->stereo?"Yes (stereo)":m->follow?"Yes":"No");
  if(row==2)strcpy_s(text,sizeof(text),"Recenter screen  [Enter]");
  if(row==3)sprintf_s(text,sizeof(text),"Screen distance: %.1f m  [Left / Right]",m->distance_cm/100.0);
  if(row==4)sprintf_s(text,sizeof(text),"Stereo: %s",!m->stereo_available?"Unavailable":m->stereo?"On":"Off (mono theater)");
  if(row==5)strcpy_s(text,sizeof(text),"Up / Down: select    Enter: apply");
  if(row==6)strcpy_s(text,sizeof(text),"Home / Esc: close    Gamepad: play");
  rect.left+=w/60;DrawTextA(dc,text,-1,&rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
 }
 SelectObject(dc,old_font);DeleteObject(font);GdiFlush();
 for(i=0;i<w*h;++i){unsigned char t=pixels[i*4];pixels[i*4]=pixels[i*4+2];pixels[i*4+2]=t;pixels[i*4+3]=255;}
 previous=*m;have=1;return pixels;
}
