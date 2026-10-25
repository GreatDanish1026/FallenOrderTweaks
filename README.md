# FallenOrderTweaks

A ReShade add-on for **Star Wars Jedi: Fallen Order** (PC) that sets the game's graphics from
the ReShade overlay: no Engine.ini, no launcher.

The overlay has one tab per area. Every setting has a **Game** choice (the game's own
behaviour), and hovering it explains it and shows the game's value.

- **Image**: the **DLSS mode** for [Luma](https://github.com/Filoppi/Luma-Framework) (DLAA,
  Quality, Balanced, Performance or Ultra Performance, changed live: Luma replaces the game's
  TAA with DLSS but follows the game's render resolution, which this sets) and texture
  sharpness to match the DLSS mode.
- **Camera**: turn off motion blur, film grain, chromatic aberration, depth of field (or make
  it higher quality), vignette, lens flares and camera shake.
- **Detail**: texture memory sized to your video card, 16x texture filtering, further view,
  grass and foliage distance with less pop-in, and more detailed characters and terrain.
- **Shadows**: sharper (4096) and further shadows, more cascades with softer blending,
  **soft sun shadows** that soften with distance from what casts them, **contact shadows**
  where objects meet surfaces, and smoother character shadows.
- **Lighting**: ambient occlusion with more of the game's levels or **GTAO** in their place
  (deeper shading in corners and creases, none on open surfaces, no dark halos; strength,
  radius, quality and an approximate multi-bounce), better reflections, finer volumetric
  fog, light shafts, bloom, smoke and glass lighting, and skin scattering.
- **Advanced**: every console variable the settings control, the game's value and the current
  one, and a reset for everything.

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

## Building

Visual Studio 2022+ with the C++ x64 tools, then `.\build.ps1` (`-Install` copies it to the
game; the game must be closed). See [docs/FINDINGS.md](docs/FINDINGS.md) for how it works.

MIT License. ReShade and Dear ImGui headers: see THIRD_PARTY_NOTICES.txt.
