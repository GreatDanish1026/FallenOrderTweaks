// FallenOrderTweaks: a ReShade add-on for Star Wars Jedi: Fallen Order (UE 4.21, DirectX 11)
// that sets the game's console variables from the ReShade overlay, so there is no Engine.ini
// to edit. Jedi: Survivor reads the same %LOCALAPPDATA%\SwGame config folder, which is why the
// settings live here instead. See docs/FINDINGS.md.
//
// Saved settings (ReShade.ini, [FallenOrderTweaks], applied at every start), by overlay tab:
//   Image     the render resolution DLSS (Luma) upscales from: DLAA / Quality / Balanced /
//             Performance / Ultra Performance, i.e. r.ScreenPercentage with TAA upsampling
//             (Luma replaces the game's TAA with DLSS but follows the game's resolution);
//             texture sharpness to match (r.MipMapLODBias = log2(render scale)); cutscene
//             depth of field
//   Detail    texture memory by video card, 16x texture filtering, view, grass and foliage
//             distance, character and terrain detail
//   Shadows   shadow map quality, cascades and blending; soft (PCSS) and contact shadows for
//             the sun (src/shaders/shadow.hlsl); full-resolution character shadows
//   Lighting  ambient occlusion: more of the game's levels, or GTAO in their place
//             (src/shaders/gtao.hlsl); reflections, volumetric fog, light shafts, bloom,
//             smoke and glass lighting, skin scattering
//   Advanced  every console variable the settings control, with the game's value
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
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "cvars.hpp"
#include "shaders/gtao_blob.h"
#include "shaders/shadow_blobs.h"
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
            bool ok = cvars::Set(o.var, Text(o.value).c_str(), cvars::kSetByConsole) && cvars::MirrorToRenderer(o.var);
            float after = 0;
            cvars::GetFloat(o.var, after);
            Log("override: %ls = %g%s (was %g, now %g)", o.cvar, o.value, ok ? "" : " (Set or mirror failed)", before, after);
            o.active = ok;
            o.applied = o.value;
            if (!ok) o.wanted = false;
        } else {
            // Back to the game's priority first, or Set would refuse the lower one.
            cvars::Flags(o.var) = o.gameFlags;
            bool ok = cvars::Set(o.var, Text(o.game).c_str(), o.gameFlags & 0xff000000) && cvars::MirrorToRenderer(o.var);
            float after = 0;
            cvars::GetFloat(o.var, after);
            Log("override: %ls released to %g%s (was %g, now %g)", o.cvar, o.game, ok ? "" : " (Set or mirror failed)", before,
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
        cvars::MirrorToRenderer(o.var);
    }
}

// ---------------------------------------------------------------- settings

// Every setting is kept in ReShade.ini ([FallenOrderTweaks]), read at load (before the game
// creates its shaders) and applied every frame. Most are rows: a choice whose option 0 is the
// game's own and whose option n forces values[n - 1]. The rest (DLSS mode, texture
// sharpness, texture pool, ambient occlusion, sun shadows) have their own logic below.

struct CvarValue {
    const wchar_t* cvar;
    float value;
};

struct CvarRow {
    const char* key;   // ReShade.ini
    const char* name;
    const char* help;
    const wchar_t* shown;  // the cvar whose game value the tooltip shows
    std::vector<const char*> labels;
    std::vector<std::vector<CvarValue>> values;
    int byDefault = 1;  // with nothing saved (and after Reset): the first improved choice
    int choice = 0;
};

struct RowList {
    const char* label;  // for the log
    std::vector<CvarRow> rows;
};

RowList g_imageRows{"image"};
RowList g_cameraRows{"camera"};
RowList g_detailRows{"detail"};
RowList g_shadowRows{"shadows"};
RowList g_characterShadowRows{"character shadows"};
RowList g_lightingRows{"lighting"};
RowList* const kAllRows[] = {&g_imageRows, &g_cameraRows, &g_detailRows, &g_shadowRows, &g_characterShadowRows, &g_lightingRows};

// Tried and dropped (docs/FINDINGS.md, "Experimental results"): distance field AO and shadows
// (no distance fields shipped), the engine's PCSS and contact shadows (no visible change).
void DefineRows() {
    // Camera effects are a matter of taste, so they stay on Game unless picked (depth of field
    // keeps its earlier default, High quality).
    g_cameraRows.rows = {
        {"CamMotionBlur", "Motion blur",
         "Blur from camera and object movement. Game: follows the game's menu, whose Off still leaves some "
         "object blur. Off: no motion blur at all.",
         L"r.MotionBlurQuality", {"Game", "Off"}, {{{L"r.MotionBlurQuality", 0}}}, 0},
        {"CamFilmGrain", "Film grain", "Game: follows the game's menu. Off: no film grain.", L"r.EnableFilmGrain",
         {"Game", "Off"}, {{{L"r.EnableFilmGrain", 0}}}, 0},
        {"CamChromaticAberration", "Chromatic aberration",
         "Colour fringes towards the edges of the screen. Game: follows the game's menu. Off: none, also where a "
         "scene adds its own.",
         L"r.EnableChromaticAberration", {"Game", "Off"},
         {{{L"r.EnableChromaticAberration", 0}, {L"r.SceneColorFringe.Max", 0}}}, 0},
        {"FxDepthOfField", "Depth of field",
         "Background blur in cutscenes, menus and photo mode. High quality: the higher quality accumulator, "
         "smoother bokeh with less flicker. Off: no depth of field anywhere.",
         L"r.DepthOfFieldQuality", {"Game", "High quality", "Off"},
         {{{L"r.DOF.Gather.AccumulatorQuality", 1}}, {{L"r.DepthOfFieldQuality", 0}}}},
        {"CamVignette", "Vignette",
         "Darkening towards the corners of the screen. Off: the game's tonemapper without vignette, which also "
         "drops its shadow tint and grain.",
         L"r.Tonemapper.Quality", {"Game", "Off"}, {{{L"r.Tonemapper.Quality", 1}}}, 0},
        {"CamLensFlares", "Lens flares", "Flares from bright lights such as the sun. Off: none.", L"r.LensFlareQuality",
         {"Game", "Off"}, {{{L"r.LensFlareQuality", 0}}}, 0},
    };
    g_detailRows.rows = {
        {"DetailAnisotropy16", "Texture filtering",
         "Sharper floors and walls at shallow angles. Nearly free.", L"r.MaxAnisotropy", {"Game", "16x"},
         {{{L"r.MaxAnisotropy", 16}}}},
        {"DetailViewDistance", "View distance",
         "Far: objects drawn 1.5x further, detailed meshes and foliage detail kept twice as far, so there is "
         "less pop-in.",
         L"r.ViewDistanceScale", {"Game", "Far"},
         {{{L"r.ViewDistanceScale", 1.5f}, {L"r.StaticMeshLODDistanceScale", 0.5f}, {L"foliage.LODDistanceScale", 2}}}},
        {"DetailGrassDistance", "Grass draw distance",
         "How far away grass is still drawn. Far: twice the distance. Farthest: three times. Less grass pop-in, at "
         "some cost in open areas.",
         L"grass.CullDistanceScale", {"Game", "Far", "Farthest"},
         {{{L"grass.CullDistanceScale", 2}}, {{L"grass.CullDistanceScale", 3}}}},
        {"DetailFoliageDensity", "Foliage density",
         "How much of the placed foliage is drawn. Full: all of it (changes nothing if the game already draws it "
         "all).",
         L"foliage.DensityScale", {"Game", "Full"}, {{{L"foliage.DensityScale", 1}}}},
        {"DetailCharacterLod", "Character detail",
         "Detail level of characters and creatures. Full: their most detailed model at a greater distance.",
         L"r.SkeletalMeshLODBias", {"Game", "Full"}, {{{L"r.SkeletalMeshLODBias", -1}}}},
        {"DetailTerrainLod", "Terrain detail", "Detail level of the landscape. High: more detailed terrain further away.",
         L"r.LandscapeLODBias", {"Game", "High"}, {{{L"r.LandscapeLODBias", -1}}}},
    };
    g_shadowRows.rows = {
        {"DetailShadows", "Shadow quality",
         "Sharp: shadow maps at 4096 (Epic uses 2048), so shadow edges are crisper.\nUltra: also drawn 1.5x "
         "further away, and small objects cast shadows too. Costs the most.",
         L"r.Shadow.MaxCSMResolution", {"Game", "Sharp", "Ultra"},
         {{{L"r.Shadow.MaxResolution", 4096}, {L"r.Shadow.MaxCSMResolution", 4096}},
          {{L"r.Shadow.MaxResolution", 4096}, {L"r.Shadow.MaxCSMResolution", 4096}, {L"r.Shadow.DistanceScale", 1.5f},
           {L"r.Shadow.RadiusThreshold", 0.01f}}}},
        {"DetailShadowCascades", "Shadow cascades",
         "Cascades of the sun's shadow map. More keep detailed shadows further from the camera.\nMore: 4. Most: 5.",
         L"r.Shadow.CSM.MaxCascades", {"Game", "More", "Most"},
         {{{L"r.Shadow.CSM.MaxCascades", 4}}, {{L"r.Shadow.CSM.MaxCascades", 5}}}},
        {"DetailCascadeBlend", "Cascade blending",
         "How gradually one shadow cascade fades into the next. Soft: twice as wide, so the line where shadows get "
         "blurrier is harder to see.",
         L"r.Shadow.CSM.TransitionScale", {"Game", "Soft"}, {{{L"r.Shadow.CSM.TransitionScale", 2}}}},
    };
    g_characterShadowRows.rows = {
        {"ExpCapsuleShadows", "Character soft shadows",
         "Soft shadows characters cast from indirect light (capsule shadows). Full resolution: computed at full "
         "screen resolution instead of half, so they're smoother.",
         L"r.CapsuleShadowsFullResolution", {"Game", "Full resolution"},
         {{{L"r.CapsuleShadows", 1}, {L"r.CapsuleShadowsFullResolution", 1}}}},
    };
    g_lightingRows.rows = {
        {"DetailReflections", "Reflections",
         "High: the highest quality of the game's screen space reflections (water, wet and polished floors).",
         L"r.SSR.Quality", {"Game", "High"}, {{{L"r.SSR.Quality", 4}}}},
        {"FxReflectionRoughness", "Rough surface reflections",
         "How rough a surface can be and still get screen space reflections. More: up to 0.8, so wet stone, "
         "metal and floors reflect too. Costs some performance and can look noisy.",
         L"r.SSR.MaxRoughness", {"Game", "More"}, {{{L"r.SSR.MaxRoughness", 0.8f}}}},
        {"ExpVolumetricFog", "Volumetric fog",
         "Detail of the volumetric fog and the light shafts through it.\nHigh: 6 px cells (Epic uses 8). Ultra: "
         "4 px cells and more depth slices, about 4x the fog work.",
         L"r.VolumetricFog.GridPixelSize", {"Game", "High", "Ultra"},
         {{{L"r.VolumetricFog.GridPixelSize", 6}},
          {{L"r.VolumetricFog.GridPixelSize", 4}, {L"r.VolumetricFog.GridSizeZ", 192}}}},
        {"FxLightShafts", "Light shafts",
         "Screen space light shafts (god rays) from the sun through gaps and foliage. On: the high quality version.",
         L"r.LightShaftQuality", {"Game", "On"}, {{{L"r.LightShaftQuality", 1}}}},
        {"FxBloom", "Bloom", "Quality of the glow around bright lights. Max: the highest (5), smoother and wider.",
         L"r.BloomQuality", {"Game", "Max"}, {{{L"r.BloomQuality", 5}}}},
        {"FxTranslucencyLighting", "Smoke and glass lighting",
         "Resolution of the lighting volume for particles, smoke, glass and water surfaces. High: 128 (Epic uses "
         "64), so they pick up light and shadow more precisely.",
         L"r.TranslucencyLightingVolumeDim", {"Game", "High"}, {{{L"r.TranslucencyLightingVolumeDim", 128}}}},
        {"ExpSkin", "Skin scattering",
         "Light scattering under the skin of faces and hands. High: the high quality, full resolution version.",
         L"r.SSS.Quality", {"Game", "High"}, {{{L"r.SSS.Quality", 1}, {L"r.SSS.HalfRes", 0}}}},
    };
}

void SaveRows(const RowList& list) {
    for (const CvarRow& e : list.rows) reshade::set_config_value(nullptr, kConfigSection, e.key, e.choice);
}

void ResetRows(RowList& list) {
    for (CvarRow& e : list.rows) e.choice = e.byDefault;
    SaveRows(list);
}

void InitRows(RowList& list) {
    std::string line;
    for (CvarRow& e : list.rows) {
        e.choice = e.byDefault;
        reshade::get_config_value(nullptr, kConfigSection, e.key, e.choice);
        e.choice = std::clamp(e.choice, 0, static_cast<int>(e.labels.size()) - 1);
        line += std::string(line.empty() ? "" : ", ") + e.name + " " + e.labels[e.choice];
    }
    Log("%s: %s", list.label, line.c_str());
}

void ApplyRows(const RowList& list) {
    for (const CvarRow& e : list.rows) {
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

CvarRow* FindRow(const char* key) {
    for (RowList* list : kAllRows)
        for (CvarRow& e : list->rows)
            if (strcmp(e.key, key) == 0) return &e;
    return nullptr;
}

// ---------------------------------------------------------------- DLSS mode and texture sharpness

struct Mode {
    const char* label;
    float percent;  // r.ScreenPercentage; 0: the game's own
};
const Mode kModes[] = {
    {"Game", 0},           {"DLAA", 100},      {"Quality", 66.7f},
    {"Balanced", 58.0f},   {"Performance", 50.0f}, {"Ultra Performance", 33.3f},
};
constexpr int kModeDlaa = 1;

struct UpscaleSettings {
    int mode = kModeDlaa;
    bool textureBias = true;
};
UpscaleSettings g_upscale;

void SaveUpscale() {
    reshade::set_config_value(nullptr, kConfigSection, "UpscaleMode", g_upscale.mode);
    reshade::set_config_value(nullptr, kConfigSection, "UpscaleTextureBias", g_upscale.textureBias);
}

void InitUpscale() {
    reshade::get_config_value(nullptr, kConfigSection, "UpscaleMode", g_upscale.mode);
    reshade::get_config_value(nullptr, kConfigSection, "UpscaleTextureBias", g_upscale.textureBias);
    g_upscale.mode = std::clamp(g_upscale.mode, 0, static_cast<int>(std::size(kModes)) - 1);
    Log("upscaling: mode %s, texture sharpness %s", kModes[g_upscale.mode].label,
        g_upscale.textureBias ? "match DLSS" : "game");
}

// The texture mip bias for a render scale: log2(render / output), NVIDIA's recommendation for
// DLSS, so textures keep the detail of the output resolution (0 at DLAA, -1 at Performance).
float TextureBias(float percent) { return percent > 0 && percent < 100 ? log2f(percent / 100.0f) : 0.0f; }

void ApplyUpscale() {
    float percent = kModes[g_upscale.mode].percent;
    Drive(L"r.ScreenPercentage", percent > 0, percent);
    // Below 100 % the frame reaches the TAA pass (which Luma replaces with DLSS) at the lower
    // resolution, instead of being stretched after it.
    Drive(L"r.TemporalAA.Upsampling", percent > 0 && percent < 100, 1.0f);
    float bias = TextureBias(percent);
    Drive(L"r.MipMapLODBias", g_upscale.textureBias && bias < 0, bias);
}

// ---------------------------------------------------------------- texture pool

struct VramChoice {
    const char* label;
    int gb;      // the smallest card this row is for
    int poolMb;  // r.Streaming.PoolSize
};
const VramChoice kVram[] = {
    {"6 GB or less", 0, 1024}, {"8 GB", 8, 2048},   {"10 GB", 10, 2560},
    {"12 GB", 12, 3072},       {"16 GB", 16, 4096}, {"20 GB or more", 20, 6144},
};
constexpr int kVramFallback = 1;  // 8 GB, if the card's memory can't be read
// Saved choice: 0 the game's own, 1 Auto, else 2 + a kVram row.
constexpr int kPoolGame = 0;
constexpr int kPoolAuto = 1;

uint64_t g_vramMb = 0;  // dedicated memory of the game's adapter, from DXGI (0: unknown)
int g_texturePool = kPoolAuto;

int DetectedVramRow() {
    if (!g_vramMb) return kVramFallback;
    int gb = static_cast<int>((g_vramMb + 512) / 1024);  // a "16 GB" card reports a little under
    int row = 0;
    for (int i = 0; i < static_cast<int>(std::size(kVram)); ++i)
        if (gb >= kVram[i].gb) row = i;
    return row;
}

int VramRow() { return g_texturePool == kPoolAuto ? DetectedVramRow() : g_texturePool - 2; }

void SaveTexturePool() { reshade::set_config_value(nullptr, kConfigSection, "TexturePool", g_texturePool); }

void ApplyTexturePool() {
    Drive(L"r.Streaming.PoolSize", g_texturePool != kPoolGame, static_cast<float>(kVram[VramRow()].poolMb));
}

// ---------------------------------------------------------------- ambient occlusion

// One choice: the game's SSAO, more of its levels, or GTAO (src/shaders/gtao.hlsl), which
// replaces the single-level AO pass and needs r.AmbientOcclusionLevels 1.
enum AoChoice : int { kAoGame = 0, kAoMore = 1, kAoMax = 2, kAoGtao = 3 };
const char* const kAoLabels[] = {"Game", "More levels", "Max levels", "GTAO"};

const char* const kGtaoQualityLabels[] = {"High", "Ultra", "Max"};  // 4x8, 6x12, 8x16 samples
const char* const kOffOn[] = {"Off", "On"};

struct AoSettings {
    int choice = kAoGtao;
    int radiusPercent = 100;    // r.AmbientOcclusionRadiusScale x 100
    int strengthPercent = 100;  // carried to the shader in r.AmbientOcclusionMipLevelFactor
    int quality = 1;            // kGtaoQualityLabels (Ultra); carried in the settings code (below)
    int multiBounce = 1;        // 0/1; carried in the settings code
};
AoSettings g_ao;
// The GTAO shader reads strength as r.AmbientOcclusionMipLevelFactor / 0.5 (the cvar's
// default): the HZB mip scale the replaced pass receives (cb0[34].z), which GTAO doesn't need.
constexpr float kMipFactorPerStrength = 0.5f;

bool GtaoOn() { return g_ao.choice == kAoGtao; }

void SaveAo() {
    reshade::set_config_value(nullptr, kConfigSection, "AmbientOcclusion", g_ao.choice);
    reshade::set_config_value(nullptr, kConfigSection, "GtaoRadiusPercent", g_ao.radiusPercent);
    reshade::set_config_value(nullptr, kConfigSection, "GtaoStrengthPercent", g_ao.strengthPercent);
    reshade::set_config_value(nullptr, kConfigSection, "GtaoQuality", g_ao.quality);
    reshade::set_config_value(nullptr, kConfigSection, "GtaoMultiBounce", g_ao.multiBounce);
}

void InitAo() {
    if (!reshade::get_config_value(nullptr, kConfigSection, "AmbientOcclusion", g_ao.choice)) {
        // Before 0.7: a GTAO switch and a separate levels row.
        bool gtao = true;
        int levels = 1;
        reshade::get_config_value(nullptr, kConfigSection, "GtaoOn", gtao);
        reshade::get_config_value(nullptr, kConfigSection, "ExpAmbientOcclusion", levels);
        g_ao.choice = gtao ? kAoGtao : std::clamp(levels, 0, 2);
    }
    reshade::get_config_value(nullptr, kConfigSection, "GtaoRadiusPercent", g_ao.radiusPercent);
    reshade::get_config_value(nullptr, kConfigSection, "GtaoStrengthPercent", g_ao.strengthPercent);
    reshade::get_config_value(nullptr, kConfigSection, "GtaoQuality", g_ao.quality);
    reshade::get_config_value(nullptr, kConfigSection, "GtaoMultiBounce", g_ao.multiBounce);
    g_ao.quality = std::clamp(g_ao.quality, 0, static_cast<int>(std::size(kGtaoQualityLabels)) - 1);
    g_ao.multiBounce = std::clamp(g_ao.multiBounce, 0, 1);
    g_ao.choice = std::clamp(g_ao.choice, 0, static_cast<int>(kAoGtao));
    g_ao.radiusPercent = std::clamp(g_ao.radiusPercent, 50, 200);
    g_ao.strengthPercent = std::clamp(g_ao.strengthPercent, 25, 300);
    Log("ambient occlusion: %s, GTAO radius %d %%, strength %d %%, quality %s, multi-bounce %s", kAoLabels[g_ao.choice],
        g_ao.radiusPercent, g_ao.strengthPercent, kGtaoQualityLabels[g_ao.quality], kOffOn[g_ao.multiBounce]);
}

void ApplyAo() {
    const float levels[] = {0, 3, 4, 1};
    Drive(L"r.AmbientOcclusionLevels", g_ao.choice != kAoGame, levels[g_ao.choice]);
    Drive(L"r.AmbientOcclusionRadiusScale", GtaoOn() && g_ao.radiusPercent != 100, g_ao.radiusPercent / 100.0f);
    // Always while on, so the shader never reads the game's own value as a strength.
    Drive(L"r.AmbientOcclusionMipLevelFactor", GtaoOn(), kMipFactorPerStrength * g_ao.strengthPercent / 100.0f);
}

// ---------------------------------------------------------------- sun shadows

// Soft (PCSS) and contact shadows for the sun, in the rebuilt shadow projection pass
// (src/shaders/shadow.hlsl).
struct SunSize {
    const char* label;
    int tenths;  // apparent diameter, 0.1 degree steps
};
const SunSize kSunSizes[] = {{"Off", 0}, {"Real sun (0.5\xc2\xb0)", 5}, {"Soft (1\xc2\xb0)", 10}, {"Very soft (2\xc2\xb0)", 20}};
struct ContactLength {
    const char* label;
    int steps;  // 2 cm steps
};
const ContactLength kContactLengths[] = {{"Off", 0}, {"Short (20 cm)", 10}, {"Medium (50 cm)", 25}, {"Long (1 m)", 50}};

struct ShadowSettings {
    int sun = 1;      // kSunSizes
    int contact = 1;  // kContactLengths
};
ShadowSettings g_shadows;

bool ShadowsOn() { return g_shadows.sun > 0 || g_shadows.contact > 0; }

void SaveShadows() {
    reshade::set_config_value(nullptr, kConfigSection, "ShadowSunSize", g_shadows.sun);
    reshade::set_config_value(nullptr, kConfigSection, "ShadowContact", g_shadows.contact);
}

void InitShadows() {
    reshade::get_config_value(nullptr, kConfigSection, "ShadowSunSize", g_shadows.sun);
    reshade::get_config_value(nullptr, kConfigSection, "ShadowContact", g_shadows.contact);
    g_shadows.sun = std::clamp(g_shadows.sun, 0, static_cast<int>(std::size(kSunSizes)) - 1);
    g_shadows.contact = std::clamp(g_shadows.contact, 0, static_cast<int>(std::size(kContactLengths)) - 1);
    Log("sun shadows: soft %s, contact %s", kSunSizes[g_shadows.sun].label, kContactLengths[g_shadows.contact].label);
}

// ---------------------------------------------------------------- settings code

// Settings for the rebuilt shaders ride in r.AOSpecularOcclusionMode, which reaches every
// shader as View.DistanceFieldAOSpecularOcclusionMode (cb1[166].x) and otherwise only matters
// for distance field AO, which the game can't use (it ships no distance fields):
//   shadows (lowest four digits): 1000 + sun size in 0.1 degree steps * 100 + contact length
//                                 in 2 cm steps; 0 = the game's shadows
//   GTAO: quality * 10000 + multi-bounce * 100000
// With everything at the shaders' defaults it's left to the game (its 0 or 1 means the same).
int SettingsCode() {
    int code = 0;
    if (ShadowsOn()) code += 1000 + kSunSizes[g_shadows.sun].tenths * 100 + kContactLengths[g_shadows.contact].steps;
    if (GtaoOn()) code += g_ao.quality * 10000 + g_ao.multiBounce * 100000;
    return code;
}

void ApplySettingsCode() {
    int code = SettingsCode();
    Drive(L"r.AOSpecularOcclusionMode", code != 0, static_cast<float>(code));
}

// ---------------------------------------------------------------- loading and applying

// At load, before the game creates its shaders (the shader replacement reads GtaoOn and
// ShadowsOn).
void InitSettings() {
    DefineRows();
    for (RowList* list : kAllRows) InitRows(*list);
    InitUpscale();
    InitAo();
    InitShadows();

    // Before 0.7: the texture pool was "DetailVideoMemory" (0 Auto, else 1 + a row), and a
    // "DetailOverride" switch turned the old Detail tab's settings off.
    bool detailOn = true;
    reshade::get_config_value(nullptr, kConfigSection, "DetailOverride", detailOn);
    if (!reshade::get_config_value(nullptr, kConfigSection, "TexturePool", g_texturePool)) {
        int old = 0;
        reshade::get_config_value(nullptr, kConfigSection, "DetailVideoMemory", old);
        g_texturePool = detailOn ? old + 1 : kPoolGame;
    }
    if (!detailOn) {
        for (const char* key : {"DetailAnisotropy16", "DetailViewDistance", "DetailShadows", "DetailReflections"})
            if (CvarRow* e = FindRow(key)) e->choice = 0;
        for (RowList* list : kAllRows) SaveRows(*list);
        SaveTexturePool();
        reshade::set_config_value(nullptr, kConfigSection, "DetailOverride", true);  // migrated
    }
    g_texturePool = std::clamp(g_texturePool, 0, static_cast<int>(std::size(kVram)) + 1);
    Log("texture pool: %s", g_texturePool == kPoolGame ? "game" : g_texturePool == kPoolAuto ? "auto" : kVram[VramRow()].label);
}

// Every frame. ApplyAo after the rows, so its levels win.
void ApplySettings() {
    ApplyUpscale();
    ApplyTexturePool();
    for (RowList* list : kAllRows) ApplyRows(*list);
    ApplyAo();
    ApplySettingsCode();
}

void SaveSettings() {
    for (RowList* list : kAllRows) SaveRows(*list);
    SaveTexturePool();
    SaveAo();
    SaveShadows();
}

// ---------------------------------------------------------------- presets
//
// A preset sets the quality settings (key -> choice); a key a preset doesn't list takes the
// Game preset's value (0 unless Game lists it). The DLSS mode, texture sharpness and the Camera
// tab (taste) are in no preset and left alone; so are GTAO strength and radius. The preset shown
// is derived from the settings, "Custom" when none matches. Ultra+ is the add-on's defaults;
// Cinematic takes every setting to its top choice except the sun's size (a matter of taste).
// Costs aren't measured yet.

struct QualityPreset {
    const char* label;
    const char* cost;  // shown next to the label in the picker ("" = not measured)
    const char* tip;
    std::vector<std::pair<const char*, int>> values;
};

const QualityPreset kPresets[] = {
    // GTAO quality and multi-bounce only matter with GTAO on; Game lists them at their defaults.
    {"Game", "", "The game's own settings: every detail, shadow and lighting setting at Game.",
     {{"GtaoQuality", 1}, {"GtaoMultiBounce", 1}}},
    {"Ultra+", "",
     "The add-on's defaults: texture memory sized to your card, 16x filtering, further view and\n"
     "grass distance, detailed characters and terrain, 4096 shadows with 4 soft cascades, soft\n"
     "and contact sun shadows, GTAO (Ultra, multi-bounce), better reflections, finer fog, light\n"
     "shafts, bloom, smoke and glass lighting, and skin scattering.",
     {{"TexturePool", 1}, {"DetailAnisotropy16", 1}, {"DetailViewDistance", 1}, {"DetailGrassDistance", 1},
      {"DetailFoliageDensity", 1}, {"DetailCharacterLod", 1}, {"DetailTerrainLod", 1}, {"DetailShadows", 1},
      {"DetailShadowCascades", 1}, {"DetailCascadeBlend", 1}, {"ShadowSunSize", 1}, {"ShadowContact", 1},
      {"ExpCapsuleShadows", 1}, {"AmbientOcclusion", 3}, {"GtaoQuality", 1}, {"GtaoMultiBounce", 1},
      {"DetailReflections", 1}, {"FxReflectionRoughness", 1}, {"ExpVolumetricFog", 1}, {"FxLightShafts", 1},
      {"FxBloom", 1}, {"FxTranslucencyLighting", 1}, {"ExpSkin", 1}}},
    {"Cinematic", "",
     "Ultra+ with every setting at its top choice: the farthest grass, Ultra shadows (further,\n"
     "small objects cast shadows) with 5 cascades, medium contact shadows, GTAO at Max quality\n"
     "and Ultra volumetric fog. Costs the most.",
     {{"TexturePool", 1}, {"DetailAnisotropy16", 1}, {"DetailViewDistance", 1}, {"DetailGrassDistance", 2},
      {"DetailFoliageDensity", 1}, {"DetailCharacterLod", 1}, {"DetailTerrainLod", 1}, {"DetailShadows", 2},
      {"DetailShadowCascades", 2}, {"DetailCascadeBlend", 1}, {"ShadowSunSize", 1}, {"ShadowContact", 2},
      {"ExpCapsuleShadows", 1}, {"AmbientOcclusion", 3}, {"GtaoQuality", 2}, {"GtaoMultiBounce", 1},
      {"DetailReflections", 1}, {"FxReflectionRoughness", 1}, {"ExpVolumetricFog", 2}, {"FxLightShafts", 1},
      {"FxBloom", 1}, {"FxTranslucencyLighting", 1}, {"ExpSkin", 1}}},
};
constexpr int kPresetCount = static_cast<int>(std::size(kPresets));

// The choice a preset key stands for: a row's, or one of the settings with their own logic.
int* PresetChoice(const std::string& key) {
    if (key == "TexturePool") return &g_texturePool;
    if (key == "AmbientOcclusion") return &g_ao.choice;
    if (key == "GtaoQuality") return &g_ao.quality;
    if (key == "GtaoMultiBounce") return &g_ao.multiBounce;
    if (key == "ShadowSunSize") return &g_shadows.sun;
    if (key == "ShadowContact") return &g_shadows.contact;
    CvarRow* e = FindRow(key.c_str());
    return e ? &e->choice : nullptr;
}

std::string ChoiceLabel(const std::string& key, int choice) {
    if (key == "TexturePool") return "Texture memory Auto";
    if (key == "AmbientOcclusion") return std::string("Ambient occlusion ") + kAoLabels[choice];
    if (key == "GtaoQuality") return std::string("GTAO quality ") + kGtaoQualityLabels[choice];
    if (key == "GtaoMultiBounce") return std::string("GTAO multi-bounce ") + kOffOn[choice];
    if (key == "ShadowSunSize") return std::string("Soft sun shadows ") + kSunSizes[choice].label;
    if (key == "ShadowContact") return std::string("Contact shadows ") + kContactLengths[choice].label;
    CvarRow* e = FindRow(key.c_str());
    if (!e || choice < 0 || choice >= static_cast<int>(e->labels.size())) return key;
    return std::string(e->name) + " " + e->labels[choice];
}

std::set<std::string> PresetKeys() {
    std::set<std::string> keys;
    for (const QualityPreset& p : kPresets)
        for (auto& [key, choice] : p.values) keys.insert(key);
    return keys;
}

int PresetValue(const QualityPreset& p, const std::string& key) {
    for (const QualityPreset* q : {&p, &kPresets[0]})
        for (auto& [k, choice] : q->values)
            if (key == k) return choice;
    return 0;
}

int CurrentPreset() {
    std::set<std::string> keys = PresetKeys();
    for (int i = 0; i < kPresetCount; ++i) {
        bool match = true;
        for (const std::string& key : keys)
            if (int* c = PresetChoice(key); c && *c != PresetValue(kPresets[i], key)) match = false;
        if (match) return i;
    }
    return -1;
}

void ApplyPreset(int index) {
    for (const std::string& key : PresetKeys())
        if (int* c = PresetChoice(key)) *c = PresetValue(kPresets[index], key);
    SaveSettings();
    Log("preset: %s", kPresets[index].label);
}

// ---------------------------------------------------------------- frame time and measuring
//
// A measurement runs stages one after another: each sets the preset settings (everything at the
// Game preset's value except the stage's own choices), settles, then averages the time between
// presents. "Presets" runs Game and each preset; "each option" runs Game, every choice a preset
// uses on its own (the GTAO choices with GTAO on), then Game again (to show drift). The settings
// come back afterwards, unsaved in between.

int64_t g_qpcFreq = 1;
std::vector<float> g_frameMs;  // recent frames, a ring of kFrameRing
size_t g_framePos = 0;
constexpr size_t kFrameRing = 240;

float Mean(const std::vector<float>& v) {
    if (v.empty()) return 0;
    double sum = 0;
    for (float x : v) sum += x;
    return static_cast<float>(sum / v.size());
}

struct Stage {
    std::string label;
    std::vector<std::pair<std::string, int>> values;
};

struct Measure {
    bool running = false;
    bool perOption = false;
    ULONGLONG settleMs = 5000, sampleMs = 8000;
    std::vector<Stage> stages;
    size_t stage = 0;
    ULONGLONG stageStart = 0;
    bool sampling = false;
    std::vector<float> samples;
    std::vector<std::pair<std::string, int>> saved;
    std::vector<float> resultMs;
    bool done = false;
    std::string note;
    std::string summary;
} g_measure;

void ApplyStage(const Stage& st) {
    for (const std::string& key : PresetKeys())
        if (int* c = PresetChoice(key)) *c = PresetValue(kPresets[0], key);
    for (auto& [key, choice] : st.values)
        if (int* c = PresetChoice(key)) *c = choice;
}

void StartMeasure(bool perOption) {
    g_measure = {};
    g_measure.running = true;
    g_measure.perOption = perOption;
    if (perOption) g_measure.settleMs = 4000, g_measure.sampleMs = 6000;
    for (const std::string& key : PresetKeys())
        if (int* c = PresetChoice(key)) g_measure.saved.push_back({key, *c});
    g_measure.stages.push_back({"Game", {}});
    if (perOption) {
        for (const std::string& key : PresetKeys()) {
            std::set<int> choices;
            for (int i = 1; i < kPresetCount; ++i)
                if (int c = PresetValue(kPresets[i], key); c != PresetValue(kPresets[0], key)) choices.insert(c);
            const bool gtaoKey = key == "GtaoQuality" || key == "GtaoMultiBounce";
            for (int c : choices) {
                Stage st{ChoiceLabel(key, c), {{key, c}}};
                if (gtaoKey) st.label += " (with GTAO)", st.values.push_back({"AmbientOcclusion", kAoGtao});
                g_measure.stages.push_back(st);
            }
        }
        g_measure.stages.push_back({"Game (again)", {}});
    } else {
        for (int i = 1; i < kPresetCount; ++i) {
            Stage st{kPresets[i].label, {}};
            for (auto& [key, c] : kPresets[i].values) st.values.push_back({key, c});
            g_measure.stages.push_back(st);
        }
    }
    g_measure.resultMs.assign(g_measure.stages.size(), 0);
    g_measure.stageStart = GetTickCount64();
    ApplyStage(g_measure.stages[0]);
    Log("measure: started (%s, %zu stages)", perOption ? "each option" : "presets", g_measure.stages.size());
}

void FinishMeasure(const char* why) {
    for (auto& [key, choice] : g_measure.saved)
        if (int* c = PresetChoice(key)) *c = choice;
    g_measure.running = false;
    g_measure.note = why;
    Log("measure: %s", why);
}

void SummarizeMeasure() {
    const std::vector<float>& r = g_measure.resultMs;
    const float base = g_measure.perOption ? (r.front() + r.back()) / 2 : r.front();
    char line[160];
    if (!g_measure.perOption) {
        std::string sum = "Game " + std::to_string(static_cast<int>(1000.0f / r[0] + 0.5f)) + " fps";
        for (size_t i = 1; i < r.size(); ++i) {
            snprintf(line, sizeof(line), ", %s %.0f fps (%+.0f%% frame time)", g_measure.stages[i].label.c_str(),
                     1000.0f / r[i], (r[i] / base - 1) * 100);
            sum += line;
        }
        g_measure.summary = sum;
        Log("measure: result: %s", sum.c_str());
        return;
    }
    std::vector<size_t> order;  // each option, costliest first
    for (size_t i = 1; i + 1 < r.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return r[x] > r[y]; });
    Log("measure: each option vs Game %.2f ms (start %.2f, end %.2f), costliest first:", base, r.front(), r.back());
    std::string sum;
    for (size_t k = 0; k < order.size(); ++k) {
        size_t i = order[k];
        Log("measure:   %+6.1f%%  %+6.2f ms  %s", (r[i] / base - 1) * 100, r[i] - base, g_measure.stages[i].label.c_str());
        if (k < 4) {
            snprintf(line, sizeof(line), "%s%s %+.0f%%", k ? ", " : "Costliest: ", g_measure.stages[i].label.c_str(),
                     (r[i] / base - 1) * 100);
            sum += line;
        }
    }
    g_measure.summary = sum + " (all in " FOT_PRODUCT ".log)";
}

void MeasureOnFrame(float ms, bool focused) {
    if (!g_measure.running) return;
    if (!focused) return FinishMeasure("stopped: the game lost focus");
    ULONGLONG now = GetTickCount64();
    if (!g_measure.sampling) {
        if (now - g_measure.stageStart >= g_measure.settleMs) g_measure.sampling = true, g_measure.stageStart = now;
        return;
    }
    g_measure.samples.push_back(ms);
    if (now - g_measure.stageStart < g_measure.sampleMs) return;
    float avg = Mean(g_measure.samples);
    g_measure.resultMs[g_measure.stage] = avg;
    Log("measure: %s %.2f ms average (%.1f fps) over %zu frames", g_measure.stages[g_measure.stage].label.c_str(), avg,
        1000.0f / avg, g_measure.samples.size());
    g_measure.samples.clear();
    g_measure.sampling = false;
    g_measure.stageStart = now;
    if (++g_measure.stage < g_measure.stages.size()) {
        ApplyStage(g_measure.stages[g_measure.stage]);
        return;
    }
    g_measure.done = true;
    SummarizeMeasure();
    FinishMeasure("done");
}

bool GameFocused() {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

void FrameOnPresent() {
    static int64_t last = 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    float ms = last ? static_cast<float>((t.QuadPart - last) * 1000.0 / g_qpcFreq) : 0;
    last = t.QuadPart;
    bool focused = GameFocused();
    if (!ms || !focused || ms > 500) {
        if (g_measure.running && !focused) MeasureOnFrame(ms, false);
        return;
    }
    if (g_frameMs.size() < kFrameRing)
        g_frameMs.push_back(ms);
    else
        g_frameMs[g_framePos] = ms;
    g_framePos = (g_framePos + 1) % kFrameRing;
    MeasureOnFrame(ms, true);
}

// ---------------------------------------------------------------- shader replacement

// Game shaders swapped for FallenOrderTweaks' own when the game creates them, matched by DXBC
// container digest and size. All from GlobalShaderCache-PCD3D_SM5.bin in
// pakchunk0-WindowsNoEditor_1_P.pak (tools/gsc.py); docs/FINDINGS.md has their bindings.
enum class Feature { Gtao, Shadows, None };

struct ShaderTarget {
    const char* name;
    uint8_t digest[16];  // DXBC container hash (bytes 4..20)
    size_t size;
    Feature feature;
    const uint8_t* replacement;  // null: only logged
    size_t replacementSize;
};

const ShaderTarget kTargets[] = {
    // The single-level, full-resolution AO pass (FPostProcessAmbientOcclusionPS<0, 0, Q>,
    // quality 0-4; 3 and 4 compile to the same code). It only runs with
    // r.AmbientOcclusionLevels 1, which GTAO forces; with GTAO off the levels go back to the
    // game's and its own multi-level passes, never replaced, run as before.
    {"AO pass, quality 0", {0x81, 0x18, 0xe6, 0xa8, 0xdf, 0xbe, 0x54, 0xcf, 0x93, 0x6f, 0x2b, 0x5a, 0xcd, 0x8b, 0x3d, 0xad},
     7964, Feature::Gtao, kGtaoPs, sizeof(kGtaoPs)},
    {"AO pass, quality 1", {0x9d, 0x53, 0x9f, 0xd1, 0x50, 0x0c, 0x71, 0xcb, 0x2c, 0x2e, 0x4b, 0x1c, 0x0c, 0x2f, 0x29, 0x75},
     8880, Feature::Gtao, kGtaoPs, sizeof(kGtaoPs)},
    {"AO pass, quality 2", {0x4a, 0x36, 0x41, 0x52, 0xd0, 0x35, 0x27, 0x4f, 0x53, 0xd7, 0xc3, 0x3f, 0x01, 0x43, 0x6a, 0x34},
     13824, Feature::Gtao, kGtaoPs, sizeof(kGtaoPs)},
    {"AO pass, quality 3-4", {0x19, 0x6c, 0x63, 0xcb, 0x44, 0x8b, 0x24, 0x93, 0x0f, 0x2b, 0x5a, 0xcf, 0xb0, 0xf5, 0x9a, 0x3b},
     16952, Feature::Gtao, kGtaoPs, sizeof(kGtaoPs)},
    // The shadow projection pass at shadow quality 4-5 (Epic), FShadowProjectionPS<Q, fade
    // plane, transmission>; 4 and 5 compile to the same code. Rebuilt with soft and contact
    // shadows for the sun; without settings it runs the game's own filtering.
    {"shadow projection", {0x24, 0x43, 0x25, 0x87, 0xeb, 0x61, 0xe7, 0x4a, 0x6c, 0xe3, 0xe1, 0xb8, 0xc5, 0xa8, 0xb2, 0x38},
     7236, Feature::Shadows, kShadowPs, sizeof(kShadowPs)},
    {"shadow projection, fade plane", {0x96, 0x1d, 0x82, 0x0c, 0x5c, 0x59, 0x68, 0xd6, 0x40, 0xdc, 0xcb, 0xa9, 0x0b, 0x62, 0xa9, 0xd4},
     7336, Feature::Shadows, kShadowFadePs, sizeof(kShadowFadePs)},
    // With subsurface transmission (lights that shine through skin and foliage).
    {"shadow projection, transmission", {0xe0, 0x93, 0x3f, 0x0d, 0xb7, 0xd8, 0x68, 0x75, 0x80, 0x41, 0x52, 0xbd, 0x4c, 0x19, 0x85, 0x38},
     9696, Feature::Shadows, kShadowTransPs, sizeof(kShadowTransPs)},
    {"shadow projection, fade plane and transmission", {0x17, 0xf3, 0x16, 0xac, 0x15, 0x1b, 0x1e, 0x9f, 0xe3, 0x4b, 0x24, 0xdb, 0xac, 0x91, 0xa4, 0x63},
     9796, Feature::Shadows, kShadowFadeTransPs, sizeof(kShadowFadeTransPs)},
};

struct FeatureStats {
    std::atomic<int> replaced{0};  // shaders swapped
    std::atomic<int> missed{0};    // created while the feature was off: the game's until restart
};
FeatureStats g_gtaoStats, g_shadowStats;

const ShaderTarget* FindTarget(const void* code, size_t size) {
    if (!code || size < 20 || memcmp(code, "DXBC", 4) != 0) return nullptr;
    for (const ShaderTarget& t : kTargets)
        if (size == t.size && memcmp(static_cast<const uint8_t*>(code) + 4, t.digest, 16) == 0) return &t;
    return nullptr;
}

// ---------------------------------------------------------------- creating shaders

// ReShade hands us its copy of the shader description, so changing it changes what the
// driver compiles. The replacements are static, so they outlive the call. A feature's
// shaders are swapped only if it is on when the game creates them (at load, or the first
// time the pass runs); its settings then change live.
bool OnCreatePipeline(device* dev, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects) {
    if (dev->get_api() != device_api::d3d11) return false;
    bool changed = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (subobjects[i].type != pipeline_subobject_type::pixel_shader || !subobjects[i].data) continue;
        auto* desc = static_cast<shader_desc*>(subobjects[i].data);
        const ShaderTarget* t = FindTarget(desc->code, desc->code_size);
        if (!t) continue;
        if (t->feature == Feature::None) {
            Log("shaders: %s created (not rebuilt: the game's own)", t->name);
            continue;
        }
        bool on = t->feature == Feature::Gtao ? GtaoOn() : ShadowsOn();
        FeatureStats& stats = t->feature == Feature::Gtao ? g_gtaoStats : g_shadowStats;
        if (!on) {
            ++stats.missed;
            Log("shaders: %s created while its feature is off: the game's own until restart", t->name);
            continue;
        }
        desc->code = t->replacement;
        desc->code_size = t->replacementSize;
        changed = true;
        ++stats.replaced;
        Log("shaders: replacing the %s", t->name);
    }
    return changed;
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
    FrameOnPresent();
    ApplySettings();
    ApplyOverrides();
    if (g_frames % 60 == 0) CheckOverrides();
}

// ---------------------------------------------------------------- overlay

// Each tab is one table, name | control, with the same column widths on every tab. Hovering a
// setting explains it and shows the game's own value; the Advanced tab lists every console
// variable. Notes appear only when something needs doing (a restart, a missing Luma).

bool LumaLoaded() { return GetModuleHandleW(L"Luma-Unreal Engine.addon") != nullptr; }

// Tooltip for the hovered item: the help text, then the game's value of `cvar` if given.
void Help(const char* text, const wchar_t* cvar = nullptr) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
    if (text && *text) ImGui::TextUnformatted(text);
    float game = 0;
    if (cvar && GameValue(*FindOverride(cvar), game)) ImGui::TextDisabled("Game value: %g", game);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

const char* const kWidestNames[] = {"Rough surface reflections", "Smoke and glass lighting", "Chromatic aberration",
                                    "Character soft shadows"};

bool BeginSettings(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
    float name = 0;
    for (const char* n : kWidestNames) name = std::max(name, ImGui::CalcTextSize(n).x);
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, name + ImGui::GetFontSize() * 1.5f);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 15);
    return true;
}

// One row: the name (hover for help), then the control cell, sized for the caller's widget.
// A sub-setting is indented under the row it belongs to.
void SettingRow(const char* name, const char* help, const wchar_t* cvar, bool sub = false) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (sub) ImGui::Indent(ImGui::GetFontSize());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    Help(help, cvar);
    if (sub) ImGui::Unindent(ImGui::GetFontSize());
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool ComboRow(const char* id, const char* name, const char* help, const wchar_t* cvar, int* choice,
              const char* const* labels, int count, bool sub = false) {
    SettingRow(name, help, cvar, sub);
    bool changed = ImGui::Combo(id, choice, labels, count);
    Help(help, cvar);
    return changed;
}

bool SliderRow(const char* id, const char* name, const char* help, const wchar_t* cvar, int* value, int min, int max,
               const char* format) {
    SettingRow(name, help, cvar, true);
    bool changed = ImGui::SliderInt(id, value, min, max, format);
    Help(help, cvar);
    return changed;
}

// A row list's rows, inside an open table. True if a choice changed.
bool DrawRows(RowList& list) {
    bool changed = false;
    for (CvarRow& e : list.rows) {
        ImGui::PushID(e.key);
        changed |= ComboRow("##c", e.name, e.help, e.shown, &e.choice, e.labels.data(), static_cast<int>(e.labels.size()));
        ImGui::PopID();
    }
    return changed;
}

// Right-aligned button at the end of a tab.
bool ResetButton(const char* label = "Reset tab") {
    ImGui::Spacing();
    float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
    return ImGui::Button(label);
}

void Note(const char* text, bool warn = false) {
    ImGui::PushTextWrapPos(0.0f);
    if (warn)
        ImGui::TextColored(kWarn, "%s", text);
    else
        ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

void ResetImage() {
    g_upscale = UpscaleSettings{};
    SaveUpscale();
    ResetRows(g_imageRows);
}

void ResetCamera() { ResetRows(g_cameraRows); }

void ResetDetail() {
    g_texturePool = kPoolAuto;
    SaveTexturePool();
    ResetRows(g_detailRows);
}

void ResetShadows() {
    g_shadows = ShadowSettings{};
    SaveShadows();
    ResetRows(g_shadowRows);
    ResetRows(g_characterShadowRows);
}

void ResetLighting() {
    g_ao = AoSettings{};
    SaveAo();
    ResetRows(g_lightingRows);
}

void DrawImage() {
    bool upscaleChanged = false, rowsChanged = false;
    if (BeginSettings("##image")) {
        const char* labels[std::size(kModes)];
        for (size_t i = 0; i < std::size(kModes); ++i) labels[i] = kModes[i].label;
        upscaleChanged |= ComboRow("##mode", "DLSS mode",
                                   "The resolution the game renders at before DLSS upscales it to your screen.\n"
                                   "DLAA: full resolution, the best image.\nQuality 67 %, Balanced 58 %, Performance "
                                   "50 %, Ultra Performance 33 %: faster, softer.\nChanges apply at once.",
                                   L"r.ScreenPercentage", &g_upscale.mode, labels, static_cast<int>(std::size(kModes)));
        int bias = g_upscale.textureBias ? 1 : 0;
        const char* biases[] = {"Game", "Match DLSS"};
        if (ComboRow("##bias", "Texture sharpness",
                     "Below DLAA the game picks texture detail for the lower render resolution, so textures look "
                     "softer than they should. Match DLSS sharpens them back to your screen resolution's detail "
                     "(NVIDIA's recommended texture bias for DLSS). No effect at DLAA.",
                     L"r.MipMapLODBias", &bias, biases, 2)) {
            g_upscale.textureBias = bias == 1;
            upscaleChanged = true;
        }
        rowsChanged |= DrawRows(g_imageRows);
        ImGui::EndTable();
    }
    float percent = kModes[g_upscale.mode].percent;
    if (percent > 0 && percent < 100 && g_outputWidth) {
        char line[128];
        snprintf(line, sizeof(line), "Renders at %u x %u for %u x %u.",
                 static_cast<uint32_t>(lroundf(g_outputWidth * percent / 100)),
                 static_cast<uint32_t>(lroundf(g_outputHeight * percent / 100)), g_outputWidth, g_outputHeight);
        Note(line);
    }
    if (!LumaLoaded()) Note("Luma isn't loaded, so the game's own TAA upscales instead of DLSS.", true);
    if (ResetButton()) ResetImage();
    if (upscaleChanged) SaveUpscale();
    if (rowsChanged) SaveRows(g_imageRows);
}

void DrawCamera() {
    bool rowsChanged = false;
    if (BeginSettings("##camera")) {
        rowsChanged |= DrawRows(g_cameraRows);
        ImGui::EndTable();
    }
    if (ResetButton()) ResetCamera();
    if (rowsChanged) SaveRows(g_cameraRows);
}

void DrawDetail() {
    bool poolChanged = false, rowsChanged = false;
    if (BeginSettings("##detail")) {
        char autoLabel[64];
        snprintf(autoLabel, sizeof(autoLabel), "Auto (%s)", kVram[DetectedVramRow()].label);
        const char* pools[2 + std::size(kVram)] = {"Game", autoLabel};
        for (size_t i = 0; i < std::size(kVram); ++i) pools[2 + i] = kVram[i].label;
        char poolHelp[256];
        snprintf(poolHelp, sizeof(poolHelp),
                 "Video memory for textures, sized to your graphics card: about a quarter of it (now %d MB), so "
                 "textures load at full resolution with less pop-in. Auto reads your card's memory.",
                 kVram[VramRow()].poolMb);
        poolChanged |= ComboRow("##pool", "Texture memory", poolHelp, L"r.Streaming.PoolSize", &g_texturePool, pools,
                                static_cast<int>(std::size(pools)));
        rowsChanged |= DrawRows(g_detailRows);
        ImGui::EndTable();
    }
    if (ResetButton()) ResetDetail();
    if (poolChanged) SaveTexturePool();
    if (rowsChanged) SaveRows(g_detailRows);
}

void DrawShadows() {
    bool rowsChanged = false, sunChanged = false;
    if (BeginSettings("##shadows")) {
        rowsChanged |= DrawRows(g_shadowRows);
        const char* suns[std::size(kSunSizes)];
        for (size_t i = 0; i < std::size(kSunSizes); ++i) suns[i] = kSunSizes[i].label;
        sunChanged |= ComboRow("##sun", "Soft sun shadows",
                               "Sun shadows that soften the farther they fall from what casts them, as real ones do: "
                               "sharp where a pole meets the ground, soft at the tip of its shadow. A larger sun "
                               "gives softer shadows.",
                               nullptr, &g_shadows.sun, suns, static_cast<int>(std::size(kSunSizes)));
        const char* contacts[std::size(kContactLengths)];
        for (size_t i = 0; i < std::size(kContactLengths); ++i) contacts[i] = kContactLengths[i].label;
        sunChanged |= ComboRow("##contact", "Contact shadows",
                               "Small sun shadows where objects meet surfaces (feet, rocks, crates, grass), which the "
                               "shadow map is too coarse for. Longer reaches further from the contact.",
                               nullptr, &g_shadows.contact, contacts, static_cast<int>(std::size(kContactLengths)));
        rowsChanged |= DrawRows(g_characterShadowRows);
        ImGui::EndTable();
    }
    if (ShadowsOn() && g_shadowStats.replaced == 0 && g_shadowStats.missed > 0)
        Note("Restart the game to switch soft and contact shadows on.", true);
    if (ResetButton()) ResetShadows();
    if (sunChanged) SaveShadows();
    if (rowsChanged) {
        SaveRows(g_shadowRows);
        SaveRows(g_characterShadowRows);
    }
}

void DrawLighting() {
    bool aoChanged = false, rowsChanged = false;
    if (BeginSettings("##lighting")) {
        aoChanged |= ComboRow("##ao", "Ambient occlusion",
                              "Shading in corners, creases and under objects.\nMore / Max levels: the game's own, "
                              "deeper and smoother.\nGTAO: ground-truth ambient occlusion in place of the game's: "
                              "more accurate contact shading, none on open surfaces, no dark halos around thin "
                              "objects. The game's AO strength and distance fade still apply.",
                              L"r.AmbientOcclusionLevels", &g_ao.choice, kAoLabels, static_cast<int>(std::size(kAoLabels)));
        if (GtaoOn()) {
            aoChanged |= SliderRow("##strength", "Strength",
                                   "How dark GTAO's shading is, on top of the game's own AO strength. 100 % is about "
                                   "as dark as the game's AO.",
                                   nullptr, &g_ao.strengthPercent, 25, 300, "%d %%");
            aoChanged |= SliderRow("##radius", "Radius",
                                   "How far GTAO looks for occluders, relative to the game's AO radius. Larger reaches "
                                   "further from corners; smaller keeps it to contact shading.",
                                   L"r.AmbientOcclusionRadiusScale", &g_ao.radiusPercent, 50, 200, "%d %%");
            aoChanged |= ComboRow("##quality", "Quality",
                                  "Samples per pixel: more give smoother, steadier shading with less noise, at more "
                                  "GPU cost.\nHigh: 4 directions x 8 steps. Ultra: 6 x 12 (about 2x). Max: 8 x 16 "
                                  "(about 4x).",
                                  nullptr, &g_ao.quality, kGtaoQualityLabels, static_cast<int>(std::size(kGtaoQualityLabels)),
                                  true);
            aoChanged |= ComboRow("##bounce", "Multi-bounce",
                                  "Light bouncing between surfaces brightens occluded areas: broad, shallow shading "
                                  "lifts while deep contact shading stays. An approximation: the AO pass doesn't see "
                                  "surface colours, so a typical surface brightness is assumed.",
                                  nullptr, &g_ao.multiBounce, kOffOn, 2, true);
        }
        rowsChanged |= DrawRows(g_lightingRows);
        ImGui::EndTable();
    }
    if (GtaoOn() && g_gtaoStats.replaced == 0 && g_gtaoStats.missed > 0)
        Note("Restart the game to switch GTAO on.", true);
    if (ResetButton()) ResetLighting();
    if (aoChanged) SaveAo();
    if (rowsChanged) SaveRows(g_lightingRows);
}

void DrawAdvanced() {
    ImGui::TextDisabled("Console variables: %s", g_status.c_str());
    ImGui::TextDisabled("GTAO: %s.  Sun shadows: %s.",
                        g_gtaoStats.replaced > 0 ? "active" : GtaoOn() ? "waiting for the AO pass" : "off",
                        g_shadowStats.replaced > 0 ? "active" : ShadowsOn() ? "waiting for the shadow pass" : "off");
    ImGui::Spacing();
    Note("Every console variable the settings control: the game's own value, and what it is now.");
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
    if (ResetButton("Reset all settings")) {
        ResetImage();
        ResetCamera();
        ResetDetail();
        ResetShadows();
        ResetLighting();
    }
}

// The preset picker, the frame time, and the measure buttons, above the tabs.
void DrawPresets() {
    const bool measuring = g_measure.running;
    ImGui::BeginDisabled(measuring);
    const int current = CurrentPreset();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::BeginCombo("Quality preset", current >= 0 ? kPresets[current].label : "Custom")) {
        for (int i = 0; i < kPresetCount; ++i) {
            std::string item = kPresets[i].label;
            if (*kPresets[i].cost) item += std::string(" (") + kPresets[i].cost + ")";
            if (ImGui::Selectable(item.c_str(), i == current) && i != current) ApplyPreset(i);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kPresets[i].tip);
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    Help("Sets the detail, shadow and lighting settings together (the DLSS mode, texture sharpness and "
         "camera settings are left alone). Measure preset costs checks them on your PC. Changing a setting "
         "afterwards shows Custom.");
    float ms = Mean(g_frameMs);
    if (ms > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("   %.1f ms (%.0f fps)", ms, 1000.0f / ms);
    }
    if (measuring) {
        ImGui::TextColored(kWarn, "Measuring %s (%zu of %zu)... keep the camera still.",
                           g_measure.stages[g_measure.stage].label.c_str(), g_measure.stage + 1, g_measure.stages.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop")) FinishMeasure("stopped");
        return;
    }
    if (ImGui::SmallButton("Measure preset costs")) StartMeasure(false);
    Help("Runs Game and each preset for 13 s (5 s to settle, 8 s measured) and shows how much longer each "
         "frame takes than with Game. Stand still in a typical scene, close the overlay and leave the mouse "
         "alone. Your settings come back afterwards. GTAO and soft shadows only count if they were on when "
         "the game started.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Measure each option")) StartMeasure(true);
    Help("Runs every choice the presets use on its own (10 s each, about 5 minutes in all) and writes how much "
         "each one costs to " FOT_PRODUCT ".log, costliest first.");
    if (g_measure.done)
        Note(g_measure.summary.c_str());
    else if (!g_measure.note.empty())
        Note(("Measurement " + g_measure.note + ".").c_str(), true);
}

void DrawOverlay(effect_runtime*) {
    std::lock_guard lock(g_mutex);
    ImGui::TextDisabled(FOT_PRODUCT " " FOT_VERSION "  -  hover a setting for details; saved and applied at every start");
    if (g_unsupported) {
        Note((std::string("Not active: ") + g_status + ".").c_str(), true);
        return;
    }
    if (!g_manager) {
        Note("Waiting for the game's console variables...", true);
        return;
    }
    if (g_timestamp != kKnownTimestamp) Note("A different game build than the one analysed; the layout check passed.", true);
    ImGui::Spacing();
    DrawPresets();
    ImGui::Spacing();
    if (g_measure.running) return;  // the settings belong to the measurement until it ends
    if (!ImGui::BeginTabBar("##tabs")) return;
    struct Tab {
        const char* name;
        void (*draw)();
    };
    const Tab tabs[] = {{"Image", DrawImage}, {"Camera", DrawCamera}, {"Detail", DrawDetail}, {"Shadows", DrawShadows},
                        {"Lighting", DrawLighting}, {"Advanced", DrawAdvanced}};
    for (const Tab& t : tabs)
        if (ImGui::BeginTabItem(t.name)) {
            ImGui::Spacing();
            t.draw();
            ImGui::EndTabItem();
        }
    ImGui::EndTabBar();
}

void Install() {
    OpenLog();
    LogGameInfo();
    InitSettings();
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = f.QuadPart;
    reshade::register_event<reshade::addon_event::create_pipeline>(&OnCreatePipeline);
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
