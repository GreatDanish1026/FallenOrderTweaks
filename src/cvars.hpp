// Finds Unreal console variables in Jedi: Fallen Order (UE 4.21, no PDB) and reads and sets
// them. Adapted from UntilDawnTweaks (UE 5); every offset below was read from
// SwGame-Win64-Shipping.exe itself (docs/FINDINGS.md, "Console variables").
//
// IConsoleManager::Singleton is found from the game's own code: cvar lookups load it with
// `mov rcx, [rip+Singleton]` shortly before `lea rdx, [rip+L"r.ScreenPercentage"]`, then call
// FindConsoleVariable through the vtable. A few unrelated matches exist, so the address and
// vtable offset most matches agree on win.
//
// Vtable offsets (UE 4.21: no bool cvars and only one IsVariable* check, so everything after
// AsVariable sits lower than in 4.26):
//   IConsoleManager  +0x90  FindConsoleVariable(const TCHAR* Name, bool bTrackFrequentCalls)
//   IConsoleObject   +0x18  GetFlags        +0x28 AsVariable
//                    +0x30  IsVariableInt
//                    +0x38  AsVariableInt   +0x40 AsVariableFloat   (TConsoleVariableData<T>*,
//                                                                   null for Ref cvars)
//   IConsoleVariable +0x60  Set(const TCHAR* Value, EConsoleVariableFlags SetBy)
//                    +0x68  GetInt          +0x70 GetFloat
//
// Storage:
//   data cvar  main (game thread) value +0x38, renderer's copy right after it (+0x3c)
//   Ref cvar   pointer to the global the engine reads at +0x38, main value +0x40
// Set stores the main value and calls OnCVarChange, which on the game thread queues the copy
// to the renderer's value, and on any other thread (ReShade's present) copies it at once. So
// unlike UE 5 no manual mirroring is needed.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>

namespace cvars {

constexpr uint32_t kFindConsoleVariable = 0x90;
constexpr uint32_t kIsVariableInt = 0x30;
constexpr uint32_t kAsVariableInt = 0x38;
constexpr uint32_t kAsVariableFloat = 0x40;
constexpr uint32_t kSet = 0x60;
constexpr uint32_t kGetFloat = 0x70;
constexpr uint32_t kFlags = 0x18;      // EConsoleVariableFlags; top byte = SetBy priority
constexpr uint32_t kDataValue = 0x38;  // TConsoleVariableData<T> inside FConsoleVariable<T>
// What FConsoleManager::ProcessUserConsoleInput passes to Set: the highest priority the game
// uses (its own code sets cvars at 0x08000000, the graphics menu at 0x01000000).
constexpr uint32_t kSetByConsole = 0x09000000;

struct Section {
    const uint8_t* begin = nullptr;
    size_t size = 0;
};

inline bool FindSection(const uint8_t* base, const char* name, Section& out) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (strncmp(reinterpret_cast<const char*>(sec[i].Name), name, 8) == 0) {
            out = {base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize};
            return true;
        }
    return false;
}

inline bool InSection(const Section& s, const void* p) {
    auto b = static_cast<const uint8_t*>(p);
    return s.begin && b >= s.begin && b < s.begin + s.size;
}

// A NUL-terminated UTF-16 string that starts right after another string's terminator.
inline const uint8_t* FindWideString(const Section& s, const wchar_t* text) {
    const size_t n = (wcslen(text) + 1) * 2;
    for (size_t i = 2; i + n <= s.size; i += 2)
        if (s.begin[i - 2] == 0 && s.begin[i - 1] == 0 && memcmp(s.begin + i, text, n) == 0) return s.begin + i;
    return nullptr;
}

struct ManagerScan {
    void** singleton = nullptr;  // IConsoleManager::Singleton (the variable holding the pointer)
    int votes = 0, matches = 0;  // how many lookups agreed on it, out of how many
    int32_t findOffset = -1;     // the vtable offset those lookups call
    int findVotes = 0, findMatches = 0;
};

// Cvars the game looks up by name; each is only looked up a few times (r.ScreenPercentage 4),
// so the vote runs over all of them.
const wchar_t* const kLookupNames[] = {
    L"r.ScreenPercentage", L"r.MaxAnisotropy",     L"r.Shadow.MaxResolution", L"r.Shadow.MaxCSMResolution",
    L"r.ViewDistanceScale", L"r.SSR.Quality",      L"r.TemporalAA.Upsampling", L"r.Streaming.PoolSize",
    L"r.DetailMode",        L"r.MaterialQualityLevel", L"r.PostProcessAAQuality", L"r.VSync",
    L"r.MotionBlurQuality", L"r.DepthOfFieldQuality",  L"r.BloomQuality",         L"r.Upscale.Quality",
    L"r.SceneColorFringeQuality", L"r.Tonemapper.Sharpen", L"r.HDR.EnableHDROutput", L"t.MaxFPS",
};

// Scans .text for lookups of those names: mov rcx,[rip+Singleton] within 0x40 bytes before
// `lea rdx, [rip+name]`, and the first call [rax+disp] within 0x30 bytes after it.
inline ManagerScan ScanConsoleManager(const uint8_t* base) {
    ManagerScan out;
    Section text, rdata;
    if (!FindSection(base, ".text", text) || !FindSection(base, ".rdata", rdata)) return out;
    const uint8_t* names[sizeof(kLookupNames) / sizeof(kLookupNames[0])];
    size_t nameCount = 0;
    for (const wchar_t* n : kLookupNames)
        if (const uint8_t* s = FindWideString(rdata, n)) names[nameCount++] = s;
    if (!nameCount) return out;
    std::map<void**, int> singletons;
    std::map<int32_t, int> offsets;
    for (size_t i = 0x40; i + 0x30 <= text.size; ++i) {
        const uint8_t* p = text.begin + i;
        // lea rdx, [rip+disp32] -> one of the names
        if (p[0] != 0x48 || p[1] != 0x8d || p[2] != 0x15) continue;
        int32_t disp;
        memcpy(&disp, p + 3, 4);
        const uint8_t* target = p + 7 + disp;
        bool isName = false;
        for (size_t k = 0; k < nameCount && !isName; ++k) isName = target == names[k];
        if (!isName) continue;
        for (const uint8_t* q = p - 7; q >= p - 0x40; --q) {
            if (q[0] != 0x48 || q[1] != 0x8b || q[2] != 0x0d) continue;
            memcpy(&disp, q + 3, 4);
            ++singletons[reinterpret_cast<void**>(const_cast<uint8_t*>(q + 7 + disp))];
            ++out.matches;
            break;
        }
        for (const uint8_t* c = p + 7; c + 6 <= p + 0x30; ++c) {
            if (c[0] != 0xff || (c[1] != 0x50 && c[1] != 0x90)) continue;
            int32_t off = c[2];
            if (c[1] == 0x90) memcpy(&off, c + 2, 4);
            ++offsets[off];
            ++out.findMatches;
            break;
        }
    }
    for (auto& [addr, n] : singletons)
        if (n > out.votes) out.singleton = addr, out.votes = n;
    for (auto& [off, n] : offsets)
        if (n > out.findVotes) out.findOffset = off, out.findVotes = n;
    return out;
}

using FindFn = void*(__fastcall*)(void* self, const wchar_t* name, bool track);
using PtrFn = void*(__fastcall*)(void* self);
using BoolFn = bool(__fastcall*)(void* self);
using GetFloatFn = float(__fastcall*)(void* self);
using SetFn = void(__fastcall*)(void* self, const wchar_t* value, uint32_t setBy);

template <typename Fn>
inline Fn Virtual(void* object, uint32_t offset) {
    return *reinterpret_cast<Fn*>(*reinterpret_cast<uint8_t**>(object) + offset);
}

// memcpy that reports a fault instead of raising it.
inline bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The IConsoleVariable object for <name> (null if absent or a command). Guarded: a wrong
// vtable layout faults here instead of taking the game down.
inline void* FindVariable(void* manager, const wchar_t* name) {
    __try {
        return Virtual<FindFn>(manager, kFindConsoleVariable)(manager, name, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

inline bool CallPtr(void* var, uint32_t offset, void*& out) {
    __try {
        out = Virtual<PtrFn>(var, offset)(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool CallBool(void* var, uint32_t offset, bool& out) {
    __try {
        out = Virtual<BoolFn>(var, offset)(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The engine's own GetFloat, for any numeric cvar (ints convert). Off the game thread a data
// cvar returns the renderer's copy; a Ref cvar returns its main value.
inline bool GetFloat(void* var, float& out) {
    __try {
        out = Virtual<GetFloatFn>(var, kGetFloat)(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// IConsoleVariable::Set: refuses a SetBy priority lower than the last one (CanChange), stores
// the value (and, off the game thread, the renderer's copy), records the priority and runs
// the change delegates. False if it faulted.
inline bool Set(void* var, const wchar_t* value, uint32_t setBy) {
    __try {
        Virtual<SetFn>(var, kSet)(var, value, setBy);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The flags word; the top byte is the priority of the last Set.
inline uint32_t& Flags(void* var) { return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(var) + kFlags); }

}  // namespace cvars
