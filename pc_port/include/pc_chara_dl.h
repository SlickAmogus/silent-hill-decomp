/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_CHARA_DL_H
#define PC_CHARA_DL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Additional Characters: the character catalog (characters.json on the
 * qol-mods release, one zip per character), what is installed locally, and
 * the downloads between them. Platform-neutral core; the screens are in
 * pc_chara_dl_menu.c.
 *
 * A character is a HERO_<File>.glb + .ini pair in gamedata/load/CHARA. Its ini
 * may carry text keys the skin loader skips: name, category, author, version.
 * Downloads are recorded in gamedata/chara_downloads.txt, which the launcher
 * reads too: a recorded file wins over a mod's copy of the same name. */

#define CD_ID_MAX   48
#define CD_NAME_MAX 64
#define CD_FILE_MAX 64

typedef struct
{
    char      id[CD_ID_MAX];
    char      name[CD_NAME_MAX];
    char      category[CD_NAME_MAX];
    char      author[96];
    char      version[16];
    char      file[CD_FILE_MAX];  /* the zip's asset name */
    char      hero[CD_FILE_MAX];  /* HERO_<File>, no extension */
    long long size;
    char      sha256[65];
    long long glbSize;            /* the model inside, to recognise a copy of it */
    unsigned  glbCrc;
    int       tris;               /* triangle count, 0 when the list does not say */
} CdEntry;

/* Where a local character came from. */
enum { CD_SRC_DOWNLOAD, CD_SRC_PACK, CD_SRC_LOOSE };

typedef struct
{
    char hero[CD_FILE_MAX];        /* HERO_<File> */
    char name[CD_NAME_MAX];        /* ini name=, else the file name after HERO_ */
    char category[CD_NAME_MAX];    /* ini category=, may be empty */
    char author[96];               /* ini author=, else the pack's */
    char version[16];
    int  source;                   /* CD_SRC_* */
    int  group;                    /* index into the groups */
    int  enabled;                  /* 0 = parked in CHARA_off (or a disabled launcher mod) */
    int  oldJojo;                  /* a file of jojo670's old unversioned pack */
    int  shadowed;                 /* another copy of the same file loads instead */
} CdLocal;

typedef struct
{
    char name[CD_NAME_MAX];        /* pack / mod display name, or the category for downloads */
    char key[CD_NAME_MAX];         /* folder-safe id: mod folder, QoL id, "loose" */
    char author[96];
    int  source;                   /* CD_SRC_* */
    int  enabled;
    int  oldJojo;                  /* jojo670's Additional Characters before the catalog */
} CdGroup;

/* ---- catalog ----------------------------------------------------------- */
enum { CD_CAT_NONE, CD_CAT_LOADING, CD_CAT_READY, CD_CAT_FAILED };
/* Fetches the list unless it is here or on its way; after a failure, tries
 * again. */
void           Cd_CatalogFetch(void);
/* Once per visit to the menu: fetches the list again. The list already shown
 * stays until the new one lands, and stays if the fetch fails. With none yet
 * this session, the one saved from the last good fetch is shown meanwhile. */
void           Cd_CatalogRefresh(void);
/* "Don't ask again" on the old jojo670 pack offer. */
int            Cd_OldJojoNoAsk(void);
void           Cd_SetOldJojoNoAsk(void);
/* A QoL character pack (the old QoL list's, any launcher or port) is installed
 * whose characters the catalog now has: offer to replace it. */
int            Cd_LegacyPending(void);
void           Cd_UpdateLegacy(void);
int            Cd_LegacyNoAsk(void);
void           Cd_SetLegacyNoAsk(void);
int            Cd_CatalogState(void);
int            Cd_CatalogCount(void);
/* Bumped when a new list takes over: catalog indices held from before are
 * stale (the core moves its own; a screen rebuilds its rows). */
int            Cd_CatalogGen(void);
const CdEntry* Cd_CatalogGet(int i);

/* ---- local ------------------------------------------------------------- */
void           Cd_LocalScan(void);
int            Cd_LocalCount(void);
/* Bumped by every scan (one follows each job): local and group indices held
 * from before are stale. */
int            Cd_LocalGen(void);
const CdLocal* Cd_LocalGet(int i);
int            Cd_GroupCount(void);
const CdGroup* Cd_GroupGet(int i);
int            Cd_LocalFind(const char* hero);  /* -1 when not installed */

/* The catalog entry is installed from the catalog (any version). */
int Cd_IsDownloaded(const CdEntry* e);
/* Installed from the catalog, but an older version or build of it. */
int Cd_NeedsUpdate(const CdEntry* e);
/* A file of that name is present that the catalog did not install. */
/* Load order: the downloads in CD_DL_DIR first, then gamedata/load/CHARA
 * (mods, packs, files copied in by hand). A download always wins a file name;
 * the game resolves it, so a launcher that knows nothing of downloads (an
 * old one) cannot change that. */
#define CD_DL_DIR "gamedata/load/CHARA_dl"
/* A pack's or hand-copied copy of e's file (not the download): its local
 * index, else -1. */
int Cd_PackCopy(const CdEntry* e);
/* `base` (a path without extension) as the load order resolves it: a file
 * under load/CHARA that has a download is the download. Returns `base`
 * itself or `out`. */
const char* Cd_ResolveBase(const char* base, char* out, int outSz);

/* Display text from an ini (name/category/author/version); empty when absent. */
void Cd_IniMeta(const char* hero, char* name, int nameSz, char* category, int catSz,
                char* author, int authSz, char* version, int verSz);
/* The name a player sees: ini name, else the file name after HERO_. */
void Cd_DisplayName(const char* hero, char* out, int outSz);

/* ---- jobs (one at a time, queued) -------------------------------------- */
void Cd_QueueInstall(int catalogIdx);
void Cd_QueueRemove(int catalogIdx);
/* A download, by its local index: needs no list, so it works offline. */
void Cd_QueueRemoveLocal(int localIdx);
void Cd_QueuePreview(int catalogIdx);
void Cd_SetGroupEnabled(int group, int enabled);
int  Cd_Busy(void);
/* What the current or last job is doing, and its progress (0..1, -1 unknown). */
const char* Cd_Status(float* progress);
/* Set when an installed file replaced one the running game may already hold. */
int  Cd_RestartAdvised(void);
void Cd_Tick(void);

/* The preview file for an entry, relative to gamedata/load/CHARA (for the
 * skin loader), once a preview download has finished; NULL otherwise. */
const char* Cd_PreviewFile(int catalogIdx);

/* jojo670's pre-catalog pack is installed and the catalog has newer copies. */
int  Cd_OldJojoPending(void);
void Cd_UpdateOldJojo(void);   /* queue the catalog copies, park the old pack */

#ifdef __cplusplus
}
#endif

#endif
