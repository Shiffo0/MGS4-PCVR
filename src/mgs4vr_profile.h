/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
#ifndef MGS4VR_PROFILE_H
#define MGS4VR_PROFILE_H

#define MGS4VR_HOST_EXE          "mgs4.exe"
#define MGS4VR_PE_TIMESTAMP      0x6a8cfc47u
#define MGS4VR_SIZE_OF_IMAGE     0x241be000u
#define MGS4VR_EXE_SHA256 \
    "9e8df67ea7f41e7f8306ce1a77584707209069b3c75389b3f00445efe459fe41"
#define MGS4VR_STEAM_BUILDID     "24921893"
#define MGS4VR_VERSION_ID \
    "Pela_[MPA]_x64_BGFX_0.0.3_Release_ww_[Code]a84606af_[DataNew]d06ab525_2026_0825"

#define MGS4VR_RVA_CAMERA_BUILDER   0x0b9bb0u
#define MGS4VR_RVA_PROJ_SETTER      0x0e3410u
#define MGS4VR_RVA_RET_GAMEPLAY     0x0ba3a3u
#define MGS4VR_RVA_RET_CINE_REBUILD 0x0eb0ebu

#define MGS4VR_RVA_RET_WRAPPER      0x0b9ba0u
#define MGS4VR_RVA_MAIN_CAMERA      0x1DDDC50u

#define MGS4VR_SIG_MOTION_BLUR      "40 53 48 83 EC ?? 0F 29 74 24 ?? BA ?? ?? ?? ?? B9"
#define MGS4VR_SIG_CAMERA_BUILDER   { 0x48, 0x8b, 0xc4, 0x53, 0x56, 0x57 }
#define MGS4VR_SIG_PROJ_SETTER      { 0x48, 0x83, 0xec, 0x68 }

#endif
