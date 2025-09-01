/* Replace the existing TexPackOverlay_YesClick function body in src/Menus.c with this code. */
/* This will trigger the soundpack overlay immediately after the texture overlay finishes. */

static void TexPackOverlay_YesClick(void* screen, void* widget) {
    struct TexPackOverlay* s = (struct TexPackOverlay*)screen;
    TexturePack_Extract(&s->url);
    if (TexPackOverlay_IsAlways(s, widget)) TextureUrls_Accept(&s->url);
    Gui_Remove((struct Screen*)s);

#ifdef CC_BUILD_SOUNDPACK
    if (SoundPack_Url.length) {
        if (!SoundUrls_HasDenied(&SoundPack_Url) && !SoundUrls_HasAccepted(&SoundPack_Url)) {
            SoundPackOverlay_Show(&SoundPack_Url);
        } else if (SoundUrls_HasAccepted(&SoundPack_Url)) {
            SoundPack_Extract(&SoundPack_Url);
        }
    }
#endif
}
