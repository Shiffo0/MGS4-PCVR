/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
/* Hand poses for the desktop pose tool (16 Oct 2026, user: "an in-game modding tool to calibrate / save hand poses";
   phase 1 of 4: per weapon and hand an offset of the hand and finger curls, live from a file). The tool
   (tools/Mgs4PoseTool.py) writes mgs4vr_poses.txt in the game folder; the worker reloads it within ~250 ms and the game
   thread (mgs4vr_armsrc) applies the pose after the arm IK. The worker also writes mgs4vr_poses_status_<pid>.txt (current
   weapon, equip list), once mgs4vr_weapon_names.txt (all weapon codes with the game's names) and takes commands from
   mgs4vr_pose_cmd.txt ("equip <code>": only a code from the equip list, in first person, as the radial menu does).
   File format:  POSES1  then lines  <key> <R|L> <pose> dx dy dz yaw pitch roll thumb index middle ring pinky spread
   key = w<decimal weapon code> or "any"; pose = grip (right hand on the gun), support (left hand on the foregrip);
   dx dy dz mm and yaw pitch roll degrees in the hand's own frame; finger values = extra curl in degrees per joint,
   spread degrees. Lookup: w<code> first, then any. */
#ifndef MGS4VR_POSES_H
#define MGS4VR_POSES_H
#define MGS4VR_POSES_MAX 256
/* slide_rear / slide_mid / mag (16 Oct 2026, phases 3-4): LEFT hand relative to the RIGHT hand (the pistol): d = left
   hand bone position in the right hand's frame (mm; pistol barrel along -x, sights at +z), rot = its orientation (deg). */
enum { MGS4VR_POSE_GRIP, MGS4VR_POSE_SUPPORT, MGS4VR_POSE_SLIDE_REAR, MGS4VR_POSE_SLIDE_MID, MGS4VR_POSE_MAG, MGS4VR_POSE_N };
enum { MGS4VR_EV_SLIDE_GRAB, MGS4VR_EV_RACK, MGS4VR_EV_MAG_HOLD, MGS4VR_EV_MAG_INSERT };
int mgs4vr_poses_freeze(void);   /* tool: controllers held still (command "freeze 1|0") */
int mgs4vr_poses_cam(float v[6]);   /* tool: pose camera az el dist(mm) target xyz (pistol frame); 0 = off */
int mgs4vr_poses_show(void);     /* tool: left hand forced into this pose (command "show <pose>"), -1 none */
void mgs4vr_poses_event(int kind);   /* interaction counters for the status file */
enum { MGS4VR_FINGER_THUMB, MGS4VR_FINGER_INDEX, MGS4VR_FINGER_MIDDLE, MGS4VR_FINGER_RING, MGS4VR_FINGER_PINKY, MGS4VR_FINGERS };
typedef struct { int code; int hand; int pose; float d[3], rot[3], curl[MGS4VR_FINGERS], spread; } MGS4VR_POSE;   /* code -1 = any, hand 0 R 1 L */
typedef struct { int count; MGS4VR_POSE row[MGS4VR_POSES_MAX]; } MGS4VR_POSE_TABLE;
/* Pure: parse text (modified in place) into t. 1 on success (header POSES1, every line valid). */
int mgs4vr_poses_parse(char *text, MGS4VR_POSE_TABLE *t);
/* Pure: best row for (code, hand, pose): exact code, else any. NULL = none. */
const MGS4VR_POSE *mgs4vr_poses_find(const MGS4VR_POSE_TABLE *t, int code, int hand, int pose);
/* Worker: reload, status, names, commands (~every 250 ms). */
void mgs4vr_poses_poll(void *image, const char *exe);
/* Game thread: copy of the pose for (code, hand, pose) from the published table; 0 = none or busy. */
int mgs4vr_poses_get(int code, int hand, int pose, MGS4VR_POSE *out);
#endif
