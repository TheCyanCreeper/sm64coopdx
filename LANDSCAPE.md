# Castle Grounds expedition

Peach's Castle and its original grounds occupy a fixed patch of the endless landscape. Climb the grassy banks around the lawn and walk over the crest into the wilderness. A continuous grass mesh joins the original perimeter to the streaming ground in the same area. The rear courtyard shares the grounds. Follow either stone causeway over its east or west wall, or take the rear hall door from inside the castle. The courtyard fountain, trees, signs, and Big Boo’s Haunt entrance occupy the garden behind the main roof.

Beyond the home meadow, temperature varies over roughly 41,000-unit intervals and grove density over 24,600-unit intervals. Terrain height, textures and vegetation blend between regions. Broad landings and ridges use a slope profile sampled from the castle lawn. Steeper wilderness faces show the original outdoor rock texture. The seed fixes the locations of outcrops, groves and encounters.

| Region | Scenery | Enemies |
| --- | --- | --- |
| Meadow | Bubbly trees and scattered rocks | Goombas |
| Woodland | Denser tree cover, butterflies and outcrops | Piranha Plants |
| Sandy hills | Palms, boulders and pyramid caps | Fly Guys |
| Snow ridges | Snowy pines, snowballs and exposed rock | Spindrifts |

The central grounds are safe from generated encounters. Outside that buffer, each block of four sites can hold one enemy, with a maximum of nine encounter sites in the loaded window. Sites stay populated while nearby, so crossing a streaming boundary keeps surviving enemies in place.

On Windows, run **Play Landscape.cmd**, then host a game and select a save. On first launch the script copies the US ROM from your existing CoopDX profile into `landscape-profile` beside the launcher. The expedition starts with fresh save slots and stores its progress there between launches. Other players need this fork for the expedition. Terrain and encounter sites are deterministic across clients. Each client simulates the enemies in its own loaded window, so enemy defeats are local in this prototype.

To rebuild from an MSYS2 MinGW64 shell, run `make -j8 EXE=build/us_pc/castle-expedition.exe` in the repository root. The executable is `build/us_pc/castle-expedition.exe`.
