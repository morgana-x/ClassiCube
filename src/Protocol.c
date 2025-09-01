/* Modify PlaySound handling in src/Protocol.c to route ids >= 100 to custom playback.
   Replace the existing call where sounds are played with the snippet below. */

/* Example replacement: */
if (id >= 100) {
    Audio_PlayCustom2D(channel, id, volume, rate);
} else {
    CPE_PlaySoundBase(channel, volume, rate, id);
}

/* Do the same for 3D sound packets, routing id >= 100 to Audio_PlayCustom3D(...) */
