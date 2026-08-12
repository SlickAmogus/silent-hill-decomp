# Copyright and licensing

This repository contains work from several sources under different terms. This
file says exactly which is which, because a single blanket statement would be
wrong for most of the tree.

Short version: **the PC port is GPL-3.0. The decompilation is not ours to
license. No Konami game data is included, and none is distributed.**

---

## 1. The PC port — GPL-3.0-or-later

Copyright (C) 2025-2026 Chris Hardin and the Silent Hill PC Port contributors.

Covered:

| Path | What it is |
|---|---|
| `pc_port/src/`, `pc_port/include/` | PC port engine code, excluding the vendored files listed in §3 |
| `pc_port/launcher/` | The launcher application |
| `pc_port/docs/` | Port documentation |
| `pc_port/assets/gamedata/lang/*.lang` (format + tooling) | The language-pack **format**, loader and import tooling |
| `tools/` | Certain tooling written for this project, only things that were added by this fork |
| Every `#ifdef SH_PC_PORT` block in `src/` and `include/` | Port-specific code added to decompiled sources |

This work is free software: you may redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [`LICENSE`](LICENSE) for the full text.

It is distributed in the hope that it will be useful, but **WITHOUT ANY
WARRANTY**; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE.

The last row above deserves a note: the `SH_PC_PORT` blocks are original work
authored for this port, but they are interleaved with decompiled code covered by
§2. The GPL applies to those additions; it does not and cannot reach the
surrounding decompiled material.

---

## 2. The decompilation — no license claimed by this project

`src/`, `include/`, `configs/`, `asm/`, `lib/` and `rom/` (excluding the
`SH_PC_PORT` additions noted above) are a decompilation of **Silent Hill**,
originally developed and published by **Konami**.

- Silent Hill and all original game code, data, characters and audiovisual
  content are © Konami. Konami is not affiliated with this project and has not
  endorsed it.
- The decompiled sources originate with the
  [silent-hill-decomp](https://github.com/shdecompilations/silent-hill-decomp) project and
  its contributors, whose work is gratefully acknowledged.
- **This project asserts no copyright over, and grants no license to, that
  material.** It is present because the port is built on it. Nothing in §1
  changes its status, and the GPL grant above does not extend to it.

If you intend to reuse the decompiled sources, look to their origin, not to
this repository.

---

## 3. Bundled third-party components

Each keeps its own license. None of them are covered by §1.

| Component | Location | License |
|---|---|---|
| rcheevos | `pc_port/third_party/rcheevos/` | MIT (see its own `LICENSE`) |
| PsyCross | `pc_port/PsyCross/` (submodule) | See that repository |
| cgltf | `pc_port/include/cgltf.h` | MIT |
| stb_image, stb_truetype | `pc_port/include/stb_*.h` | MIT / public domain (dual) |
| miniz | `pc_port/include/miniz.h`, `pc_port/src/miniz.c` | MIT |
| xxHash | `pc_port/include/xxhash.h` | BSD 2-Clause |
| UnRAR | `pc_port/third_party/unrar/` (when present) | UnRAR license — **may not be used to recreate the RAR compression algorithm** |
| Oswald, Barlow Semi Condensed | `pc_port/assets/gamedata/font/` | SIL Open Font License 1.1 (`pc_port/assets/licenses/OFL.txt`) |
| 7-Zip components | as used by the launcher | `pc_port/assets/licenses/LICENSE-7zip.txt` |
| SDL2, OpenAL Soft | linked at build time, not vendored | zlib / LGPL respectively |

### GPL-compatible derivations inside §1

Three files under `pc_port/src/fmv/` carry a GPL-3.0 SPDX tag because the
combined work is GPL, but they are **adaptations of other people's code**, not
original to this project. Their upstream licenses (BSD, LGPL) permit
redistribution under the GPL, and the original attributions are preserved in
each file header:

| File | Derived from |
|---|---|
| `mdec.c` | FFmpeg `libavcodec/mdec.c` (LGPL) — MPEG-1 RL VLC tables and IDCT |
| `ReadAVI.h` | REDRIVER2 (olegvedi@gmail.com, 2018), originally Michael Kohn (2004-2013) |
| `fmv_player.h` | REDRIVER2's VideoPlayer design (BSD) |

Do not strip those attribution headers.

---

## 4. Bundled assets that are NOT covered by §1

These ship in the repository but are **not** this project's to license:

- **`pc_port/assets/gamedata/sound/achievement.wav`, `steam.wav`,
  `trophy.wav`** — platform achievement/trophy notification sounds associated
  with Microsoft, Valve and Sony respectively. Included as unlock cues; no
  ownership is claimed and no license is granted. *Scheduled for replacement
  with original audio.*
- **`pc_port/assets/gamedata/ra/sym.*.txt`** — symbol maps derived from the
  decompilation project's `configs/`. See §2.
- **`pc_port/assets/gamedata/lang/*.lang` (translated text)** — fan
  translations. The **format and tooling** are §1; the translated dialogue is a
  derivative of Konami's script (§2) and remains the work of the individual
  translators who contributed it.

---

## 5. Game data

**No Konami game data is included in this repository or in any release.**

Running the port requires a legally obtained copy of Silent Hill for
PlayStation, supplied by the user. The port reads that disc image directly; it
neither ships nor redistributes any part of it.

Mods distributed separately by community members are the responsibility of
their authors and are not part of this project.

---

## 6. Contributing

Contributions to the §1 code are accepted under GPL-3.0-or-later. By opening a
pull request you confirm you have the right to submit the work under those
terms.

Please do not submit Konami-owned material, assets extracted from the game
disc, or code copied from other projects without a compatible license.
