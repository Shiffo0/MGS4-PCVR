/* mgs4vr_profile.h - the ONE executable this build knows.
 *
 * Validated against Steam build 25292043; evidence/compat_2026-09-24.
 * The PE tuple is what mgs4Ultra120 gates on; the SHA-256 is our own
 * stronger identity of the file on disk. A mismatch never blocks the
 * renderer observation (IAT hooks are version-independent) but it must
 * block every later feature that reads or writes game memory. */
#ifndef MGS4VR_PROFILE_H
#define MGS4VR_PROFILE_H

#define MGS4VR_HOST_EXE          "mgs4.exe"
#define MGS4VR_PE_TIMESTAMP      0x6aa36b7cu
#define MGS4VR_SIZE_OF_IMAGE     0x241be000u
#define MGS4VR_EXE_SHA256 \
    "656ede900b03467e4ed05a00eca35f0ac306ce57a8f29a76a20e5a5410d02900"
#define MGS4VR_STEAM_BUILDID     "25292043"
#define MGS4VR_VERSION_ID \
    "Steam 25292043 - camera/render addresses verified 2026-09-24"

/* W03 camera addresses relocated and code-compared on 2026-09-24. Original source: mgs4Ultra120 @ 2177a8f7 (MIT), valid only
   for the PE tuple above. Each is re-verified in memory before arming:
   prologue bytes for entries, "E8 rel32 -> builder" for return sites. */
#define MGS4VR_RVA_CAMERA_BUILDER   0x0b9b70u
#define MGS4VR_RVA_PROJ_SETTER      0x0e34d0u
#define MGS4VR_RVA_RET_GAMEPLAY     0x0ba363u
#define MGS4VR_RVA_RET_CINE_REBUILD 0x0eb1abu
/* Measured in run 03 (2026-09-19), evidence/run03_2026-09-19_camera/camerakaart.md:
   the end-of-frame camera update builds the main camera twice from a temporary
   camera-to-world matrix on the stack, from these two call sites. */
#define MGS4VR_RVA_RET_WRAPPER      0x0b9b60u
#define MGS4VR_RVA_MAIN_CAMERA      0x1DDDC70u
/* Motion blur query. Source: MGSPatriotFix @ d7cc89fe (MIT), graphics_tuning.cpp
   "GraphicsTuning - Disable Motion Blur": that mod replaces the function with
   one that returns 0. Resolved at runtime in the decrypted .text and used only
   on exactly one match. */
#define MGS4VR_SIG_MOTION_BLUR      "40 53 48 83 EC ?? 0F 29 74 24 ?? BA ?? ?? ?? ?? B9"
#define MGS4VR_SIG_CAMERA_BUILDER   { 0x48, 0x8b, 0xc4, 0x53, 0x56, 0x57 }
#define MGS4VR_SIG_PROJ_SETTER      { 0x48, 0x83, 0xec, 0x68 }

/* W06 renderer addresses: disassembly comparison plus read-only runtime chain.
   Frame-internal offsets are unchanged in the compared native submit. */
#define MGS4VR_RVA_SUBMIT          0x793470u
#define MGS4VR_RVA_RENDERER        0x23e0bc58u
#define MGS4VR_RVA_RENDERER_VTABLE 0x1887d08u
#define MGS4VR_RVA_CONTEXT         0x23d37270u
#define MGS4VR_RVA_UNIFORM_SIZES   0x1883950u
#define MGS4VR_RVA_PROJECTION_ID   0x1c509a0u
#define MGS4VR_RVA_COMBINED_ID     0x1c5099cu

#endif
