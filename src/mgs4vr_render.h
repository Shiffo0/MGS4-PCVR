#ifndef MGS4VR_RENDER_H
#define MGS4VR_RENDER_H
#include "mgs4vr_packet.h"
int mgs4vr_render_start(void *base);
int mgs4vr_render_serial(unsigned *serial);
int mgs4vr_render_take(MGS4VR_EYE_PACKET *packet);
void mgs4vr_render_stop(void);
#endif
