// AMD FSR 3.1's upscaler on Linux for vk_tmp_ffx.cpp, through the FidelityFX SDK's own context and Vulkan backend
// built into the runtime (third_party/FidelityFX: SDK 1.1.4, MIT), since AMD ships its FidelityFX API library for
// Windows only. Adapted from Savage Wind Recomp's runtime/nvn/vulkan/fsr3.cpp (MIT, Revivedx and the Savage Wind
// Recomp contributors), which drives the same vendored SDK on Linux and the Steam Deck.
//
// The inputs are vk_temporal.c's, as on Windows: the tonemapped scene, the depth as plain R32F (0 near, 1 far), the
// motion vectors in render pixels from this frame to the last, every image in VK_IMAGE_LAYOUT_GENERAL (the SDK's
// common state). No reactive mask: the SDK then uses its own empty one.
//
// The shaders are third_party/FidelityFX/spirv's: one permutation per pass, for SDR input, low-resolution motion
// vectors without jitter and depth not inverted, in 32-bit floats. A game that draws HDR has no permutation here, so
// the upscale is refused (vk_temporal.c then says so) rather than fed a picture of another range.
//
// A few contexts at once, as vk_tmp_ffx.cpp keeps on Windows: the game's upscale writes its output at another size
// in some frames. One leaves once the GPU is done with the submission being recorded (vk_tmp_defer).
#if defined(__linux__)
#include "../../../../third_party/FidelityFX/ffx_compat.h"

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffx_shader_blobs.h"
#include "ffx_fsr3upscaler_vk.h"

extern "C" {
#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "vk.h"
#include "vk_temporal.h"
}

// ---- the shaders: the blobs of spirv/ffx_fsr3upscaler_vk.h in place of the SDK's FidelityFX_SC permutation tables
extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effect, FfxPass pass, FfxBindStage, uint32_t,
                                                     FfxShaderBlob *out) {
    if (effect == FFX_EFFECT_FSR3UPSCALER && pass < FFX_FSR3UPSCALER_PASS_COUNT && ffx_fu_blobs[0][pass].data) {
        memcpy(out, &ffx_fu_blobs[0][pass], sizeof *out);   // [0]: 32-bit floats
        return FFX_OK;
    }
    rt_log("fsr3: no shader for effect %d pass %d", (int)effect, (int)pass);
    return FFX_ERROR_INVALID_ARGUMENT;
}

extern "C" FfxErrorCode ffxIsWave64(FfxEffect, uint32_t, bool &wave64) {
    wave64 = false;   // only wave32 blobs: the backend then asks for no subgroup size
    return FFX_OK;
}

// The backend's hook into its frame interpolation swapchain, which is not built
FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const *) { return FFX_OK; }

namespace {

struct Ctx {
    FfxFsr3UpscalerContext ctx;
    FfxInterface iface;
    FfxUInt32 shared_ctx;
    FfxResourceInternal shared[3];   // dilated depth, dilated motion vectors, the previous frame's nearest depth
    void *scratch;
    uint32_t rw, rh, ow, oh;
    uint64_t used;
};
struct {
    Ctx *c[3];
    int cur;
    int failed;
    uint64_t tick;
} U;
VkDeviceContext dc;   // the backend keeps a pointer to it

void message(FfxMsgType type, const wchar_t *msg) {
    char s[512];
    size_t n = 0;
    for (; msg && msg[n] && n < sizeof s - 1; n++) s[n] = msg[n] < 128 ? (char)msg[n] : '?';
    s[n] = 0;
    static int told;
    if (told++ < 40) rt_log("fsr3: %s %s", type == FFX_MESSAGE_TYPE_ERROR ? "error:" : "warning:", s);
}

// The backend's device functions: an extension's KHR name this device was not made with is the core function it was
// promoted to (the backend calls vkGetBufferMemoryRequirements2KHR without checking it), and its debug labels do
// nothing without VK_EXT_debug_utils.
VKAPI_ATTR void VKAPI_CALL no_label(VkCommandBuffer, const VkDebugUtilsLabelEXT *) {}
VKAPI_ATTR void VKAPI_CALL no_label_end(VkCommandBuffer) {}
PFN_vkVoidFunction VKAPI_PTR device_proc(VkDevice dev, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(dev, name);
    if (!f && !strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label;
    if (!f && !strcmp(name, "vkCmdEndDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label_end;
    size_t n = strlen(name);
    if (!f && n > 3 && n < 128 && !strcmp(name + n - 3, "KHR")) {
        char core[128];
        memcpy(core, name, n - 3);
        core[n - 3] = 0;
        f = vkGetDeviceProcAddr(dev, core);
    }
    return f;
}

void ctx_free(void *arg) {   // completion thread, or at once outside a recording
    Ctx *x = (Ctx *)arg;
    for (int i = 0; i < 3; i++) x->iface.fpDestroyResource(&x->iface, x->shared[i], x->shared_ctx);
    x->iface.fpDestroyBackendContext(&x->iface, x->shared_ctx);
    ffxFsr3UpscalerContextDestroy(&x->ctx);
    free(x->scratch);
    delete x;
}

Ctx *ctx_make(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    const VkCtx *c = vk_ctx();
    enum { CONTEXTS = FFX_FSR3UPSCALER_CONTEXT_COUNT + 1 };   // + the shared images'
    size_t n = ffxGetScratchMemorySizeVK(c->phys, CONTEXTS);
    n = (n + 63) & ~(size_t)63;
    Ctx *x = new Ctx();
    // 64-byte aligned: the backend's contexts are alignas(32) and its arrays start on 64-byte boundaries
    x->scratch = aligned_alloc(64, n);
    if (!x->scratch) { delete x; return nullptr; }
    memset(x->scratch, 0, n);
    dc = {c->device, c->phys, device_proc};
    FfxFsr3UpscalerContextDescription cd = {};
    FfxErrorCode e = ffxGetInterfaceVK(&cd.backendInterface, ffxGetDeviceVK(&dc), x->scratch, n, CONTEXTS);
    if (e != FFX_OK) {
        rt_log("fsr3: the backend could not be set up (%d)", (int)e);
        free(x->scratch);
        delete x;
        return nullptr;
    }
    const char *dbg = cfg_get("MKW_FSR3_DEBUG");   // the SDK's checks of its inputs, in the log
    cd.flags = dbg && *dbg && strcmp(dbg, "0") ? FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING : 0;
    cd.maxRenderSize = {rw, rh};
    cd.maxUpscaleSize = {ow, oh};
    cd.fpMessage = message;
    e = ffxFsr3UpscalerContextCreate(&x->ctx, &cd);
    if (e != FFX_OK) {
        rt_log("fsr3: upscaling context %ux%u -> %ux%u could not be created (%d)", rw, rh, ow, oh, (int)e);
        free(x->scratch);
        delete x;
        return nullptr;
    }
    x->iface = cd.backendInterface;
    FfxFsr3UpscalerSharedResourceDescriptions sd = {};
    e = ffxFsr3UpscalerGetSharedResourceDescriptions(&x->ctx, &sd);
    if (e == FFX_OK) e = x->iface.fpCreateBackendContext(&x->iface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &x->shared_ctx);
    const FfxCreateResourceDescription *rd[3] = {&sd.dilatedDepth, &sd.dilatedMotionVectors,
                                                 &sd.reconstructedPrevNearestDepth};
    for (int i = 0; i < 3 && e == FFX_OK; i++) e = x->iface.fpCreateResource(&x->iface, rd[i], x->shared_ctx, &x->shared[i]);
    if (e != FFX_OK) {
        rt_log("fsr3: the context's shared images could not be created (%d)", (int)e);
        ffxFsr3UpscalerContextDestroy(&x->ctx);
        free(x->scratch);
        delete x;
        return nullptr;
    }
    x->rw = rw, x->rh = rh, x->ow = ow, x->oh = oh;
    rt_log("fsr3: FSR upscaling context %ux%u -> %ux%u (FidelityFX SDK 1.1.4, built in)", rw, rh, ow, oh);
    return x;
}

Ctx *context_for(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, int *changed) {
    U.tick++;
    for (int i = 0; i < 3; i++) {
        Ctx *x = U.c[i];
        if (x && x->rw == rw && x->rh == rh && x->ow == ow && x->oh == oh) {
            *changed = i != U.cur;
            U.cur = i, x->used = U.tick;
            return x;
        }
    }
    if (U.failed >= 4) return nullptr;   // not every frame again
    int pick = 0;
    for (int i = 0; i < 3; i++) {
        if (!U.c[i]) { pick = i; break; }
        if (U.c[i]->used < U.c[pick]->used) pick = i;
    }
    if (U.c[pick]) {   // the least used one goes once the GPU is done with the submission being recorded
        vk_tmp_defer(ctx_free, U.c[pick]);
        U.c[pick] = nullptr;
    }
    Ctx *x = ctx_make(rw, rh, ow, oh);
    if (!x) { U.failed++; return nullptr; }
    U.c[pick] = x, U.cur = pick, x->used = U.tick, *changed = 1;
    return x;
}

FfxResource resource(const VkTmpRes *r, bool uav, const wchar_t *name) {
    FfxResourceDescription rd = {};
    rd.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    rd.format = ffxGetSurfaceFormatVK(r->format);
    rd.width = r->w, rd.height = r->h, rd.depth = 1, rd.mipCount = 1;
    rd.flags = FFX_RESOURCE_FLAGS_NONE;
    rd.usage = uav ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
    return ffxGetResourceVK((void *)r->image, rd, name, FFX_RESOURCE_STATE_COMMON);   // COMMON: GENERAL, kept
}

}  // namespace

extern "C" int vk_tmp_fsr3_sdk_load(void) {
    static int told;
    if (!told++) rt_log("fsr3: FSR 3.1 upscaling through the built-in FidelityFX SDK 1.1.4 (Linux)");
    return 1;
}

extern "C" int vk_tmp_fsr3_sdk_upscale(VkCommandBuffer cb, const VkTmpRes *color, const VkTmpRes *depth,
                                       const VkTmpRes *mv, const VkTmpRes *out, float jx, float jy, int reset,
                                       float frame_ms, float znear, float zfar, float fov_y, int hdr,
                                       float sharpness) {
    if (hdr) {
        static int told;
        if (!told++) rt_log("fsr3: the game draws HDR and the built-in upscaler has SDR shaders only (MKW_HDR=off)");
        return 0;
    }
    int changed = 0;
    Ctx *x = context_for(color->w, color->h, out->w, out->h, &changed);
    if (!x) return 0;
    if (changed) reset = 1;
    FfxFsr3UpscalerDispatchDescription d = {};
    d.commandList = ffxGetCommandListVK(cb);
    d.color = resource(color, false, L"FSR3_Color");
    d.depth = resource(depth, false, L"FSR3_Depth");
    d.motionVectors = resource(mv, false, L"FSR3_MotionVectors");
    d.output = resource(out, true, L"FSR3_Output");
    d.dilatedDepth = x->iface.fpGetResource(&x->iface, x->shared[0]);
    d.dilatedMotionVectors = x->iface.fpGetResource(&x->iface, x->shared[1]);
    d.reconstructedPrevNearestDepth = x->iface.fpGetResource(&x->iface, x->shared[2]);
    d.jitterOffset = {jx, jy};
    d.motionVectorScale = {1.0f, 1.0f};
    d.renderSize = {color->w, color->h};
    d.upscaleSize = {out->w, out->h};
    d.enableSharpening = sharpness > 0;
    d.sharpness = sharpness > 1 ? 1.f : sharpness;
    d.frameTimeDelta = frame_ms;
    d.preExposure = 1.0f;
    d.reset = reset != 0;
    d.cameraNear = znear;
    d.cameraFar = zfar;
    d.cameraFovAngleVertical = fov_y;
    d.viewSpaceToMetersFactor = 1.0f;   // as vk_tmp_ffx.cpp on Windows
    static int view = -1;   // MKW_FSR3_VIEW=1: the SDK's own debug view of its inputs and state in the output
    if (view < 0) { const char *v = cfg_get("MKW_FSR3_VIEW"); view = v && *v && strcmp(v, "0"); }
    if (view) d.flags |= FFX_FSR3UPSCALER_DISPATCH_DRAW_DEBUG_VIEW;
    FfxErrorCode e = ffxFsr3UpscalerContextDispatch(&x->ctx, &d);
    if (e != FFX_OK) {
        if (U.failed++ < 4) rt_log("fsr3: upscale dispatch failed (%d)", (int)e);
        return 0;
    }
    return 1;
}

extern "C" void vk_tmp_fsr3_sdk_shutdown(void) {
    int any = 0;
    for (int i = 0; i < 3; i++) any |= U.c[i] != nullptr;
    if (!any) return;
    vkDeviceWaitIdle(vk_ctx()->device);
    for (int i = 0; i < 3; i++)
        if (U.c[i]) ctx_free(U.c[i]), U.c[i] = nullptr;
}
#endif
