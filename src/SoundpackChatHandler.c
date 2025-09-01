/* Optional: add this snippet to your chat/packet handler so the client can respond to
   a server broadcast in the format "SOUNDPACK:<url>" and automatically call Server_RetrieveSoundPack(). */

static void HandleServerChatMessage(const char* text) {
    const char* prefix = "SOUNDPACK:";
    size_t plen = strlen(prefix);
    if (strncmp(text, prefix, plen) == 0) {
        const char* url = text + plen;
        cc_string s; String_InitArray(s, NULL);
        String_CopyCString(&s, url);
        Server_RetrieveSoundPack(&s);
        String_Free(&s);
        return;
    }
    /* existing chat handling continues... */
}
