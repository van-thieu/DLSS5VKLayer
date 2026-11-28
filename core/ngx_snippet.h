#pragma once
#include "ngx_abi.h"
#include <windows.h>
#include <string>
#include <vulkan/vulkan.h>
#include "../common/shm_protocol.h"
#include "sl_host.h"

namespace dlssnr {

// Owns the nvngx_dlssnr.dll snippet lifecycle: caller-identity IAT spoof,
// core (nvngx.dll) init, parameter block, Feature 18 create/evaluate/teardown.
// All NGX calls are SEH-guarded; any failure latches `disabled` (pass-through).
struct NgxSnippet {
    HMODULE snippet = nullptr;
    HMODULE core = nullptr;
    HMODULE nvapi = nullptr;

    FnVkInitExt initExt = nullptr;
    FnVkInitExt initExt2 = nullptr;
    FnVkInitExt initPlain = nullptr;
    FnVkCreateFeature createFeature = nullptr;
    FnVkEvaluateFeature evaluateFeature = nullptr;
    FnVkReleaseFeature releaseFeature = nullptr;
    FnVkShutdown1 shutdown1 = nullptr;

    NVSDK_NGX_Parameter* params = nullptr;
    FnVkDestroyParameters paramsDestroy = nullptr;
    bool ownParams = false;
    // Last tuning written before evaluate. The Streamline host needs the same values to fill
    // DLSSNROptions before slEvaluateFeature; storing them here keeps its call site unchanged.
    float nrIntensity = 1.0f;
    float nrLocalTone = 1.0f;
    float nrLocalStructure = 1.0f;
    float nrSkinStructure = -1.0f;
    float nrGlobalTone = 1.0f;
    uint32_t nrStyle = 0;
    uint32_t nrPreset = 0;
    uint32_t nrAutoMask = 0;
    // One NGX feature per pass, not one feature evaluated repeatedly.
    //
    // A feature carries its own temporal history, and the passes are meant to be a chain of distinct
    // models over one frame rather than the same model shown its own output. Sharing a handle meant
    // every pass in a frame wrote into one history, which is not what "run the model N times" means.
    NVSDK_NGX_Handle* features[kMaxPasses] = {};
    uint32_t featureCount = 0;
    uint32_t featureW = 0, featureH = 0;

    bool ready = false;     // snippet init + CreateFeature(18) succeeded
    bool disabled = false;  // latched failure -> pass-through forever

    // Captured from NVSDK_NGX_VULKAN_GetFeatureRequirements (bit 0 = HDR path).
    unsigned int featureFlags = 0;
    bool hdrCapable = false;
    // Whether the feature is being built as HDR. The helper sets it before the first create and on
    // every switch; NgxCreatePass rewrites the create flags and the tonemap hint from it, so a
    // rebuild after a toggle carries the new contract without re-running init.
    bool hdrActive = false;

    // Last values the evaluate contract reported, so it says something only when it changes.
    unsigned int loggedAutoMask = 0xFFFFFFFFu;
    float loggedMVecScaleX = -1.0f, loggedMVecScaleY = -1.0f;
    bool loggedDepthBound = false;

    // Caller-identity spoof state
    void** iatSlot = nullptr;
    decltype(&GetModuleFileNameW) originalGetModuleFileNameW = nullptr;

    // Resource structs passed to the DLL (must outlive eval calls)
    NVSDK_NGX_Resource_VK resColor{}, resOut{}, resMV{}, resDepth{};

    std::wstring binDir;

    // Optional Streamline host path: drives WWM's sl.dlss_nr.dll instead of raw NGX.
    SLHost* slHost = nullptr;
    bool useStreamline = false;
};

// Layer module handle (set in DllMain), used as the spoofed caller identity.
extern HMODULE g_layerModule;


// What the model latches when a feature is built. Writing any of it at evaluate time does nothing at
// all, which is why every one of these controls appeared to be dead: the model reads them once, here.
struct NgxTuning {
    float intensity = 1.0f;
    float localTone = 1.0f;
    float localStructure = 1.0f;
    float skinStructure = -1.0f;
    float globalTone = 1.0f;
    uint32_t style = 0;
    uint32_t preset = 0;
    uint32_t autoMask = 1;
    uint32_t perfMode = 0u;
    uint32_t maskBlend = 0u;
    uint32_t modelMask = 0xFFFFFFFFu;

    bool operator==(const NgxTuning& o) const {
        return intensity == o.intensity && localTone == o.localTone &&
               localStructure == o.localStructure && skinStructure == o.skinStructure &&
               globalTone == o.globalTone && style == o.style && preset == o.preset &&
               autoMask == o.autoMask && perfMode == o.perfMode &&
               maskBlend == o.maskBlend && modelMask == o.modelMask;
    }
    bool operator!=(const NgxTuning& o) const { return !(*this == o); }
};

// Writes the create-time block. Must be called before NgxCreatePass, never instead of it.
void NgxSetCreateTuning(NgxSnippet& s, const NgxTuning& t);
// Writes the same tuning into the block again before every evaluate. The reference Vulkan
// forwarder does both: a feature reads the values when it is built, and the running feature reads
// them from the parameter block on each dispatch. Doing only the first made the model sliders look
// wired in the log while the picture never answered them.
void NgxSetEvaluateTuning(NgxSnippet& s, const NgxTuning& t);
void NgxSetHdr(NgxSnippet& s, bool want);

// Loads the snippet, initialises it, and builds pass 0.
//
// The tuning is passed in rather than set by the caller beforehand because this function writes its
// own create contract -- preset among it -- and would otherwise overwrite whatever the caller had
// just chosen. It is applied last, immediately before the feature is built.
bool NgxLoadAndInit(NgxSnippet& s, VkInstance instance, VkPhysicalDevice pd, VkDevice device,
                    uint32_t width, uint32_t height, VkCommandBuffer recordingCmd,
                    const NgxTuning& tuning,
                    float mvecScaleX = 1.0f, float mvecScaleY = 1.0f);

bool NgxCreatePass(NgxSnippet& s, uint32_t pass, uint32_t width, uint32_t height,
                   VkCommandBuffer recordingCmd);
void NgxReleasePass(NgxSnippet& s, uint32_t pass, VkDevice device);
void NgxReleaseAllPasses(NgxSnippet& s, VkDevice device);
void NgxSetResources(NgxSnippet& s, const NVSDK_NGX_Resource_VK& color,
                     const NVSDK_NGX_Resource_VK& out, const NVSDK_NGX_Resource_VK& mv,
                     const NVSDK_NGX_Resource_VK& depth, uint32_t width, uint32_t height);
void NgxSetReset(NgxSnippet& s, bool reset, bool logValue = false);
// The one strength the model reads at evaluate rather than at create, so it follows the setting
// without a rebuild. The others were removed from this interface deliberately: writing them here did
// nothing at all, which is what made every one of them look like a control that was simply ignored.
// They live in NgxTuning and are read when the feature is built.
void NgxSetSharpness(NgxSnippet& s, float sharpness);
// How the motion field's units are read. Written every evaluate, because it goes with the field.
void NgxSetMotionScale(NgxSnippet& s, float scaleX, float scaleY);
bool NgxEvaluatePass(NgxSnippet& s, uint32_t pass, VkCommandBuffer recordingCmd);
void NgxTeardown(NgxSnippet& s, VkDevice device);

}  // namespace dlssnr