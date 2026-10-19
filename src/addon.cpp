// FallenOrderTweaks: a ReShade add-on for Star Wars Jedi: Fallen Order (UE 4.21, DirectX 11)
// that sets the game's console variables from the ReShade overlay, so there is no Engine.ini
// to edit. Jedi: Survivor reads the same %LOCALAPPDATA%\SwGame config folder, which is why the
// settings live here instead. See docs/FINDINGS.md.
//
// Saved settings (ReShade.ini, [FallenOrderTweaks], applied at every start):
//   Upscaling  the render resolution DLSS (Luma) upscales from: DLAA / Quality / Balanced /
//              Performance / Ultra Performance, i.e. r.ScreenPercentage with TAA upsampling.
//              Luma replaces the game's TAA with DLSS but follows the game's resolution, so
//              without this it always runs DLAA.
//   Detail     texture streaming pool by video memory, 16x anisotropic filtering, sharper and
//              further shadows, further view distance, and top-quality screen space reflections
//   Lighting   more SSAO levels, finer volumetric fog, full-resolution capsule shadows and
//              high-quality skin scattering, each shown next to the game's own value
//
// What it records, next to the game executable:
//   FallenOrderTweaks.log  the console manager scan, the layout check, and every value it
//                          forces or hands back

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "cvars.hpp"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = FOT_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Sets " FOT_GAME "'s graphics from the ReShade overlay: the DLSS mode for Luma, and sharper shadows, "
    "textures and view distance.";

namespace {

using namespace reshade::api;

const ImVec4 kWarn(1.0f, 0.75f, 0.3f, 1.0f);
constexpr const char* kConfigSection = FOT_PRODUCT;

// ---------------------------------------------------------------- logging

std::mutex g_mutex;  // present (render thread) and overlay
FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fflush(g_log);
    }
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring dir(path, n);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    std::wstring log = dir + L"" FOT_PRODUCT ".log";
    std::wstring prev = dir + L"" FOT_PRODUCT ".prev.log";
    // ReShade may load and unload the add-on more than once at startup, so only the
    // first load in a process starts a new log.
    if (GetEnvironmentVariableW(L"FOT_LOG_OPEN", nullptr, 0) == 0) {
        SetEnvironmentVariableW(L"FOT_LOG_OPEN", L"1");
        MoveFileExW(log.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    g_log = _wfopen(log.c_str(), L"a");
}

// ---------------------------------------------------------------- executable image

constexpr uint32_t kKnownTimestamp = FOT_KNOWN_PE_TIMESTAMP;

const uint8_t* g_base = nullptr;
uint32_t g_imageSize = 0;
uint32_t g_timestamp = 0;
cvars::Section g_text, g_rdata;

void LogGameInfo() {
    g_base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    g_imageSize = nt->OptionalHeader.SizeOfImage;
    g_timestamp = nt->FileHeader.TimeDateStamp;
    cvars::FindSection(g_base, ".text", g_text);
    cvars::FindSection(g_base, ".rdata", g_rdata);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    Log("%s %s", FOT_PRODUCT, FOT_VERSION);
    Log("exe: %ls", path);
    Log("exe: PE timestamp 0x%08x (%s), image size 0x%x", g_timestamp,
        g_timestamp == kKnownTimestamp ? "the build the layouts come from" : "a different build than the layouts",
        g_imageSize);
}

std::string Where(const void* p) {
    auto b = static_cast<const uint8_t*>(p);
    if (!g_base || b < g_base || b >= g_base + g_imageSize) return "heap";
    char buf[48];
    snprintf(buf, sizeof(buf), "exe+0x%llx", static_cast<unsigned long long>(b - g_base));
    return buf;
}

// ---------------------------------------------------------------- console manager

void* g_manager = nullptr;
bool g_unsupported = false;  // the scan or the layout check failed: nothing is ever set
std::string g_status = "waiting for the game";

void Unsupported(const char* why) {
    g_unsupported = true;
    g_status = why;
    Log("cvars: %s - not changing anything", why);
}

bool IsFloat(float a, float b) { return fabsf(a - b) <= 1e-4f * std::max(1.0f, fabsf(b)); }

// Checks the vtable offsets and storage against two cvars every build has, before anything is
// set: r.ScreenPercentage (float) and r.MaxAnisotropy (int).
bool CheckLayout() {
    void* sp = cvars::FindVariable(g_manager, L"r.ScreenPercentage");
    void* af = cvars::FindVariable(g_manager, L"r.MaxAnisotropy");
    if (!sp || !af) {
        Log("layout: lookup failed (r.ScreenPercentage %p, r.MaxAnisotropy %p)", sp, af);
        return false;
    }
    void* spVt = *static_cast<void**>(sp);
    void* afVt = *static_cast<void**>(af);
    bool spInt = true, afInt = false;
    void* spData = nullptr;
    void* afData = nullptr;
    float spValue = -1, afValue = -1, spStored = -2, spRender = -2;
    int32_t afStored = -2, afRender = -2;
    bool ok = cvars::InSection(g_rdata, spVt) && cvars::InSection(g_rdata, afVt) &&
              cvars::CallBool(sp, cvars::kIsVariableInt, spInt) && cvars::CallBool(af, cvars::kIsVariableInt, afInt) &&
              cvars::CallPtr(sp, cvars::kAsVariableFloat, spData) && cvars::CallPtr(af, cvars::kAsVariableInt, afData) &&
              cvars::GetFloat(sp, spValue) && cvars::GetFloat(af, afValue);
    auto spBytes = static_cast<uint8_t*>(sp) + cvars::kDataValue;
    auto afBytes = static_cast<uint8_t*>(af) + cvars::kDataValue;
    ok = ok && cvars::SafeCopy(&spStored, spBytes, 4) && cvars::SafeCopy(&spRender, spBytes + 4, 4) &&
         cvars::SafeCopy(&afStored, afBytes, 4) && cvars::SafeCopy(&afRender, afBytes + 4, 4);
    ok = ok && !spInt && afInt && spData == spBytes && afData == afBytes &&
         (IsFloat(spValue, spStored) || IsFloat(spValue, spRender)) &&
         (IsFloat(afValue, static_cast<float>(afStored)) || IsFloat(afValue, static_cast<float>(afRender)));
    Log("layout: r.ScreenPercentage vtable %s, IsVariableInt %d, data %s, GetFloat %g, stored %g/%g", Where(spVt).c_str(),
        spInt, spData == spBytes ? "at +0x38" : "elsewhere", spValue, spStored, spRender);
    Log("layout: r.MaxAnisotropy vtable %s, IsVariableInt %d, data %s, GetFloat %g, stored %d/%d", Where(afVt).c_str(),
        afInt, afData == afBytes ? "at +0x38" : "elsewhere", afValue, afStored, afRender);
    Log("layout: %s", ok ? "matches" : "MISMATCH");
    return ok;
}

// The singleton's address once (a scan of .text), then waits for the engine to create the
// manager and checks the layout. Called every frame until it succeeds or gives up.
bool FindManager() {
    static void** singleton = nullptr;
    static bool scanned = false;
    if (g_manager) return true;
    if (g_unsupported) return false;
    if (!scanned) {
        scanned = true;
        cvars::ManagerScan s = cvars::ScanConsoleManager(g_base);
        Log("cvars: IConsoleManager::Singleton at %s (%d of %d lookups agree), FindConsoleVariable at +0x%x (%d of %d)",
            s.singleton ? Where(s.singleton).c_str() : "?", s.votes, s.matches, s.findOffset, s.findVotes, s.findMatches);
        // Most lookups must agree (offline over 38 names: 120 of 121).
        if (!s.singleton || s.votes < 5 || s.votes * 4 < s.matches * 3)
            return Unsupported("IConsoleManager::Singleton not found"), false;
        if (s.findOffset != static_cast<int32_t>(cvars::kFindConsoleVariable))
            return Unsupported("FindConsoleVariable is not at the expected vtable offset"), false;
        singleton = s.singleton;
    }
    void* manager = nullptr;
    if (!cvars::SafeCopy(&manager, singleton, 8) || !manager) return false;
    void* vt = nullptr;
    if (!cvars::SafeCopy(&vt, manager, 8) || !cvars::InSection(g_rdata, vt))
        return Unsupported("the console manager's vtable is not in the executable"), false;
    g_manager = manager;
    Log("cvars: manager %p, vtable %s", manager, Where(vt).c_str());
    if (!CheckLayout()) {
        g_manager = nullptr;
        return Unsupported("console variable layout check failed (see FallenOrderTweaks.log)"), false;
    }
    g_status = "ready";
    return true;
}

// ---------------------------------------------------------------- forced values

// A cvar the saved settings force. Released (handed back to the game at its own priority)
// when no setting wants it any more.
struct Override {
    const wchar_t* cvar;
    bool wanted = false;  // a setting wants it forced
    float value = 0;      // to this
    bool active = false;  // forced right now
    float applied = 0;
    void* var = nullptr;
    bool missing = false;
    bool haveGame = false;
    float game = 0;           // the game's value before we first forced it
    uint32_t gameFlags = 0;   // and its flags (priority)
    uint32_t repairs = 0;
};

std::vector<Override> g_overrides;

Override* FindOverride(const wchar_t* cvar) {
    for (Override& o : g_overrides)
        if (wcscmp(o.cvar, cvar) == 0) return &o;
    g_overrides.push_back({cvar});
    return &g_overrides.back();
}

void* Resolve(Override& o) {
    if (o.var || o.missing) return o.var;
    o.var = cvars::FindVariable(g_manager, o.cvar);
    if (!o.var) {
        o.missing = true;
        Log("override: %ls is not a cvar in this build", o.cvar);
    }
    return o.var;
}

// The game's value: what it had when we took the cvar over, or else what it has now. False
// if the cvar doesn't exist (or the manager isn't ready).
bool GameValue(Override& o, float& out) {
    if (o.active && o.haveGame) {
        out = o.game;
        return true;
    }
    return g_manager && Resolve(o) && cvars::GetFloat(o.var, out);
}

std::wstring Text(float v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%g", v);
    return buf;
}

// What a setting asks for this frame.
void Drive(const wchar_t* cvar, bool want, float value) {
    Override* o = FindOverride(cvar);
    o->wanted = want;
    if (want) o->value = value;
}

void ApplyOverrides() {
    for (Override& o : g_overrides) {
        if (o.wanted == o.active && (!o.wanted || o.value == o.applied)) continue;
        if (!Resolve(o)) {
            o.wanted = false;
            continue;
        }
        float before = 0;
        cvars::GetFloat(o.var, before);
        if (o.wanted) {
            if (!o.active) {  // what to hand back later
                o.haveGame = cvars::GetFloat(o.var, o.game);
                o.gameFlags = cvars::Flags(o.var);
            }
            bool ok = cvars::Set(o.var, Text(o.value).c_str(), cvars::kSetByConsole);
            float after = 0;
            cvars::GetFloat(o.var, after);
            Log("override: %ls = %g%s (was %g, now %g)", o.cvar, o.value, ok ? "" : " (Set faulted)", before, after);
            o.active = ok;
            o.applied = o.value;
            if (!ok) o.wanted = false;
        } else {
            // Back to the game's priority first, or Set would refuse the lower one.
            cvars::Flags(o.var) = o.gameFlags;
            bool ok = cvars::Set(o.var, Text(o.game).c_str(), o.gameFlags & 0xff000000);
            float after = 0;
            cvars::GetFloat(o.var, after);
            Log("override: %ls released to %g%s (was %g, now %g)", o.cvar, o.game, ok ? "" : " (Set faulted)", before,
                after);
            o.active = false;
        }
    }
}

// Puts a forced value back if something changed it (nothing should, given the priority).
void CheckOverrides() {
    for (Override& o : g_overrides) {
        if (!o.active) continue;
        float now = 0;
        if (cvars::GetFloat(o.var, now) && IsFloat(now, o.applied)) continue;
        if (++o.repairs <= 5)
            Log("override: %ls changed to %g behind our back; setting %g again%s", o.cvar, now, o.applied,
                o.repairs == 5 ? " (not logging further repairs)" : "");
        cvars::Set(o.var, Text(o.applied).c_str(), cvars::kSetByConsole);
    }
}

// ---------------------------------------------------------------- upscaling

struct Mode {
    const char* label;
    float percent;  // r.ScreenPercentage; 0: the game's own
};
const Mode kModes[] = {
    {"Game", 0},           {"DLAA", 100},      {"Quality", 66.7f},
    {"Balanced", 58.0f},   {"Performance", 50.0f}, {"Ultra Performance", 33.3f},
};
constexpr int kModeGame = 0;
constexpr int kModeDlaa = 1;

struct UpscaleSettings {
    int mode = kModeDlaa;
    bool initialised = false;
};
UpscaleSettings g_upscale;

void SaveUpscale() { reshade::set_config_value(nullptr, kConfigSection, "UpscaleMode", g_upscale.mode); }

void InitUpscale() {
    g_upscale.initialised = true;
    bool saved = reshade::get_config_value(nullptr, kConfigSection, "UpscaleMode", g_upscale.mode);
    g_upscale.mode = std::clamp(g_upscale.mode, 0, static_cast<int>(std::size(kModes)) - 1);
    Log("upscaling: %s, mode %s", saved ? "from ReShade.ini" : "default", kModes[g_upscale.mode].label);
}

void ApplyUpscale() {
    if (!g_upscale.initialised) InitUpscale();
    float percent = kModes[g_upscale.mode].percent;
    Drive(L"r.ScreenPercentage", percent > 0, percent);
    // Below 100 % the frame reaches the TAA pass (which Luma replaces with DLSS) at the lower
    // resolution, instead of being stretched after it.
    Drive(L"r.TemporalAA.Upsampling", percent > 0 && percent < 100, 1.0f);
}

// ---------------------------------------------------------------- detail

struct VramChoice {
    const char* label;
    int gb;      // the smallest card this row is for
    int poolMb;  // r.Streaming.PoolSize
};
const VramChoice kVram[] = {
    {"6 GB or less", 0, 1024}, {"8 GB", 8, 2048},   {"10 GB", 10, 2560},
    {"12 GB", 12, 3072},       {"16 GB", 16, 4096}, {"20 GB or more", 20, 6144},
};
constexpr int kVramAuto = 0;      // saved choice: 0 Auto, else 1 + a kVram row
constexpr int kVramFallback = 1;  // 8 GB, if the card's memory can't be read

uint64_t g_vramMb = 0;  // dedicated memory of the game's adapter, from DXGI (0: unknown)

int DetectedVramRow() {
    if (!g_vramMb) return kVramFallback;
    int gb = static_cast<int>((g_vramMb + 512) / 1024);  // a "16 GB" card reports a little under
    int row = 0;
    for (int i = 0; i < static_cast<int>(std::size(kVram)); ++i)
        if (gb >= kVram[i].gb) row = i;
    return row;
}

enum Shadows : int { kShadowsGame = 0, kShadowsSharp = 1, kShadowsUltra = 2 };
enum ViewDistance : int { kViewGame = 0, kViewFar = 1 };

struct DetailSettings {
    bool on = true;  // with nothing saved: on, at these defaults
    int vram = kVramAuto;
    bool aniso16 = true;
    int shadows = kShadowsSharp;
    int view = kViewFar;
    bool reflections = true;
    bool initialised = false;
};
DetailSettings g_detail;

int VramRow() { return g_detail.vram == kVramAuto ? DetectedVramRow() : g_detail.vram - 1; }

void SaveDetail() {
    reshade::set_config_value(nullptr, kConfigSection, "DetailOverride", g_detail.on);
    reshade::set_config_value(nullptr, kConfigSection, "DetailVideoMemory", g_detail.vram);
    reshade::set_config_value(nullptr, kConfigSection, "DetailAnisotropy16", g_detail.aniso16);
    reshade::set_config_value(nullptr, kConfigSection, "DetailShadows", g_detail.shadows);
    reshade::set_config_value(nullptr, kConfigSection, "DetailViewDistance", g_detail.view);
    reshade::set_config_value(nullptr, kConfigSection, "DetailReflections", g_detail.reflections);
}

void InitDetail() {
    g_detail.initialised = true;
    bool saved = reshade::get_config_value(nullptr, kConfigSection, "DetailOverride", g_detail.on);
    if (saved) {
        reshade::get_config_value(nullptr, kConfigSection, "DetailVideoMemory", g_detail.vram);
        reshade::get_config_value(nullptr, kConfigSection, "DetailAnisotropy16", g_detail.aniso16);
        reshade::get_config_value(nullptr, kConfigSection, "DetailShadows", g_detail.shadows);
        reshade::get_config_value(nullptr, kConfigSection, "DetailViewDistance", g_detail.view);
        reshade::get_config_value(nullptr, kConfigSection, "DetailReflections", g_detail.reflections);
    }
    g_detail.vram = std::clamp(g_detail.vram, 0, static_cast<int>(std::size(kVram)));
    g_detail.shadows = std::clamp(g_detail.shadows, static_cast<int>(kShadowsGame), static_cast<int>(kShadowsUltra));
    g_detail.view = std::clamp(g_detail.view, static_cast<int>(kViewGame), static_cast<int>(kViewFar));
    Log("detail: %s, override %s: video memory %s (detected %llu MB) -> streaming pool %d MB, anisotropy %s, "
        "shadows %d, view distance %d, reflections %s",
        saved ? "from ReShade.ini" : "defaults", g_detail.on ? "on" : "off",
        g_detail.vram == kVramAuto ? "auto" : kVram[g_detail.vram - 1].label,
        static_cast<unsigned long long>(g_vramMb), kVram[VramRow()].poolMb, g_detail.aniso16 ? "16x" : "game",
        g_detail.shadows, g_detail.view, g_detail.reflections ? "high" : "game");
}

void ApplyDetail() {
    if (!g_detail.initialised) InitDetail();
    Drive(L"r.Streaming.PoolSize", g_detail.on, static_cast<float>(kVram[VramRow()].poolMb));
    Drive(L"r.MaxAnisotropy", g_detail.on && g_detail.aniso16, 16.0f);

    // Sharp: 4096 shadow maps (Epic is 2048). Ultra: also drawn 1.5x further, and smaller
    // objects cast them.
    bool shadows = g_detail.on && g_detail.shadows != kShadowsGame;
    bool ultra = g_detail.on && g_detail.shadows == kShadowsUltra;
    Drive(L"r.Shadow.MaxResolution", shadows, 4096.0f);
    Drive(L"r.Shadow.MaxCSMResolution", shadows, 4096.0f);
    Drive(L"r.Shadow.DistanceScale", ultra, 1.5f);
    Drive(L"r.Shadow.RadiusThreshold", ultra, 0.01f);

    bool view = g_detail.on && g_detail.view == kViewFar;
    Drive(L"r.ViewDistanceScale", view, 1.5f);
    Drive(L"r.StaticMeshLODDistanceScale", view, 0.5f);
    Drive(L"foliage.LODDistanceScale", view, 2.0f);

    Drive(L"r.SSR.Quality", g_detail.on && g_detail.reflections, 4.0f);
}

// ---------------------------------------------------------------- lighting

// Engine features the game leaves below their best, confirmed in game to make a visible
// difference. Each row's choice 0 is the game's own; choice n forces values[n - 1].
struct CvarValue {
    const wchar_t* cvar;
    float value;
};

struct LightingRow {
    const char* key;   // ReShade.ini
    const char* name;
    const char* help;
    const wchar_t* shown;  // the cvar whose game value the row shows
    std::vector<const char*> labels;
    std::vector<std::vector<CvarValue>> values;
    int byDefault = 1;  // with nothing saved (and after Reset): the first improved choice
    int choice = 0;
};

std::vector<LightingRow> g_lighting;
bool g_lightingInitialised = false;

void DefineLighting() {
    g_lighting = {
        // Tried and dropped (docs/FINDINGS.md, "Experimental results"): distance field AO and
        // shadows (no distance fields shipped), PCSS soft shadows and contact shadows (no
        // visible change in game).
        {"ExpAmbientOcclusion", "Ambient occlusion",
         "The game's screen space ambient occlusion: more levels give deeper, smoother shading in corners and "
         "creases.\nHigh: 3 levels. Max: 4.",
         L"r.AmbientOcclusionLevels", {"Game", "High", "Max"},
         {{{L"r.AmbientOcclusionLevels", 3}}, {{L"r.AmbientOcclusionLevels", 4}}}},
        {"ExpVolumetricFog", "Volumetric fog",
         "Detail of the volumetric fog and the light shafts through it.\nHigh: 6 px cells (Epic uses 8). Ultra: "
         "4 px cells and more depth slices, about 4x the fog work.",
         L"r.VolumetricFog.GridPixelSize", {"Game", "High", "Ultra"},
         {{{L"r.VolumetricFog.GridPixelSize", 6}},
          {{L"r.VolumetricFog.GridPixelSize", 4}, {L"r.VolumetricFog.GridSizeZ", 192}}}},
        {"ExpCapsuleShadows", "Character soft shadows",
         "Capsule shadows: soft shadows characters cast from indirect light. Full resolution: computed at full "
         "screen resolution instead of half, so they're smoother.",
         L"r.CapsuleShadowsFullResolution", {"Game", "Full resolution"},
         {{{L"r.CapsuleShadows", 1}, {L"r.CapsuleShadowsFullResolution", 1}}}},
        {"ExpSkin", "Skin scattering",
         "Light scattering under the skin of faces and hands.\nHigh: the high quality, full resolution version.",
         L"r.SSS.Quality", {"Game", "High"}, {{{L"r.SSS.Quality", 1}, {L"r.SSS.HalfRes", 0}}}},
    };
}

void SaveLighting() {
    for (const LightingRow& e : g_lighting) reshade::set_config_value(nullptr, kConfigSection, e.key, e.choice);
}

void InitLighting() {
    g_lightingInitialised = true;
    std::string line;
    for (LightingRow& e : g_lighting) {
        e.choice = e.byDefault;
        reshade::get_config_value(nullptr, kConfigSection, e.key, e.choice);
        e.choice = std::clamp(e.choice, 0, static_cast<int>(e.labels.size()) - 1);
        line += std::string(line.empty() ? "" : ", ") + e.name + " " + e.labels[e.choice];
    }
    Log("lighting: %s", line.c_str());
}

void ApplyLighting() {
    if (!g_lightingInitialised) InitLighting();
    for (const LightingRow& e : g_lighting) {
        // Every cvar any choice of this row touches: forced if the current choice lists it.
        for (const std::vector<CvarValue>& set : e.values)
            for (const CvarValue& cv : set) {
                bool want = false;
                float value = 0;
                if (e.choice > 0)
                    for (const CvarValue& c : e.values[e.choice - 1])
                        if (wcscmp(c.cvar, cv.cvar) == 0) want = true, value = c.value;
                Drive(cv.cvar, want, value);
            }
    }
}

// ---------------------------------------------------------------- events

uint64_t g_frames = 0;
uint32_t g_outputWidth = 0, g_outputHeight = 0;

// Dedicated memory of the adapter a D3D11 device runs on, in MB (0 if unknown).
uint64_t AdapterMemoryMb(ID3D11Device* d3d) {
    uint64_t mb = 0;
    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(d3d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice)))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc = {};
            if (SUCCEEDED(adapter->GetDesc(&desc))) mb = desc.DedicatedVideoMemory >> 20;
            adapter->Release();
        }
        dxgiDevice->Release();
    }
    return mb;
}

void OnInitDevice(device* dev) {
    std::lock_guard lock(g_mutex);
    bool d3d11 = dev->get_api() == device_api::d3d11;
    Log("device created: %s", d3d11 ? "Direct3D 11" : "not Direct3D 11");
    if (!d3d11) return;
    // The game also creates a device on the integrated GPU at startup; keep the largest.
    uint64_t mb = AdapterMemoryMb(reinterpret_cast<ID3D11Device*>(dev->get_native()));
    if (mb > g_vramMb) {
        g_vramMb = mb;
        Log("device: %llu MB dedicated video memory", static_cast<unsigned long long>(mb));
    }
}

void OnPresent(command_queue*, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    std::lock_guard lock(g_mutex);
    ++g_frames;
    if (sc) {
        resource_desc desc = sc->get_device()->get_resource_desc(sc->get_current_back_buffer());
        g_outputWidth = desc.texture.width;
        g_outputHeight = desc.texture.height;
    }
    if (!FindManager()) return;
    ApplyUpscale();
    ApplyDetail();
    ApplyLighting();
    ApplyOverrides();
    if (g_frames % 60 == 0) CheckOverrides();
}

// ---------------------------------------------------------------- overlay

bool LumaLoaded() { return GetModuleHandleW(L"Luma-Unreal Engine.addon") != nullptr; }

void Help(const char* text) {
    if (!text || !*text || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// Settings as a two-column table, so names and controls line up: name | control.
bool BeginSettings(const char* id, const char* widestName) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize(widestName).x + ImGui::GetFontSize());
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 15);
    return true;
}

// One row: the name (hover for help), then the control cell, sized for the caller's widget.
void SettingRow(const char* name, const char* help) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    Help(help);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// Right-aligned "Reset to defaults" at the end of a tab.
bool ResetButton() {
    ImGui::Spacing();
    const char* label = "Reset to defaults";
    float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
    return ImGui::Button(label);
}

void Wrapped(const char* text, bool disabled = true) {
    ImGui::PushTextWrapPos(0.0f);
    if (disabled)
        ImGui::TextDisabled("%s", text);
    else
        ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
}

void DrawUpscale() {
    bool changed = false;
    if (BeginSettings("##upscale", "DLSS mode")) {
        const char* help = "The resolution the game renders at before DLSS upscales it to your screen.\n"
                           "DLAA: full resolution, best image.\nQuality 67 %, Balanced 58 %, Performance 50 %, "
                           "Ultra Performance 33 %: faster, softer.\nGame: leave the game's own setting.\n"
                           "Changes apply at once.";
        SettingRow("DLSS mode", help);
        const char* labels[std::size(kModes)];
        for (size_t i = 0; i < std::size(kModes); ++i) labels[i] = kModes[i].label;
        changed |= ImGui::Combo("##mode", &g_upscale.mode, labels, static_cast<int>(std::size(kModes)));
        Help(help);
        ImGui::EndTable();
    }
    float percent = kModes[g_upscale.mode].percent;
    if (percent > 0 && g_outputWidth) {
        ImGui::TextDisabled("Renders at %u x %u, output %u x %u", static_cast<uint32_t>(lroundf(g_outputWidth * percent / 100)),
                            static_cast<uint32_t>(lroundf(g_outputHeight * percent / 100)), g_outputWidth, g_outputHeight);
    }
    ImGui::Spacing();
    if (LumaLoaded())
        Wrapped("Luma replaces the game's TAA with DLSS. Pick DLSS as Luma's Super Resolution in its own tab; "
                "its DLSS Preset setting picks the model.");
    else
        ImGui::TextColored(kWarn, "Luma is not loaded: below 100 %% the game's own TAA upscales instead of DLSS.");
    if (changed) SaveUpscale();
}

void DrawDetail() {
    bool changed = ImGui::Checkbox("Improve textures, shadows and view distance", &g_detail.on);
    Help("Kept in ReShade.ini and applied every time the game starts.");
    ImGui::Spacing();
    if (!g_detail.on) ImGui::BeginDisabled();
    if (BeginSettings("##detail", "Video card memory")) {
        char autoLabel[64];
        if (g_vramMb)
            snprintf(autoLabel, sizeof(autoLabel), "Auto (%s)", kVram[DetectedVramRow()].label);
        else
            snprintf(autoLabel, sizeof(autoLabel), "Auto (not detected: %s)", kVram[kVramFallback].label);
        const char* vram[1 + std::size(kVram)] = {autoLabel};
        for (size_t i = 0; i < std::size(kVram); ++i) vram[1 + i] = kVram[i].label;
        char vramHelp[256];
        snprintf(vramHelp, sizeof(vramHelp),
                 "Your graphics card's memory. Textures get about a quarter of it (now %d MB), so they load at "
                 "full resolution with less pop-in. Auto reads it from the card.",
                 kVram[VramRow()].poolMb);
        SettingRow("Video card memory", vramHelp);
        changed |= ImGui::Combo("##vram", &g_detail.vram, vram, static_cast<int>(std::size(vram)));
        Help(vramHelp);

        const char* afHelp = "16x: sharper floors and walls at shallow angles. Nearly free.";
        SettingRow("Texture filtering", afHelp);
        int af = g_detail.aniso16 ? 1 : 0;
        const char* afs[] = {"Game", "16x"};
        if (ImGui::Combo("##af", &af, afs, 2)) {
            g_detail.aniso16 = af == 1;
            changed = true;
        }
        Help(afHelp);

        const char* shadowHelp = "Sharp: shadow maps at 4096 (Epic uses 2048), so shadow edges are crisper.\n"
                                 "Ultra: also drawn 1.5x further away, and small objects cast shadows too. Costs "
                                 "the most.";
        SettingRow("Shadows", shadowHelp);
        const char* shadows[] = {"Game", "Sharp", "Ultra"};
        changed |= ImGui::Combo("##shadows", &g_detail.shadows, shadows, 3);
        Help(shadowHelp);

        const char* viewHelp = "Far: objects drawn 1.5x further, detailed meshes kept twice as far, and foliage "
                               "detail kept twice as far, so there is less pop-in.";
        SettingRow("View distance", viewHelp);
        const char* view[] = {"Game", "Far"};
        changed |= ImGui::Combo("##view", &g_detail.view, view, 2);
        Help(viewHelp);

        const char* ssrHelp = "High: the highest quality of the game's screen space reflections (water, wet and "
                              "polished floors).";
        SettingRow("Reflections", ssrHelp);
        int ssr = g_detail.reflections ? 1 : 0;
        const char* ssrs[] = {"Game", "High"};
        if (ImGui::Combo("##ssr", &ssr, ssrs, 2)) {
            g_detail.reflections = ssr == 1;
            changed = true;
        }
        Help(ssrHelp);
        ImGui::EndTable();
    }
    if (!g_detail.on) ImGui::EndDisabled();
    ImGui::Spacing();
    Wrapped("These take priority over the game's graphics menu for the same settings. Film grain, motion blur and "
            "chromatic aberration are in the game's own menu.");
    if (ResetButton()) {
        g_detail = DetailSettings{};
        g_detail.initialised = true;
        changed = true;
    }
    if (changed) SaveDetail();
}

void DrawLighting() {
    Wrapped("Lighting effects the game runs below their best. Each row shows the game's own value. Kept in "
            "ReShade.ini and applied every time the game starts.");
    ImGui::Spacing();

    bool changed = false;
    if (ImGui::BeginTable("##lighting", 3, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("Character soft shadows").x + ImGui::GetFontSize());
        ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 11);
        ImGui::TableSetupColumn("game", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6);
        for (LightingRow& e : g_lighting) {
            ImGui::PushID(e.key);
            SettingRow(e.name, e.help);
            changed |= ImGui::Combo("##c", &e.choice, e.labels.data(), static_cast<int>(e.labels.size()));
            Help(e.help);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            float game = 0;
            if (GameValue(*FindOverride(e.shown), game))
                ImGui::TextDisabled("game: %g", game);
            else
                ImGui::TextDisabled("game: -");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ResetButton()) {
        for (LightingRow& e : g_lighting) e.choice = e.byDefault;
        changed = true;
    }
    if (changed) SaveLighting();
}

void DrawAdvanced() {
    Wrapped("The console variables these settings control: the game's own value, and what it is now.");
    ImGui::TextDisabled("Status: %s", g_status.c_str());
    if (ImGui::BeginTable("##cvars", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Console variable");
        ImGui::TableSetupColumn("Game");
        ImGui::TableSetupColumn("Now");
        ImGui::TableHeadersRow();
        for (Override& o : g_overrides) {
            ImGui::TableNextColumn();
            ImGui::Text("%ls", o.cvar);
            ImGui::TableNextColumn();
            float game = 0;
            if (o.missing)
                ImGui::TextDisabled("not in this build");
            else if (GameValue(o, game))
                ImGui::Text("%g", game);
            else
                ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            float now = 0;
            if (o.var && cvars::GetFloat(o.var, now)) {
                if (o.active)
                    ImGui::Text("%g (forced)", now);
                else
                    ImGui::TextDisabled("%g", now);
            } else {
                ImGui::TextDisabled("-");
            }
        }
        ImGui::EndTable();
    }
}

void DrawOverlay(effect_runtime*) {
    std::lock_guard lock(g_mutex);
    ImGui::TextDisabled(FOT_PRODUCT " " FOT_VERSION "  -  settings are saved and applied every time the game starts");
    if (g_unsupported) {
        ImGui::TextColored(kWarn, "Not active: %s.", g_status.c_str());
        return;
    }
    if (!g_manager) {
        ImGui::TextColored(kWarn, "Waiting for the game's console variables...");
        return;
    }
    if (g_timestamp != kKnownTimestamp)
        ImGui::TextColored(kWarn, "A different game build than the one analysed; the layout check passed.");
    if (!ImGui::BeginTabBar("##tabs")) return;
    if (ImGui::BeginTabItem("Upscaling")) {
        DrawUpscale();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Detail")) {
        DrawDetail();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Lighting")) {
        DrawLighting();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Advanced")) {
        DrawAdvanced();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void Install() {
    OpenLog();
    LogGameInfo();
    DefineLighting();
    reshade::register_event<reshade::addon_event::init_device>(&OnInitDevice);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
    reshade::register_overlay(FOT_PRODUCT, &DrawOverlay);
}

void Uninstall() {
    reshade::unregister_overlay(FOT_PRODUCT, &DrawOverlay);
    if (g_log) {
        Log("unloading");
        fclose(g_log);
        g_log = nullptr;
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            Install();
            break;
        case DLL_PROCESS_DETACH:
            Uninstall();
            reshade::unregister_addon(module);
            break;
    }
    return TRUE;
}
