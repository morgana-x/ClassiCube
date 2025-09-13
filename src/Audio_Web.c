#include "Core.h"

#if defined CC_BUILD_WEBAUDIO
struct AudioContext { int contextID, count; void* data; };

#define AUDIO_OVERRIDE_SOUNDS
#define AUDIO_OVERRIDE_ALLOC
#include "_AudioBase.h"
#include "Funcs.h"

extern int  interop_InitAudio(void);
extern int  interop_AudioCreate(void);
extern void interop_AudioClose(int contextID);
extern int  interop_AudioPlay(int contextID, const void* name, int rate);
extern int  interop_AudioPoll(int contextID, int* inUse);
extern int  interop_AudioVolume(int contextID, int volume);
extern int  interop_AudioDescribe(int res, char* buffer, int bufferLen);

cc_bool AudioBackend_Init(void) {
	cc_result res = interop_InitAudio();
	if (res) { Audio_Warn(res, "initing WebAudio context"); return false; }
	return true;
}

void AudioBackend_Tick(void) { }
void AudioBackend_Free(void) { }

cc_result Audio_Init(struct AudioContext* ctx, int buffers) {
	ctx->count     = buffers;
	ctx->contextID = interop_AudioCreate();
	ctx->data      = NULL;
	return 0;
}

void Audio_Close(struct AudioContext* ctx) {
	if (ctx->contextID) interop_AudioClose(ctx->contextID);
	ctx->contextID = 0;
	ctx->count     = 0;
}

void Audio_SetVolume(struct AudioContext* ctx, int volume) {
	interop_AudioVolume(ctx->contextID, volume);
}


/*########################################################################################################################*
*------------------------------------------------------Stream context-----------------------------------------------------*
*#########################################################################################################################*/
cc_result StreamContext_SetFormat(struct AudioContext* ctx, int channels, int sampleRate, int playbackRate) {
	return ERR_NOT_SUPPORTED;
}

cc_result StreamContext_Enqueue(struct AudioContext* ctx, struct AudioChunk* chunk) {
	return ERR_NOT_SUPPORTED;
}

cc_result StreamContext_Play(struct AudioContext* ctx) {
	return ERR_NOT_SUPPORTED;
}

cc_result StreamContext_Pause(struct AudioContext* ctx) {
	return ERR_NOT_SUPPORTED;
}

cc_result StreamContext_Update(struct AudioContext* ctx, int* inUse) {
	return ERR_NOT_SUPPORTED;
}


/*########################################################################################################################*
*------------------------------------------------------Sound context------------------------------------------------------*
*#########################################################################################################################*/
cc_bool SoundContext_FastPlay(struct AudioContext* ctx, struct AudioData* data) {
	/* Channels/Sample rate is per buffer, not a per source property */
	return true;
}

cc_result SoundContext_PlayData(struct AudioContext* ctx, struct AudioData* data) {
	return interop_AudioPlay(ctx->contextID, data->chunk.data, data->rate);
}

cc_result SoundContext_PollBusy(struct AudioContext* ctx, cc_bool* isBusy) {
	int inUse = 1;
	cc_result res;
	if ((res = interop_AudioPoll(ctx->contextID, &inUse))) return res;

	*isBusy = inUse > 0;
	return 0;
}


/*########################################################################################################################*
*--------------------------------------------------------Audio misc-------------------------------------------------------*
*#########################################################################################################################*/
cc_bool Audio_DescribeError(cc_result res, cc_string* dst) {
	char buffer[NATIVE_STR_LEN];
	int len = interop_AudioDescribe(res, buffer, NATIVE_STR_LEN);

	String_AppendUtf8(dst, buffer, len);
	return len > 0;
}


/*########################################################################################################################*
*----------------------------------------------------------Sounds---------------------------------------------------------*
*#########################################################################################################################*/
static const struct SoundID { int group; const char* name; } sounds_list[] =
{
	{ SOUND_CLOTH,  "step_cloth1"  }, { SOUND_CLOTH,  "step_cloth2"  }, { SOUND_CLOTH,  "step_cloth3"  }, { SOUND_CLOTH,  "step_cloth4"  },
	{ SOUND_GRASS,  "step_grass1"  }, { SOUND_GRASS,  "step_grass2"  }, { SOUND_GRASS,  "step_grass3"  }, { SOUND_GRASS,  "step_grass4"  },
	{ SOUND_GRAVEL, "step_gravel1" }, { SOUND_GRAVEL, "step_gravel2" }, { SOUND_GRAVEL, "step_gravel3" }, { SOUND_GRAVEL, "step_gravel4" },
	{ SOUND_SAND,   "step_sand1"   }, { SOUND_SAND,   "step_sand2"   }, { SOUND_SAND,   "step_sand3"   }, { SOUND_SAND,   "step_sand4"   },
	{ SOUND_SNOW,   "step_snow1"   }, { SOUND_SNOW,   "step_snow2"   }, { SOUND_SNOW,   "step_snow3"   }, { SOUND_SNOW,   "step_snow4"   },
	{ SOUND_STONE,  "step_stone1"  }, { SOUND_STONE,  "step_stone2"  }, { SOUND_STONE,  "step_stone3"  }, { SOUND_STONE,  "step_stone4"  },
	{ SOUND_WOOD,   "step_wood1"   }, { SOUND_WOOD,   "step_wood2"   }, { SOUND_WOOD,   "step_wood3"   }, { SOUND_WOOD,   "step_wood4"   },
	{ SOUND_NONE, NULL },

	{ SOUND_CLOTH,  "dig_cloth1"   }, { SOUND_CLOTH,  "dig_cloth2"   }, { SOUND_CLOTH,  "dig_cloth3"   }, { SOUND_CLOTH,  "dig_cloth4"   },
	{ SOUND_GRASS,  "dig_grass1"   }, { SOUND_GRASS,  "dig_grass2"   }, { SOUND_GRASS,  "dig_grass3"   }, { SOUND_GRASS,  "dig_grass4"   },
	{ SOUND_GLASS,  "dig_glass1"   }, { SOUND_GLASS,  "dig_glass2"   }, { SOUND_GLASS,  "dig_glass3"   },
	{ SOUND_GRAVEL, "dig_gravel1"  }, { SOUND_GRAVEL, "dig_gravel2"  }, { SOUND_GRAVEL, "dig_gravel3"  }, { SOUND_GRAVEL, "dig_gravel4"  },
	{ SOUND_SAND,   "dig_sand1"    }, { SOUND_SAND,   "dig_sand2"    }, { SOUND_SAND,   "dig_sand3"    }, { SOUND_SAND,   "dig_sand4"    },
	{ SOUND_SNOW,   "dig_snow1"    }, { SOUND_SNOW,   "dig_snow2"    }, { SOUND_SNOW,   "dig_snow3"    }, { SOUND_SNOW,   "dig_snow4"    },
	{ SOUND_STONE,  "dig_stone1"   }, { SOUND_STONE,  "dig_stone2"   }, { SOUND_STONE,  "dig_stone3"   }, { SOUND_STONE,  "dig_stone4"   },
	{ SOUND_WOOD,   "dig_wood1"    }, { SOUND_WOOD,   "dig_wood2"    }, { SOUND_WOOD,   "dig_wood3"    }, { SOUND_WOOD,   "dig_wood4"    },
    { SOUND_PLING, "step_pling" },
    { SOUND_PLING, "dig_pling" },

    { SOUND_CHALLENGE_COMPLETE, "step_challenge_complete" },
    { SOUND_CHALLENGE_COMPLETE, "dig_challenge_complete" },
	    /* ----- Custom sounds ----- */
    { SOUND_ANVIL_USE, "step_anvil_use" },
    { SOUND_ANVIL_USE, "dig_anvil_use" },

    { SOUND_CHEST_CLOSED, "step_chestclosed" },
    { SOUND_CHEST_CLOSED, "dig_chestclosed" },

    { SOUND_CHEST_OPEN, "step_chestopen" },
    { SOUND_CHEST_OPEN, "dig_chestopen" },

    { SOUND_CLICK, "step_click" },
    { SOUND_CLICK, "dig_click" },

    { SOUND_DOOR_CLOSE, "step_door_close" },
    { SOUND_DOOR_CLOSE, "dig_door_close" },

    { SOUND_DOOR_OPEN, "step_door_open" },
    { SOUND_DOOR_OPEN, "dig_door_open" },

    { SOUND_EXPLODE, "step_explode1" }, /* from explode11.wav */
    { SOUND_EXPLODE, "dig_explode1" },

    { SOUND_LEVELUP, "step_levelup" },
    { SOUND_LEVELUP, "dig_levelup" },

    { SOUND_PICKUP, "step_pickup1" },   /* from pickup11.wav */
    { SOUND_PICKUP, "dig_pickup1" },

    { SOUND_POP, "step_pop" },
    { SOUND_POP, "dig_pop" },

    { SOUND_PORTAL, "step_portal" },
    { SOUND_PORTAL, "dig_portal" },

    { SOUND_HIT, "step_hit" },
    { SOUND_HIT, "dig_hit" },

};

/* TODO this is a terrible solution */
void AudioBackend_LoadSounds(void) {
	struct Soundboard* board = &stepBoard;
	struct SoundGroup* group;
	int i;

	for (i = 0; i < Array_Elems(sounds_list); i++) {
		if (sounds_list[i].group == SOUND_NONE) {
			board = &digBoard;
		} else {
			group = &board->groups[sounds_list[i].group];
			group->sounds[group->count++].chunk.data = sounds_list[i].name;
		}
	}
}
#endif

