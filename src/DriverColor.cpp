#include "DriverColor.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace pv {

namespace {

// ---- NVAPI (nvapi64.dll), queried by interface id ----
using NvQueryInterface = void*(__cdecl*)(unsigned int);
using NvInitialize = int(__cdecl*)();
using NvEnumDisplayHandle = int(__cdecl*)(unsigned int index, void** handle);
using NvGetDVCInfo = int(__cdecl*)(void* handle, unsigned int outputId, void* info);
using NvSetDVCLevel = int(__cdecl*)(void* handle, unsigned int outputId, int level);

constexpr unsigned int kNvInitialize = 0x0150E828;
constexpr unsigned int kNvEnumNvidiaDisplayHandle = 0x9ABDD40D;
constexpr unsigned int kNvGetDVCInfo = 0x4085DE45;
constexpr unsigned int kNvSetDVCLevel = 0x172409B4;

struct NvDvcInfo {
    unsigned int version;
    unsigned int currentLevel;
    unsigned int minLevel;
    unsigned int maxLevel;
};

// ---- ADL (atiadlxx.dll) ----
using AdlMalloc = void*(__stdcall*)(int);
using AdlMainControlCreate = int(__cdecl*)(AdlMalloc, int);
using AdlMainControlDestroy = int(__cdecl*)();
using AdlNumberOfAdapters = int(__cdecl*)(int*);
using AdlAdapterInfoGet = int(__cdecl*)(void*, int);
using AdlDisplayInfoGet = int(__cdecl*)(int, int*, void**, int);
using AdlColorGet = int(__cdecl*)(int, int, int, int*, int*, int*, int*, int*);
using AdlColorSet = int(__cdecl*)(int, int, int, int);

constexpr int kAdlOk = 0;
constexpr int kAdlColorSaturation = 1 << 2;
constexpr int kAdlDisplayConnected = 0x1;
constexpr int kAdlDisplayMapped = 0x2;

struct AdlAdapterInfo {
    int iSize;
    int iAdapterIndex;
    char strUDID[256];
    int iBusNumber;
    int iDeviceNumber;
    int iFunctionNumber;
    int iVendorID;
    char strAdapterName[256];
    char strDisplayName[256];
    int iPresent;
    int iExist;
    char strDriverPath[256];
    char strDriverPathExt[256];
    char strPNPString[256];
    int iOSDisplayIndex;
};

struct AdlDisplayID {
    int iDisplayLogicalIndex;
    int iDisplayPhysicalIndex;
    int iDisplayLogicalAdapterIndex;
    int iDisplayPhysicalAdapterIndex;
};

struct AdlDisplayInfo {
    AdlDisplayID displayID;
    int iDisplayControllerIndex;
    char strDisplayName[256];
    char strDisplayManufacturerName[256];
    int iDisplayType;
    int iDisplayOutputType;
    int iDisplayConnector;
    int iDisplayInfoMask;
    int iDisplayInfoValue;
};

void* __stdcall AdlAlloc(int size) { return std::malloc(static_cast<size_t>(size)); }

}  // namespace

struct DriverSaturation::Impl {
    enum class Vendor { None, Nvidia, Amd } vendor = Vendor::None;
    HMODULE dll = nullptr;

    // nvidia
    NvSetDVCLevel nvSet = nullptr;
    struct NvTarget {
        void* handle;
        int original, min, max;
    };
    std::vector<NvTarget> nvTargets;

    // amd
    AdlColorSet adlSet = nullptr;
    AdlMainControlDestroy adlDestroy = nullptr;
    struct AmdTarget {
        int adapter, display, original, def, min, max;
    };
    std::vector<AmdTarget> amdTargets;

    bool modified = false;

    bool InitNvidia() {
        dll = LoadLibraryW(L"nvapi64.dll");
        if (!dll) return false;
        auto query = reinterpret_cast<NvQueryInterface>(GetProcAddress(dll, "nvapi_QueryInterface"));
        if (!query) return false;
        auto init = reinterpret_cast<NvInitialize>(query(kNvInitialize));
        auto enumHandle = reinterpret_cast<NvEnumDisplayHandle>(query(kNvEnumNvidiaDisplayHandle));
        auto getInfo = reinterpret_cast<NvGetDVCInfo>(query(kNvGetDVCInfo));
        nvSet = reinterpret_cast<NvSetDVCLevel>(query(kNvSetDVCLevel));
        if (!init || !enumHandle || !getInfo || !nvSet || init() != 0) return false;

        for (unsigned int i = 0; i < 16; ++i) {
            void* h = nullptr;
            if (enumHandle(i, &h) != 0 || !h) break;
            NvDvcInfo info{};
            info.version = sizeof(NvDvcInfo) | (1u << 16);
            if (getInfo(h, 0, &info) != 0) continue;
            nvTargets.push_back({h, static_cast<int>(info.currentLevel), static_cast<int>(info.minLevel),
                                 static_cast<int>(info.maxLevel)});
        }
        if (nvTargets.empty()) return false;
        vendor = Vendor::Nvidia;
        return true;
    }

    bool InitAmd() {
        dll = LoadLibraryW(L"atiadlxx.dll");
        if (!dll) return false;
        auto create = reinterpret_cast<AdlMainControlCreate>(GetProcAddress(dll, "ADL_Main_Control_Create"));
        adlDestroy = reinterpret_cast<AdlMainControlDestroy>(GetProcAddress(dll, "ADL_Main_Control_Destroy"));
        auto count = reinterpret_cast<AdlNumberOfAdapters>(GetProcAddress(dll, "ADL_Adapter_NumberOfAdapters_Get"));
        auto adapterInfo = reinterpret_cast<AdlAdapterInfoGet>(GetProcAddress(dll, "ADL_Adapter_AdapterInfo_Get"));
        auto displayInfo = reinterpret_cast<AdlDisplayInfoGet>(GetProcAddress(dll, "ADL_Display_DisplayInfo_Get"));
        auto colorGet = reinterpret_cast<AdlColorGet>(GetProcAddress(dll, "ADL_Display_Color_Get"));
        adlSet = reinterpret_cast<AdlColorSet>(GetProcAddress(dll, "ADL_Display_Color_Set"));
        if (!create || !adlDestroy || !count || !adapterInfo || !displayInfo || !colorGet || !adlSet) return false;
        if (create(AdlAlloc, 1) != kAdlOk) {
            adlDestroy = nullptr;
            return false;
        }

        int n = 0;
        if (count(&n) != kAdlOk || n <= 0) return false;
        std::vector<AdlAdapterInfo> adapters(static_cast<size_t>(n));
        for (auto& a : adapters) a.iSize = sizeof(AdlAdapterInfo);
        if (adapterInfo(adapters.data(), static_cast<int>(sizeof(AdlAdapterInfo) * adapters.size())) != kAdlOk) return false;

        for (const auto& a : adapters) {
            int numDisplays = 0;
            void* raw = nullptr;
            if (displayInfo(a.iAdapterIndex, &numDisplays, &raw, 0) != kAdlOk || !raw) continue;
            auto* displays = static_cast<AdlDisplayInfo*>(raw);
            for (int d = 0; d < numDisplays; ++d) {
                const AdlDisplayInfo& di = displays[d];
                const int need = kAdlDisplayConnected | kAdlDisplayMapped;
                if ((di.iDisplayInfoValue & need) != need) continue;
                if (di.displayID.iDisplayLogicalAdapterIndex != a.iAdapterIndex) continue;
                int cur = 0, def = 0, mn = 0, mx = 0, step = 0;
                if (colorGet(a.iAdapterIndex, di.displayID.iDisplayLogicalIndex, kAdlColorSaturation, &cur, &def, &mn,
                             &mx, &step) != kAdlOk)
                    continue;
                const bool dup = std::any_of(amdTargets.begin(), amdTargets.end(), [&](const AmdTarget& t) {
                    return t.adapter == a.iAdapterIndex && t.display == di.displayID.iDisplayLogicalIndex;
                });
                if (!dup) amdTargets.push_back({a.iAdapterIndex, di.displayID.iDisplayLogicalIndex, cur, def, mn, mx});
            }
            std::free(raw);
        }
        if (amdTargets.empty()) return false;
        vendor = Vendor::Amd;
        return true;
    }

    void Unload() {
        if (adlDestroy) adlDestroy();
        adlDestroy = nullptr;
        if (dll) FreeLibrary(dll);
        dll = nullptr;
        nvTargets.clear();
        amdTargets.clear();
        vendor = Vendor::None;
    }
};

DriverSaturation::DriverSaturation() : m(std::make_unique<Impl>()) {
    if (!m->InitNvidia()) {
        m->Unload();
        if (!m->InitAmd()) m->Unload();
    }
}

DriverSaturation::~DriverSaturation() {
    Restore();
    m->Unload();
}

bool DriverSaturation::Available() const noexcept { return m->vendor != Impl::Vendor::None; }

bool DriverSaturation::SupportsDesaturate() const noexcept { return m->vendor == Impl::Vendor::Amd; }

const char* DriverSaturation::Name() const noexcept {
    switch (m->vendor) {
        case Impl::Vendor::Nvidia: return "NVIDIA Digital Vibrance";
        case Impl::Vendor::Amd: return "AMD Saturation";
        default: return "none";
    }
}

bool DriverSaturation::Set(float saturation) noexcept {
    if (!Available()) return false;
    if (std::fabs(saturation - 1.0f) < 1e-4f) {
        Restore();
        return true;
    }
    bool ok = true;
    if (m->vendor == Impl::Vendor::Nvidia) {
        // DVC level: min = driver default (50% in NVCP), max = 100%. Map 1.0x..2.0x onto it.
        for (const auto& t : m->nvTargets) {
            const float k = std::clamp(saturation - 1.0f, 0.0f, 1.0f);
            const int level = t.min + static_cast<int>(std::lround(k * static_cast<float>(t.max - t.min)));
            ok &= m->nvSet(t.handle, 0, level) == 0;
        }
    } else {
        // ADL saturation: default is usually 100 on a 0..200 scale
        for (const auto& t : m->amdTargets) {
            const int value = std::clamp(static_cast<int>(std::lround(t.def * saturation)), t.min, t.max);
            ok &= m->adlSet(t.adapter, t.display, kAdlColorSaturation, value) == kAdlOk;
        }
    }
    m->modified = true;
    return ok;
}

void DriverSaturation::Restore() noexcept {
    if (!m->modified) return;
    for (const auto& t : m->nvTargets) m->nvSet(t.handle, 0, t.original);
    for (const auto& t : m->amdTargets) m->adlSet(t.adapter, t.display, kAdlColorSaturation, t.original);
    m->modified = false;
}

}  // namespace pv
