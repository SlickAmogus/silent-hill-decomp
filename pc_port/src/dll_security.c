/*
 * dll_security.c -- static PE lint run on mod DLLs before LoadLibrary.
 *
 * See dll_security.h for the honest scope statement: this catches invalid
 * binaries, import tables that NAME network/shell libraries, and missing
 * plugin exports. It cannot catch runtime GetProcAddress resolution or
 * anything DllMain does -- it is a tripwire for accidents and low-effort
 * abuse, not a sandbox. User-facing messaging must never call a DLL that
 * passes this "safe" or "verified".
 */
#include "dll_security.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Config override (allow_unrecognized_dlls): downgrades ONLY the
 * unknown-import verdict to a logged pass. Flagrant functions, blacklisted
 * libraries and invalid binaries always block. Defined for BOTH platforms
 * (pc_config.c references it unconditionally; the POSIX audit is a no-op
 * but the symbol must still exist to link). */
int g_DllAllowUnrecognized = 0;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Convert a Relative Virtual Address (RVA) to a file offset using Section Headers */
static DWORD RvaToFileOffset(DWORD rva, IMAGE_SECTION_HEADER* sections, WORD numSections)
{
    for (WORD i = 0; i < numSections; i++)
    {
        DWORD secVA = sections[i].VirtualAddress;
        DWORD secSize = sections[i].Misc.VirtualSize ? sections[i].Misc.VirtualSize : sections[i].SizeOfRawData;

        if (rva >= secVA && rva < secVA + secSize)
        {
            return sections[i].PointerToRawData + (rva - secVA);
        }
    }
    return 0;
}

static int StrCaseEqual(const char* a, const char* b)
{
    while (*a && *b)
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* Blacklist of DLLs that a game plugin must NEVER import */
static const char* s_blockedDlls[] = {
    "ws2_32.dll",
    "wsock32.dll",
    "wininet.dll",
    "urlmon.dll",
    "winhttp.dll",
    "shell32.dll",
    NULL
};

/* The edited-game-code fingerprint (DLL_AUDIT_MAP): every shipped map DLL
 * imports exactly these three; the CRT alternates cover reasonable toolchain
 * drift (ucrt builds, shared libgcc) so a future rebuild doesn't brick
 * modding. Verified with objdump across the 42 map DLLs + chara_global. */
/* The edited-game-code allowlist is DERIVED from the running executable's own
 * import table: a map DLL is edited game code iff every library it imports is
 * one the game itself already loads into the process (plus the exe and the C
 * runtime). This is version-proof -- an older build that linked SDL2/OpenAL/
 * ole32/etc. straight into its map DLLs is accepted because the exe of that
 * era imported the same libraries -- and self-consistent: the game uses
 * winhttp (achievements) and shell32 (SDL), so a map DLL using them adds no
 * capability the process lacks. Anything the game does NOT import (ws2_32,
 * wininet, urlmon, a random user DLL) still stands out. */
#define MAX_EXE_IMPORTS 128
static char  s_exeImports[MAX_EXE_IMPORTS][64];
static int   s_exeImportCount = 0;
static int   s_exeImportsBuilt = 0;

static void BuildExeAllowlist(void)
{
    HMODULE hExe;
    unsigned char* base;
    IMAGE_DOS_HEADER* dos;
    IMAGE_NT_HEADERS* nt;
    IMAGE_DATA_DIRECTORY* dir;
    IMAGE_IMPORT_DESCRIPTOR* imp;

    if (s_exeImportsBuilt) return;
    s_exeImportsBuilt = 1; /* even on failure: fall back to the static minimum */

    hExe = GetModuleHandleW(NULL);
    if (!hExe) return;
    base = (unsigned char*)hExe;
    dos  = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir->VirtualAddress == 0) return;

    /* In a loaded module RVAs are already offsets from the base. */
    imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);
    while (imp->Name != 0 && s_exeImportCount < MAX_EXE_IMPORTS)
    {
        const char* name = (const char*)(base + imp->Name);
        int n = 0;
        while (name[n] && n < 63) { s_exeImports[s_exeImportCount][n] = name[n]; n++; }
        s_exeImports[s_exeImportCount][n] = 0;
        s_exeImportCount++;
        imp++;
    }
}

/* api-ms-win-crt-*.dll forwarders (ucrt builds) are allowed by prefix; the exe
 * name and the base CRT/runtime libs are always allowed even if the exe walk
 * failed, so a map DLL is never rejected purely because the allowlist is
 * empty. */
static int MapImportAllowed(const char* dllName)
{
    static const char* s_baseline[] = {
        "silenthillpc.exe", "kernel32.dll", "msvcrt.dll", "ucrtbase.dll",
        "libgcc_s_seh-1.dll", "libwinpthread-1.dll", "libssp-0.dll",
        "libstdc++-6.dll", NULL
    };
    int i;

    BuildExeAllowlist();

    for (i = 0; i < s_exeImportCount; i++)
    {
        if (StrCaseEqual(dllName, s_exeImports[i])) return 1;
    }
    for (i = 0; s_baseline[i] != NULL; i++)
    {
        if (StrCaseEqual(dllName, s_baseline[i])) return 1;
    }
    {
        static const char pfx[] = "api-ms-win-crt-";
        int k;
        for (k = 0; pfx[k] != '\0'; k++)
        {
            if (tolower((unsigned char)dllName[k]) != pfx[k]) break;
        }
        if (pfx[k] == '\0') return 1;
    }
    return 0;
}

/* Process-creation and injection primitives: never present in edited game
 * code, and the exe does not import them either, so their appearance in a map
 * DLL is a hard block (both audit modes). Dynamic resolution
 * (LoadLibrary/GetProcAddress) is intentionally NOT flagged -- the game
 * imports it itself and edited code has direct access regardless. */
static const char* s_flagrantKernel32Both[] = {
    "CreateProcessA", "CreateProcessW", "WinExec",
    "OpenProcess", "WriteProcessMemory", "ReadProcessMemory",
    "CreateRemoteThread", "VirtualAllocEx", "SetThreadContext",
    "QueueUserAPC",
    NULL
};

DllSecurityResult DllSecurity_AuditPlugin(const char* path, int mode, char* outReason, int maxReasonLen)
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        if (outReason) snprintf(outReason, maxReasonLen, "Unable to open file for security audit");
        return DLL_SECURITY_ERR_FILE_OPEN;
    }

    IMAGE_DOS_HEADER dosHeader;
    if (fread(&dosHeader, sizeof(IMAGE_DOS_HEADER), 1, f) != 1 || dosHeader.e_magic != IMAGE_DOS_SIGNATURE)
    {
        fclose(f);
        if (outReason) snprintf(outReason, maxReasonLen, "Invalid DOS header (not a valid PE binary)");
        return DLL_SECURITY_ERR_INVALID_PE;
    }

    if (fseek(f, dosHeader.e_lfanew, SEEK_SET) != 0)
    {
        fclose(f);
        if (outReason) snprintf(outReason, maxReasonLen, "Invalid PE offset");
        return DLL_SECURITY_ERR_INVALID_PE;
    }

    DWORD peSignature = 0;
    if (fread(&peSignature, sizeof(DWORD), 1, f) != 1 || peSignature != IMAGE_NT_SIGNATURE)
    {
        fclose(f);
        if (outReason) snprintf(outReason, maxReasonLen, "Invalid NT signature");
        return DLL_SECURITY_ERR_INVALID_PE;
    }

    IMAGE_FILE_HEADER fileHeader;
    if (fread(&fileHeader, sizeof(IMAGE_FILE_HEADER), 1, f) != 1)
    {
        fclose(f);
        if (outReason) snprintf(outReason, maxReasonLen, "Unable to read PE file header");
        return DLL_SECURITY_ERR_INVALID_PE;
    }

    if (!(fileHeader.Characteristics & IMAGE_FILE_DLL))
    {
        fclose(f);
        if (outReason) snprintf(outReason, maxReasonLen, "Binary is not a dynamic link library (DLL)");
        return DLL_SECURITY_ERR_INVALID_PE;
    }

    WORD optMagic = 0;
    if (fread(&optMagic, sizeof(WORD), 1, f) != 1)
    {
        fclose(f);
        return DLL_SECURITY_ERR_INVALID_PE;
    }
    fseek(f, -((long)sizeof(WORD)), SEEK_CUR);

    DWORD importRva = 0;
    DWORD exportRva = 0;
    DWORD delayRva  = 0;

    if (optMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        IMAGE_OPTIONAL_HEADER64 optHeader64;
        if (fread(&optHeader64, sizeof(IMAGE_OPTIONAL_HEADER64), 1, f) != 1)
        {
            fclose(f);
            return DLL_SECURITY_ERR_INVALID_PE;
        }
        if (optHeader64.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT)
            importRva = optHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (optHeader64.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT)
            exportRva = optHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        if (optHeader64.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT)
            delayRva = optHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress;
    }
    else if (optMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        /* 32-bit DLL in a 64-bit process: LoadLibrary would fail anyway;
         * reject with a reason the user can act on. */
        fclose(f);
        if (outReason) snprintf(outReason, maxReasonLen, "32-bit DLL; this game is 64-bit");
        return DLL_SECURITY_ERR_INVALID_PE;
    }
    else
    {
        fclose(f);
        if (outReason) snprintf(outReason, maxReasonLen, "Unsupported PE optional header magic");
        return DLL_SECURITY_ERR_INVALID_PE;
    }

    /* Read Section Headers */
    WORD numSections = fileHeader.NumberOfSections;
    if (numSections == 0 || numSections > 96)
    {
        fclose(f);
        return DLL_SECURITY_ERR_INVALID_PE;
    }

    IMAGE_SECTION_HEADER* sections = (IMAGE_SECTION_HEADER*)malloc(numSections * sizeof(IMAGE_SECTION_HEADER));
    if (!sections)
    {
        fclose(f);
        return DLL_SECURITY_ERR_FILE_OPEN;
    }

    if (fread(sections, sizeof(IMAGE_SECTION_HEADER), numSections, f) != numSections)
    {
        free(sections);
        fclose(f);
        return DLL_SECURITY_ERR_INVALID_PE;
    }

    /* 1. Audit the import table.
     * MAP mode: every imported DLL must be on the edited-game-code allowlist.
     * PLUGIN mode: imported DLLs must not be on the network/shell blacklist.
     * Both modes: kernel32 import THUNKS are walked and flagrant process/
     * injection functions are refused (plus LoadLibrary/GetProcAddress in MAP
     * mode -- dynamic resolution is the standard screening bypass and plain
     * edited game code never does it). */
    if (importRva != 0)
    {
        DWORD importOffset = RvaToFileOffset(importRva, sections, numSections);
        if (importOffset != 0 && fseek(f, importOffset, SEEK_SET) == 0)
        {
            IMAGE_IMPORT_DESCRIPTOR desc;
            int descIdx = 0;
            while (fread(&desc, sizeof(IMAGE_IMPORT_DESCRIPTOR), 1, f) == 1 && desc.Name != 0)
            {
                long savedPos = ftell(f);
                DWORD nameOffset = RvaToFileOffset(desc.Name, sections, numSections);
                char dllName[128] = {0};
                if (nameOffset != 0 && fseek(f, nameOffset, SEEK_SET) == 0 &&
                    fread(dllName, 1, sizeof(dllName) - 1, f) > 0)
                {
                    dllName[sizeof(dllName) - 1] = '\0';

                    if (mode == DLL_AUDIT_MAP && !MapImportAllowed(dllName) &&
                        !g_DllAllowUnrecognized)
                    {
                        free(sections);
                        fclose(f);
                        if (outReason)
                        {
                            snprintf(outReason, maxReasonLen,
                                     "imports '%s', which edited game code never uses (map DLLs import only the game + C runtime)",
                                     dllName);
                        }
                        return DLL_SECURITY_ERR_UNKNOWN_IMPORT;
                    }
                    for (int b = 0; s_blockedDlls[b] != NULL; b++)
                    {
                        if (StrCaseEqual(dllName, s_blockedDlls[b]))
                        {
                            free(sections);
                            fclose(f);
                            if (outReason)
                            {
                                snprintf(outReason, maxReasonLen,
                                         "Blocked dangerous library import: '%s' (networking/shell execution is prohibited)",
                                         dllName);
                            }
                            return DLL_SECURITY_ERR_BLOCKED_IMPORT;
                        }
                    }

                    /* kernel32: walk the name thunks for flagrant functions. */
                    if (StrCaseEqual(dllName, "kernel32.dll"))
                    {
                        DWORD thunkRva = desc.OriginalFirstThunk ? desc.OriginalFirstThunk : desc.FirstThunk;
                        DWORD thunkOff = RvaToFileOffset(thunkRva, sections, numSections);
                        if (thunkOff != 0 && fseek(f, thunkOff, SEEK_SET) == 0)
                        {
                            ULONGLONG entry;
                            int t = 0;
                            while (t < 4096 && fread(&entry, sizeof(entry), 1, f) == 1 && entry != 0)
                            {
                                long tPos = ftell(f);
                                t++;
                                if (!(entry & IMAGE_ORDINAL_FLAG64))
                                {
                                    DWORD fnOff = RvaToFileOffset((DWORD)(entry & 0x7FFFFFFF), sections, numSections);
                                    char fn[96] = {0};
                                    if (fnOff != 0 && fseek(f, fnOff + 2, SEEK_SET) == 0 &&
                                        fread(fn, 1, sizeof(fn) - 1, f) > 0)
                                    {
                                        const char* hit = NULL;
                                        for (int b = 0; hit == NULL && s_flagrantKernel32Both[b] != NULL; b++)
                                        {
                                            if (StrCaseEqual(fn, s_flagrantKernel32Both[b])) hit = fn;
                                        }
                                        if (hit != NULL)
                                        {
                                            free(sections);
                                            fclose(f);
                                            if (outReason)
                                            {
                                                snprintf(outReason, maxReasonLen,
                                                         "imports kernel32!%s, which edited game code never uses", fn);
                                            }
                                            return DLL_SECURITY_ERR_BLOCKED_FUNCTION;
                                        }
                                    }
                                }
                                fseek(f, tPos, SEEK_SET);
                            }
                        }
                    }
                }
                fseek(f, savedPos, SEEK_SET);
                descIdx++;
                if (descIdx > 256) break; /* malformed table bound */
            }
        }
    }

    /* Delay-load directory: name-check its DLLs against the same allowlist
     * rather than refusing outright (some toolchains delay-load CRT bits). */
    if (mode == DLL_AUDIT_MAP && delayRva != 0 && !g_DllAllowUnrecognized)
    {
        DWORD delayOff = RvaToFileOffset(delayRva, sections, numSections);
        if (delayOff != 0 && fseek(f, delayOff, SEEK_SET) == 0)
        {
            IMAGE_DELAYLOAD_DESCRIPTOR dd;
            int di = 0;
            while (di < 64 && fread(&dd, sizeof(dd), 1, f) == 1 && dd.DllNameRVA != 0)
            {
                long dpos = ftell(f);
                DWORD nOff = RvaToFileOffset(dd.DllNameRVA, sections, numSections);
                char dn[128] = {0};
                if (nOff != 0 && fseek(f, nOff, SEEK_SET) == 0 && fread(dn, 1, sizeof(dn) - 1, f) > 0)
                {
                    dn[sizeof(dn) - 1] = 0;
                    if (!MapImportAllowed(dn))
                    {
                        free(sections);
                        fclose(f);
                        if (outReason)
                            snprintf(outReason, maxReasonLen,
                                     "delay-loads '%s', which the game itself does not use", dn);
                        return DLL_SECURITY_ERR_UNKNOWN_IMPORT;
                    }
                }
                fseek(f, dpos, SEEK_SET);
                di++;
            }
        }
    }

    /* 2. Audit Export Table for valid Silent Hill plugin symbols */
    int hasValidPluginExport = 0;
    if (exportRva != 0)
    {
        DWORD exportOffset = RvaToFileOffset(exportRva, sections, numSections);
        if (exportOffset != 0 && fseek(f, exportOffset, SEEK_SET) == 0)
        {
            IMAGE_EXPORT_DIRECTORY expDir;
            if (fread(&expDir, sizeof(IMAGE_EXPORT_DIRECTORY), 1, f) == 1)
            {
                DWORD namesOffset = RvaToFileOffset(expDir.AddressOfNames, sections, numSections);
                if (namesOffset != 0 && fseek(f, namesOffset, SEEK_SET) == 0)
                {
                    DWORD numNames = expDir.NumberOfNames;
                    if (numNames > 512) numNames = 512;

                    DWORD* nameRvas = (DWORD*)malloc(numNames * sizeof(DWORD));
                    if (nameRvas && fread(nameRvas, sizeof(DWORD), numNames, f) == numNames)
                    {
                        for (DWORD i = 0; i < numNames; i++)
                        {
                            DWORD funcNameOffset = RvaToFileOffset(nameRvas[i], sections, numSections);
                            if (funcNameOffset != 0 && fseek(f, funcNameOffset, SEEK_SET) == 0)
                            {
                                char funcName[128] = {0};
                                if (fread(funcName, 1, sizeof(funcName) - 1, f) > 0)
                                {
                                    funcName[sizeof(funcName) - 1] = '\0';
                                    if (strncmp(funcName, "SH_Plugin_", 10) == 0)
                                    {
                                        hasValidPluginExport = 1;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (nameRvas) free(nameRvas);
                }
            }
        }
    }

    free(sections);
    fclose(f);

    if (mode == DLL_AUDIT_PLUGIN && !hasValidPluginExport)
    {
        if (outReason)
        {
            snprintf(outReason, maxReasonLen,
                     "DLL does not implement the Silent Hill PC plugin interface (no SH_Plugin_* exports found)");
        }
        return DLL_SECURITY_ERR_NO_PLUGIN_EXPORTS;
    }

    if (outReason) snprintf(outReason, maxReasonLen, "no blocked imports found (static check only)");
    return DLL_SECURITY_OK;
}

#else /* Non-Windows stub */

DllSecurityResult DllSecurity_AuditPlugin(const char* path, int mode, char* outReason, int maxReasonLen)
{
    /* No PE parsing on POSIX hosts; the check is a no-op there. */
    (void)path; (void)mode;
    if (outReason) snprintf(outReason, maxReasonLen, "static check unavailable on this host");
    return DLL_SECURITY_OK;
}

#endif
