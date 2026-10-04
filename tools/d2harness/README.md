# D2 movement harness

Non-visual checks for the Diablo 2 movement mod. Everything runs headless except the FPS benchmark,
which has to draw frames to measure them (it opens a game window briefly and closes itself).

## Scripts

| Script | What it does | Hard limit |
|---|---|---|
| `build.ps1 -BuildDir <dir> [-Testing] [-Targets ...]` | Configure + build with VS 2022, reusing dependency sources already fetched into `build/` or `build-tests/` (otherwise CMake downloads them). `-Testing` builds without LTO, which MSVC needs for tests. | configure 15 min, build 60 min, script `-MaxMinutes` (45) |
| `run_harness.ps1` | Runs every check below, then `filter_probes.py`. Output in `runs/<timestamp>/` (`summary.md`, `results.json`, all logs). | per step (below), script `-MaxMinutes` (60) |
| `filter_probes.py <run>` | Turns test XML, probe logs and benchmark numbers into pass/fail. | 120 s |
| `blast_radius.py [out.md] [--insert-probes]` | Maps touched and movement-dependent functions (`blast_radius.md`); `--insert-probes` adds `D2_PROBE_FN();` to any of them that lack a probe. | 120 s |
| `package.ps1` | Copies the exe to a play folder outside the repository and hard links your MPQs next to it. | 5 min |

Every script has a watchdog that kills it and its child processes at the limit; nothing can run forever.

## Your own files

The harness never ships or commits game files. Point it at your own copies with parameters,
`D2H_<Name>` environment variables, or a git-ignored `local.psd1` (copy `local.example.psd1`):

| Setting | What |
|---|---|
| `DataDir` | Folder with your `DIABDAT.MPQ` and `devilutionx.mpq` |
| `D2CommonDll` | Your Diablo 2 1.12 `D2Common.dll` (reference for the direction table checks) |
| `StockExe` | A stock 1.5.3 build for the FPS baseline |
| `OutDir` | Play folder for `package.ps1`, outside the repository |
| `CMake` | cmake.exe, if it is not on PATH |

## Exit codes (all scripts)

| Code | Meaning |
|---|---|
| 0 | PASS: ran to completion, every check passed |
| 1 | FAIL: ran to completion, at least one check failed (see `summary.md`) |
| 2 | BUILD_ERROR: configure or compile failed |
| 3 | MISSING_PREREQ: a tool, build output, MPQ or reference file is missing |
| 4 | CRASH: a child process died without producing results |
| 124 | TIMEOUT: a step or the whole script hit its time limit and was killed |

## What `run_harness.ps1` runs

| Step | Limit | Checks |
|---|---|---|
| `d2harness_test` | 300 s | D2Common.dll tangent table identical byte for byte; headings identical to a D2 reference for 40,000 deltas; every trip in 16 directions as long as stock D1 walking (dungeon) and jogging (town); slipping between barrels at hundreds of angles; gamepad walking keeps its walk cycle; straight lines; stride and footsteps; walls, closed rooms, blocking monsters; stock mode untouched; probes fire |
| `d2mod_test`, `d2glide_sim_test` | 120 s / 300 s | Path straightening and on-screen glide line |
| `d2demo_test` (D1 mode) | 600 s | Stock demo replay with stock movement: hero must match the stock recording exactly; no D2 code may run |
| `d2demo_test` (D2 mode) | 600 s | Same demo with D2 movement: no invariant failures, in-game speeds match stock D1, levels identical to D1 mode, all subsystems active |
| timedemo FPS (stock, mod D1, mod D2) x `-BenchRuns` | 300 s each | Mod FPS at least 95% of the stock 1.5.3 build (best run of each) |

## Probes

`Source/d2probe.h`. `D2_PROBE(name)` counters sit in the functions the mod changed and in key dependents;
`D2_PROBE_FN()` sites sit in every other function the blast radius lists. Set `D2PROBE_LOG=<file>` to get:

- `FAIL` lines from per-tick invariants: hero inside a wall, off the grid, overlapping a live monster, moving
  outside `PM_STAND`, moving faster than D2 running, sub-tile position outside its tile, D2 state in D1 mode
- `LEVEL` hashes of every level load (map, monsters, objects, items)
- `COUNT` / `SITE` / `STAT` dumps at exit
