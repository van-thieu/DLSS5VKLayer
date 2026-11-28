#include "sl_host.h"
#include "logging.h"
#include "guard.h"

#include <sl.h>
#include <sl_core_api.h>
#include <sl_helpers_vk.h>

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace dlssnr {

static void SLPrint(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("%s", buf);
}

struct SLHost {
    HMODULE module = nullptr;
    PFun_slInit* init = nullptr;
    PFun_slGetFeatureRequirements* getRequirements = nullptr;
    PFun_slGetFeatureFunction* getFunction = nullptr;
    PFun_slGetNewFrameToken* getToken = nullptr;
    PFun_slEvaluateFeature* evaluate = nullptr;
    PFun_slSetVulkanInfo* setVulkanInfo = nullptr;
    PFun_slSetConstants* setConstants = nullptr;
    PFun_slIsFeatureSupported* isSupported = nullptr;
    PFun_slSetFeatureLoaded* setFeatureLoaded = nullptr;
    sl::FrameToken* token = nullptr;
    void* setOptionsFn = nullptr;
    void* presentCommonFn = nullptr;
    unsigned char optionsGuid[16] = {};
    bool hasOptionsGuid = false;
    bool inited = false;
    bool deviceSet = false;
};

SLHost* SLHostCreate(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                     const wchar_t* pluginDir, uint32_t applicationId)
{
    wchar_t dll[1024];
    swprintf(dll, 1024, L"%ls\\sl.interposer.dll", pluginDir);
    DWORD gslSeh = 0;
    HMODULE mod = LoadLibraryW(dll);
    SLPrint("[sl] interposer load=%p err=%lu\n", (void*)mod, GetLastError());
    if (!mod) return nullptr;

    SLHost* h = new SLHost();
    h->module = mod;
    h->init = reinterpret_cast<PFun_slInit*>(reinterpret_cast<void*>(GetProcAddress(mod, "slInit")));
    h->getRequirements = reinterpret_cast<PFun_slGetFeatureRequirements*>(reinterpret_cast<void*>(GetProcAddress(mod, "slGetFeatureRequirements")));
    h->getFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(reinterpret_cast<void*>(GetProcAddress(mod, "slGetFeatureFunction")));
    h->getToken = reinterpret_cast<PFun_slGetNewFrameToken*>(reinterpret_cast<void*>(GetProcAddress(mod, "slGetNewFrameToken")));
    h->evaluate = reinterpret_cast<PFun_slEvaluateFeature*>(reinterpret_cast<void*>(GetProcAddress(mod, "slEvaluateFeature")));
    h->setVulkanInfo = reinterpret_cast<PFun_slSetVulkanInfo*>(reinterpret_cast<void*>(GetProcAddress(mod, "slSetVulkanInfo")));
    h->setConstants = reinterpret_cast<PFun_slSetConstants*>(reinterpret_cast<void*>(GetProcAddress(mod, "slSetConstants")));
    h->isSupported = reinterpret_cast<PFun_slIsFeatureSupported*>(reinterpret_cast<void*>(GetProcAddress(mod, "slIsFeatureSupported")));
    h->setFeatureLoaded = reinterpret_cast<PFun_slSetFeatureLoaded*>(reinterpret_cast<void*>(GetProcAddress(mod, "slSetFeatureLoaded")));
    if (!h->init || !h->getRequirements || !h->getFunction || !h->getToken ||
        !h->evaluate || !h->setVulkanInfo || !h->setConstants) {
        SLPrint("[sl] missing exports\n");
        delete h;
        return nullptr;
    }

    sl::Preferences pref{};
    sl::Feature features[] = { sl::kFeatureDLSS_NR };
    const wchar_t* paths[] = { pluginDir, nullptr };
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 1;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.applicationId = applicationId;
    pref.renderAPI = sl::RenderAPI::eVulkan;
    pref.showConsole = true;
    pref.logLevel = sl::LogLevel::eVerbose;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking;

    sl::Result r = h->init(pref, sl::kSDKVersion);
    sl::Result r2 = sl::Result::eErrorIO;
    SLPrint("[sl] slInit=%llu\n", (unsigned long long)r);
    if (r != sl::Result::eOk) { delete h; return nullptr; }
    h->inited = true;
    if (h->setFeatureLoaded) {
        sl::Result lr = h->setFeatureLoaded(sl::kFeatureDLSS_NR, true);
        SLPrint("[sl] setFeatureLoaded=%llu\n", (unsigned long long)lr);
    }

    sl::FeatureRequirements req{};
    r = h->getRequirements(sl::kFeatureDLSS_NR, req);
    SLPrint("[sl] requirements=%llu flags=%llu maxViewports=%u requiredTags=%u\n",
            (unsigned long long)r, (unsigned long long)req.flags, req.maxNumViewports, req.numRequiredTags);
    SLPrint("[sl] requiredTags ptr=%p\n", (void*)req.requiredTags);
    if (req.requiredTags)
        for (uint32_t ti = 0; ti < req.numRequiredTags; ++ti)
            SLPrint("[sl] requiredTag[%u]=%d\n", ti, (int)req.requiredTags[ti]);

    sl::VulkanInfo vi{};
    vi.instance = instance;
    vi.physicalDevice = physical;
    vi.device = device;
    vi.graphicsQueueFamily = 0;
    vi.graphicsQueueIndex = 0;
    vi.computeQueueFamily = 0;
    vi.computeQueueIndex = 0;
    r = h->setVulkanInfo(vi);
    SLPrint("[sl] slSetVulkanInfo=%llu\n", (unsigned long long)r);
    h->deviceSet = (r == sl::Result::eOk);

    // Plugin-only options entry point; layout is private, so only report availability.
    void* fn = nullptr;
    void* setData = nullptr;
    r2 = h->getFunction(sl::kFeatureDLSS_NR, "slSetData", setData);
    Log("[sl] slSetData=%llu ptr=%p module=%p rva=%llx", (unsigned long long)r2, setData, (void*)mod,
        setData ? (unsigned long long)((unsigned char*)setData - (unsigned char*)mod) : 0);
    void* commonFn = nullptr;
    // The common plugin exposes presentCommon through its own slGetPluginFunction(name), which
    // returns the function pointer directly in RAX. slGetFeatureFunction(kFeatureCommon, ...) is
    // not the route for it.
    HMODULE common = GetModuleHandleA("sl.common.dll");
    SLPrint("[sl] common module=%p\n", (void*)common);
    if (common) {
        using GetPluginFn = void* (*)(const char*);
        if (auto* getFn = reinterpret_cast<GetPluginFn>(GetProcAddress(common, "slGetPluginFunction"))) {
            commonFn = Guarded<void*>([&] { return getFn("presentCommon"); }, nullptr, &gslSeh);
        }
    }
    h->presentCommonFn = commonFn;
    SLPrint("[sl] presentCommon ptr=%p\n", commonFn);
    r = h->getFunction(sl::kFeatureDLSS_NR, "slDLSSNRSetOptions", fn);
    h->setOptionsFn = fn;
    SLPrint("[sl] slDLSSNRSetOptions=%llu ptr=%p\n", (unsigned long long)r, fn);

    return h;
}

static void ResolvePresentCommon(SLHost* h)
{
    if (h->presentCommonFn) return;
    using GetPluginFn = void* (*)(const char*);
    static const char* modules[] = { "sl.common.dll", "sl.interposer.dll", "sl.dlss_nr.dll" };
    static const char* names[] = { "presentCommon", "slPresentCommon", "presentCommon()", "slCommonPresent" };
    DWORD seh = 0;
    for (const char* modName : modules) {
        HMODULE common = GetModuleHandleA(modName);
        if (!common) continue;
        auto* getFn = reinterpret_cast<GetPluginFn>(GetProcAddress(common, "slGetPluginFunction"));
        if (!getFn) continue;
        for (const char* name : names) {
            void* fn = Guarded<void*>([&] { return getFn(name); }, nullptr, &seh);
            if (fn && !seh) {
                h->presentCommonFn = fn;
                SLPrint("[sl] presentCommon resolved from %s as '%s' -> %p\n", modName, name, fn);
                return;
            }
        }
    }
}

bool SLHostEvaluate(SLHost* h, VkCommandBuffer cmd,
                    VkImage color, VkImageView colorView, VkFormat colorFormat,
                    VkImage output, VkImageView outputView, VkFormat outputFormat,
                    VkImage mvec, VkImageView mvecView, VkFormat mvecFormat,
                    VkImage depth, VkImageView depthView, VkFormat depthFormat,
                    uint32_t width, uint32_t height, const SLNrOptions& nr)
{
    if (!h || !h->deviceSet) return false;

    sl::Resource rc(sl::ResourceType::eTex2d, (void*)color, nullptr, (void*)colorView,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sl::Resource ro(sl::ResourceType::eTex2d, (void*)output, nullptr, (void*)outputView,
                    VK_IMAGE_LAYOUT_GENERAL);
    sl::Resource rm(sl::ResourceType::eTex2d, (void*)mvec, nullptr, (void*)mvecView,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sl::Resource rd(sl::ResourceType::eTex2d, (void*)depth, nullptr, (void*)depthView,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    auto fill = [&](sl::Resource& r, VkFormat f) {
        r.width = width;
        r.height = height;
        r.nativeFormat = (uint32_t)f;
        r.mipLevels = 1;
        r.arrayLayers = 1;
        r.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    };
    fill(rc, colorFormat);
    fill(ro, outputFormat);
    fill(rm, mvecFormat);
    fill(rd, depthFormat);

    // Streamline requires a complete common constants block every frame. The layer has no
    // camera matrices, so provide a neutral identity/view contract with correct motion scale.
    sl::Constants constants{};
    auto identity = [](sl::float4x4& m) {
        memset(&m, 0, sizeof(m));
        m.row[0].x = m.row[1].y = m.row[2].z = m.row[3].w = 1.0f;
    };
    identity(constants.cameraViewToClip);
    identity(constants.clipToCameraView);
    identity(constants.clipToLensClip);
    identity(constants.clipToPrevClip);
    identity(constants.prevClipToClip);
    constants.jitterOffset = sl::float2(0.0f, 0.0f);
    constants.mvecScale = sl::float2(1.0f / width, 1.0f / height);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(0.0f, 0.0f, 0.0f);
    constants.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
    constants.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
    constants.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
    constants.cameraNear = 0.1f;
    constants.cameraFar = 1000.0f;
    constants.cameraFOV = 1.0f;
    constants.cameraAspectRatio = float(width) / float(height);
    constants.motionVectorsInvalidValue = 0.0f;
    constants.depthInverted = sl::Boolean::eTrue;
    constants.cameraMotionIncluded = sl::Boolean::eFalse;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;

    sl::ResourceTag tags[4];
    tags[0] = sl::ResourceTag(&rc, sl::kBufferTypeUpliftInputColor, sl::ResourceLifecycle::eOnlyValidNow);
    tags[1] = sl::ResourceTag(&ro, sl::kBufferTypeUpliftOutputColor, sl::ResourceLifecycle::eOnlyValidNow);
    tags[2] = sl::ResourceTag(&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow);
    tags[3] = sl::ResourceTag(&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow);
    ResolvePresentCommon(h);
    if (h->presentCommonFn) {
        DWORD seh = 0;
        using PresentFn = void (*)();
        Guarded([&] {
            reinterpret_cast<PresentFn>(h->presentCommonFn)();
            return true;
        }, false, &seh);
        if (seh) SLPrint("[sl] presentCommon seh=%#x\n", seh);
    }
    sl::ViewportHandle viewport(0);
    const sl::BaseStructure* inputs[] = { &viewport, &constants, &tags[0], &tags[1], &tags[2], &tags[3] };

    h->token = nullptr;
    {
        sl::Result r = h->getToken(h->token, nullptr);
        if (r != sl::Result::eOk || !h->token) {
            SLPrint("[sl] getToken=%llu\n", (unsigned long long)r);
            return false;
        }
    }

    // The plugin's private DLSSNROptions entry point. The StructType GUID below is the second
    // hardcoded 16-byte constant in slSetData (the first belongs to the common constants block).
    // This call must happen before every evaluate with the same frame token and viewport.
    if (h->setOptionsFn) {
        static const unsigned char kDlssNrOptionsGuid[16] = {
            0xE0,0xDF,0xDF,0x29,0x3A,0x27,0x72,0x4E,0xB4,0x92,0x2D,0xC8,0x23,0xD5,0xB1,0xAD,
        };
        unsigned char options[0x48] = {};
        *(uint64_t*)(options + 0x00) = 0;                       // BaseStructure::next
        memcpy(options + 0x08, kDlssNrOptionsGuid, 16);          // StructType
        char verEnv[8] = {};
        const uint64_t version = GetEnvironmentVariableA("SL_NR_VERSION", verEnv, sizeof(verEnv)) > 0
                                     ? (uint64_t)atoi(verEnv) : 1ull;
        *(uint64_t*)(options + 0x18) = version;                  // structVersion
        *(double*)(options + 0x20) = nr.intensity;
        using SetOptions = sl::Result (*)(const sl::ViewportHandle&, const void*);
        DWORD seh = 0;
        const sl::Result rr = Guarded<sl::Result>([&]() -> sl::Result {
            return ((SetOptions)h->setOptionsFn)(viewport, options);
        }, sl::Result::eErrorIO, &seh);
        if (rr != sl::Result::eOk || seh) SLPrint("[sl] slDLSSNRSetOptions=%llu seh=%#x\n",
                                                  (unsigned long long)rr, seh);
    }

    static bool logged = false;
    if (!logged) { Log("[sl] first slEvaluateFeature call on %ux%u", width, height); logged = true; }
    {
        sl::Result cr = h->setConstants(constants, *h->token, viewport);
        if (cr != sl::Result::eOk) { Log("[sl] setConstants=%llu", (unsigned long long)cr); return false; }
    }

    sl::Result r = h->evaluate(sl::kFeatureDLSS_NR, *h->token, inputs, 6, (sl::CommandBuffer*)cmd);
    if (r != sl::Result::eOk) SLPrint("[sl] evaluate=%llu\n", (unsigned long long)r);
    return r == sl::Result::eOk;
}

void SLHostDestroy(SLHost* h)
{
    if (!h) return;
    if (h->module) FreeLibrary(h->module);
    delete h;
}

}  // namespace dlssnr
