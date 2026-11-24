# AMD FidelityFX SDK 1.1.4 (subset): the FSR 3 upscaler on Linux

From <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4>, MIT (`LICENSE.txt`), as vendored and
patched for clang and Linux by Savage Wind Recomp (<https://github.com/Revivedx/SavageWindRecomp>, MIT,
`third_party/FidelityFX`): `ffx_compat.h`, the 64-byte alignment and descriptor pool fixes in `sdk/src/backends/vk/ffx_vk.cpp`,
the doubled context size outside Windows in `ffx_types.h`, and `spirv/ffx_fsr3upscaler_vk.h` (the upscaler's passes
compiled to SPIR-V with glslangValidator: SDR input, low-resolution motion vectors without jitter, depth not inverted).

Only what the upscaler needs is here: the public headers, `sdk/src/components/fsr3upscaler`, the Vulkan backend and
`sdk/src/shared`. `runtime/gfx/vk/exec/vk_ffxsdk_*.cpp` compile the sources as they are; `vk_tmp_fsr3_sdk.cpp` drives
them for `vk_tmp_ffx.cpp` on Linux, where AMD ships no FidelityFX API library. Windows keeps loading AMD's DLLs.
