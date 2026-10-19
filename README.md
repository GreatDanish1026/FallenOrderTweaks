# FallenOrderTweaks

A ReShade add-on for **Star Wars Jedi: Fallen Order** (PC) that sets the game's graphics from
the ReShade overlay: no Engine.ini, no launcher.

- **DLSS mode** for [Luma](https://github.com/Filoppi/Luma-Framework): DLAA, Quality,
  Balanced, Performance or Ultra Performance, changed live. Luma replaces the game's TAA with
  DLSS but follows the game's render resolution, which this sets. Optionally sharpens
  textures to match the DLSS mode.
- **Detail**: texture streaming pool sized to your video card, 16x texture filtering, sharper
  (4096) and further shadows, further view distance with less pop-in, and top-quality screen
  space reflections; more shadow cascades and softer cascade blending, further grass, full
  foliage density, and more detailed characters and terrain.
- **GTAO**: ground-truth ambient occlusion in place of the game's SSAO, with strength and
  radius settings: deeper shading in corners and creases, none on open surfaces, no dark halos.
- **Soft and contact sun shadows**: sun shadows that soften with distance from what casts
  them (sun size setting), and small contact shadows where objects meet surfaces.
- **Lighting**: more SSAO levels, finer volumetric fog, full-resolution character shadows,
  higher-quality skin, smoke and glass lighting, bloom, light shafts, cutscene depth of field
  and reflections on rough surfaces, each shown next to the game's own value.

Settings are kept in `ReShade.ini` under `[FallenOrderTweaks]` and applied every time the game
starts. Jedi: Survivor isn't affected: it shares Fallen Order's config folder, which is why
these settings don't go in an Engine.ini.

## Install

1. Install [Luma UE](https://github.com/Filoppi/Luma-Framework/wiki/Mods-List) into
   `SwGame\Binaries\Win64` (it includes ReShade with add-on support). For DLSS pick DLSS as
   Luma's Super Resolution in its tab.
2. Copy `FallenOrderTweaks.addon64` into the same folder.
3. Start the game, press **Home**, and open the **FallenOrderTweaks** tab.

Uninstall: delete `FallenOrderTweaks.addon64`. Values go back to the game's at the next start.

## Notes

- If the game build differs from the one analysed, the add-on checks the console variable
  layout first and changes nothing if it doesn't match (the overlay says so).
- It writes `FallenOrderTweaks.log` next to the game executable.
- Film grain, motion blur and chromatic aberration are in the game's own menu.

## Building

Visual Studio 2022+ with the C++ x64 tools, then `.\build.ps1` (`-Install` copies it to the
game; the game must be closed). See [docs/FINDINGS.md](docs/FINDINGS.md) for how it works.

MIT License. ReShade and Dear ImGui headers: see THIRD_PARTY_NOTICES.txt.
