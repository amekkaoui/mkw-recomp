// AMD FidelityFX SDK 1.1.4's FSR 3 upscaler component (third_party/FidelityFX, MIT), compiled as it is for
// vk_tmp_fsr3_sdk.cpp: on Linux AMD ships no FidelityFX API library, so the SDK's own sources are built in. Windows
// loads AMD's DLLs (vk_tmp_ffx.cpp) and compiles nothing here.
#if defined(__linux__)
#pragma clang diagnostic ignored "-Weverything"
#include "../../../../third_party/FidelityFX/ffx_compat.h"
#include "../../../../third_party/FidelityFX/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
#endif
