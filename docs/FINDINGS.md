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

OnCVarChange (RVA `0xb1fc10`): only on the game thread does it copy the main value to the
renderer's (through the manager's thread propagation callback at `+0xc0` for
`ECVF_RenderThreadSafe` cvars, else directly); off it, it copies nothing and only sets the
manager's call-the-sinks flag (`+0xcc`). ReShade's present runs on the render thread, so after
every Set the add-on copies the value itself (`cvars::MirrorToRenderer`): data cvars `+0x38` to
`+0x3c`, Ref cvars `+0x40` to the engine global (one byte for a bool Ref). Before 0.4.2 this was
missing (an earlier reading had the branch backwards), so only cvars the game thread reads,
such as `r.ScreenPercentage`, took effect.

SetBy priorities passed to Set in the game's code: 0x01 (scalability, 7 sites), 0x02 (game
setting, 19), 0x04, 0x05, 0x07, 0x08 (code, 23), 0x09 (console, 5). The add-on uses
`0x09000000`, above everything the game sets, including its graphics menu.

## Layout check

Before setting anything the add-on checks `r.ScreenPercentage` (float) and `r.MaxAnisotropy`
(int): vtables in `.rdata`, `IsVariableInt` false/true, `AsVariableFloat`/`AsVariableInt`
returning `+0x38`, and GetFloat matching the stored value. On a mismatch it changes nothing and
says so in the overlay and the log.

## The EA-app executable

Steam starts `starwarsjedifallenorder.exe`, not `SwGame-Win64-Shipping.exe`. It has the same
PE timestamp, but its code is encrypted on disk (an extra `.ooa` section, EA's DRM wrapper)
and decrypted only in memory, so offline analysis uses `SwGame-Win64-Shipping.exe`. At run
time the add-on scans the decrypted image. `r.ScreenPercentage` alone is looked up only 4
times, so the scan votes over 20 names (62 of 63 lookups agree on the singleton).

## Distance fields

The game ships without mesh distance fields: `r.GenerateMeshDistanceFields` reads 0 in game
(checked through the add-on). So distance field AO (`r.DistanceFieldAO`) and distance field
shadows (`r.DistanceFieldShadowing`) have nothing to trace against, and the add-on doesn't
offer them.

## Experimental results

Tried in game through the Experimental tab:

- `r.Shadow.FilterMethod 1` (UE 4.21's experimental PCSS) and `r.ContactShadows 1`: no visible
  change, so both were dropped. Contact shadows also need a contact shadow length on each light,
  which the game's lights apparently don't set.
- More SSAO levels, finer volumetric fog, full-resolution capsule shadows and high-quality skin
  scattering all make a visible difference, so they became the Lighting tab (0.3.0).

## Ambient occlusion

Unreal's global shaders ship precompiled in `Engine/GlobalShaderCache-PCD3D_SM5.bin` (the
newest copy is in `pakchunk0-WindowsNoEditor_1_P.pak`, Oodle-compressed; `tools/pak.py` reads
pak version 7). In the cache each shader's type name (an FString) is followed by its
zlib-compressed code: a variable-length Unreal header (packed resource table, 24-124 bytes),
the DXBC container, and Unreal's own data after it. `tools/gsc.py` extracts and names them;
`tools/disasm.py` disassembles them (reflection is stripped: only ISGN, OSGN and SHEX).

`FPostProcessAmbientOcclusionPS<SetupAsInput, Upsample, Quality>`: `PS1xx` read the
downsampled setup texture (coarse levels), `PS0xx` the scene textures directly (the
full-resolution level). With `r.AmbientOcclusionLevels 1` only `PS00Q` runs (no setup, no
upsample). Its bindings:

| Register | Meaning |
|---|---|
| v0 | xy scene texture UV, zw screen position |
| t0 / t1 / t2 | scene depth (device Z) / GBuffer A (normal) / GBuffer B (shading model in a; 7 = hair gets no normal) |
| t3 | HZB, sampled at a mip per step through cb0[38] |
| t4 | random rotation texture, at `v0.xy * cb0[31].xy + cb0[33].xy` |
| cb0[30] | x power, y normal bias, w intensity |
| cb0[31] | z radius, w aspect ratio |
| cb0[32] | x bias scale, z radius in world space (1) or screen space (0) |
| cb0[33] | w 1 / tan(half FOV) |
| cb0[34] | xy distance fade (`saturate(depth * x + y)`), z HZB mip scale |
| cb1[8..10] | View TranslatedWorldToView rows |
| cb1[57] | View InvDeviceZToWorldZTransform (`z * x + y + 1 / (z * z' - w)`) |

Output (every channel): `1 - intensity * (1 - lerp(visibility, 1, fade) ^ power)`, with
visibility the average of `(1 - max horizon)^2` over 10 HZB sample pairs.

The multi-level variants (`PS01Q`) also read the coarser level (t3 setup, t4 AO, cb0[4].zw its
texel size) and blend it in with weight cb0[32].w; there t5 is the HZB and t6 the random
texture.

GTAO (`src/shaders/gtao.hlsl`) replaces `PS000`-`PS004` with the same bindings and output
formula, tracing the full-resolution depth instead of the HZB. The add-on forces
`r.AmbientOcclusionLevels 1` while it's on, and `r.AmbientOcclusionRadiusScale` sets its
radius.

GTAO strength rides in `r.AmbientOcclusionMipLevelFactor` (default 0.5): the replaced pass
receives it as cb0[34].z (the HZB mip scale, which GTAO doesn't use), and the shader reads
strength = cb0[34].z / 0.5. The add-on drives it whenever GTAO is on.

First in-game comparison (overcast Bogano, 4K DLAA): GTAO is about as dark as the game's AO on
average, with tighter contact shading in recesses and creases, no halos and no visible noise.

- Retested with 0.4.2's renderer mirroring: PCSS and contact shadows still make no visible
  change, so they stay out.

## View uniform buffer

The View buffer's member table (name, offset, type) is built at startup by code that sets
each member's offset (`mov dword [rbp-0x20], imm32`) just before loading its name (a UTF-16
string); `tools/viewoff.py` reads it. It agrees with every View register the game's shaders
use (TranslatedWorldToView 0x80, InvDeviceZToWorldZTransform 0x390, ScreenPositionScaleBias
0x3a0, BufferSizeAndInvSize 0x7b0), and gives:

| Member | Offset | Register |
|---|---|---|
| ViewToClip | 0x180 | cb1[24..27] (row 2: TAA jitter) |
| StateFrameIndexMod8 | 0x870 | cb1[135].x (uint) |
| DistanceFieldAOSpecularOcclusionMode | 0xa60 | cb1[166].x (uint, from `r.AOSpecularOcclusionMode`) |

`r.GeneralPurposeTweak` is compiled out of the shipping build (its View member exists but
nothing fills it). `r.AOSpecularOcclusionMode` only matters for distance field AO, which the
game can't use (no distance fields), so its View member carries settings to any shader.

## Shadows

`FShadowProjectionPS<Quality, UseFadePlane, UseTransmission>` projects each shadow (sun
cascades, per-object and spot shadows) into the screen's light attenuation. Quality 4 and 5
compile to the same code; Epic uses 5. Bindings of the quality-5 variants:

| Register | Meaning |
|---|---|
| v0 | SV_Position (`[earlydepthstencil]`) |
| t0 / t1 / t2 | scene depth / custom data (a = subsurface opacity) / GBuffer B (shading model) |
| t3 / s3 | the shadow depth map, read with nine `gather4` over 6x6 texels |
| cb0[2].z | soft transition: `saturate((occluder - receiver) * k + 1)` |
| cb0[3] | shadow map size, 1 / size |
| cb0[28] | y fade fraction, z the light's Shadow Sharpen |
| cb0[30..33] | ScreenToShadowMatrix rows, on (screen position * depth, depth, 1) |
| cb0[34] | xy subsurface depth bias and density; zw fade plane offset and 1 / length |
| cb1 | View: [57], [58], [123] |

The filter is a 5x5 PCF with bilinear-weighted outer rows and columns (sum / 25). Shading
models 2, 3, 5, 6, 7 and 9 also get a subsurface shadow from the same texels:
`exp(-max(receiver + bias - occluder, 0) * density)` (1 past depth 0.99), density 1 for 5, 7
and 9, else `-0.034657 * ln(1 - opacity)` (0 with GBuffer B bit 16). Then
`shadow = saturate((shadow - 0.5) * sharpen + 0.5)`, both faded by `lerp(1, x^2, fade)`, and
the output is `sqrt(shadow, sss, shadow, sss)`; with the fade plane the alpha is
`1 - saturate((depth - offset) * invLength)` instead. The transmission variants add GBuffer
normals and View cb1[8..10].

`src/shaders/shadow.hlsl` rebuilds the two non-transmission variants, and for the sun only (its
projection is orthographic: the matrix's w column is 0, 0, 0, 1) adds PCSS soft shadows and a
screen-space contact shadow march, ported from AWTweaks. The light space comes from the
matrix: with ViewToClip's scale, its rows are the view-space gradients of shadow u, v and z,
so their lengths give shadow UV and depth per cm and `cross(du, dv)` the light direction.

In game (0.6.0): the base variant is replaced and looks good with soft and contact shadows; the
settings code reaches the shader live. The game also creates the transmission variant (lights
with subsurface transmission), which isn't rebuilt yet, so what it draws keeps the game's
shadows. The fade plane variant hasn't been seen created so far.

### Transmission variants

With transmission the registers shift: t1/s1 GBuffer A (world normal), t2/s2 the subsurface
profile ID (a), t3/s3 custom data (a = opacity), t4/s4 GBuffer B, t5/s5 the shadow depth
map, t6 the subsurface profiles texture; the fade plane moves to cb0[34].w and cb0[35].x.
Shading model 5 (subsurface profile) replaces the gathered falloff with the profile's
transmission: the profile's extinction and normal scale from `t6.Load(1, profile)` (x * 3.1 *
density, y * 0.5), the lookup point (screen position * depth, depth) pushed back along the
view-space normal (cb1[8..10]) by the normal scale and re-projected, then 16 Poisson taps
(the shader's 64-entry table, entries 15-30, scaled by 1 / shadow map height) summing
`clamp(|v|, 0.15, 5) + 0.25`, where `v = delta * extinction + normalScale` (clamped at 0 unless
delta * extinction > 0), giving `1 - sum * 0.0125`. Rebuilt in 0.6.1 (`USE_TRANSMISSION`).

### GTAO upgrades (0.8.0)

- Specular occlusion: the reflection environment pass (`FReflectionEnvironmentSkyLightingPS`,
  33 permutations) already derives specular occlusion from the AO buffer (it unpacks AO by
  256/255, squares it and combines it with N.V, blended by roughness), so GTAO's AO improves
  reflections in crevices by itself. A true cone-based GTAO specular occlusion would need
  bent normals in another buffer and all 33 permutations replaced, so it isn't done.
- Multi-bounce: the AO pass doesn't bind GBuffer C (albedo), so Jimenez et al.'s fit uses an
  assumed albedo of 0.5.
- Settings: GTAO's quality (High 4x8, Ultra 6x12, Max 8x16 slices x steps) and multi-bounce
  ride in the same settings code as the sun shadows (`r.AOSpecularOcclusionMode` ->
  cb1[166].x), as `quality * 10000 + multiBounce * 100000`; the shadow shader reads only the
  lowest four digits.

### Camera effects (1.1.0)

The game's menu switches are Respawn cvars: `r.EnableCameraAndMeshMotionBlur` (its help says
velocity overrides can still blur with it off), `r.EnableFilmGrain` (0 off, 1 on, 2 high) and
`r.EnableChromaticAberration`. The Camera tab forces, on top of those: motion blur
`r.MotionBlurQuality 0`; film grain `r.EnableFilmGrain 0`; chromatic aberration
`r.EnableChromaticAberration 0` + `r.SceneColorFringe.Max 0` (clamps post-process volumes);
depth of field `r.DepthOfFieldQuality 0` (or the High quality accumulator); vignette
`r.Tonemapper.Quality 1` (0 basic, 1 + film contrast, 2 + vignette, 3 + shadow tint, 4 + grain,
5 + grain jitter); lens flares `r.LensFlareQuality 0`; camera shake `rs.CameraShakeMultiplier`
0.5 / 0. Vignette changes the tonemapper permutation, which Luma has to recognise; in-game
test pending.
