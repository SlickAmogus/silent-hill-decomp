Note from KushAstronaut/SlickAmogus: For the below I asked claude to break down how matching the data works in the decomp, as well as an issue with enemies attacking early and how it relates to the unmatched data. At the time of writing, I am in the middle of other things and haven't read it yet, but saving it here in case it is useful. 


What a "decompilation" actually is

The original Silent Hill (1999) shipped as a PS1 disc containing compiled machine code — MIPS R3000 CPU instructions, plus data. There's no source code; Konami never released it. A decompilation project like silent-hill-decomp reverse-engineers that machine code back into human-readable C with one strict goal:

▎ When you compile the reconstructed C with the same ancient compiler Konami used (a specific 1997-era GCC targeting MIPS), the output must be byte-for-byte identical to the retail disc.

That property is called matching. It's the gold standard because it proves the C is a faithful reconstruction — if your C compiles to the exact same instructions, it must mean the same thing the original did.

So a decomp is really two intertwined things:

┌──────┬────────────────────────────────────────────────────────────────────────────────────────────────────────────┬─────────────────────────────────────────────────────────────────────────────┐
│      │                                                 What it is                                                 │                               Where it lives                                │
├──────┼────────────────────────────────────────────────────────────────────────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
│ Code │ The reverse-engineered C — the game's logic (AI, physics, rendering calls). Meant to be the original game. │ src/… (bodyprog/, maps/, etc.)                                              │
├──────┼────────────────────────────────────────────────────────────────────────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────┤
│ Data │ The actual bytes off the disc — models, textures, animation tables, collision keyframes, lookup tables.    │ Extracted from the .bin disc image into extracted_data/, *_rodata.inc, etc. │
└──────┴────────────────────────────────────────────────────────────────────────────────────────────────────────────┴─────────────────────────────────────────────────────────────────────────────┘

"Matching the code" = the recompiled C reproduces the original instructions.
"Matching the data" = the game reads the real extracted bytes (not a zero-filled placeholder or a guessed value). A lot of this project's bugs were data problems — a table stubbed as u8 X[256] of zeros instead of the real values, so an enemy read garbage and, say, never took damage. That's a data fidelity issue, completely separate from whether the code matches.

This enemy bug is a code-matching issue, not a data one. More on that below.

---
Matched vs. non-matching functions

Not every function is matched. Reconstructing the exact C that a 1997 compiler turned into some blob of MIPS is genuinely hard — compilers reorder things, allocate registers in odd ways, and inline aggressively. So each function is in one of two states:

- Matched — the C compiles to the identical instructions. Certified faithful.
- Non-matching — the decompiler wrote C that is functionally close (it does roughly the right thing) but doesn't reproduce the exact instructions yet. It's a best-effort approximation left in place so the game is playable while someone keeps chipping away at an exact match.

A non-matching function is a place where the reconstructed code and the true original can quietly diverge in behavior — usually harmlessly, but sometimes not.

---
Where your // TODO: Doesn't match? comment came from

That comment is stalker.c:690, and yes — it was written by a decompilation contributor, not by the PC-port side. It's a matching marker. Here's the exact spot:

// TODO: Doesn't match?
//distToPlayer  = Math_Vector2MagCalc(sharedData_800E3A18 - stalker->position.vx,   ← their first guess
//                                     sharedData_800E3A1C - stalker->position.vz);
distToPlayer   = SquareRoot0(SQUARE((sharedData_800E3A18 - stalker->position.vx) >> 6)   ← the form that DID match
                           + SQUARE((sharedData_800E3A1C - stalker->position.vz) >> 6));
distToPlayer <<= 6;

Read that as a little archaeological record of someone doing matching work:

1. They first wrote the obvious thing: Math_Vector2MagCalc(...), a tidy helper macro that means "length of this 2D vector."
2. They compiled it, diffed the instructions against the retail disc, and it didn't match — hence the TODO.
3. They dug in and discovered the original code actually pre-shifts each component right by 6 bits (>> 6) before squaring, then shifts the result back left by 6. That version did match. So they kept it and commented out their first guess.

That >> 6 isn't a random detail — it's the whole ballgame, and it's why the original never had this bug.

---
What Math_Vector2MagCalc actually does, and why the shift matters

Positions in this game are fixed-point numbers: Q19.12 means "the real value × 4096." So 1 world unit = 4096. To get the distance between two points you do the Pythagorean thing: sqrt(dx² + dz²).

The tidy macro is literally:

#define Math_Vector2MagCalc(x, z)   SquareRoot0(SQUARE(x) + SQUARE(z))
#define SQUARE(x)                   ((x) * (x))          // a plain 32-bit multiply

Here's the trap. SQUARE multiplies two 32-bit numbers and keeps a 32-bit result. A 32-bit signed integer maxes out at ~2.1 billion. If Harry is 8 units away, one component is 8 × 4096 = 32768, and 32768² ≈ 1.07 billion. Two of those summed ≈ 2.1 billion — it overflows. Past ~8–11 units apart, SQUARE(dx) + SQUARE(dz) wraps around into a negative number.

The original avoided this by shifting the inputs down first (>> 6 divides by 64), squaring the smaller numbers (which can't overflow), then scaling the answer back up. Same distance, no overflow. That's exactly what Math_Vector2MagCalcSafeQ6 does as a named helper — and, tellingly, it's what almost every other monster in the game already uses:

floatstinger, creeper, hanged_scratcher, puppet_nurse, romper, split_head, unknown23
        → all use Math_Vector2MagCalcSafeQ6 for player distance
groaner, stalker, larval_stalker
        → were the three outliers still using the unsafe Math_Vector2MagCalc  ← the bug

So this overflow was discovered and fixed for most enemies already — someone just never converted these three.

---
So: matched code, or wrong data? (The precise answer)

The code at these sites is non-matching, and it's wrong. The decompiler stuck the convenient Math_Vector2MagCalc macro in as a placeholder for "measure the distance here." The retail game doesn't do that — it does the overflow-safe shifted version (proven by the sibling site's matching work, and independently by the fact that the groaner code checks distToPlayer > Q12(12.0f), a comparison that would be meaningless if the value overflowed at 8). The disc data is fine; nothing is being read in the wrong format. It's purely that the reconstructed C doesn't faithfully represent the original arithmetic.

Why it shows up on PC but not on a real PS1

This is the subtle part, and it involves the other half of the port — the new code.

- SquareRoot0 is a library routine, not a CPU instruction. On the real PS1 it's a small BIOS/libgte function backed by a lookup table.
- On PC, the PS1 has no hardware, so PsyCross (our hardware-abstraction layer — the big chunk of new code that reimplements the PS1's GPU, sound chip, and math coprocessor on top of SDL2/OpenGL/OpenAL) provides its own SquareRoot0.

When the overflow feeds a negative number into SquareRoot0:
- You should never square-root a negative — it's garbage-in either way. The retail game never does, because its real code doesn't overflow.
- But our broken code does feed it a negative, and PsyCross's reimplementation handles that badly: its bit-scan helper returns 0 for a negative input, which makes SquareRoot0 index its lookup table out of bounds (SQRT[idx - 64] with a negative idx) and return a garbage-small number.

So the enemy's "how far is Harry?" comes back as a tiny value → distToPlayer < 1.0 passes → it lunges from across the courtyard. It's a two-part failure: wrong reconstructed code (overflow) meeting the reimplemented library (misbehaves on the bad input the real game never produces).

What we did

Changed those 12 distance calls in the three outlier enemies from Math_Vector2MagCalc → Math_Vector2MagCalcSafeQ6 — the exact pre-shift-then-square form the decomp already proved matches the original's numbers, and the form every other monster already uses. In normal (in-range) situations it produces an identical value; it only differs by refusing to overflow at long range. Net effect: enemies again measure real distance and only attack when actually next to Harry. Left untouched: the knockback-vector magnitudes (small, never overflow), the already-fixed alley grey-child site, and monster_cybil (already shifted).

---
The layers, summarized

- src/… (reverse-engineered C) — the original game's brain. Meant to be Silent Hill. This bug was here (non-matching code).
- PsyCross (new) — reimplements the PS1's chips (GPU/GTE/SPU/pad) on PC. Its SquareRoot0 is what turned the overflow into visible garbage.
- #ifdef SH_PC_PORT blocks (new) — surgical patches inside the game C for 64-bit/x86 realities (pointer sizes, struct growth, x86 divide-by-zero faults). This fix wasn't one of those — it's a correction to the reconstructed logic itself, matching what the rest of the enemies already do.
- Extracted data (original bytes) — untouched here; the disc data was never the problem.