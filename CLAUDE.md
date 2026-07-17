# Silent Hill PC Port — Claude Instructions

## Debugging Regressions

When a bug appeared after a known commit or set of commits:
1. Run `git show <commit> -- <relevant_file>` FIRST. Read the diff before touching anything else.
2. If the commit is unknown, run `git log --oneline --since="<date>" -- <relevant_files>` to find candidates, then diff them.
3. Do NOT add logging to diagnose the root cause. Read the diff, reason about what changed, make the fix.
4. Do NOT read unrelated code files looking for clues. Stay on the diff.
5. The git repo is at `C:\Claude\silenthill\silent-hill-decomp`. Always cd there before git commands.

## Build & Run

```
# Build (always via msys2 bash)
"C:/msys64/usr/bin/bash.exe" -lc 'cd /c/Claude/silenthill/silent-hill-decomp/pc_port/build && cmake --build . 2>&1'

# With map DLLs
"C:/msys64/usr/bin/bash.exe" -lc 'cd /c/Claude/silenthill/silent-hill-decomp/pc_port/build && cmake .. -DSH_BUILD_MAP_DLLS=ON && cmake --build . 2>&1'
```

Log file: `C:\Claude\silenthill\silent-hill-decomp\pc_port\build\SilentHill.log` — always read this, never sh_stderr.txt.

Never build the launcher — user builds it themselves.

## Code Rules

- Always use `SH_DBG(...)`, never `fprintf(stderr, ...)`.
- Never add comments explaining what code does. Only add comments for non-obvious WHY.
- Game mechanics must match the original PSX game behavior exactly.
- Do not add features or abstractions beyond what the task requires.

## Working Directory Layout

- `C:/Claude/silenthill/silent-hill-decomp/` — main decomp + pc_port
- `C:/Claude/silenthill/PsyCross/` — PSX HAL (SDL2 + OpenGL + OpenAL)
- Git repo root: `C:/Claude/silenthill/silent-hill-decomp/`
- PsyCross git repo: `C:/Claude/silenthill/PsyCross/` (also at `pc_port/PsyCross/`)

## Keyboard Controls

Cross=C, Circle=V, Triangle=Z, Square=X, Start=Enter, Select=Space, DPad=Arrows, L1=A, R1=D, L2=RSHIFT, R2=LSHIFT
