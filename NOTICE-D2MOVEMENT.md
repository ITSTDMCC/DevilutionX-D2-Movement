# DevilutionX-D2-Movement notice

This repository is a fork of [DevilutionX](https://github.com/diasurgical/devilutionX) that adds
Diablo 2 style movement. It is distributed under the same license as DevilutionX, the
Sustainable Use License in [LICENSE.md](LICENSE.md): you may use, copy, change and share it free of
charge, but not sell it or offer it as a commercial service.

- DevilutionX: Copyright (c) the DevilutionX contributors.
- Diablo 2 movement changes and the files they add (`Source/d2mod.*`, `Source/d2probe.*`,
  `test/d2*_test.cpp`, `tools/d2harness/`): Copyright (c) 2026 the DevilutionX-D2-Movement contributors.
- The movement port was written from the behaviour of Diablo 2 1.12, with the
  [D2MOO](https://github.com/ThePhrozenKeep/D2MOO) reimplementation (MIT License, Copyright (c)
  The Phrozen Keep community) as a reference. No Diablo 2 code or data is included; the direction
  table is computed from a formula, and the tests compare it against a D2Common.dll the user supplies.

## No game data

This repository contains no Diablo or Diablo 2 game files and must never contain any. To play, use
your own `DIABDAT.MPQ` from a copy of Diablo you own (for example from GOG), as with DevilutionX.
Diablo and Diablo II are trademarks of Blizzard Entertainment, which does not endorse this project.
