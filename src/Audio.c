#include "Audio.h"
#include "String.h"
#include "Logger.h"
#include "Event.h"
#include "Block.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "Game.h"
#include "Errors.h"
#include "Vorbis.h"
#include "Chat.h"
#include "Stream.h"
#include "Utils.h"
#include "Options.h"
#include "Deflate.h"
#ifdef CC_BUILD_MOBILE
/* TODO: Refactor maybe to not rely on checking WinInfo.Handle != NULL */
#include "Window.h"
#endif

int Audio_SoundsVolume, Audio_MusicVolume;

const char* const Sound_Names[SOUND_COUNT] = {
    "none", "wood", "gravel", "grass", "stone",
    "metal", "glass", "cloth", "sand", "snow",
    /* custom groups we added */
    "pling",               /* SOUND_PLING */
    "challenge_complete",  /* SOUND_CHALLENGE_COMPLETE */
};


const cc_string Sounds_ZipPathMC = String_FromConst("audio/default.zip");
const cc_string Sounds_ZipPathCC = String_FromConst("audio/classicube.zip");
static const cc_string audio_dir = String_FromConst("audio");


/*########################################################################################################################*
*--------------------------------------------------------Sounds-----------------------------------------------------------*
*#########################################################################################################################*/
#ifdef CC_BUILD_NOSOUNDS
/* Can't use mojang's sound assets, so just stub everything out */
static void Sounds_Init(void) { }
static void Sounds_Free(void) { }
static void Sounds_Stop(void) { }
static void Sounds_Start(void) {
	Chat_AddRaw("&cSounds are not supported currently");
	Audio_SoundsVolume = 0;
}

void Audio_PlayDigSound(cc_uint8 type)  { }
void Audio_PlayStepSound(cc_uint8 type) { }

void Sounds_LoadDefault(void) { }
#else
struct Soundboard digBoard, stepBoard;
static RNGState sounds_rnd;
/* --------------------
 * Per-zip custom mapping: filename (no ext) -> id
 * -------------------- */
struct CustomSoundMap {
    cc_string name;   /* e.g. "dig_testsound1" */
    cc_uint16  id;    /* mapped group id, e.g. 12 */
};

struct CustomSoundMapList {
    struct CustomSoundMap* items;
    int count, capacity;
};

static struct CustomSoundMapList customMaps_global; /* used while extracting a zip */
/* Custom map helpers */
static void CustomMap_Init(struct CustomSoundMapList* list) {
    list->items = NULL; list->count = list->capacity = 0;
}
static void CustomMap_Free(struct CustomSoundMapList* list) {
    int i;
    for (i = 0; i < list->count; i++) {
        String_Free(&list->items[i].name);
    }
    Mem_Free(list->items);
    list->items = NULL; list->count = list->capacity = 0;
}
static void CustomMap_Add(struct CustomSoundMapList* list, const cc_string* name, cc_uint16 id) {
    if (list->count == list->capacity) {
        int newCap = list->capacity ? list->capacity * 2 : 8;
        list->items = (struct CustomSoundMap*)Mem_TryRealloc(list->items, newCap * sizeof(*list->items));
        if (!list->items) return;
        list->capacity = newCap;
    }
    list->items[list->count].name = String_InitArray(0);
    String_CopyTo(&list->items[list->count].name, name);
    list->items[list->count].id = id;
    list->count++;
}
static int CustomMap_Find(const struct CustomSoundMapList* list, const cc_string* name) {
    int i;
    for (i = 0; i < list->count; i++) {
        if (String_CaselessEquals(&list->items[i].name, name)) return i;
    }
    return -1;
}

#define WAV_FourCC(a, b, c, d) (((cc_uint32)a << 24) | ((cc_uint32)b << 16) | ((cc_uint32)c << 8) | (cc_uint32)d)
#define WAV_FMT_SIZE 16

static cc_result Sound_ReadWaveData(struct Stream* stream, struct Sound* snd) {
	cc_uint32 fourCC, size;
	cc_uint8 tmp[WAV_FMT_SIZE];
	cc_result res;
	int bitsPerSample;

	if ((res = Stream_Read(stream, tmp, 12)))  return res;
	fourCC = Stream_GetU32_BE(tmp + 0);
	if (fourCC == WAV_FourCC('I','D','3', 2))  return AUDIO_ERR_MP3_SIG; /* ID3 v2.2 tags header */
	if (fourCC == WAV_FourCC('I','D','3', 3))  return AUDIO_ERR_MP3_SIG; /* ID3 v2.3 tags header */
	if (fourCC != WAV_FourCC('R','I','F','F')) return WAV_ERR_STREAM_HDR;

	/* tmp[4] (4) file size */
	fourCC = Stream_GetU32_BE(tmp + 8);
	if (fourCC != WAV_FourCC('W','A','V','E')) return WAV_ERR_STREAM_TYPE;

	for (;;) {
		if ((res = Stream_Read(stream, tmp, 8))) return res;
		fourCC = Stream_GetU32_BE(tmp + 0);
		size   = Stream_GetU32_LE(tmp + 4);

		if (fourCC == WAV_FourCC('f','m','t',' ')) {
			if ((res = Stream_Read(stream, tmp, sizeof(tmp)))) return res;
			if (Stream_GetU16_LE(tmp + 0) != 1) return WAV_ERR_DATA_TYPE;

			snd->channels   = Stream_GetU16_LE(tmp + 2);
			snd->sampleRate = Stream_GetU32_LE(tmp + 4);
			/* tmp[8] (6) alignment data and stuff */

			bitsPerSample = Stream_GetU16_LE(tmp + 14);
			if (bitsPerSample != 16) return WAV_ERR_SAMPLE_BITS;
			size -= WAV_FMT_SIZE;
		} else if (fourCC == WAV_FourCC('d','a','t','a')) {
			if ((res = Audio_AllocChunks(size, &snd->chunk, 1))) return res;
			res = Stream_Read(stream, (cc_uint8*)snd->chunk.data, size);

			#ifdef CC_BUILD_BIGENDIAN
			Utils_SwapEndian16((cc_int16*)snd->chunk.data, size / 2);
			#endif
			return res;
		}

		/* Skip over unhandled data */
		if (size && (res = stream->Skip(stream, size))) return res;
	}
}

static struct SoundGroup* Soundboard_FindGroup(struct Soundboard* board, const cc_string* name) {
	struct SoundGroup* groups = board->groups;
	int i;

	for (i = 0; i < SOUND_COUNT; i++) 
	{
		if (String_CaselessEqualsConst(name, Sound_Names[i])) return &groups[i];
	}
	return NULL;
}

static void Soundboard_Load(struct Soundboard* board, const cc_string* boardName, const cc_string* file, struct Stream* stream) {
	struct SoundGroup* group;
	struct Sound* snd;
	cc_string name = *file;
	cc_result res;
	int dotIndex;
	Utils_UNSAFE_TrimFirstDirectory(&name);

	/* dig_grass1.wav -> dig_grass1 */
	dotIndex = String_LastIndexOf(&name, '.');
	if (dotIndex >= 0) name.length = dotIndex;
	if (!String_CaselessStarts(&name, boardName)) return;

	/* Convert dig_grass1 to grass */
/* Convert dig_grass1 to grass  OR dig_pling to pling (do not blindly chop last char) */
name = String_UNSAFE_SubstringAt(&name, boardName->length);

/* strip trailing digit ONLY if present: dig_pling  -> pling ; dig_pling1 -> pling */
if (name.length > 0) {
    char last = name.buffer[name.length - 1];
    if (last >= '0' && last <= '9') {
        name = String_UNSAFE_Substring(&name, 0, name.length - 1);
    }
}

	group = Soundboard_FindGroup(board, &name);
	if (!group) {
		Chat_Add1("&cUnknown sound group '%s'", &name); return;
	}
	if (group->count == Array_Elems(group->sounds)) {
		Chat_AddRaw("&cCannot have more than 10 sounds in a group"); return;
	}

	snd = &group->sounds[group->count];
	res = Sound_ReadWaveData(stream, snd);

	if (res) {
		Logger_SysWarn2(res, "decoding", file);
		Audio_FreeChunks(&snd->chunk, 1);
		snd->chunk.data = NULL;
		snd->chunk.size = 0;
	} else { group->count++; }
}
/* Load the given zip entry directly into a specific group id (bypass name lookup) */
static void Soundboard_LoadMapped(struct Soundboard* board, const cc_string* file, cc_uint16 targetGroup, struct Stream* stream) {
    struct SoundGroup* group;
    struct Sound* snd;
    cc_string name = *file;
    cc_result res;
    int dotIndex;

    Utils_UNSAFE_TrimFirstDirectory(&name);

    /* remove extension */
    dotIndex = String_LastIndexOf(&name, '.');
    if (dotIndex >= 0) name.length = dotIndex;

    if (targetGroup >= SOUND_COUNT) {
        Chat_Add1("&cCustom sound mapping %s references invalid id %i", &name, (int)targetGroup);
        return;
    }

    group = &board->groups[targetGroup];
    if (group->count == Array_Elems(group->sounds)) {
        Chat_Add1("&cCustom sound mapping %s cannot add - group full", &name);
        return;
    }

    snd = &group->sounds[group->count];
    res = Sound_ReadWaveData(stream, snd);

    if (res) {
        Logger_SysWarn2(res, "decoding", &name);
        Audio_FreeChunks(&snd->chunk, 1);
        snd->chunk.data = NULL;
        snd->chunk.size = 0;
    } else { group->count++; }
}

static const struct Sound* Soundboard_PickRandom(struct Soundboard* board, cc_uint8 type) {
	struct SoundGroup* group;
	int idx;

	if (type == SOUND_NONE || type >= SOUND_COUNT) return NULL;
	if (type == SOUND_METAL) type = SOUND_STONE;

	group = &board->groups[type];
	if (!group->count) return NULL;

	idx = Random_Next(&sounds_rnd, group->count);
	return &group->sounds[idx];
}


CC_NOINLINE static void Sounds_Fail(cc_result res) {
	Audio_Warn(res, "playing sounds");
	Chat_AddRaw("&cDisabling sounds");
	Audio_SetSounds(0);
}

static void Sounds_Play(cc_uint8 type, struct Soundboard* board) {
	cc_uint32 rate = 100;
	cc_uint32 volume = Audio_SoundsVolume;

	/* https://minecraft.wiki/w/Block_of_Gold#Sounds */
	/* https://minecraft.wiki/w/Grass#Sounds */
	if (board == &digBoard) {
		if (type == SOUND_METAL) rate = 120;
		else rate = 80;
	}
	else {
		volume /= 2;
		if (type == SOUND_METAL) rate = 140;
	}
	Sounds_PlayAdvanced(type, board, volume, rate);
}

void Sounds_PlayAdvanced(cc_uint8 type, struct Soundboard* board, cc_uint32 volume, cc_int32 rate) {

	const struct Sound* snd;
	struct AudioData data;
	cc_result res;

	if (type == SOUND_NONE || !volume) return;
	snd = Soundboard_PickRandom(board, type);
	if (!snd) return;

	data.chunk = snd->chunk;
	data.channels = snd->channels;
	data.sampleRate = snd->sampleRate;
	data.rate = rate;
	data.volume = volume;

	res = AudioPool_Play(&data);
	if (res) Sounds_Fail(res);
}
/* ---------------------------------------------------------------------------
 * Helpers used by CPE / Protocol.c to play arbitrary sound IDs
 * --------------------------------------------------------------------------*/
void Audio_PlayCustom2D(cc_uint8 channel, cc_uint16 id, cc_uint32 volume, cc_uint8 rate) {
    /* Skip if volume is zero or sounds disabled */
    if (!volume) return;
    if (!Audio_SoundsVolume) return;

    /* Avoid out-of-range IDs for step/dig channels */
    if (channel <= 1 && id >= SOUND_COUNT) return;

    /* Calculate scaled volume (plugin uses 0-255, where 255 means "full") */
    cc_uint32 volume_calculated = (volume == 255) ? Audio_SoundsVolume :
        (cc_uint32)((float)Audio_SoundsVolume * (float)volume / 255.0f);

    /* Route to appropriate board */
    if (channel == 0) { /* dig */
        Sounds_PlayAdvanced((cc_uint8)id, &digBoard, volume_calculated, rate);
    } else if (channel == 1) { /* step */
        Sounds_PlayAdvanced((cc_uint8)id, &stepBoard, volume_calculated, rate);
    }
}

void Audio_PlayCustom3D(cc_uint8 channel, cc_uint16 id, cc_uint32 volume, cc_uint8 rate,
                        cc_uint16 pos_x, cc_uint16 pos_y, cc_uint16 pos_z) {
    /* 3D audio not implemented uniformly across platforms; fallback to 2D */
    /* You can extend this to do true 3D panning if you have a 3D backend. */
    (void)pos_x; (void)pos_y; (void)pos_z;
    Audio_PlayCustom2D(channel, id, volume, rate);
}

static void Audio_PlayBlockSound(void* obj, IVec3 coords, BlockID old, BlockID now) {
	if (now == BLOCK_AIR) {
		Audio_PlayDigSound(Blocks.DigSounds[old]);
	} else if (!Game_ClassicMode) {
		/* use StepSounds instead when placing, as don't want */
		/*  to play glass break sound when placing glass */
		Audio_PlayDigSound(Blocks.StepSounds[now]);
	}
}

static cc_bool SelectZipEntry(const cc_string* path) { return true; }
/* Parse a simple sounds.txt stream into customMaps_global.
   Format: lines like "dig_pling1 = 10" (# comments ignored). */
/* Robust parsing of sounds.txt: read bytes until EOF, build temporary buffer, parse lines.
   This reads one byte at a time to avoid relying on partial-read semantics of Stream_Read.
   sounds.txt is expected to be tiny, so bytewise reads are acceptable. */
static void ParseSoundsTxtStream(struct Stream* stream, struct CustomSoundMapList* maps) {
    /* dynamic byte buffer */
    unsigned char* buf = NULL;
    int bufLen = 0, bufCap = 0;
    cc_result res;
    cc_uint8 ch;

    for (;;) {
        res = Stream_Read(stream, &ch, 1);
        if (res) {
            if (res == ERR_END_OF_STREAM) break;
            Logger_SysWarn2(res, "reading sounds.txt");
            break;
        }
        /* append ch */
        if (bufLen + 1 > bufCap) {
            int newCap = bufCap ? bufCap * 2 : 256;
            unsigned char* ptr = (unsigned char*)Mem_TryRealloc(buf, newCap);
            if (!ptr) {
                Mem_Free(buf);
                return;
            }
            buf = ptr; bufCap = newCap;
        }
        buf[bufLen++] = ch;
    }

    if (!bufLen) { Mem_Free(buf); return; }

    /* parse lines: operate on buf[0..bufLen-1] */
    int i = 0;
    while (i < bufLen) {
        int j = i;
        /* find end of line */
        while (j < bufLen && buf[j] != '\n' && buf[j] != '\r') j++;
        if (j > i) {
            /* create cc_string for the line */
            /* NOTE: if your repo lacks String_UNSAFE_Create, see compile notes below. */
            cc_string line = String_UNSAFE_Create((char*)&buf[i], j - i);
            Utils_UNSAFE_Trim(&line);
            if (line.length && line.buffer[0] != '#' && line.buffer[0] != '/') {
                int eq = String_IndexOf(&line, '=');
                if (eq >= 0) {
                    cc_string key = String_UNSAFE_Substring(&line, 0, eq);
                    cc_string val = String_UNSAFE_SubstringAt(&line, eq + 1);
                    Utils_UNSAFE_Trim(&key); Utils_UNSAFE_Trim(&val);
                    if (key.length && val.length) {
                        int id = String_ToInt(&val);
                        if (id > 0 && id < 0x10000) CustomMap_Add(maps, &key, (cc_uint16)id);
                    }
                    String_FreeArray(&key);
                    String_FreeArray(&val);
                }
            }
            String_FreeArray(&line);
        }
        /* skip newline(s) */
        while (j < bufLen && (buf[j] == '\n' || buf[j] == '\r')) j++;
        i = j;
    }

    Mem_Free(buf);
}


static cc_result ProcessZipEntry(const cc_string* path, struct Stream* stream, struct ZipEntry* source) {
    static const cc_string dig  = String_FromConst("dig_");
    static const cc_string step = String_FromConst("step_");

    /* Determine filename without directory */
    cc_string name = *path;
    Utils_UNSAFE_TrimFirstDirectory(&name);
    /* get basename without extension */
    int dotIndex = String_LastIndexOf(&name, '.');
    if (dotIndex >= 0) name.length = dotIndex;

    /* If this is the sounds mapping file (sounds.txt), parse it first */
    if (String_CaselessEqualsConst(&name, "sounds") || String_CaselessEqualsConst(&name, "sounds.txt")) {
        ParseSoundsTxtStream(stream, &customMaps_global);
        return 0;
    }

    /* If custom map contains this filename, use mapped id */
    int idx = CustomMap_Find(&customMaps_global, &name);
    if (idx >= 0) {
        cc_uint16 mapped = customMaps_global.items[idx].id;
        if (mapped < SOUND_COUNT) {
            /* pick which board by prefix on original path */
            if (String_CaselessStarts(path, &dig)) {
                Soundboard_LoadMapped(&digBoard, path, mapped, stream);
            } else if (String_CaselessStarts(path, &step)) {
                Soundboard_LoadMapped(&stepBoard, path, mapped, stream);
            } else {
                Chat_Add1("&cCustom sound file %s not prefixed with dig_/step_", path);
            }
        } else {
            Chat_Add1("&cCustom sound mapping for %s uses id %i (out of range)", path, mapped);
        }
        return 0;
    }

    /* Fallback: old behaviour (improved trimming is already in Soundboard_Load) */
    Soundboard_Load(&digBoard,  &dig,  path, stream);
    Soundboard_Load(&stepBoard, &step, path, stream);
    return 0;
}


static cc_result Sounds_ExtractZip(const cc_string* path) {
	struct ZipEntry entries[128];
	struct Stream stream;
	cc_result res;

res = Stream_OpenFile(&stream, path);
if (res) { Logger_SysWarn2(res, "opening", path); return res; }

CustomMap_Init(&customMaps_global);

res = Zip_Extract(&stream, SelectZipEntry, ProcessZipEntry,
                    entries, Array_Elems(entries));
if (res) Logger_SysWarn2(res, "extracting", path);

/* free custom map state for this zip */
CustomMap_Free(&customMaps_global);

/* No point logging error for closing readonly file */
(void)stream.Close(&stream);
return res;

}

void Sounds_LoadDefault(void) {
	cc_result res = Sounds_ExtractZip(&Sounds_ZipPathMC);
	if (res == ReturnCode_FileNotFound)
		Sounds_ExtractZip(&Sounds_ZipPathCC);
}

static cc_bool sounds_loaded;
static void Sounds_Start(void) {
	if (!AudioBackend_Init()) { 
		AudioBackend_Free(); 
		Audio_SoundsVolume = 0; 
		return; 
	}

	if (sounds_loaded) return;
	sounds_loaded = true;
	AudioBackend_LoadSounds();
}

static void Sounds_Stop(void) { AudioPool_Close(); }

static void Sounds_Init(void) {
	int volume = Options_GetInt(OPT_SOUND_VOLUME, 0, 100, DEFAULT_SOUNDS_VOLUME);
	Audio_SetSounds(volume);
	Event_Register_(&UserEvents.BlockChanged, NULL, Audio_PlayBlockSound);
}
static void Sounds_Free(void) { Sounds_Stop(); }

void Audio_PlayDigSound(cc_uint8 type)  { Sounds_Play(type, &digBoard); }
void Audio_PlayStepSound(cc_uint8 type) { Sounds_Play(type, &stepBoard); }
#endif


/*########################################################################################################################*
*--------------------------------------------------------Music------------------------------------------------------------*
*#########################################################################################################################*/
#ifdef CC_BUILD_NOMUSIC
/* Can't use mojang's music assets, so just stub everything out */
static void Music_Init(void) { }
static void Music_Free(void) { }
static void Music_Stop(void) { }
static void Music_Start(void) {
	Chat_AddRaw("&cMusic is not supported currently");
	Audio_MusicVolume = 0;
}
#else
static void* music_thread;
static void* music_waitable;
static volatile cc_bool music_stopping, music_joining;
static int music_minDelay, music_maxDelay;

static cc_result Music_Buffer(struct AudioChunk* chunk, int maxSamples, struct VorbisState* ctx) {
	int samples = 0;
	cc_int16* cur;
	cc_result res = 0, res2;
	cc_int16* data = (cc_int16*)chunk->data;

	while (samples < maxSamples) {
		if ((res = Vorbis_DecodeFrame(ctx))) break;

		cur = &data[samples];
		samples += Vorbis_OutputFrame(ctx, cur);
	}

	chunk->size = samples * 2;
	res2 = StreamContext_Enqueue(&music_ctx, chunk);
	if (res2) { music_stopping = true; return res2; }
	return res;
}

static cc_result Music_PlayOgg(struct Stream* source) {
	int channels, sampleRate, volume;
	int chunkSize, samplesPerSecond;
	struct AudioChunk chunks[AUDIO_MAX_BUFFERS] = { 0 };
	int inUse, i, cur;
	cc_result res;

#if CC_BUILD_MAXSTACK <= (64 * 1024)
	struct VorbisState* vorbis = (struct VorbisState*)Mem_TryAllocCleared(1, sizeof(struct VorbisState));
	struct OggState* ogg = (struct OggState*)Mem_TryAllocCleared(1, sizeof(struct OggState));
	if (!vorbis || !ogg) return ERR_OUT_OF_MEMORY;
#else
	struct OggState _ogg;
	struct OggState* ogg = &_ogg;
	struct VorbisState _vorbis;
	struct VorbisState* vorbis = &_vorbis;
#endif

	Ogg_Init(ogg, source);
	Vorbis_Init(vorbis);
	vorbis->source = ogg;
	if ((res = Vorbis_DecodeHeaders(vorbis))) goto cleanup;
	
	channels   = vorbis->channels;
	sampleRate = vorbis->sampleRate;
	if ((res = StreamContext_SetFormat(&music_ctx, channels, sampleRate, 100))) goto cleanup;

	/* largest possible vorbis frame decodes to blocksize1 * channels samples, */
	/*  so can end up decoding slightly over a second of audio */
	chunkSize        = channels * (sampleRate + vorbis->blockSizes[1]);
	samplesPerSecond = channels * sampleRate;

	if ((res = Audio_AllocChunks(chunkSize * 2, chunks, AUDIO_MAX_BUFFERS))) goto cleanup;
    volume = Audio_MusicVolume;
    Audio_SetVolume(&music_ctx, volume);	

	/* fill up with some samples before playing */
	for (i = 0; i < AUDIO_MAX_BUFFERS && !res; i++) 
	{
		res = Music_Buffer(&chunks[i], samplesPerSecond, vorbis);
	}
	if (music_stopping) goto cleanup;

	res  = StreamContext_Play(&music_ctx);
	if (res) goto cleanup;
	cur  = 0;

	while (!music_stopping) {
#ifdef CC_BUILD_MOBILE
		/* Don't play music while in the background on Android */
    	/* TODO: Not use such a terrible approach */
    	if (!Window_Main.Handle.ptr || Window_Main.Inactive) {
    		StreamContext_Pause(&music_ctx);
    		while ((!Window_Main.Handle.ptr || Window_Main.Inactive) && !music_stopping) {
    			Thread_Sleep(10); continue;
    		}
    		StreamContext_Play(&music_ctx);
    	}
#endif
        if (volume != Audio_MusicVolume) {
            volume = Audio_MusicVolume;
            Audio_SetVolume(&music_ctx, volume);
        }

		res = StreamContext_Update(&music_ctx, &inUse);
		if (res) { music_stopping = true; break; }

		if (inUse >= AUDIO_MAX_BUFFERS) {
			Thread_Sleep(10); continue;
		}

		res = Music_Buffer(&chunks[cur], samplesPerSecond, vorbis);
		cur = (cur + 1) % AUDIO_MAX_BUFFERS;

		/* need to specially handle last bit of audio */
		if (res) break;
	}

	if (music_stopping) {
		/* must close audio context, as otherwise some of the audio */
		/*  context's internal audio buffers may have a reference */
		/*  to the `data` buffer which will be freed after this */
		Audio_Close(&music_ctx);
	} else {
		/* Wait until the buffers finished playing */
		for (;;) {
			if (StreamContext_Update(&music_ctx, &inUse) || inUse == 0) break;
			Thread_Sleep(10);
		}
	}

cleanup:
	Audio_FreeChunks(chunks, AUDIO_MAX_BUFFERS);
	Vorbis_Free(vorbis);
#if CC_BUILD_MAXSTACK <= (64 * 1024)
	Mem_Free(ogg);
	Mem_Free(vorbis);
#endif
	return res == ERR_END_OF_STREAM ? 0 : res;
}

static void Music_AddFile(const cc_string* path, void* obj, int isDirectory) {
	struct StringsBuffer* files = (struct StringsBuffer*)obj;
	static const cc_string ogg  = String_FromConst(".ogg");

	if (isDirectory) {
		Directory_Enum(path, obj, Music_AddFile);
	} else if (String_CaselessEnds(path, &ogg)) {
		StringsBuffer_Add(files, path);
	}
}

static void Music_RunLoop(void) {
	struct StringsBuffer files;
	cc_string path;
	RNGState rnd;
	struct Stream stream;
	int idx, delay;
	cc_result res = 0;

	StringsBuffer_SetLengthBits(&files, STRINGSBUFFER_DEF_LEN_SHIFT);
	StringsBuffer_Init(&files);
	Directory_Enum(&audio_dir, &files, Music_AddFile);

	Random_SeedFromCurrentTime(&rnd);
	res = Audio_Init(&music_ctx, AUDIO_MAX_BUFFERS);
	if (res) music_stopping = true;

	while (!music_stopping && files.count) {
		idx  = Random_Next(&rnd, files.count);
		path = StringsBuffer_UNSAFE_Get(&files, idx);
		Platform_Log1("playing music file: %s", &path);

		res = Stream_OpenFile(&stream, &path);
		if (res) { Logger_SysWarn2(res, "opening", &path); break; }

		res = Music_PlayOgg(&stream);
		if (res) { Logger_SimpleWarn2(res, "playing", &path); }

		/* No point logging error for closing readonly file */
		(void)stream.Close(&stream);

		if (music_stopping) break;
		delay = Random_Range(&rnd, music_minDelay, music_maxDelay);
		Waitable_WaitFor(music_waitable, delay);
	}

	if (res) {
		Chat_AddRaw("&cDisabling music");
		Audio_MusicVolume = 0;
	}
	Audio_Close(&music_ctx);
	StringsBuffer_Clear(&files);

	if (music_joining) return;
	Thread_Detach(music_thread);
	music_thread = NULL;
}

static void Music_Start(void) {
	if (music_thread) return;
	if (!AudioBackend_Init()) {
		AudioBackend_Free(); 
		Audio_MusicVolume = 0;
		return; 
	}

	music_joining  = false;
	music_stopping = false;

	Thread_Run(&music_thread, Music_RunLoop, 256 * 1024, "Music");
}

static void Music_Stop(void) {
	music_joining  = true;
	music_stopping = true;
	Waitable_Signal(music_waitable);
	
	if (music_thread) Thread_Join(music_thread);
	music_thread = NULL;
}

static void Music_Init(void) {
	int volume;
	/* music is delayed between 2 - 7 minutes by default */
	music_minDelay = Options_GetInt(OPT_MIN_MUSIC_DELAY, 0, 3600, 120) * MILLIS_PER_SEC;
	music_maxDelay = Options_GetInt(OPT_MAX_MUSIC_DELAY, 0, 3600, 420) * MILLIS_PER_SEC;
	music_waitable = Waitable_Create("Music sleep");

	volume = Options_GetInt(OPT_MUSIC_VOLUME, 0, 100, DEFAULT_MUSIC_VOLUME);
	Audio_SetMusic(volume);
}

static void Music_Free(void) {
	Music_Stop();
	Waitable_Free(music_waitable);
}
#endif


/*########################################################################################################################*
*--------------------------------------------------------General----------------------------------------------------------*
*#########################################################################################################################*/
void Audio_SetSounds(int volume) {
	Audio_SoundsVolume = volume;
	if (volume) Sounds_Start();
	else        Sounds_Stop();
}

void Audio_SetMusic(int volume) {
	Audio_MusicVolume = volume;
	if (volume) Music_Start();
	else        Music_Stop();
}

static void OnInit(void) {
	Sounds_Init();
	Music_Init();
}

static void OnFree(void) {
	Sounds_Free();
	Music_Free();
	AudioBackend_Free();
}

struct IGameComponent Audio_Component = {
	OnInit, /* Init  */
	OnFree  /* Free  */
};

