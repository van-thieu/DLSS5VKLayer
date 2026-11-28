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
    sl::FrameToken* token = nullptr;
    void* setOptionsFn = nullptr;
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
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                   sl::PreferenceFlags::eUseFrameBasedResourceTagging;

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

    // Brute-force the private DLSSNROptions StructType GUID from the plugin's .rdata.
    // No v4 filter: the constant may not follow the UUID version convention. Try every 16-byte
    // window and every plausible signature, guarded, and keep only a clean eOk result.
    if (fn) {
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
        if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>((const unsigned char*)mod + dos->e_lfanew);
            if (nt && nt->Signature == IMAGE_NT_SIGNATURE) {
                const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
                for (uint32_t si = 0; si < nt->FileHeader.NumberOfSections; ++si, ++sec) {
                    if (strncmp((const char*)sec->Name, ".rdata", 6) != 0) continue;
                    const unsigned char* base = (const unsigned char*)mod + sec->VirtualAddress;
                    const size_t size = sec->Misc.VirtualSize;
                    using A = sl::Result (*)(uint32_t, const void*);
                    using B = sl::Result (*)(const sl::ViewportHandle&, const void*);
                    using C = sl::Result (*)(const void*);
                    int tried = 0;
                    for (size_t off = 0; off + 16 <= size && tried < 6000; off += 4) {
                        const unsigned char* g = base + off;
                        unsigned char options[512] = {};
                        *(uint64_t*)(options + 0x00) = 0;            // BaseStructure::next
                        memcpy(options + 0x08, g, 16);                // StructType GUID
                        *(uint64_t*)(options + 0x18) = 1;             // structVersion
                        *(uint32_t*)(options + 0xd8) = 1920;
                        *(uint32_t*)(options + 0xdc) = 1080;
                        *(uint32_t*)(options + 0xe0) = 1920;
                        *(uint32_t*)(options + 0xe4) = 1080;
                        *(float*)(options + 0xf8) = 1.0f;
                        *(float*)(options + 0xfc) = 1.0f;
                        *(float*)(options + 0x100) = 1.0f;
                        *(float*)(options + 0x114) = -1.0f;
                        options[0x122] = 1;
                        sl::ViewportHandle vp(0);
                        for (int sig = 0; sig < 3 && !h->hasOptionsGuid; ++sig) {
                            DWORD seh = 0;
                            const sl::Result rr = Guarded<sl::Result>([&]() -> sl::Result {
                                if (sig == 0) return ((A)fn)(0u, options);
                                if (sig == 1) return ((B)fn)(vp, options);
                                return ((C)fn)(options);
                            }, sl::Result::eErrorIO, &seh);
                            ++tried;
                            if (seh == 0 && rr == sl::Result::eOk) {
                                Log("[sl] DLSSNROptions GUID found at rva=%llx sig=%d result=%llu bytes=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                                    (unsigned long long)(sec->VirtualAddress + off), sig, (unsigned long long)rr,
                                    g[0],g[1],g[2],g[3],g[4],g[5],g[6],g[7],g[8],g[9],g[10],g[11],g[12],g[13],g[14],g[15]);
                                memcpy(h->optionsGuid, g, 16);
                                h->hasOptionsGuid = true;
                            }
                        }
                    }
                    SLPrint("[sl] GUID probe tried=%d found=%d\n", tried, (int)h->hasOptionsGuid);
                }
            }
        }
    }

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

    // Try the plugin's private options entry point with a populated DLSSNROptions buffer.
    // The StructType GUID is guessed from the two v4 UUIDs in the plugin's .rdata; select with
    // SL_NR_GUID=0/1. Nothing is written unless SL_NR_OPTIONS=1.
    if (h->setOptionsFn) {
        static bool triedOptions = false;
        if (!triedOptions) {
            triedOptions = true;
            char flag[8] = {};
            if (GetEnvironmentVariableA("SL_NR_OPTIONS", flag, sizeof(flag)) > 0 && flag[0] == '1') {
                static const unsigned char guids[2][16] = {
                    {0xDD,0x65,0xB8,0xA8,0x3D,0x2E,0x94,0x40,0xAD,0x97,0xE5,0x93,0xA7,0x0C,0x75,0xD6},
                    {0x82,0x5D,0xAB,0x62,0xC1,0xFD,0xC3,0x4D,0xA9,0xDD,0x07,0x0D,0x1D,0x49,0x5D,0x97},
                };
                char gsel[8] = {};
                int gi = GetEnvironmentVariableA("SL_NR_GUID", gsel, sizeof(gsel)) > 0 ? atoi(gsel) : 0;
                if (gi < 0 || gi > 1) gi = 0;
                unsigned char options[512] = {};
                memcpy(options, guids[gi], 16);
                *(uint32_t*)(options + 16) = 1;                 // structVersion
                *(uint32_t*)(options + 0xd0) = 0;               // subrect baseX
                *(uint32_t*)(options + 0xd4) = 0;               // subrect baseY
                *(uint32_t*)(options + 0xd8) = width;
                *(uint32_t*)(options + 0xdc) = height;
                *(uint32_t*)(options + 0xe0) = width;
                *(uint32_t*)(options + 0xe4) = height;
                *(float*)(options + 0xf8) = 1.0f;               // intensity
                *(float*)(options + 0xfc) = 1.0f;               // local tone
                *(float*)(options + 0x100) = 1.0f;              // local structure
                *(uint32_t*)(options + 0x108) = 0;              // style
                options[0x110] = 0;                             // auto mask
                *(float*)(options + 0x114) = -1.0f;             // skin structure
                *(uint32_t*)(options + 0x11c) = 0;              // mask blend
                options[0x122] = 1;                             // UI correction
                using SetOptions = sl::Result (*)(const sl::ViewportHandle&, const void*);
                DWORD seh = 0;
                sl::Result rr = Guarded([&] { return ((SetOptions)h->setOptionsFn)(viewport, options); },
                                        sl::Result::eErrorIO, &seh);
                SLPrint("[sl] slDLSSNRSetOptions guid %d -> %llu seh=%#x\n", gi,
                        (unsigned long long)rr, seh);
            }
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
