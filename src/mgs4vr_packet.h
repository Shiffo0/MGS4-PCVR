#ifndef MGS4VR_PACKET_H
#define MGS4VR_PACKET_H
#include <stdint.h>
#define MGS4VR_PACKET_HISTORY 16
typedef struct {
 uint32_t epoch, camera_serial, pose_sequence;
 int valid, eye;
 int64_t display_time;
 float quat[4],pos[3],fov[4],combined[16];
} MGS4VR_EYE_PACKET;
typedef struct { MGS4VR_EYE_PACKET slots[MGS4VR_PACKET_HISTORY];uint32_t epoch; } MGS4VR_PACKET_HISTORY_STATE;
/* Caller serialises access. No allocation, waiting, OS calls or external pointers. */
void mgs4vr_packet_reset(MGS4VR_PACKET_HISTORY_STATE *h,uint32_t epoch);
int mgs4vr_packet_store(MGS4VR_PACKET_HISTORY_STATE *h,const MGS4VR_EYE_PACKET *p);
int mgs4vr_packet_candidate(const MGS4VR_PACKET_HISTORY_STATE *h,uint32_t render_serial,MGS4VR_EYE_PACKET *out);
int mgs4vr_packet_matrix_matches(const MGS4VR_EYE_PACKET *p,const float uniform[16]);
int mgs4vr_packet_pair(const MGS4VR_EYE_PACKET *left,const MGS4VR_EYE_PACKET *right);
/* One render submission on one render thread. Begin snapshots ownership;
   uniforms are checked before finish; Present consumes at most once. */
typedef struct {
 MGS4VR_EYE_PACKET packet;
 uint32_t render_serial;
 int matched,complete;
} MGS4VR_PACKET_SUBMISSION;
void mgs4vr_packet_begin(MGS4VR_PACKET_SUBMISSION *s,const MGS4VR_PACKET_HISTORY_STATE *h,uint32_t render_serial);
void mgs4vr_packet_uniform(MGS4VR_PACKET_SUBMISSION *s,const float combined[16]);
void mgs4vr_packet_finish(MGS4VR_PACKET_SUBMISSION *s,int decode_ok);
int mgs4vr_packet_take(MGS4VR_PACKET_SUBMISSION *s,uint32_t current_epoch,MGS4VR_EYE_PACKET *out);
#endif
