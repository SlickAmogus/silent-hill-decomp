# Silent Hill PC Port - Technical Analysis

## Overview

This project is an **experimental PC port** of the original Silent Hill (1999) PlayStation game. It is built on top of the PSX decompilation project and uses **PsyCross** as a hardware abstraction layer (SDL2 + OpenGL + OpenAL). The port enables the game to run natively on PC with enhanced features like high resolution, 16:9 aspect ratio, high refresh rates, and uncapped framerates.

---

## Project Architecture

### Core Components

| Component | Description | Source |
|-----------|-------------|--------|
| **Original Decompilation** | Reverse-engineered C code from the PSX binary | Human (Decomp Team) |
| **PsyCross** | PSX hardware compatibility layer (OpenGL/SDL2/OpenAL) | Third-party (OpenDriver2) |
| **PC Port Layer** | Platform-specific code, stubs, and reformatters | AI-Assisted |
| **Map Overlays** | Per-map game logic (compiled as DLLs or statically linked) | Original Decomp |

---

## Code Attribution Analysis

### 1. Original Decompilation Code (~75% of codebase)

**Location:** `src/`, `include/`

The decompilation represents the bulk of the project and consists of:

- **Main System** (`src/main/`): File I/O queue, memory management, file table
- **BODYPROG Engine** (`src/bodyprog/`): Core game engine (~500K lines across 30+ files)
  - 3D rendering pipeline
  - Audio system (BGM, SFX)
  - Player mechanics, combat, animations
  - Event system, cutscenes (DMS)
  - Save/load, memcard, ranking systems
- **Screen Overlays** (`src/screens/`): Title screen, menus, credits
- **Map Overlays** (`src/maps/`): 42 distinct maps with their own:
  - Character AI and spawning
  - Particle effects
  - Event triggers
  - Map-specific player logic

**Characteristics:**
- Uses original PSX memory addresses and offsets
- Contains MIPS assembly remnants (inline macros)
- Retains original function names (e.g., `func_800706E4`)
- Maintains binary compatibility with the original game data files

### 2. PC Port Specific Code (~15% of codebase)

**Location:** `pc_port/src/`, `pc_port/include/`

This is the **AI-assisted portion** of the project, comprising:

| Module | Purpose | Lines |
|--------|---------|-------|
| `main_pc.c` | PC entry point, initialization | ~250 |
| `gpu.h` / `gpu_gte_pc.h` | GTE macro replacements for PC | ~400 |
| `stubs/` | PSX API stubs (libapi, libgs, libspu, etc.) | ~2,500 |
| `debug_console.c` | In-game debug console with OpenGL font rendering | ~540 |
| `map_registry.c` | Dynamic map overlay loading system | ~210 |
| `ipd_reformat.c` | 32-bit to 64-bit struct conversion for map data | ~290 |
| `dms_reformat.c` | DMS cutscene data reformatting | ~260 |
| `fmv/` | FMV player (MJPG AVI playback via libjpeg + OpenGL) | ~600 |
| `psx_memory.c` | PSX RAM emulation (2MB + guard) | ~35 |
| `math_impl.c` | Math function implementations | ~200 |
| `pc_config.c` | Configuration file parsing | ~150 |
| `dll_loader.c` | Runtime map DLL loading | ~50 |
| Other utilities | Logging, FS abstraction, etc. | ~300 |

**AI-Generated Code Characteristics:**
- Comprehensive header comments explaining purpose
- Clean C/C++ code with modern patterns
- Extensive inline documentation
- Conditional compilation (`#ifdef SH_PC_PORT`)
- Integration points marked with clear comments

### 3. PsyCross Dependency (~10% external)

**Location:** `pc_port/PsyCross/` (submodule)

A third-party library providing:
- OpenGL-based GPU emulation
- SDL2 window/input handling
- GTE (Geometry Transformation Engine) emulation
- SPU (Sound Processing Unit) emulation via OpenAL
- PSY-Q API compatibility layer

**Source:** https://github.com/OpenDriver2/PsyCross (BSD licensed, originally from REDRIVER2 project)

---

## Key Technical Challenges Solved

### 1. Memory Address Translation

**Problem:** PSX code uses hardcoded memory addresses (e.g., `0x80024B60`) for overlays and buffers.

**Solution:**
```c
// psx_memory.h
#define PSX_ADDR(offset) ((uintptr_t)g_PsxRam + (offset))

// Runtime initialization
g_OvlBodyprog = PSX_ADDR(0x00024B60);
g_OvlDynamic  = PSX_ADDR(0x000C9578);
```

### 2. 32-bit to 64-bit Pointer Conversion

**Problem:** Original game data contains 32-bit pointers that need to work on 64-bit systems.

**Solution:** `ipd_reformat.c` and `dms_reformat.c` manually parse binary data and reconstruct structs with proper 64-bit pointer layouts.

### 3. GTE (Geometry Transformation Engine)

**Problem:** Original uses MIPS coprocessor instructions for 3D math.

**Solution:** `gpu_gte_pc.h` redefines all custom GTE macros to use PsyCross's C-based GTE emulation.

### 4. Map Overlay System

**Problem:** PSX loads one map overlay at a time; static linking causes 500+ symbol collisions.

**Solution:**
- Map registry with stub headers for unloaded maps
- Optional DLL compilation for maps (`-DSH_BUILD_MAP_DLLS`)
- Runtime symbol export from main executable

### 5. FMV Playback

**Problem:** Original uses PSX STR format with XA audio interleaving.

**Solution:** `fmv_player.cpp` plays pre-converted MJPG AVI files with PCM audio via SDL.

---

## Data Flow

```
┌─────────────────────────────────────────────────────────────┐
│                      PC Hardware                            │
│              (OpenGL / SDL2 / OpenAL)                        │
└────────────────────┬────────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────────┐
│                    PsyCross                                │
│     (PSX GPU/GTE/SPU/CD Emulation Layer)                     │
└────────────────────┬────────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────────┐
│                   PC Port Layer                              │
│  (Memory Emulation, Stubs, Reformatters, Debug Console)    │
└────────────────────┬────────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────────┐
│              Original Decompiled Game Code                   │
│    (BODYPROG, Map Overlays, Screens - ~75% of codebase)    │
└─────────────────────────────────────────────────────────────┘
```

---

## Build System

**Toolchain:** CMake + Ninja (MinGW64 on Windows)

**Key Options:**
- `SH_BUILD_MAP_DLLS` - Build maps as loadable DLLs
- `SH_DEBUG` - Enable debug output
- `SH_VERSION` - Game version (USA, EUR, JAP0, JAP1, JAP2)

**Dependencies:**
- SDL2
- OpenGL
- OpenAL (via PsyCross)
- libjpeg (for FMV)

---

## Current Status (as of README)

| Feature | Status |
|---------|--------|
| Main Menu | Fully working |
| Opening Cutscene | Fully working |
| In-game 3D World | Rendering working (textures, fog, snow) |
| Player (Harry) | Fully visible (23 bones, gouraud shading) |
| Movement | Working (collision, stairs, walls) |
| Camera | Functional (PSX fixed-cam + debug free-cam) |
| Audio | SFX working, BGM loads |
| Maps | 31 of 42 compile as DLLs |
| NPC AI | Enabled (Grey Children spawn) |
| Memory Card | Stubbed (save/load non-functional) |

---

## Human vs AI Code Breakdown

| Category | Percentage | Description |
|----------|------------|-------------|
| **Human (Decomp)** | ~75% | Reverse-engineered game logic, data structures, original algorithms |
| **AI-Assisted (PC Port)** | ~15% | Platform abstraction, stubs, reformatters, debug tools, build system |
| **Third-Party** | ~10% | PsyCross library for PSX hardware emulation |

### AI's Role in This Project

The AI (Claude) was instrumental in:

1. **Architecture Design**: Designing the PC port layer structure, memory emulation approach, and build system integration

2. **Compatibility Layer**: Writing stubs for PSX APIs that have no PC equivalent (libapi, libgs, libspu, etc.)

3. **Data Reformatting**: Creating parsers to convert 32-bit PSX data structures to 64-bit PC layouts

4. **Debugging Infrastructure**: Building the in-game debug console, logging system, and map registry

5. **Integration**: Connecting the decompiled code to PsyCross, handling edge cases, and resolving symbol conflicts

6. **Documentation**: Comprehensive code comments explaining the translation between PSX and PC paradigms

### What Was NOT AI-Generated

- The original game logic (AI behavior, combat mechanics, rendering algorithms)
- File format parsers (TIM, TMD, ANM, DMS, etc.)
- Original data structures and memory layouts
- The actual reverse-engineering work

---

## Original Code Alterations

### Minimal Changes Philosophy

The project follows a **minimal upstream changes** approach. The original decomp code is altered only when necessary:

1. **Conditional Compilation**: Most changes are wrapped in `#ifdef SH_PC_PORT` blocks, preserving the original PSX build path

2. **Pointer Initialization**: Hardcoded PSX addresses are converted to runtime-initialized pointers

3. **Overlay Loading**: The overlay decryption/loading system is bypassed on PC (everything statically linked)

4. **Data Loading**: File I/O goes through the original `Fs_Queue` system, but reading from disc image via PsyCross's CDFS emulation

### Example Alteration Pattern

```c
// Original (PSX):
void* g_OvlBodyprog = (void*)0x80024B60;

// PC Port:
#ifdef SH_PC_PORT
    void* g_OvlBodyprog = NULL; // initialized in main to PSX_ADDR(0x00024B60)
#else
    void* g_OvlBodyprog = (void*)0x80024B60;
#endif
```

---

## Conclusion

This PC port represents a successful collaboration between:
- **Human reverse engineers** who decompiled the original game
- **AI assistance** that built the PC compatibility layer
- **Open-source community** (PsyCross, REDRIVER2)

The project demonstrates how AI can accelerate porting efforts by handling the tedious but critical work of platform abstraction, allowing humans to focus on the creative and analytical aspects of game preservation.

---

## Related Projects

- **Silent Hill Decompilation**: https://github.com/Vatuu/silent-hill-decomp
- **PsyCross**: https://github.com/OpenDriver2/PsyCross
- **REDRIVER2**: https://github.com/OpenDriver2/REDRIVER2
- **Silent Engine**: https://github.com/Sezzary/SilentEngine (non-AI multiplatform port)

---

*Analysis generated based on project source code examination*
