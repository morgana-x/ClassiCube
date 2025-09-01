\
/* SoundPack implementation wired for texturepack-style helpers.
 *
 * Assumed available in your project (defaults used here):
 *   int Http_DownloadFile(const char* url, const char* outPath, void* progressCb, void* userData);
 *   int Zip_Extract(const char* zipPath, const char* destDir);
 *
 * Cache dir: soundpackcache/<8-hex-hash>/
 * ids.txt expected at: sounds/ids.txt
 * ids.txt format: filename.ogg = 101 (IDs must be >= 100)
 */

#include "SoundPack.h"
#include "String.h"
#include "Stream.h"
#include "Http.h"
#include "Logger.h"
#include "Utils.h"
#include "Vorbis.h"
#include "Chat.h"
#include "Platform.h"
#include "Deflate.h"
#include "Event.h"
#include "Options.h"
#include "Files.h"
#include "Funcs.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>

#define SOUNDCACHE_DIR "soundpackcache"
#define ACCEPTED_TXT   SOUNDCACHE_DIR "/acceptedurls.txt"
#define DENIED_TXT     SOUNDCACHE_DIR "/deniedurls.txt"
#define IDS_TXT_SUBPATH "sounds/ids.txt"

#define CUSTOM_SOUND_MIN_ID 100u
#define CUSTOM_SOUND_MAX_ID 65535u

/* External helpers assumed from project */
extern int Http_DownloadFile(const char* url, const char* outPath, void* progressCb, void* userData);
extern int Zip_Extract(const char* zipPath, const char* destDir);

struct CustomSoundEntry { cc_uint16 id; cc_string filename; cc_bool loaded; };
static struct CustomSoundEntry* customSounds = NULL;
static int customSoundsCount = 0;

static struct StringsBuffer acceptedList = {0}, deniedList = {0};

static uint32_t UrlHash(const cc_string* url) {
    uint32_t h = 5381;
    int i;
    for (i = 0; i < url->length; i++) {
        unsigned char c = (unsigned char)url->buffer[i];
        h = ((h << 5) + h) + c;
    }
    return h;
}

static cc_string MakeCacheDirForUrl(const cc_string* url) {
    char tmp[64];
    uint32_t h = UrlHash(url);
    snprintf(tmp, sizeof(tmp), SOUNDCACHE_DIR "/%08X", h);
    cc_string out; String_InitArray(out, NULL);
    String_AppendConst(&out, tmp);
    return out;
}

static cc_result EnsureDirExists(const cc_string* path) {
    if (File_Exists(path->buffer)) return 0;
    return File_CreateDir(path->buffer);
}

static int SoundUrls_Init(void) {
    EntryList_UNSAFE_Load(&acceptedList, ACCEPTED_TXT);
    EntryList_UNSAFE_Load(&deniedList,   DENIED_TXT);
    return 0;
}

cc_bool SoundUrls_HasAccepted(const cc_string* url) { return EntryList_Find(&acceptedList, url, ' ') >= 0; }
cc_bool SoundUrls_HasDenied(const cc_string* url)   { return EntryList_Find(&deniedList,   url, ' ') >= 0; }
void SoundUrls_Accept(const cc_string* url) { EntryList_Set(&acceptedList, url, &String_Empty, ' '); EntryList_Save(&acceptedList, ACCEPTED_TXT); }
void SoundUrls_Deny(const cc_string* url)   { EntryList_Set(&deniedList,   url, &String_Empty, ' '); EntryList_Save(&deniedList,   DENIED_TXT); }
int  SoundUrls_ClearDenied(void) { int c = deniedList.count; StringsBuffer_Clear(&deniedList); EntryList_Save(&deniedList, DENIED_TXT); return c; }

static void CustomSounds_Clear(void) {
    int i;
    for (i = 0; i < customSoundsCount; i++) {
        String_Free(&customSounds[i].filename);
    }
    Mem_Free(customSounds);
    customSounds = NULL; customSoundsCount = 0;
}

/* Parse ids.txt */
static cc_result ParseIdsTxt(Stream* s) {
    int size = (int)Stream_Length(s);
    if (size <= 0) return ERR_INVALID_DATA;
    char* buff = (char*)Mem_TryAlloc(size + 1, 1);
    if (!buff) return ERR_OUT_OF_MEMORY;
    Stream_Read(s, buff, size);
    buff[size] = '\0';

    char* cur = buff;
    while (*cur) {
        char* line = cur;
        while (*cur && *cur != '\n' && *cur != '\r') cur++;
        if (*cur) { *cur = '\0'; cur++; while (*cur == '\n' || *cur == '\r') cur++; }

        char* a = line; while (*a && isspace((unsigned char)*a)) a++;
        if (!*a) continue;
        char* eq = strchr(a, '=');
        if (!eq) {
            Chat_AddRaw("&cInvalid line in soundpack ids.txt (missing '=')");
            Mem_Free(buff);
            return ERR_INVALID_DATA;
        }
        *eq = '\0';
        char* name = a;
        char* idstr = eq + 1;
        char* end = name + strlen(name) - 1;
        while (end >= name && isspace((unsigned char)*end)) { *end = '\0'; end--; }
        while (*idstr && isspace((unsigned char)*idstr)) idstr++;
        char* idend = idstr + strlen(idstr) - 1;
        while (idend >= idstr && isspace((unsigned char)*idend)) { *idend = '\0'; idend--; }
        if (!*name || !*idstr) {
            Chat_AddRaw("&cInvalid ids.txt line (empty name/id)");
            Mem_Free(buff);
            return ERR_INVALID_DATA;
        }
        long id = strtol(idstr, NULL, 10);
        if (id < (long)CUSTOM_SOUND_MIN_ID || id > (long)CUSTOM_SOUND_MAX_ID) {
            Chat_Add1("&cSound id %i in ids.txt is invalid: must be >= %i", &id, &((int){CUSTOM_SOUND_MIN_ID}));
            Mem_Free(buff);
            return ERR_INVALID_DATA;
        }

        struct CustomSoundEntry* tmp = (struct CustomSoundEntry*)Mem_TryRealloc(customSounds, sizeof(struct CustomSoundEntry) * (customSoundsCount + 1));
        if (!tmp) { Mem_Free(buff); return ERR_OUT_OF_MEMORY; }
        customSounds = tmp;
        customSounds[customSoundsCount].id = (cc_uint16)id;
        String_InitArray(customSounds[customSoundsCount].filename, NULL);
        customSounds[customSoundsCount].filename.length = 0;
        customSounds[customSoundsCount].filename.capacity = (int)strlen(name) + 1;
        customSounds[customSoundsCount].filename.buffer = (char*)Mem_TryAlloc(customSounds[customSoundsCount].filename.capacity, 1);
        if (!customSounds[customSoundsCount].filename.buffer) { Mem_Free(buff); return ERR_OUT_OF_MEMORY; }
        String_CopyBuf(&customSounds[customSoundsCount].filename, name, (int)strlen(name));
        customSounds[customSoundsCount].loaded = false;
        customSoundsCount++;
    }

    Mem_Free(buff);
    return 0;
}

/* Validate mappings (ensure files exist under extractRoot/sounds/) */
static cc_result ValidateAndRegister(const cc_string* extractRoot) {
    int i;
    cc_string full;
    for (i = 0; i < customSoundsCount; i++) {
        cc_string rel = customSounds[i].filename;
        if (rel.length >= 7 && strncmp(rel.buffer, "sounds/", 7) == 0) {
            /* ok */
        } else {
            cc_string tmp; String_InitArray(tmp, NULL);
            String_Format2(&tmp, "sounds/%s", rel.buffer);
            String_Free(&customSounds[i].filename);
            customSounds[i].filename = tmp;
        }
        String_InitArray(full, NULL);
        String_Format2(&full, "%s/%s", extractRoot->buffer, customSounds[i].filename.buffer);
        if (!File_Exists(full.buffer)) {
            Chat_Add1("&cSoundpack is missing file %s", &customSounds[i].filename);
            String_Free(&full);
            return ERR_NOT_FOUND;
        }
        String_Free(&full);
    }
    return 0;
}

/* wrappers assume project has Http_DownloadFile and Zip_Extract */
static cc_result DownloadToFile(const cc_string* url, const cc_string* outPath) {
    Chat_Add1("&e[SoundPack] Downloading %s ...", url);
    if (Http_DownloadFile(url->buffer, outPath->buffer, NULL, NULL) != 0) {
        Chat_AddRaw("&c[SoundPack] Download failed");
        return ERR_NETWORK;
    }
    return 0;
}

static cc_result UnzipToDir(const cc_string* zipPath, const cc_string* destDir) {
    int r = Zip_Extract(zipPath->buffer, destDir->buffer);
    if (r != 0) {
        Chat_AddRaw("&c[SoundPack] Zip extraction failed");
        return ERR_IO;
    }
    return 0;
}

/* extract flow */
void SoundPack_Extract(const cc_string* url) {
    if (!url || !url->length) return;

    cc_string cacheDir = MakeCacheDirForUrl(url);
    cc_string baseDir; String_InitArray(baseDir, NULL);
    String_Format2(&baseDir, "%s", SOUNDCACHE_DIR);
    EnsureDirExists(&baseDir);
    EnsureDirExists(&cacheDir);

    cc_string idsPath; String_InitArray(idsPath, NULL);
    String_Format2(&idsPath, "%s/%s", cacheDir.buffer, IDS_TXT_SUBPATH);
    if (File_Exists(idsPath.buffer)) {
        Stream* s = Stream_OpenFile(idsPath.buffer, STREAM_IN);
        if (s) {
            CustomSounds_Clear();
            if (ParseIdsTxt(s) == 0) {
                if (ValidateAndRegister(&cacheDir) == 0) {
                    Chat_AddRaw("&a[SoundPack] Soundpack already cached and loaded.");
                    Stream_Close(s);
                    String_Free(&idsPath);
                    String_Free(&cacheDir);
                    String_Free(&baseDir);
                    return;
                }
            }
            Stream_Close(s);
        }
    }
    String_Free(&idsPath)

    /* Download zip */
    cc_string tmpZip; String_InitArray(tmpZip, NULL);
    String_Format2(&tmpZip, "%s/%s.zip", cacheDir.buffer, "soundpack");
    cc_result res = DownloadToFile(url, &tmpZip);
    if (res) {
        Chat_AddRaw("&c[SoundPack] Failed to download soundpack.");
        String_Free(&tmpZip);
        String_Free(&cacheDir);
        String_Free(&baseDir);
        return;
    }

    /* Extract */
    res = UnzipToDir(&tmpZip, &cacheDir);
    if (res) {
        Chat_AddRaw("&c[SoundPack] Failed to extract soundpack zip.");
        File_Delete(tmpZip.buffer);
        String_Free(&tmpZip);
        String_Free(&cacheDir);
        String_Free(&baseDir);
        return;
    }

    File_Delete(tmpZip.buffer);
    String_Free(&tmpZip);

    /* parse ids.txt */
    cc_string idsFull; String_InitArray(idsFull, NULL);
    String_Format2(&idsFull, "%s/%s", cacheDir.buffer, IDS_TXT_SUBPATH);
    if (!File_Exists(idsFull.buffer)) {
        Chat_AddRaw("&c[SoundPack] ids.txt not found in soundpack 'sounds/ids.txt'");
        String_Free(&idsFull);
        String_Free(&cacheDir);
        String_Free(&baseDir);
        return;
    }
    Stream* idsStream = Stream_OpenFile(idsFull.buffer, STREAM_IN);
    if (!idsStream) {
        Chat_AddRaw("&c[SoundPack] Failed to open ids.txt");
        String_Free(&idsFull);
        String_Free(&cacheDir);
        String_Free(&baseDir);
        return;
    }
    CustomSounds_Clear();
    if (ParseIdsTxt(idsStream) != 0) {
        Chat_AddRaw("&c[SoundPack] Failed to parse ids.txt");
        Stream_Close(idsStream);
        String_Free(&idsFull);
        String_Free(&cacheDir);
        String_Free(&baseDir);
        return;
    }
    Stream_Close(idsStream);

    if (ValidateAndRegister(&cacheDir) != 0) {
        Chat_AddRaw("&c[SoundPack] Validation failed for soundpack files.");
        String_Free(&idsFull);
        String_Free(&cacheDir);
        String_Free(&baseDir);
        return;
    }

    Chat_AddRaw("&a[SoundPack] Soundpack extracted and registered successfully.");
    String_Free(&idsFull);
    String_Free(&cacheDir);
    String_Free(&baseDir);
}

/* playback */
void Audio_PlayCustom2D(cc_uint8 channel, cc_uint16 id, cc_uint32 volume, cc_uint8 rate) {
    int i;
    for (i = 0; i < customSoundsCount; i++) {
        if (customSounds[i].id == id) {
            Platform_Log1("[SoundPack] Play custom sound: %s (id %i)", &customSounds[i].filename);
            /* Lazy-load and decode with Vorbis loader here */
            return;
        }
    }
}

/* 3D fallback to 2D */
void Audio_PlayCustom3D(cc_uint8 channel, cc_uint16 id, cc_uint32 volume, cc_uint8 rate, cc_uint16 x, cc_uint16 y, cc_uint16 z) {
    Audio_PlayCustom2D(channel, id, volume, rate);
}

static void OnInit(void) { SoundUrls_Init(); }
static void OnFree(void) { CustomSounds_Clear(); }
struct IGameComponent SoundPack_Component = { OnInit, OnFree, NULL };
