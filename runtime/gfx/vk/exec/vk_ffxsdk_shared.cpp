// AMD FidelityFX SDK 1.1.4's shared helpers (assert, messages, object management, breadcrumbs; third_party/FidelityFX,
// MIT), compiled as they are for vk_tmp_fsr3_sdk.cpp on Linux (vk_ffxsdk_upscaler.cpp says why). Nothing on Windows.
#if defined(__linux__)
#pragma clang diagnostic ignored "-Weverything"
#include "../../../../third_party/FidelityFX/ffx_compat.h"
#include "../../../../third_party/FidelityFX/sdk/src/shared/ffx_assert.cpp"
#include "../../../../third_party/FidelityFX/sdk/src/shared/ffx_message.cpp"
#include "../../../../third_party/FidelityFX/sdk/src/shared/ffx_object_management.cpp"
#include "../../../../third_party/FidelityFX/sdk/src/shared/ffx_breadcrumbs_list.cpp"
#endif
