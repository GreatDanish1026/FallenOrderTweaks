# Findings

Notes from reverse-engineering Star Wars Jedi: Fallen Order for FallenOrderTweaks.

## The game

- Unreal Engine 4.21.2, DirectX 11, `SwGame\Binaries\Win64\SwGame-Win64-Shipping.exe`
  (Steam build 10360016, PE timestamp `0x5eb208f8`). No PDB and no RTTI for engine classes, so
  everything below was read from the machine code.
- Pak files (`SwGame\Content\Paks`) are pak version 7, unencrypted (no encryption key GUID,
  index not encrypted).
- User config: `%LOCALAPPDATA%\SwGame\Saved\Config\WindowsNoEditor\`. **Jedi: Survivor uses the
  same folder** (both games rewrite `GameUserSettings.ini` there), so an `Engine.ini` placed
  there is read by both games. That's why this add-on sets console variables at run time
  instead.
- Saves: `%USERPROFILE%\Saved Games\Respawn\JediFallenOrder\`.

## Upscaling with Luma

Luma UE (Filoppi/Luma-Framework, ReShade add-on) replaces UE4's TAA pass with DLSS/DLAA (and
FSR 3) and adds native HDR; Fallen Order is on its list as working for both. Luma doesn't
choose the render resolution: it upscales from whatever the game renders, so with the game at
100 % it always runs DLAA. The DLSS modes are therefore set through `r.ScreenPercentage`
(DLAA 100, Quality 66.7, Balanced 58, Performance 50, Ultra Performance 33.3) with
`r.TemporalAA.Upsampling 1`, so the lower-resolution frame reaches the TAA pass Luma replaces.

Luma's own DLSS model choice is `[Luma] DLSSRenderPreset` in ReShade.ini (NVIDIA's preset
numbers: 11 = K, 12 = L, 13 = M).

## Console variables

Found with `tools/callsites.py`, `tools/cvvt.py` and `tools/vt.py` (run against the exe) and
llvm-objdump.

### IConsoleManager::Singleton

Game code looks cvars up as `mov rcx, [rip+Singleton]` ... `lea rdx, [rip+L"r.xxx"]` ...
`call [rax+0x90]`. Over 38 cvar names: 120 of 121 lookups load the singleton at RVA
`0x373c710`, and 95 call vtable offset `0x90`. `IConsoleManager::SetupSingleton` (RVA
`0xb34590`) allocates 0xf8 bytes and installs the FConsoleManager vtable at RVA `0x29d8c60`.

### FConsoleManager vtable

`+0x90` FindConsoleVariable (RVA `0xb26eb0`): calls `+0x98` FindConsoleObject, skips objects
whose flags (`GetFlags`, object vtable `+0x18`) have bit 3 (Unregistered), then tail-calls the
object's `AsVariable` (`+0x28`).

### Console object vtables (UE 4.21)

Compared with 4.26 there are no bool cvars and only one type check (`IsVariableInt`), so the
slots after `AsVariable` sit lower:

| Offset | Slot | Evidence |
|---|---|---|
| 0x18 | GetFlags | `mov eax, [rcx+0x18]` |
| 0x20 | SetFlags | `mov [rcx+0x18], edx` |
| 0x28 | AsVariable | returns `this` |
| 0x30 | IsVariableInt | true only in FConsoleVariable\<int\> |
| 0x38 | AsVariableInt | `&this+0x38` in the int vtable |
| 0x40 | AsVariableFloat | `&this+0x38` in the float vtable |
| 0x48 | AsVariableString | `&this+0x38` in the string vtable |
| 0x50 | AsCommand | null |
| 0x58 | Release | |
| 0x60 | Set(Value, SetBy) | CanChange, Atof, store `+0x38`, OnCVarChange(`+0x3c`, `+0x38`), flags = (flags & 0xffffff) \| SetBy |
| 0x68 | GetInt | |
| 0x70 | GetFloat | reads `[this+0x38+4*(not game thread)]` |

Vtables the register functions install (FConsoleManager slots 0x00-0x28): string `0x29d9188`,
float `0x29d9100`, int `0x29d9078`, bool Ref `0x29d9680`, float Ref `0x29d9210`, int Ref
`0x29d95f8` (RVAs).

### Storage

- Data cvars: `TConsoleVariableData<T>` at `+0x38`: game-thread value, renderer's copy at
  `+0x3c`.
- Ref cvars: pointer to the engine global at `+0x38`, own value at `+0x40` (their GetInt reads
  `[rcx+0x40]` as a byte, float or int).

### Threads and priority

OnCVarChange (RVA `0xb1fc10`): on the game thread, for `ECVF_RenderThreadSafe` cvars, it hands
the copy to the manager's thread propagation callback (`+0xc0`); on any other thread it copies
at once. ReShade's present runs off the game thread, so Set takes effect directly; no manual
mirroring as in UE 5.

SetBy priorities passed to Set in the game's code: 0x01 (scalability, 7 sites), 0x02 (game
setting, 19), 0x04, 0x05, 0x07, 0x08 (code, 23), 0x09 (console, 5). The add-on uses
`0x09000000`, above everything the game sets, including its graphics menu.

## Layout check

Before setting anything the add-on checks `r.ScreenPercentage` (float) and `r.MaxAnisotropy`
(int): vtables in `.rdata`, `IsVariableInt` false/true, `AsVariableFloat`/`AsVariableInt`
returning `+0x38`, and GetFloat matching the stored value. On a mismatch it changes nothing and
says so in the overlay and the log.
