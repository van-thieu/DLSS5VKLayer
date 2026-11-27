#include "sl_host.h"
#include "logging.h"

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
    sl::FrameToken* token = nullptr;
    void* setOptionsFn = nullptr;
    bool inited = false;
    bool deviceSet = false;
};

SLHost* SLHostCreate(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                     const wchar_t* pluginDir, uint32_t applicationId)
{
    wchar_t dll[1024];
    swprintf(dll, 1024, L"%ls\\sl.interposer.dll", pluginDir);
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
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking;

    sl::Result r = h->init(pref, sl::kSDKVersion);
    sl::Result r2 = sl::Result::eErrorIO;
    SLPrint("[sl] slInit=%llu\n", (unsigned long long)r);
    if (r != sl::Result::eOk) { delete h; return nullptr; }
    h->inited = true;

    sl::FeatureRequirements req{};
    r = h->getRequirements(sl::kFeatureDLSS_NR, req);
    SLPrint("[sl] requirements=%llu flags=%llu maxViewports=%u\n",
            (unsigned long long)r, (unsigned long long)req.flags, req.maxNumViewports);

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
    r = h->getFunction(sl::kFeatureDLSS_NR, "slDLSSNRSetOptions", fn);
    h->setOptionsFn = fn;
    SLPrint("[sl] slDLSSNRSetOptions=%llu ptr=%p\n", (unsigned long long)r, fn);
    return h;
}

bool SLHostEvaluate(SLHost* h, VkCommandBuffer cmd,
                    VkImage color, VkImageView colorView, VkFormat colorFormat,
                    VkImage output, VkImageView outputView, VkFormat outputFormat,
                    VkImage mvec, VkImageView mvecView, VkFormat mvecFormat,
                    VkImage depth, VkImageView depthView, VkFormat depthFormat,
                    uint32_t width, uint32_t height)
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

    sl::ResourceTag tags[6];
    tags[0] = sl::ResourceTag(&rc, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow);
    tags[1] = sl::ResourceTag(&rc, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow);
    tags[2] = sl::ResourceTag(&ro, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow);
    tags[3] = sl::ResourceTag(&ro, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eOnlyValidNow);
    tags[4] = sl::ResourceTag(&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow);
    tags[5] = sl::ResourceTag(&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow);
    sl::ViewportHandle viewport(0);
    const sl::BaseStructure* inputs[] = { &viewport, &constants, &tags[0], &tags[1], &tags[2], &tags[3], &tags[4], &tags[5] };

    if (!h->token) {
        sl::Result r = h->getToken(h->token, nullptr);
        if (r != sl::Result::eOk || !h->token) {
            SLPrint("[sl] getToken=%llu\n", (unsigned long long)r);
            return false;
        }
    }

    if (h->setOptionsFn) {
        unsigned char options[512] = {};
        static bool tried = false;
        if (!tried) {
            tried = true;
            using A = sl::Result (*)(uint32_t, const void*);
            using B = sl::Result (*)(const sl::ViewportHandle&, const void*);
            using C = sl::Result (*)(const void*);
            Log("[sl] setOptions A(uint,ptr)=%llu", (unsigned long long)((A)h->setOptionsFn)(0u, options));
            Log("[sl] setOptions B(vp,ptr)=%llu", (unsigned long long)((B)h->setOptionsFn)(viewport, options));
            Log("[sl] setOptions C(ptr)=%llu", (unsigned long long)((C)h->setOptionsFn)(options));
        }
    }

    static bool logged = false;
    if (!logged) { Log("[sl] first slEvaluateFeature call on %ux%u", width, height); logged = true; }
    {
        sl::Result cr = h->setConstants(constants, *h->token, viewport);
        if (cr != sl::Result::eOk) { Log("[sl] setConstants=%llu", (unsigned long long)cr); return false; }
    }

    sl::Result r = h->evaluate(sl::kFeatureDLSS_NR, *h->token, inputs, 8, (sl::CommandBuffer*)cmd);
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
