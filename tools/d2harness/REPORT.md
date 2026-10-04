# Diablo 2 movement in Diablo 1: results

**Play:** build `devilutionx.exe`, put it next to your own `DIABDAT.MPQ` (or run `package.ps1`), and double-click it.
Results below are from the harness run on 2026-10-04 (Windows 10, VS 2022, branch `d2-movement-parity`).

## Verdict: 65/65 harness checks pass

| Benchmark | Result | Evidence |
|---|---|---|
| Movement parity with D2 | Travel time within **0.019%** of Diablo 2 in 16 directions, walking and running | D2's 128-entry direction table read from Diablo 2 1.12's `D2Common.dll` (file offset 0x8c660) matches the port byte for byte; 40,401 headings identical to a reference written from D2Common; in the real game the average run speed is 89.5 of D2's 90 units per tick |
| No errors | 0 invariant failures over full demo replays in both modes; no error lines in any test or game output | Per-tick probes check grid, walls, monster overlap, speed cap, moving outside the stand state |
| FPS | Stock movement: **99.9%** of a pristine 1.5.3 build (1,371 vs 1,373 fps uncapped). D2 movement: **95.4%** over the same 1,200 frames | Timedemo with rendering and audio, best of 3 runs each |
| Audio | Footsteps keep Diablo 1's rules (frames 0 and 4, walking only); sound calls fire in both modes | 32 footsteps over 16 tiles walking, 0 running, as stock |
| Gameplay and other subsystems | With D2 movement off, the stock demo replays to the **identical** hero, headless and in the real windowed game; no D2 code runs. With it on, level generation (map, monsters, objects) is identical, combat rules stay stock, monsters, triggers, lighting and sound all run | `stock mode: replay identical`, `level generation identical`, `combat rules stay stock` |

## What changed

- **Movement:** the hero moves freely to the exact point clicked, along D2's path step: D2Common's tangent table and folding logic, velocity 6 (walk) and 9 (run) from charstats, 25 frames a second. On D1's grid that is 4.69 tiles/s walking and 7.03 running (stock D1 walks 2.5). Remainders carry between ticks the way D2's 1/65536 subtile precision does. Pathing around obstacles uses D1's A* straightened into D2-style line segments. Taking a hit, attacking, blocking or dying interrupts movement, as in D2.
- **Runtime switch:** Settings > Gameplay > "Diablo 2 Movement" (on by default). Off is stock Diablo 1, proven by the replay above. The earlier D2 combat rules are kept behind a separate "Diablo 2 Combat" option, off.
- **Multiplayer:** with D2 movement on, game IDs end in 2 so modded and stock clients never join the same game; off uses the stock IDs.
- **Monsters** keep Diablo 1 movement; only the hero's movement is ported.

## Blast radius and probes

`blast_radius.md`: 85 functions touched, 182 more read or drive movement state. Every one of the 267 carries a probe (`D2_PROBE` counters in the mod and key dependents, `D2_PROBE_FN` sites elsewhere), plus per-tick invariants and level hashes. The binary side lists matching modules in the stock exe's gamedb index.

## Harness

`tools\d2harness\run_harness.ps1` runs everything and filters it into `runs\<time>\summary.md`. Every script has a hard time limit and a watchdog that kills its whole process tree. Exit codes: 0 pass, 1 fail, 2 build error, 3 missing prerequisite, 4 crash, 124 timeout (`README.md`).

## Bugs the harness caught (all fixed)

1. Integer rounding made diagonal speeds drift up to 1.7% and paths wander; fixed by carrying remainders like D2.
2. The hero could resume a walk after hit recovery without being told; movement now stops on any other action.
3. With D2 movement the recorded demo could not quit itself and the game restarted forever; the 600 s limit killed it, and the replay now ends cleanly.
4. The first "stock" baseline build silently compiled the mod source (a script variable clash); rebuilt from the pristine 1.5.3 tag.

## Caveats

- D2 movement costs about 4% FPS uncapped (extra margin tiles drawn while the camera glides). At normal frame limits this is invisible: 1,103 fps against a 60 fps cap.
- The FPS runs use the shareware demo, so the three shareware music tracks are silent stand-ins in all three builds; every sound effect plays.
- Monster movement, D2 stamina and D2 collision footprints are not ported.
