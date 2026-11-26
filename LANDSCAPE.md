# Castle Grounds expedition

This fork adds a second outdoor area to Peach's Castle. From Mario's usual starting spot on the grass, turn away from the castle and walk between the two closely spaced bubbly trees. Stepping through the gap uses the game's instant-warp floor and carries Mario's movement into the meadow. The return crossing lies behind the arrival point, between the trees on the southern edge of the clearing.

Walk in any direction. Rolling meadows blend into groves, sandy hills with palms, and higher snowy ridges with pine trees and slippery ground. A fixed seed makes revisiting a place repeatable. The renderer uses the game's grass, sand and snow textures, tree models, skybox, and vertex shading. Mario's movement and camera run through CoopDX.

On this Windows checkout, run **Play Landscape.cmd**, then host a game and select a save. CoopDX uses the US ROM in your existing CoopDX profile. Other players need this fork for the new area.

To rebuild from an MSYS2 MinGW64 shell, run `make -j8` in the repository root. The executable is `build/us_pc/sm64coopdx.exe`.
