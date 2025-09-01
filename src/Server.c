/* Add Server_RetrieveSoundPack to src/Server.c. Also declare cc_string SoundPack_Url at top-level near TexturePack_Url. */

void Server_RetrieveSoundPack(const cc_string* url) {
    if (!Game_AllowServerSounds || SoundUrls_HasDenied(url)) return;

    if (!url->length || SoundUrls_HasAccepted(url)) {
        SoundPack_Extract(url);
    } else {
        SoundPackOverlay_Show(url);
    }
}

/* Add global declaration alongside other globals:
   cc_string SoundPack_Url;
*/
