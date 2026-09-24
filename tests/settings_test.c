#define mgs4vr_cam_set_adjust capture_adjust
#define DllMain test_dll_entry
#include "../src/mgs4vr.c"
#undef mgs4vr_cam_set_adjust
#undef DllMain
static MGS4VR_CAM_ADJUST captured;
void capture_adjust(const MGS4VR_CAM_ADJUST *a){captured=*a;}
#define CHECK(x) do{if(!(x)){printf("FAIL settings line %d\n",__LINE__);return 1;}}while(0)
int main(void){MGS4VR_MENU m;float q[4]={0,0,0,1},p[3]={0,0,0};
 mgs4vr_menu_init(&m,1,250);configure(&m);head_pose(q,p);CHECK(!captured.enabled);
 m.available=1;configure(&m);head_pose(q,p);CHECK(captured.enabled && fabsf(captured.yaw)<.001f);
 q[1]=.25881905f;q[3]=.96592583f;head_pose(q,p);CHECK(fabsf(fabsf(captured.yaw)-30)<.001f);
 m.follow=0;configure(&m);CHECK(!captured.enabled && captured.yaw==0);head_pose(q,p);CHECK(!captured.enabled);
 m.follow=1;configure(&m);head_pose(q,p);CHECK(captured.enabled && fabsf(captured.yaw)<.001f);
 q[1]=0;q[3]=1;head_pose(q,p);CHECK(fabsf(fabsf(captured.yaw)-30)<.001f);
 m.recenter++;configure(&m);head_pose(q,p);CHECK(fabsf(captured.yaw)<.001f);
 expire_pose(last_pose+401);CHECK(!captured.enabled);head_pose(q,p);CHECK(captured.enabled && fabsf(captured.yaw)<.001f);
 m.stereo_available=1;m.stereo=1;configure(&m);CHECK(stereo_on && !follow);head_pose(q,p);CHECK(!captured.enabled);
 m.open=1;configure(&m);CHECK(!stereo_on && m.stereo);
 m.open=0;configure(&m);CHECK(stereo_on);
 m.stereo=0;configure(&m);CHECK(!stereo_on && follow);
 m.stereo=1;m.stereo_available=0;configure(&m);CHECK(!stereo_on);
 puts("PASS settings: unsupported profile, tracking on/off releases pose, new neutral, recenter and stale-pose recovery");return 0;}
