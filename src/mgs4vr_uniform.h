#ifndef MGS4VR_UNIFORM_H
#define MGS4VR_UNIFORM_H
#include <stddef.h>
/* Bounded decoding of the observed bgfx copied-uniform wire format.
   Validate the entire stream before invoking callbacks. Caller owns stable bytes.
   Returns 1 at end opcode, 0 malformed/truncated/unsupported. */
int mgs4vr_uniform_walk(const unsigned char *data,size_t length,const unsigned sizes[6],
    unsigned projection,unsigned combined,void (*matrix)(unsigned,const float *,void *),void *user);
#endif
