/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * uwp_main.cpp - app entry and data folder for the UWP build (Xbox One /
 * Series in Developer Mode, Windows 10+).
 *
 * A packaged app starts with its working directory in the install folder,
 * which is read-only, and may not open anything outside its own folders unless
 * the file's ACL names app packages. Every path the port uses is relative
 * (config.cfg, gamedata/, saves, SilentHill_*.log), so the whole fix is to
 * stand in the app's LocalState folder before anything else runs and put the
 * shipped assets there. Same shape as the Android and iOS ports.
 */
#include <windows.h>

#include <direct.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <string>

#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>

extern "C" int main(int argc, char* argv[]);

static std::wstring s_installDir;
static std::wstring s_localDir;

static bool Uwp_IsDir(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;

    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
           (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Uwp_FileSize(const std::wstring& path, unsigned long long* size)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;

    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) ||
        (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    *size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return true;
}

/* Copies src into dst, creating directories. A file already there with the same
 * size is left alone, so an updated package refreshes its own assets without
 * rewriting 5 MB on every launch. config.cfg is never replaced: it belongs to
 * the player once it exists. */
static void Uwp_StageTree(const std::wstring& src, const std::wstring& dst)
{
    WIN32_FIND_DATAW fd;
    HANDLE           h = FindFirstFileExW((src + L"\\*").c_str(), FindExInfoBasic, &fd,
                                          FindExSearchNameMatch, NULL, 0);

    if (h == INVALID_HANDLE_VALUE)
        return;
    CreateDirectoryW(dst.c_str(), NULL);
    do
    {
        std::wstring name = fd.cFileName;
        std::wstring from = src + L"\\" + name;
        std::wstring to   = dst + L"\\" + name;

        if (name == L"." || name == L"..")
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            Uwp_StageTree(from, to);
            continue;
        }

        unsigned long long have = 0;
        unsigned long long want = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        bool               exists = Uwp_FileSize(to, &have);

        if (exists && (have == want || name == L"config.cfg"))
            continue;
        CopyFile2(from.c_str(), to.c_str(), NULL);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static bool Uwp_DirHasBin(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE           h = FindFirstFileExW((dir + L"\\*.bin").c_str(), FindExInfoBasic, &fd,
                                          FindExSearchNameMatch, NULL, 0);

    if (h == INVALID_HANDLE_VALUE)
        return false;
    FindClose(h);
    return true;
}

extern "C" int Uwp_EnterDataDir(void)
{
    const wchar_t* local   = SDL_WinRTGetFSPathUNICODE(SDL_WINRT_PATH_LOCAL_FOLDER);
    const wchar_t* install = SDL_WinRTGetFSPathUNICODE(SDL_WINRT_PATH_INSTALLED_LOCATION);

    if (local == NULL || _wchdir(local) != 0)
        return 0;
    s_localDir   = local;
    s_installDir = install ? install : L"";

    if (!s_installDir.empty())
        Uwp_StageTree(s_installDir + L"\\data", s_localDir);
    CreateDirectoryW((s_localDir + L"\\gamedata").c_str(), NULL);
    return 1;
}

/* The disc image is ~700 MB and the only way into LocalState on a console is
 * the Device Portal's file upload. A USB drive is the practical alternative:
 * an app may read one directly when the folder's ACL grants ALL APPLICATION
 * PACKAGES access, which is how every dev-mode emulator takes its games.
 * LocalState still wins when it holds a disc; saves and config never leave it.
 * Returns 1 and fills `out` (UTF-8) only when a drive folder holds a .bin. */
extern "C" int Uwp_FindDiscDir(char* out, int outSize)
{
    static const wchar_t* const kFolders[] = {
        L"SilentHill\\gamedata", L"Silent Hill\\gamedata", L"gamedata"
    };
    DWORD drives;
    int   d;

    if (Uwp_DirHasBin(s_localDir + L"\\gamedata"))
        return 0;

    drives = GetLogicalDrives();
    for (d = 3; d < 26; d++)
    {
        if (!(drives & (1u << d)))
            continue;
        for (const wchar_t* folder : kFolders)
        {
            std::wstring dir = std::wstring(1, (wchar_t)(L'A' + d)) + L":\\" + folder;

            if (Uwp_IsDir(dir) && Uwp_DirHasBin(dir) &&
                WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, out, outSize, NULL, NULL) > 0)
                return 1;
        }
    }
    return 0;
}

int CALLBACK WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    return SDL_WinRTRunApp(main, NULL);
}
