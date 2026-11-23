/*
 * snd_kg.c -- Quake sound DMA of the karabas-go core (karabas: replaces snd_pocket.c / audio_timer.c of
 * PocketQuake).
 *
 * The engine mixes 11025 Hz 16-bit stereo into snd_buffer (a ring of SFX_RING frames); Submit copies
 * the newly painted frames into the SFX ring of doom_audio (MMIO_SFX, { right16, left16 }), whose play
 * position (MMIO_SFX_POS) is the DMA position. Same ring and rate as the Doom / Hexen cores. kg_snd_clear:
 * the ring to silence before a load / save (no rattle of the looped leftovers).
 */

#include "quakedef.h"
#undef SDRAM_BASE
#include "mmio.h"   /* src/fw (-I) */

static short snd_buffer[SFX_RING * 2] __attribute__((aligned(4)));
static int sent;                 /* frames copied to the MMIO ring (paintedtime units) */

/* karabas Ф7: silence in the whole SFX ring of doom_audio. The ring keeps playing in a loop while the game does
   not submit (a map load, a save to the SD card): its leftovers rattled in the speakers. Called from
   S_ClearBuffer (snd_dma.c, a sed in the Makefile), SNDDMA_Init and kg_qsave.c before a write. */
void kg_snd_clear(void)
{
    extern int paintedtime;
    int i;
    for (i = 0; i < SFX_RING; i++)
        MMIO_SFX(i) = 0;
    sent = paintedtime;
}

/* PocketQuake: the audio timer interrupt (audio_timer.c) fills its ring; none here */
int audio_timer_active = 0;
void Audio_TimerStart(void) {}

qboolean SNDDMA_Init(void)
{
    int i;
    shm = &sn;
    shm->channels = 2;
    shm->samplebits = 16;
    shm->speed = SFX_RATE;
    shm->samples = SFX_RING * 2;   /* mono samples in the buffer */
    shm->submission_chunk = 1;
    shm->samplepos = 0;
    shm->buffer = (unsigned char *)snd_buffer;
    shm->soundalive = true;
    shm->gamealive = true;
    shm->splitbuffer = false;
    for (i = 0; i < SFX_RING * 2; i++)
        snd_buffer[i] = 0;
    sent = 0;
    kg_snd_clear();   /* the ring may still hold the sound of the core loaded before */
    return true;
}

int SNDDMA_GetDMAPos(void)
{
    shm->samplepos = (int)(MMIO_SFX_POS & (SFX_RING - 1)) * 2;
    return shm->samplepos;
}

void SNDDMA_FillRing(void)
{
}

void SNDDMA_Submit(void)
{
    extern int paintedtime;
    if (paintedtime - sent > SFX_RING)
        sent = paintedtime - SFX_RING;
    while (sent < paintedtime) {
        int i = sent & (SFX_RING - 1);
        MMIO_SFX(i) = ((unsigned int)(unsigned short)snd_buffer[2 * i + 1] << 16) |
                      (unsigned short)snd_buffer[2 * i];
        sent++;
    }
}

void SNDDMA_Shutdown(void)
{
}
