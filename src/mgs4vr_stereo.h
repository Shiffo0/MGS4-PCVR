/* W06 projection math; not yet connected to the game camera/render path.
   Measured game convention: row vectors, x right, y down, z forward,
   clip.w=z. Preserve native reversed-Z/depth terms for BOTH depth ranges. */
#ifndef MGS4VR_STEREO_H
#define MGS4VR_STEREO_H
/* fov = OpenXR angles left,right,up,down (radians). Rejects invalid input
   without changing out. Supports out == native. Does not validate ownership
   of a game projection; the caller must establish the camera/render contract. */
int mgs4vr_stereo_projection(const float native[16], const float fov[4], float out[16]);
/* Native camera builder inputs measured from runtime code: scale determines
   m00; aspect multiplies native width/height for m11; p4/p5 are m20/m21.
   This lets the game build its own combined matrices and culling planes. */
int mgs4vr_stereo_builder(const float fov[4],int width,int height,
 float *scale,float *p4,float *p5,float *aspect);
#endif
