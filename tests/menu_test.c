#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "../src/mgs4vr_menu.h"
#define CHECK(x) do{if(!(x)){printf("FAIL menu line %d\n",__LINE__);return 1;}}while(0)
int main(void){MGS4VR_MENU m;unsigned char *p;int i;
 mgs4vr_menu_init(&m,0,250);CHECK(!m.open && !m.follow && m.distance_cm==250);
 CHECK(!mgs4vr_menu_key(&m,VK_RETURN));mgs4vr_menu_key(&m,VK_HOME);CHECK(m.open);
 CHECK(!mgs4vr_menu_key(&m,VK_RETURN) && !m.follow);m.available=1;
 CHECK(mgs4vr_menu_key(&m,VK_RETURN) && m.follow);mgs4vr_menu_key(&m,VK_RETURN);CHECK(!m.follow);
 mgs4vr_menu_key(&m,VK_DOWN);i=m.recenter;mgs4vr_menu_key(&m,VK_RETURN);CHECK(m.recenter==i+1);
 mgs4vr_menu_key(&m,VK_DOWN);mgs4vr_menu_key(&m,VK_LEFT);CHECK(m.distance_cm==240);
 for(i=0;i<150;++i)mgs4vr_menu_key(&m,VK_LEFT);CHECK(m.distance_cm==50);
 for(i=0;i<150;++i)mgs4vr_menu_key(&m,VK_RIGHT);CHECK(m.distance_cm==1000);
 mgs4vr_menu_key(&m,VK_DOWN);CHECK(m.row==0);mgs4vr_menu_key(&m,VK_UP);CHECK(m.row==2);
 mgs4vr_menu_key(&m,VK_ESCAPE);CHECK(!m.open);mgs4vr_menu_key(&m,VK_HOME);CHECK(m.open);
 p=mgs4vr_menu_bitmap(&m,960,540);CHECK(p && p[0]==18 && p[1]==24 && p[2]==34 && p[3]==255);
 CHECK(mgs4vr_menu_bitmap(&m,960,540)==p);CHECK(mgs4vr_menu_bitmap(&m,720,400));mgs4vr_menu_free();
 puts("PASS menu: toggle, unavailable tracking, recenter, bounded distance, navigation, RGBA rendering and resize");return 0;}
