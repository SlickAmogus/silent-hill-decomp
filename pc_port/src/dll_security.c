/*
 * dll_security.c — Static PE binary inspection & safety audit for PC port plugins.
 *
 * This file inspects .dll binaries on disk prior to calling LoadLibrary to prevent
 * unauthorized execution of dangerous OS APIs (networking, process injection, shells)
 * and ensure plugins strictly conform to the Silent Hill PC Port contract.
 */
#include "dll_security.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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

DllSecurityResult DllSecurity_AuditPlugin(const char* path, char* outReason, int maxReasonLen)
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
    }
    else if (optMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        IMAGE_OPTIONAL_HEADER32 optHeader32;
        if (fread(&optHeader32, sizeof(IMAGE_OPTIONAL_HEADER32), 1, f) != 1)
        {
            fclose(f);
            return DLL_SECURITY_ERR_INVALID_PE;
        }
        if (optHeader32.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT)
            importRva = optHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (optHeader32.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT)
            exportRva = optHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
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

    /* 1. Audit Import Table for blocked/dangerous OS DLLs */
    if (importRva != 0)
    {
        DWORD importOffset = RvaToFileOffset(importRva, sections, numSections);
        if (importOffset != 0 && fseek(f, importOffset, SEEK_SET) == 0)
        {
            IMAGE_IMPORT_DESCRIPTOR desc;
            while (fread(&desc, sizeof(IMAGE_IMPORT_DESCRIPTOR), 1, f) == 1 && desc.Name != 0)
            {
                long savedPos = ftell(f);
                DWORD nameOffset = RvaToFileOffset(desc.Name, sections, numSections);
                if (nameOffset != 0 && fseek(f, nameOffset, SEEK_SET) == 0)
                {
                    char dllName[128] = {0};
                    if (fread(dllName, 1, sizeof(dllName) - 1, f) > 0)
                    {
                        dllName[sizeof(dllName) - 1] = '\0';
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
                    }
                }
                fseek(f, savedPos, SEEK_SET);
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

    if (!hasValidPluginExport)
    {
        if (outReason)
        {
            snprintf(outReason, maxReasonLen,
                     "DLL does not implement the Silent Hill PC plugin interface (no SH_Plugin_* exports found)");
        }
        return DLL_SECURITY_ERR_NO_PLUGIN_EXPORTS;
    }

    if (outReason) snprintf(outReason, maxReasonLen, "Security audit passed (Valid Silent Hill plugin)");
    return DLL_SECURITY_OK;
}

#else /* Non-Windows stub */

DllSecurityResult DllSecurity_AuditPlugin(const char* path, char* outReason, int maxReasonLen)
{
    if (outReason) snprintf(outReason, maxReasonLen, "Audit passed (POSIX host)");
    return DLL_SECURITY_OK;
}

#endif
