## PC Port — Struct Offset Portability (32-bit → 64-bit)

### The Problem

Silent Hill's decompiled code was written for the PSX (MIPS, 32-bit). When porting to a
64-bit PC target, **any struct containing pointer fields changes layout** — pointers grow
from 4 bytes to 8 bytes, and the compiler inserts alignment padding before pointer-aligned
fields. Code that uses hardcoded byte offsets instead of `offsetof()` or named field access
will silently read/write the wrong memory.

This is especially dangerous because:
- It doesn't crash — it reads valid memory, just the wrong field
- The compiler can't warn about it — it's raw `(u8*)ptr + constant` arithmetic
- The values often look plausible (small integers, flags) so the bug is hard to spot at runtime

### Case Study: `s_IpdCollisionData.field_34`

The IPD collision data struct has 7 pointer fields (`ptr_C` through `ptr_2C`):

```
PSX (32-bit) layout:                PC (64-bit) layout:
  offset 0x00: positionX_0 (s32)     offset 0x00: positionX_0 (s32)
  offset 0x04: positionZ_4 (s32)     offset 0x04: positionZ_4 (s32)
  offset 0x08: field_8     (u32)     offset 0x08: field_8     (u32)
  offset 0x0C: ptr_C       (4B)  →   offset 0x10: ptr_C       (8B) +padding
  offset 0x10: ptr_10      (4B)  →   offset 0x18: ptr_10      (8B)
  offset 0x14: ptr_14      (4B)  →   offset 0x20: ptr_14      (8B)
  offset 0x18: ptr_18      (4B)  →   offset 0x28: ptr_18      (8B)
  offset 0x1C: field_1C    (s16)     offset 0x30: field_1C    (s16)
  ...                                ...
  offset 0x34: field_34[256]     →   offset 0x5C: field_34[256]
               ^^^^ = 52                          ^^^^ = 92
```

The collision code indexed into `field_34` using raw pointer arithmetic:

```c
state->field_40 = (u8*)collData + (index + 52);
//                                         ^^ PSX offset of field_34
```

On 64-bit, `field_34` is at offset **92**, not 52. The code was reading 40 bytes before
the array — landing on the pointer fields `ptr_28`/`ptr_2C` — producing garbage collision
lookup indices.

**Fix**: Replace `52` with `offsetof(s_IpdCollisionData, field_34)`:

```c
#ifdef SH_PC_PORT
#define IPD_COLL_FIELD34_OFS  offsetof(s_IpdCollisionData, field_34)
#else
#define IPD_COLL_FIELD34_OFS  52
#endif
```

### Case Study: `s_RayState.field_5C`

The ray-tracing functions populate an `s_RayState` in the PSX scratchpad using named field
access, then read `field_5C` (the miss-distance) via a hardcoded byte offset:

```c
Ray_MissSet(ray, from, &dir, (s16)*(u16*)(&((u8*)scratchAddr)[92]));
//                                                              ^^ PSX offset of field_5C
```

`s_RayState` has a pointer at `field_20` (`s_SubCharacter*`), which shifts all subsequent
fields by 4 bytes on 64-bit. `field_5C` moves from offset 92 → 96. The old code read
`field_58` (a flag) instead of the intended distance value.

**Fix**: Use named field access via struct cast:

```c
Ray_MissSet(ray, from, &dir, ((s_RayState*)scratchAddr)->field_5C);
```

### How to Find More Instances

Search the codebase for these patterns:

```
# Raw byte-pointer arithmetic with numeric constants
grep -rn '(u8\*)\|((s8\*)\|((char\*)' src/ | grep '\[[0-9]'

# Hardcoded struct sizes in pointer arithmetic
grep -rn '+ [0-9][0-9])' src/ | grep 'u8\*\|s8\*\|char\*'
```

Any match where the numeric constant corresponds to a field offset in a struct that contains
pointers is a potential 64-bit portability bug.

### General Rules for the PC Port

1. **Never use hardcoded byte offsets** to access struct fields. Use `offsetof()` or named
   field access. PSX decomp code frequently uses magic numbers because the original MIPS
   assembly used them.

2. **Structs loaded from disc** (IPD collision data, model data, etc.) are in PSX binary
   format with 4-byte pointers. These are handled by reformatter functions
   (`ipd_reformat.c`) that convert the binary layout to native PC layout.

3. **Structs populated by C code** (like `s_RayState` in scratchpad) use the compiler's
   native layout. Accessing them via hardcoded byte offsets is always wrong on 64-bit.

4. **`STATIC_ASSERT_SIZEOF`** is disabled on PC builds (defined as empty macro when
   `SH_PC_PORT` is set) because struct sizes legitimately differ. Don't rely on it to
   catch layout issues.

5. **The `field_XX` naming convention** uses PSX offsets. On 64-bit, `field_20` is NOT at
   byte offset 0x20 if any earlier field is a pointer. The names are for identification
   only — always use named access, never compute offsets from field names.

### Affected Functions (bodyprog_800697EC.c)

| Function         | PSX Addr   | Issue | Fix |
|------------------|------------|-------|-----|
| func_8006BB50    | 0x8006BB50 | Hardcoded 52 in field_34 access | `IPD_COLL_FIELD34_OFS` |
| func_8006BF88    | 0x8006BF88 | Hardcoded 52 in field_34 access | `IPD_COLL_FIELD34_OFS` |
| func_8006C0C8    | 0x8006C0C8 | Hardcoded 52 in field_34 access | `IPD_COLL_FIELD34_OFS` |
| func_8006C45C    | 0x8006C45C | Hardcoded 52 in field_34 access | `IPD_COLL_FIELD34_OFS` |
| func_8006C794    | 0x8006C794 | Hardcoded 52 in field_34 access | `IPD_COLL_FIELD34_OFS` |
| Ray_LineCheck    | 0x8006D90C | Hardcoded 92 for field_5C       | Named field access |
| func_8006DA08    | 0x8006DA08 | Hardcoded 92 for field_5C       | Named field access |
| func_8006DB3C    | 0x8006DB3C | Hardcoded 92 for field_5C       | Named field access |
| func_8006DC18    | 0x8006DC18 | Hardcoded 92 for field_5C       | Named field access |
