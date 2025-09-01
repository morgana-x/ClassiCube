#ifndef CC_SOUNDPACK_H
#define CC_SOUNDPACK_H
#include "Core.h"
CC_BEGIN_HEADER

struct HttpRequest;

/* Public API to start retrieval/extraction of a soundpack URL (zip containing sounds/ids.txt + .ogg) */
void Server_RetrieveSoundPack(const cc_string* url);
void SoundPack_Extract(const cc_string* url);

/* Sound URL accept/deny (persisted) */
cc_bool SoundUrls_HasAccepted(const cc_string* url);
cc_bool SoundUrls_HasDenied(const cc_string* url);
void SoundUrls_Accept(const cc_string* url);
void SoundUrls_Deny(const cc_string* url);
int  SoundUrls_ClearDenied(void);

/* Playback API for custom sounds loaded from packs. channel is 0=break,1=step,2=music etc */
void Audio_PlayCustom2D(cc_uint8 channel, cc_uint16 id, cc_uint32 volume, cc_uint8 rate);
void Audio_PlayCustom3D(cc_uint8 channel, cc_uint16 id, cc_uint32 volume, cc_uint8 rate, cc_uint16 x, cc_uint16 y, cc_uint16 z);

/* Component registration */
extern struct IGameComponent SoundPack_Component;

CC_END_HEADER
#endif
