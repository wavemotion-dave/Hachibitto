// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave and
// Marat Fayzullin (fMSX core) are thanked profusely.
//
// The Hachibitto emulator is offered as-is, without any warranty. Please see readme.md
// =====================================================================================
#include <nds.h>
#include <nds/fifomessages.h>

#include <ctype.h>
#include <fat.h>
#include <maxmod9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "CRC32.h"
#include "Hachibitto.h"
#include "MSX_generic.h"
#include "V9938.h"
#include "alpha_kbd.h"
#include "cpu/vdp9938/vdp9938.h"
#include "debug_ovl.h"
#include "fdc.h"
#include "highscore.h"
#include "instructions.h"
#include "loading.h"
#include "mainmenu.h"
#include "minimenu.h"
#include "msx_kbd.h"
#include "msx_kbd_ja.h"
#include "msx_music.h"
#include "options.h"
#include "topscreen.h"
#include "cpu/scc/SCC.h"
#include "cpu/z80/Z80_interface.h"
#include "printf.h"
#include "screenshot.h"
#include "soundbank.h"
#include "soundbank_bin.h"

// ------------------------------------------------------------------
// These 16 debug registers plus indexes DX/DY are the lifeline for
// development of this emulator. We can use these to track all sorts
// of things going on and help to isolate and fix problems. The
// debugger can be turned on in options to see these values.
// ------------------------------------------------------------------
u32 debug[0x10] = {0};
u32 DX = 0;
u32 DY = 0;

u32 last_vsync_count    __attribute__((section(".dtcm"))) = 0xFEEDBEEF;
s8 temp_offset          __attribute__((section(".dtcm"))) = 0;
u8 slide_dampen         __attribute__((section(".dtcm"))) = 0;
u8 skip_render          __attribute__((section(".dtcm"))) = 0;
u8 zoom_screen          __attribute__((section(".dtcm"))) = 0;
u8 bClickRemoveNeeded   __attribute__((section(".dtcm"))) = 1;

// -------------------------------------------------------------------------------------------
// The MSX systems have ROM, RAM, BIOS, Music Carts or SRAM. So we create generic buffers
// for all this here... these are sized big enough to handle the largest memory necessary
// to render games playable. We give enough room here to run 98% of the MSX catalog.
//
// These memory buffers will be pointed to by the MemoryMap[] array. This array contains 8
// pointers that can break down the Z80 memory into 8k chunks.
// -------------------------------------------------------------------------------------------

u32 MAX_CART_SIZE_KB = 1024; // 1MB of ROM Cart... for DSi we will bump this up to 4MB

u8 *ROM_Memory;                         // ROM Carts up to 1MB/4MB (that's pretty huge in the Z80 world!)
u8 *DISK_Memory;                        // Disk Memory will be 720K to support DSDD disks
u8 RAM_Memory[0x40000] ALIGN(32) = {0}; // RAM is 256K for the MSX2 (this should run most game/utility software)
u8 BIOS_Memory[0x8000] ALIGN(32) = {0}; // To hold our MSX BIOS - always in the lower 32K memory region of slot 0
u8 SRAM_Memory[0x8000] ALIGN(32) = {0}; // To hold up to 32K of SRAM (FM-PAC, Game Master 2, or special SRAM capable carts)
u8 *SCC_Memory = RAM_Memory + 0x30000;  // To hold the 64K of SCC-backed RAM we simply use the last 64K of RAM (anything using SCC+ cart won't need that)
u8 *BoxArtBuffer = NULL;                // Used to store the Box Art for display (only the DSi allocates memory for this)

u8 io_show_status = 0;     // Used to indicate a disk activity icons
u8 sram_show_status = 0;   // Used to show SRAM icon for SRAM-based carts
u8 flash_show_status = 0;  // Used to show Flash (lightning bolt) icon for Flash-based carts
u8 zanac_glitch_patch = 0; // Only when Zanac EX is loaded to fix corrupt return to menu
u8 bMainMenuActive = 0;    // Set to 1 when main menu is active

Media_t MyMedia[3];        // Covering CART1, CART2 and DISK

// --------------------------------------------------------------------------
// For machines that have a full keybaord, we use the Left and Right
// shoulder buttons on the NDS to emulate the SHIFT and CTRL keys...
// --------------------------------------------------------------------------
u8 key_shift __attribute__((section(".dtcm"))) = false;
u8 key_ctrl  __attribute__((section(".dtcm"))) = false;
u8 key_kana  __attribute__((section(".dtcm"))) = false;
u8 key_graph __attribute__((section(".dtcm"))) = false;

// ---------------------------------------------------------------------------
// Some timing and frame rate computations to keep the emulation on pace...
// ---------------------------------------------------------------------------
u16 emuActFrames    __attribute__((section(".dtcm"))) = 0;  // Actual frames rendered - used for FPS calculation
u16 timingFrames    __attribute__((section(".dtcm"))) = 0;  // This one will cycle 0-59 for the 60Hz refresh
u8 render_blended   __attribute__((section(".dtcm"))) = 0;  // Set to 1 to alpha-blend using NDS hardware - useful for Screen 7 high-res
u8 soundEmuPause    __attribute__((section(".dtcm"))) = 1;  // Set to 1 to pause (mute) sound, 0 is sound unmuted (sound channels active)

u8 kbd_key          __attribute__((section(".dtcm"))) = 0;  // 0 if no key pressed, othewise the ASCII key (e.g. 'A', 'B', '3', etc)
u16 nds_key         __attribute__((section(".dtcm"))) = 0;  // 0 if no key pressed, othewise the NDS keys from keysCurrent() or similar
u8 last_mapped_key  __attribute__((section(".dtcm"))) = 0;  // The last mapped key which has been pressed - used for key click feedback
u8 kbd_keys_pressed __attribute__((section(".dtcm"))) = 0;  // Each frame we check for keys pressed - since we can map keyboard keys to the NDS, there may be several pressed at once
u8 kbd_keys[12]     __attribute__((section(".dtcm"))) = {0};// Up to 12 possible keys pressed at the same time (we have 12 NDS physical buttons though it's unlikely that more than 2 or maybe 3 would be pressed)

// ---------------------------------------------------------
// These less-used vars don't need to be in fast memory...
// ---------------------------------------------------------
u8 bStartSoundEngine = false;  // Set to true to unmute sound after 1 frame of rendering...
int bg0, bg1, bg0b, bg1b;      // Some vars for NDS background screen handling
volatile u16 vusCptVBL = 0;    // We use this as a basic timer for the random screenshots shown
volatile u32 dsVSyncCount = 0; // So compiler doesn't cache this as it changes dynamically with interrupt calls
u8 touch_debounce = 0;         // A bit of touch-screen debounce
u8 key_debounce = 0;           // A bit of key debounce
u8 msx_caps_lock = 0;          // Set to 1 when MSX caps lock is active
u8 msx_kana_lock = 0;          // Set to 1 when MSX kana lock is active

// The DS/DSi has 12 keys that can be mapped
u16 NDS_keyMap[12] __attribute__((section(".dtcm"))) = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_A, KEY_B, KEY_X, KEY_Y, KEY_R, KEY_L, KEY_START, KEY_SELECT};

// --------------------------------------------------------------------
// The key map for the MSX... mapped into the NDS controller
// --------------------------------------------------------------------
u32 keyCoresp[MAX_KEY_OPTIONS] __attribute__((section(".dtcm"))) = {
    JST_UP,
    JST_DOWN,
    JST_LEFT,
    JST_RIGHT,
    JST_FIRE1,
    JST_FIRE2,

    JST_UP << 16, // P2 versions of the above...
    JST_DOWN << 16,
    JST_LEFT << 16,
    JST_RIGHT << 16,
    JST_FIRE1 << 16,
    JST_FIRE2 << 16,

    META_KBD_A,
    META_KBD_B,
    META_KBD_C,
    META_KBD_D,
    META_KBD_E,
    META_KBD_F,
    META_KBD_G,
    META_KBD_H,
    META_KBD_I,
    META_KBD_J,
    META_KBD_K,
    META_KBD_L,
    META_KBD_M,
    META_KBD_N,
    META_KBD_O,
    META_KBD_P,
    META_KBD_Q,
    META_KBD_R,
    META_KBD_S,
    META_KBD_T,
    META_KBD_U,
    META_KBD_V,
    META_KBD_W,
    META_KBD_X,
    META_KBD_Y,
    META_KBD_Z,
    META_KBD_0,
    META_KBD_1,
    META_KBD_2,
    META_KBD_3,
    META_KBD_4,
    META_KBD_5,
    META_KBD_6,
    META_KBD_7,
    META_KBD_8,
    META_KBD_9,
    META_KBD_SHIFT,
    META_KBD_CTRL,
    META_KBD_KANA,
    META_KBD_GRAPH,
    META_KBD_SPACE,
    META_KBD_RETURN,
    META_KBD_ESC,
    META_KBD_HOME,
    META_KBD_UP,
    META_KBD_DOWN,
    META_KBD_LEFT,
    META_KBD_RIGHT,
    META_KBD_PERIOD,
    META_KBD_COMMA,
    META_KBD_COLON,
    META_KBD_SEMI,
    META_KBD_QUOTE,
    META_KBD_SLASH,
    META_KBD_CARET,
    META_KBD_MINUS,
    META_KBD_LBRACKET,
    META_KBD_RBRACKET,
    META_KBD_ATSIGN,
    META_KBD_YEN,
    META_KBD_BS,
    META_KBD_TAB,
    META_KBD_INS,
    META_KBD_DEL,
    META_KBD_STOP_BRK,
    META_KBD_F1,
    META_KBD_F2,
    META_KBD_F3,
    META_KBD_F4,
    META_KBD_F5,
    META_KBD_PANUP,
    META_KBD_PANDN,
    META_KBD_SHOWTOP,
    META_KBD_SHOWBOT,
};

static char tmpStr[64]; // For various sprintf() calls

// ------------------------------------------------------------
// Utility function to pause the sound...
// ------------------------------------------------------------
void SoundPause(void)
{
    soundEmuPause = 1;
}

// ------------------------------------------------------------
// Utility function to un pause the sound...
// ------------------------------------------------------------
void SoundUnPause(void)
{
    soundEmuPause = 0;
}

// --------------------------------------------------------------------------------------------
// MAXMOD streaming setup and handling...
// We were using the normal ARM7 sound core but it sounded "scratchy" and so with the help
// of FluBBa, we've swiched over to the maxmod sound core which performs much better.
// --------------------------------------------------------------------------------------------
#define sample_rate (31050)    // To match the sound driver - this is good enough quality for the DS
#define buffer_size (512 + 16) // Enough buffer that we don't have to fill it too often. Must be multiple of 16.

mm_ds_system sys   __attribute__((section(".dtcm")));
mm_stream myStream __attribute__((section(".dtcm")));

// -------------------------------------------------------------------------------------------
// maxmod will call this routine when the buffer is half-empty and requests that
// we fill the sound buffer with more samples. They will request 'len' samples and
// we will fill exactly that many. If the sound is paused, we fill with 'mute' samples.
// -------------------------------------------------------------------------------------------
s16 last_sample __attribute__((section(".dtcm"))) = 0;

#define WAVE_DIRECT_BUF_SIZE 2047
u16 mixer_read  __attribute__((section(".dtcm"))) = 0;
u16 mixer_write __attribute__((section(".dtcm"))) = 0;

s16 mixer[WAVE_DIRECT_BUF_SIZE + 1] __attribute__((section(".dtcm"))) = {0};
int breather    __attribute__((section(".dtcm"))) = 0;
int lastBeeper  __attribute__((section(".dtcm"))) = 0;

// -----------------------------------------------------------------------------------------------
// A simplified beeper handler. Very few MSX games use the 1-bit beeper. It's mainly "lazy"
// ZX Spectrum conversions. Almost no software designed originally for the MSX uses the beeper.
// So this does a simple job - we know how many times the beeper bit on Port C was "hit" by
// the program. We basically use that to know how often to toggle a square wave sound and mix
// it into the existing sound output buffer. It's crude but gets the job done and is good enough.
// -----------------------------------------------------------------------------------------------
void ProcessBeeper(mm_word len, mm_addr dest)
{
    s16 beeperTone = 0;

    if (beeperFreq > 32)
    {
        beeperFreq = 32;
    }
    int toggle = (len / beeperFreq);
    if (toggle)
    {
        beeperTone ^= 0x2000;
    }
    for (int i = 0; i < len; i++)
    {
        if (toggle)
        {
            if (--toggle == 0)
            {
                beeperTone ^= 0x2000;
                toggle = len / beeperFreq;
            }
        }
        ((s16 *)dest)[i] += beeperTone;
    }
    beeperFreq = 0;
}

#define SAMPLES_PER_DIRECT_CALL 2
s16 mixbufSCC[16] __attribute__((section(".dtcm"))) = {0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000};
u32 scc_sample_idx __attribute__((section(".dtcm"))) = 0;

ITCM_CODE void ProcessDirectAudio(void)
{
    u8 bMix = 0;
    s16 mix1[16];
    s16 mix2[16];

    if (scc_sample_idx & 0xF0) // To avoid call overhead, we grab 16 SCC samples at a time...
    {
        SCCMixer(16, mixbufSCC, &mySCC);
        scc_sample_idx = 0;
    }

    if (breather)
    {
        return;
    }

    ay38910Mixer(SAMPLES_PER_DIRECT_CALL, mix1, &myAY); // Always grab AY samples

    if (msx_scc_capable_game) // If SCC enabled, grab SCC samples...
    {
        mix2[0] = mixbufSCC[scc_sample_idx++];
        mix2[1] = mixbufSCC[scc_sample_idx++];
        bMix = 1;
    }
    else if (myConfig.expansion == EXP_2XPSG) // If 2xPSG enabled, grab those samples...
    {
        ay38910Mixer(SAMPLES_PER_DIRECT_CALL, mix2, &myAY2);
        mix2[0] += 32768;
        mix2[1] += 32768;
        bMix = 1;
    }
    else if (beeperFreq) // If beeper is hit and enabled, mix it in...
    {
        if (myConfig.expansion == EXP_BEEPER)
        {
            // This is mixed directly into mix1
            lastBeeper ^= 0x2000;
            beeperFreq = 0;
        }
    }

    // --------------------------------------------------------------------------
    // Now perform any mixing if needed and place samples into the output buffer.
    // --------------------------------------------------------------------------
    for (int i = 0; i < SAMPLES_PER_DIRECT_CALL; i++)
    {
        if (bMix)
        {
            mix1[i] -= mix1[i] >> 2;
            mix2[i] -= mix2[i] >> 2;
            s32 combined = mix1[i] + mix2[i];
            if (combined > 32767)
            {
                combined = 32767;
            }
            else if (combined < -32768)
            {
                combined = -32768;
            }
            mixer[mixer_write++] = (s16)combined;
        }
        else
        {
            mixer[mixer_write++] = mix1[i] + lastBeeper;
        }

        mixer_write &= WAVE_DIRECT_BUF_SIZE;
        if (((mixer_write + 1) & WAVE_DIRECT_BUF_SIZE) == mixer_read)
        {
            breather = (WAVE_DIRECT_BUF_SIZE >> 1);
            return;
        }
    }
}

// ------------------------------------------------------------------------------------------------------------
// On exit from Sound Pause and whenever the sound driver changes (AY -> AY+SCC or AY+MSX-MUSIC), there is a
// pop/click heard as the new mixer comes online. This routine is designed to perform a simple quick mixing
// and drive the sample output towards the new target to soften the pop/click. It's not perfect but it helps...
// ------------------------------------------------------------------------------------------------------------
mm_word ClickRemoval(mm_word len, mm_addr dest)
{
    s16 mix1[16];
    s16 mix2[16];

    ay38910Mixer(16, mix1, &myAY); // Mix AY and give us something to move towards

    if (msx_music_capable_game)
    {
        YMMixer(len - 16, dest, &myYM); // Keep the YM stepping normally
        YMMixer(16, mix1, &myYM);       // Mix directly on top of AY
    }
    else if (msx_scc_capable_game)
    {
        SCCMixer(16, mix2, &mySCC);       // Get SCC samples
        s32 sample = mix1[15] + mix2[15]; // And mix them together...
        if (sample > 32767)
        {
            sample = 32767;
        }
        if (sample < -32767)
        {
            sample = -32767;
        }
        mix1[15] = sample;
    }

    int target = mix1[15]; // This is what we are smoothing towards

    s16 *p = (s16 *)dest;
    for (int i = 0; i < len; i++)
    {
        if (last_sample < (target - 16))
        {
            last_sample += 8;
        }
        if (last_sample > (target + 16))
        {
            last_sample -= 8;
        }
        *p++ = last_sample;
    }

    if (--bClickRemoveNeeded == 0)
    {
        mixer_read = mixer_write = 0;
    }

    return len;
}

// -----------------------------------------------------------------------------------------------
// This is our main sound mixer. It's called periodically by the maxmod handler when it needs
// more samples in the buffer. We generally produce AY output but sometimes mix in SCC or beeper.
// -----------------------------------------------------------------------------------------------
ITCM_CODE mm_word OurSoundMixer(mm_word len, mm_addr dest, mm_stream_formats format)
{
    if (soundEmuPause) // If paused, just "mix" in mute sound chip... all channels are OFF
    {
        s16 *p = (s16 *)dest;
        for (int i = 0; i < len; i++)
        {
            *p++ = last_sample; // To prevent pops and clicks... just keep outputting the last sample
        }
    }
    else
    {
        if (bClickRemoveNeeded)
        {
            return ClickRemoval(len, dest);
        }

        s16 *p = (s16 *)dest;
        s16 local_sample = last_sample; // A tiny bit faster to move this locally
        for (int i = 0; i < len; i++)
        {
            if (mixer_read == mixer_write)
            {
                *p++ = local_sample;
            }
            else
            {
                local_sample = mixer[mixer_read];
                *p++ = local_sample;
                mixer_read = (mixer_read + 1) & WAVE_DIRECT_BUF_SIZE;
            }
        }
        if (breather)
        {
            breather -= len;
            if (breather < 0)
            {
                breather = 0;
            }
        }

        // If we need to mix in MSX-MUSIC, we grab those samples now and mix directly on top of the output
        if (msx_music_capable_game)
        {
            YMMixer(len, dest, &myYM);
        }

        last_sample = ((s16 *)dest)[len - 1];
    }

    return len;
}

// -----------------------------------------------------------------------------------------------
// This is the 'LITE' version of the mixer. It assumes no direct audio processing and simply
// grabs fresh samples and mixes them into the output. Faster for the older DS-Lite/Phat hardware.
// -----------------------------------------------------------------------------------------------
ITCM_CODE mm_word OurSoundMixer_Lite(mm_word len, mm_addr dest, mm_stream_formats format)
{
    if (soundEmuPause) // If paused, just "mix" in mute sound chip... all channels are OFF
    {
        s16 *p = (s16 *)dest;
        for (int i = 0; i < len; i++)
        {
            *p++ = last_sample; // To prevent pops and clicks... just keep outputting the last sample
        }
    }
    else
    {
        if (bClickRemoveNeeded)
        {
            return ClickRemoval(len, dest);
        }

        if (msx_music_capable_game) // If MSX-MUSIC is enabled, we mix AY with the FM channels
        {
            ay38910Mixer(len, dest, &myAY); // Get AY - we mix YM directly on top...
            YMMixer(len, dest, &myYM);
            last_sample = ((s16 *)dest)[len - 1];
        }
        else if (msx_scc_capable_game) // If SCC is enabled, we need to mix the AY with the SCC chips
        {
            ay38910Mixer(len, mixer, &myAY);     // Get AY samples
            SCCMixer(len, mixer + 1024, &mySCC); // Get SCC samples

            // ------------------------------------------------------------------------
            // And now we need to mix them... with a bit of audio filtering on the DSi
            // ------------------------------------------------------------------------
            s16 *output = (s16 *)dest;

            for (int i = 0; i < len; i++)
            {
                s32 ay = mixer[i];
                ay -= ay >> 2;

                s32 scc = mixer[i | 0x400];
                scc -= scc >> 2;

                s32 combined = ay + scc;

                if (combined > 32767)
                {
                    combined = 32767;
                }
                else if (combined < -32768)
                {
                    combined = -32768;
                }

                output[i] = (s16)combined;
            }

            last_sample = output[len - 1];
        }
        else // Pretty simple... just AY (and maybe beeper)
        {
            ay38910Mixer(len, dest, &myAY);

            if (myConfig.expansion == EXP_2XPSG) // 2x PSG enabled? If so... mix it in.
            {
                ay38910Mixer(len, mixer, &myAY2);
                s16 *p = (s16 *)dest;
                for (int i = 0; i < len; i++)
                {
                    s32 combined = p[i] + mixer[i] + 32768;
                    if (combined > 32767)
                    {
                        combined = 32767;
                    }
                    else if (combined < -32768)
                    {
                        combined = -32768;
                    }
                    p[i] = combined;
                }
            }
            // Did the beeper get hit at any point? If so, we need to mix it in... but it's rare so we do it on an external function.
            else if (beeperFreq && (myConfig.expansion == EXP_BEEPER))
            {
                ProcessBeeper(len, dest);
            }

            last_sample = ((s16 *)dest)[len - 1];
        }
    }

    return len;
}

// -------------------------------------------------------------------------------------------
// Setup the maxmod audio stream - this will be a 16-bit Stereo PCM output at 55KHz which
// sounds about right for the MSX audio data stream.
// -------------------------------------------------------------------------------------------
void setupStream(void)
{
    //----------------------------------------------------------------
    //  initialize maxmod with our small 4-effect soundbank
    //----------------------------------------------------------------
    mmInitDefaultMem((mm_addr)soundbank_bin);

    mmLoadEffect(SFX_CLICKNOQUIT);
    mmLoadEffect(SFX_KEYCLICK);
    mmLoadEffect(SFX_MUS_INTRO);
    mmLoadEffect(SFX_FLOPPY);

    //----------------------------------------------------------------
    //  open stream
    //----------------------------------------------------------------
    myStream.sampling_rate = sample_rate;                                   // sample_rate for the emulated MSX to match the AY/SCC drivers
    myStream.buffer_length = buffer_size;                                   // buffer length = (512+16)
    myStream.callback = (isDSiMode() ? OurSoundMixer : OurSoundMixer_Lite); // set callback function
    myStream.format = MM_STREAM_16BIT_MONO;                                 // format = mono 16-bit
    myStream.timer = MM_TIMER0;                                             // use hardware timer 0
    myStream.manual = false;                                                // use automatic filling
    mmStreamOpen(&myStream);

    //----------------------------------------------------------------
    //  when using 'automatic' filling, your callback will be triggered
    //  every time half of the wave buffer is processed.
    //
    //  so:
    //  25000 (rate)
    //  ----- = ~21 Hz for a full pass, and ~42hz for half pass
    //  1200  (length)
    //----------------------------------------------------------------
    //  with 'manual' filling, you must call mmStreamUpdate
    //  periodically (and often enough to avoid buffer underruns)
    //----------------------------------------------------------------
}

// ----------------------------------------------------------------------------------
// Ensure the sound chips are reset and associated helper variables are initialized.
// ----------------------------------------------------------------------------------
void sound_chip_reset()
{
    s16 mixbuf[16];

    SoundPause();

    bClickRemoveNeeded = 150; // When we re-enable... smooth it to avoid pops/clicks

    msx_scc_capable_game = 0;   // Until proven otherwise
    msx_music_capable_game = 0; // Until proven otherwise
    lastBeeper = 0;             // No beeper yet
    scc_sample_idx = 0;         // Direct Audio reset SCC sample position
    memset(mixer, 0x00, sizeof(mixer));

    // -----------------------------------------------------------
    // The AY sound chip is for our baseline MSX sound handling.
    // -----------------------------------------------------------
    ay38910Reset(&myAY);            // Reset the "AY" sound chip
    ay38910IndexW(0x07, &myAY);     // Register 7 is ENABLE
    ay38910DataW(0x3F, &myAY);      // All OFF (negative logic)
    ay38910Mixer(8, mixbuf, &myAY); // Do an initial mix conversion to prime the output
    myAY.ayPortBOut = 0x80;         // Kana Lock off to start

    ay38910Reset(&myAY2);            // Reset the 2xPSG "AY" sound chip
    ay38910IndexW(0x07, &myAY2);     // Register 7 is ENABLE
    ay38910DataW(0x3F, &myAY2);      // All OFF (negative logic)
    ay38910Mixer(8, mixbuf, &myAY2); // Do an initial mix conversion to prime the output

    // -----------------------------------------------------------------
    // The SCC sound chip is just for a few select Konami MSX1 games
    // -----------------------------------------------------------------
    SCCReset(&mySCC);
    SCCMixer(8, mixbuf, &mySCC); // Do an initial mix conversion to prime the output

    // -----------------------------------------------------------------
    // The YM2413 chip is the MSX MUSIC standard for late-era games...
    // -----------------------------------------------------------------
    YMReset(&myYM);
    YMMixer(8, mixbuf, &myYM); // Do an initial mix conversion to prime the output
}

// -----------------------------------------------------------------------
// We setup the sound chips - disabling all volumes to start.
// -----------------------------------------------------------------------
void dsInstallSoundEmuFIFO(void)
{
    sound_chip_reset();       // Reset the SN, AY and SCC chips
    setupStream();            // Setup maxmod stream...
    bStartSoundEngine = true; // Volume will 'unpause' after 1 frame in the main loop.
}

//*****************************************************************************
// Reset the MSX - mostly CPU and memory plus some timer vars for housekeeping.
//*****************************************************************************
void ResetMSX(void)
{
    JoyMode = JOYMODE_JOYSTICK; // Joystick mode key
    JoyState = 0x00000000;      // Nothing pressed to start

    Reset9938(); // Reset the video chip

    sound_chip_reset(); // Reset the SN, AY and SCC chips

    Z80_Interface_Reset(); // Reset the Z80 Interface
    ResetZ80(&CPU);        // Reset the Z80 CPU core

    msx_reset(); // Reset the MSX specific vars

    msx_caps_lock = 0; // MSX CAPS lock off
    msx_kana_lock = 0; // MSX KANA lock off

    zoom_screen = 0; // Assume no zoom to start

    msxWipeRAM();       // Wipe main RAM area (config chooses zero or random)
    msx_restore_bios(); // Put the BIOS back in place and point to it

    keyMapType = 0; // If we go back into options

    // -----------------------------------------------------------
    // Timer 1 is used to time frame-to-frame of actual emulation
    // -----------------------------------------------------------
    TIMER1_CR = 0;
    TIMER1_DATA = 0;
    TIMER1_CR = TIMER_ENABLE | TIMER_DIV_1024;

    // -----------------------------------------------------------
    // Timer 2 is used to time once per second events
    // -----------------------------------------------------------
    TIMER2_CR = 0;
    TIMER2_DATA = 0;
    TIMER2_CR = TIMER_ENABLE | TIMER_DIV_1024;
    timingFrames = 0;
    skip_render = 0;
    io_show_status = 0;
    sram_show_status = 0;
    flash_show_status = 0;
    render_blended = 0;
}

//*********************************************************************************
// A mini Z80 debugger of sorts. Put out some Z80, VDP and SGM/Bank info on
// screen every frame to help us debug some of the problem games. This is enabled
// via global configuration for debugger.
//*********************************************************************************
extern u8 *fake_heap_end;   // current heap start
extern u8 *fake_heap_start; // current heap end

u8 *getHeapStart() { return fake_heap_start; }
u8 *getHeapEnd() { return (u8 *)sbrk(0); }
u8 *getHeapLimit() { return fake_heap_end; }

int getMemUsed()
{ // returns the amount of used memory in bytes
    struct mallinfo mi = mallinfo();
    return mi.uordblks;
}

int getMemFree()
{ // returns the amount of free memory in bytes
    struct mallinfo mi = mallinfo();
    return mi.fordblks + (getHeapLimit() - getHeapEnd());
}

void ShowDebugZ80(void)
{
    u8 idx = 1;

    sprintf(tmpStr, "VDP: %02X %02X %02X %02X %02X %02X %02X %02X", VDP[0], VDP[1], VDP[2], VDP[3], VDP[4], VDP[5], VDP[6], VDP[7]);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "VDP: %02X %02X %02X %02X %02X %02X %02X %02X", VDP[8], VDP[9], VDP[10], VDP[11], VDP[12], VDP[13], VDP[14], VDP[15]);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "VDP: %02X %02X %02X %02X %02X %02X %02X %02X", VDP[16], VDP[17], VDP[18], VDP[19], VDP[20], VDP[21], VDP[22], VDP[23]);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "VDP: %02X %02X %02X %02X DATA=%02X", VDP[24], VDP[25], VDP[26], VDP[27], VDPDlatch);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "VStat %02X %02X %02X", VDPStatus[0], VDPStatus[1], VDPStatus[2]);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "VAddr %08X", (VDP[14] << 14) + (int)VAddr);
    DSPrint(0, idx++, 7, tmpStr);
    idx++;

    sprintf(tmpStr, "Z80PC %04X", CPU.PC.W);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "Z80SP %04X", CPU.SP.W);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "Z80AF %04X", CPU.AF.W);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "Z80BC %04X", CPU.BC.W);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "Z80DE %04X", CPU.DE.W);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "IRQ %04X %d", CPU.IRequest, (CPU.NumInts % 99999));
    DSPrint(0, idx++, 7, tmpStr);
    idx++;

    sprintf(tmpStr, "AY:%02X %02X %02X %02X", myAY.ayRegs[0], myAY.ayRegs[1], myAY.ayRegs[2], myAY.ayRegs[3]);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "AY:%02X %02X %02X %02X", myAY.ayRegs[4], myAY.ayRegs[5], myAY.ayRegs[6], myAY.ayRegs[7]);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "AY:%02X %02X %02X %02X", myAY.ayRegs[8], myAY.ayRegs[9], myAY.ayRegs[10], myAY.ayRegs[11]);
    DSPrint(0, idx++, 7, tmpStr);

    idx++;
    int screen = ScrMode;
    if (screen == 8)
    {
        if ((VDP[25] & 0x18) == 0x18) screen = 11;
        else if ((VDP[25] & 0x18) == 0x08) screen = 12;
    }
    sprintf(tmpStr, "S%-2d A8=%02X %02X", screen, Port_PPI_A, msx_subslot);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "FD.ST=%02X CM=%02X", FDC.status, FDC.command);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "Mapper %d [%02X]", MyMedia[MEDIA_CART1].mapperType, MyMedia[MEDIA_CART1].mapperMask);
    DSPrint(0, idx++, 7, tmpStr);
    extern u32 halt_counter;
    sprintf(tmpStr, "Halt C %d", halt_counter & 0x1FFFF);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "%d Free", getMemFree());
    DSPrint(0, idx++, 7, tmpStr);

    idx = 6;
    for (u8 i = 0; i < 16; i++)
    {
        sprintf(tmpStr, "D%-2d %-8lu %04X", i, debug[i], (u16)debug[i]);
        DSPrint(15, idx++, 7, tmpStr);
    }
}

// ------------------------------------------------------------------------
// The status line shows the status of the SCC, MSX-MUSIC, disk icons, etc.
// ------------------------------------------------------------------------
void DisplayStatusLine(void)
{
    if (myGlobalConfig.debugger)
    {
        return; // If debugger, skip this
    }

    if (sram_show_status)
    {
        DSPrint(20, 0, 2, "'()"); // Show SRAM icon
        DSPrint(20, 1, 2, "GHI"); // Show SRAM icon
        if (--sram_show_status == 0)
        {
            msxSaveSRAM();
        }
    }
    else if (flash_show_status)
    {
        DSPrint(20, 0, 0, " ");
        DSPrint(21, 0, 2, "?"); // Show Flash Write icon
        DSPrint(22, 0, 0, " ");
        DSPrint(20, 1, 0, " ");
        DSPrint(21, 1, 2, "_"); // Show Flash Write icon
        DSPrint(22, 1, 0, " ");
        if (--flash_show_status == 0)
        {
            // TODO: Write flash to SD?
        }
    }
    else if (io_show_status)
    {
        if (io_show_status == 8) // Disk Write
        {
            DSPrint(20, 0, 2, "678"); // Show Disk icon
            DSPrint(20, 1, 2, "VWX"); // Show Disk icon
            io_show_status = 6;       // Show icon briefly
            if (myGlobalConfig.bDiskSounds)
            {
                mmEffect(SFX_FLOPPY); // Short disk sound effect
            }
        }
        else if (io_show_status == 7) // Disk Read
        {
            DSPrint(20, 0, 2, "345"); // Show Disk icon
            DSPrint(20, 1, 2, "STU"); // Show Disk icon
            io_show_status = 6;       // Show icon briefly
            if (myGlobalConfig.bDiskSounds)
            {
                mmEffect(SFX_FLOPPY); // Short disk sound effect
            }
        }

        if (--io_show_status == 0) // Done with read/write activity
        {
            fdc_persist_disk(); // See if the disk needs to be persisted.
        }
    }
    // ------------------------------------------------------------
    // If we aren't showing the disk icon or SRAM above, we can
    // show one of the various music symbols for SCC or MSX-MUSIC.
    // ------------------------------------------------------------
    else
    {
        if (msx_scc_capable_game)
        {
            // SCC has a little cool graphic to go with it!
            DSPrint(20, 0, 2, "012");
            DSPrint(20, 1, 2, "PQR");
        }
        else if (msx_music_capable_game)
        {
            // MSX MUSIC has a little cool graphic to go with it!
            DSPrint(20, 0, 2, "$%&");
            DSPrint(20, 1, 2, "DEF");
        }
        else if (myConfig.expansion == EXP_2XPSG)
        {
            // 2X PSG
            DSPrint(20, 0, 2, "!\"#");
            DSPrint(20, 1, 2, "ABC");
        }
        else // Clear the icon display area...
        {
            if (myGlobalConfig.bShowPSG)
            {
                DSPrint(20, 0, 0, "=>?");
                DSPrint(20, 1, 0, "]^_");
            }
            else // Just clear it out
            {
                DSPrint(20, 0, 0, "   ");
                DSPrint(20, 1, 0, "   ");
            }
        }
    }

    // -----------------------------------------------------------------
    // If the full MSX keyboard is showing, we can show some symbols
    // to indicate if the CAPS lock is enabled, shift/ctrl/graph, etc.
    // -----------------------------------------------------------------
    if (myConfig.keyboard == OVL_FULLKBD)
    {
        // Caps Lock
        DSPrint(1, 23, 0, (msx_caps_lock ? "@" : " "));
        DSPrint(2, 23, (msx_caps_lock ? 2 : 0), (msx_caps_lock ? "@" : " "));

        if (msx_kana_lock != ((myAY.ayPortBOut & 0x80) ? 0 : 1))
        {
            msx_kana_lock = (myAY.ayPortBOut & 0x80) ? 0 : 1;
            BottomScreenKeypad();
        }

        DSPrint(1, 19, 0, (key_shift ? "A" : " "));
        DSPrint(2, 19, 0, (key_shift ? "!" : " "));

        DSPrint(1, 15, 0, (key_ctrl ? "@" : " "));
        DSPrint(2, 15, (key_ctrl ? 2 : 0), (key_ctrl ? "@" : " "));

        if (zoom_screen) // Pan and Scan on Mode 7 and Mode 0 with 80 Columns
        {
            key_graph = 0;
            DSPrint(4, 21, 0, ":;<");
            DSPrint(4, 22, 0, "Z[\\");
        }
        else
        {
            DSPrint(4, 21, 0, "   ");
            DSPrint(4, 22, 0, "   ");
        }
        DSPrint(5, 23, (key_graph ? 2 : 0), (key_graph ? "]" : " "));
    }
}

// ------------------------------------------------------------------------
// Handle mini-menu interface...
// ------------------------------------------------------------------------
u8 MiniMenu(void)
{
    u8 retVal = MENU_CHOICE_NONE;

    SoundPause();

    while ((keysCurrent() & (KEY_TOUCH | KEY_LEFT | KEY_RIGHT | KEY_A)) != 0)
        ;

    BottomScreenMiniMenu();

    while (true)
    {
        nds_key = keysCurrent();
        if (nds_key & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START | KEY_SELECT | KEY_L | KEY_R))
        {
            retVal = MENU_CHOICE_NONE;
            break; // We're done...
        }

        if (nds_key & KEY_TOUCH)
        {
            touchPosition touch;
            touchRead(&touch);
            if ((touch.py >= 22) && (touch.py < 98)) // Top row of Icons
            {
                if (touch.px <= 64)
                {
                    retVal = MENU_CHOICE_SAVE_GAME;
                }
                else if (touch.px <= 128)
                {
                    retVal = MENU_CHOICE_LOAD_GAME;
                }
                else if (touch.px <= 192)
                {
                    retVal = MENU_CHOICE_SWAP_DISK;
                }
                else if (touch.px <= 256)
                {
                    retVal = MENU_CHOICE_END_GAME;
                }

                break;
            }
            if ((touch.py >= 98) && (touch.py < 190)) // Bottom row of Icons
            {
                if (touch.px <= 64)
                {
                    retVal = MENU_CHOICE_HI_SCORE;
                }
                else if (touch.px <= 128)
                {
                    retVal = MENU_CHOICE_GAME_OPTIONS;
                }
                else if (touch.px <= 192)
                {
                    retVal = MENU_CHOICE_DEFINE_KEYS;
                }
                else if (touch.px <= 256)
                {
                    retVal = MENU_CHOICE_RESET_GAME;
                }

                break;
            }
        }
    }

    while ((keysCurrent() & (KEY_TOUCH | KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START | KEY_SELECT)) != 0)
        ;

    WAITVBL;
    WAITVBL;

    BottomScreenKeypad();

    SoundUnPause();

    return retVal;
}

// ------------------------------------------------------
// Handle the MSX virtual keyboard on the bottom screen.
// ------------------------------------------------------
u8 last_special_key = 0;
u8 last_special_key_dampen = 0;
u8 last_kbd_key = 0;
u8 dampen_zoom = 0;

#define CUR_W 43
#define CUR_H 58

int cursor_hit(int x, int y)   /* x,y relative to image top-left */
{
    if (x < 0 || y < 0 || x >= CUR_W || y >= CUR_H)
        return 0x00;

    int dx = 2 * x - CUR_W;    /* -43..+43 */
    int dy = 2 * y - CUR_H;    /* -58..+58 */
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;

    if (adx * CUR_H > ady * CUR_W)
        return dx < 0 ? KBD_KEY_LEFT : KBD_KEY_RIGHT;
    else
        return dy < 0 ? KBD_KEY_UP : KBD_KEY_DOWN;
}

u8 handle_msx_keyboard_press(u16 iTx, u16 iTy) // MSX Keyboard
{
    if ((iTx >= 214) && (iTy >= 103) && (iTy < 162)) // Triangular Arrow Keys... do our best
    {
        kbd_key = cursor_hit(iTx-214, iTy-103);
    }
    else if ((iTy >= 12) && (iTy < 42)) // Row 1 (top row with Function Keys)
    {
        if ((iTx >= 0) && (iTx < 22))
        {
            kbd_key = KBD_KEY_ESC;
        }
        else if ((iTx >= 22) && (iTx < 44))
        {
            kbd_key = KBD_KEY_HOME;
        }
        else if ((iTx >= 44) && (iTx < 73))
        {
            kbd_key = KBD_KEY_F1;
        }
        else if ((iTx >= 73) && (iTx < 102))
        {
            kbd_key = KBD_KEY_F2;
        }
        else if ((iTx >= 102) && (iTx < 131))
        {
            kbd_key = KBD_KEY_F3;
        }
        else if ((iTx >= 131) && (iTx < 160))
        {
            kbd_key = KBD_KEY_F4;
        }
        else if ((iTx >= 160) && (iTx < 190))
        {
            kbd_key = KBD_KEY_F5;
        }
        else if ((iTx >= 190) && (iTx < 212))
        {
            kbd_key = KBD_KEY_BS;
        }
        else if ((iTx >= 212) && (iTx < 235))
        {
            kbd_key = KBD_KEY_INS;
        }
        else if ((iTx >= 235) && (iTx < 255))
        {
            kbd_key = KBD_KEY_DEL;
        }
    }
    else if ((iTy >= 42) && (iTy < 72)) // Row 2 (number row)
    {
        if ((iTx >= 0) && (iTx < 15))
        {
            kbd_key = '@';
        }
        else if ((iTx >= 15) && (iTx < 31))
        {
            kbd_key = '1';
        }
        else if ((iTx >= 31) && (iTx < 45))
        {
            kbd_key = '2';
        }
        else if ((iTx >= 45) && (iTx < 61))
        {
            kbd_key = '3';
        }
        else if ((iTx >= 61) && (iTx < 75))
        {
            kbd_key = '4';
        }
        else if ((iTx >= 75) && (iTx < 91))
        {
            kbd_key = '5';
        }
        else if ((iTx >= 91) && (iTx < 106))
        {
            kbd_key = '6';
        }
        else if ((iTx >= 106) && (iTx < 121))
        {
            kbd_key = '7';
        }
        else if ((iTx >= 121) && (iTx < 135))
        {
            kbd_key = '8';
        }
        else if ((iTx >= 135) && (iTx < 151))
        {
            kbd_key = '9';
        }
        else if ((iTx >= 151) && (iTx < 165))
        {
            kbd_key = '0';
        }
        else if ((iTx >= 165) && (iTx < 181))
        {
            kbd_key = '-';
        }
        else if ((iTx >= 181) && (iTx < 195))
        {
            kbd_key = '=';
        }
        else if ((iTx >= 195) && (iTx < 210))
        {
            kbd_key = '|'; // YEN
        }
        else if ((iTx >= 210) && (iTx < 255))
        {
            kbd_key = KBD_KEY_SEL;
        }
    }
    else if ((iTy >= 72) && (iTy < 102)) // Row 3 (QWERTY row)
    {
        if ((iTx >= 0) && (iTx < 23))
        {
            kbd_key = KBD_KEY_TAB;
        }
        else if ((iTx >= 23) && (iTx < 39))
        {
            kbd_key = 'Q';
        }
        else if ((iTx >= 39) && (iTx < 54))
        {
            kbd_key = 'W';
        }
        else if ((iTx >= 54) && (iTx < 69))
        {
            kbd_key = 'E';
        }
        else if ((iTx >= 69) && (iTx < 83))
        {
            kbd_key = 'R';
        }
        else if ((iTx >= 83) && (iTx < 99))
        {
            kbd_key = 'T';
        }
        else if ((iTx >= 99) && (iTx < 113))
        {
            kbd_key = 'Y';
        }
        else if ((iTx >= 113) && (iTx < 129))
        {
            kbd_key = 'U';
        }
        else if ((iTx >= 129) && (iTx < 143))
        {
            kbd_key = 'I';
        }
        else if ((iTx >= 143) && (iTx < 158))
        {
            kbd_key = 'O';
        }
        else if ((iTx >= 158) && (iTx < 174))
        {
            kbd_key = 'P';
        }
        else if ((iTx >= 174) && (iTx < 189))
        {
            kbd_key = '[';
        }
        else if ((iTx >= 189) && (iTx < 203))
        {
            kbd_key = ']';
        }
        else if ((iTx >= 203) && (iTx < 214))
        {
            kbd_key = KBD_KEY_DEAD;
        }
        else if ((iTx >= 214) && (iTx < 255))
        {
            kbd_key = KBD_KEY_STOP;
        }
    }
    else if ((iTy >= 102) && (iTy < 132)) // Row 4 (ASDF row)
    {
        if ((iTx >= 0) && (iTx < 27))
        {
            kbd_key = KBD_KEY_CTRL;
            last_special_key = KBD_KEY_CTRL;
            last_special_key_dampen = 20;
        }
        else if ((iTx >= 27) && (iTx < 43))
        {
            kbd_key = 'A';
        }
        else if ((iTx >= 43) && (iTx < 58))
        {
            kbd_key = 'S';
        }
        else if ((iTx >= 58) && (iTx < 72))
        {
            kbd_key = 'D';
        }
        else if ((iTx >= 72) && (iTx < 87))
        {
            kbd_key = 'F';
        }
        else if ((iTx >= 87) && (iTx < 102))
        {
            kbd_key = 'G';
        }
        else if ((iTx >= 102) && (iTx < 117))
        {
            kbd_key = 'H';
        }
        else if ((iTx >= 117) && (iTx < 132))
        {
            kbd_key = 'J';
        }
        else if ((iTx >= 132) && (iTx < 147))
        {
            kbd_key = 'K';
        }
        else if ((iTx >= 147) && (iTx < 161))
        {
            kbd_key = 'L';
        }
        else if ((iTx >= 161) && (iTx < 178))
        {
            kbd_key = ';';
        }
        else if ((iTx >= 178) && (iTx < 192))
        {
            kbd_key = KBD_KEY_QUOTE;
        }
        else if ((iTx >= 192) && (iTx < 214))
        {
            kbd_key = KBD_KEY_RET;
        }
    }
    else if ((iTy >= 132) && (iTy < 162)) // Row 5 (ZXCV row)
    {
        if ((iTx >= 0) && (iTx < 33))
        {
            kbd_key = KBD_KEY_SHIFT;
            last_special_key = KBD_KEY_SHIFT;
            last_special_key_dampen = 20;
        }
        else if ((iTx >= 33) && (iTx < 49))
        {
            kbd_key = 'Z';
        }
        else if ((iTx >= 49) && (iTx < 64))
        {
            kbd_key = 'X';
        }
        else if ((iTx >= 64) && (iTx < 78))
        {
            kbd_key = 'C';
        }
        else if ((iTx >= 78) && (iTx < 94))
        {
            kbd_key = 'V';
        }
        else if ((iTx >= 94) && (iTx < 109))
        {
            kbd_key = 'B';
        }
        else if ((iTx >= 109) && (iTx < 123))
        {
            kbd_key = 'N';
        }
        else if ((iTx >= 123) && (iTx < 139))
        {
            kbd_key = 'M';
        }
        else if ((iTx >= 139) && (iTx < 154))
        {
            kbd_key = ',';
        }
        else if ((iTx >= 154) && (iTx < 169))
        {
            kbd_key = '.';
        }
        else if ((iTx >= 169) && (iTx < 184))
        {
            kbd_key = '/';
        }
        else if ((iTx >= 184) && (iTx < 214))
        {
            kbd_key = KBD_KEY_RET;
        }
    }
    else if ((iTy >= 162) && (iTy < 192)) // Row 6 (SPACE BAR and icons row)
    {
        if ((iTx >= 1) && (iTx < 30))
        {
            kbd_key = KBD_KEY_CAPS;
        }
        else if ((iTx >= 30) && (iTx < 53)) // Graph Key... also Zoom for Screen 7
        {
            if (zoom_screen == 1)
            {
                if (!dampen_zoom)
                {
                    zoom_screen = 2;
                }
                dampen_zoom = 10;
            }
            else if (zoom_screen == 2)
            {
                if (!dampen_zoom)
                {
                    zoom_screen = 1;
                }
                dampen_zoom = 10;
            }
            else
            {
                kbd_key = KBD_KEY_GRAPH;
                last_special_key = KBD_KEY_GRAPH;
                last_special_key_dampen = 20;
            }
        }
        else if ((iTx >= 53) && (iTx < 163))
        {
            kbd_key = ' ';
        }
        else if ((iTx >= 163) && (iTx < 192))
        {
            kbd_key = KBD_KEY_KANA;
        }
        else if ((iTx >= 192) && (iTx < 255))
        {
            return MENU_CHOICE_MENU;
        }
    }

    return MENU_CHOICE_NONE;
}

u8 handle_alpha_keyboard_press(u16 iTx, u16 iTy) // Generic and Simplified Alpha-Numeric Keyboard
{
    if ((iTy >= 14) && (iTy < 50)) // Row 1 (number row)
    {
        if ((iTx >= 0) && (iTx < 28))
        {
            kbd_key = '1';
        }
        else if ((iTx >= 28) && (iTx < 54))
        {
            kbd_key = '2';
        }
        else if ((iTx >= 54) && (iTx < 80))
        {
            kbd_key = '3';
        }
        else if ((iTx >= 80) && (iTx < 106))
        {
            kbd_key = '4';
        }
        else if ((iTx >= 106) && (iTx < 132))
        {
            kbd_key = '5';
        }
        else if ((iTx >= 132) && (iTx < 148))
        {
            kbd_key = '6';
        }
        else if ((iTx >= 148) && (iTx < 174))
        {
            kbd_key = '7';
        }
        else if ((iTx >= 174) && (iTx < 200))
        {
            kbd_key = '8';
        }
        else if ((iTx >= 200) && (iTx < 226))
        {
            kbd_key = '9';
        }
        else if ((iTx >= 226) && (iTx < 255))
        {
            kbd_key = '0';
        }
    }
    else if ((iTy >= 50) && (iTy < 88)) // Row 2 (QWERTY row)
    {
        if ((iTx >= 0) && (iTx < 28))
        {
            kbd_key = 'Q';
        }
        else if ((iTx >= 28) && (iTx < 54))
        {
            kbd_key = 'W';
        }
        else if ((iTx >= 54) && (iTx < 80))
        {
            kbd_key = 'E';
        }
        else if ((iTx >= 80) && (iTx < 106))
        {
            kbd_key = 'R';
        }
        else if ((iTx >= 106) && (iTx < 132))
        {
            kbd_key = 'T';
        }
        else if ((iTx >= 132) && (iTx < 148))
        {
            kbd_key = 'Y';
        }
        else if ((iTx >= 148) && (iTx < 174))
        {
            kbd_key = 'U';
        }
        else if ((iTx >= 174) && (iTx < 200))
        {
            kbd_key = 'I';
        }
        else if ((iTx >= 200) && (iTx < 226))
        {
            kbd_key = 'O';
        }
        else if ((iTx >= 226) && (iTx < 255))
        {
            kbd_key = 'P';
        }
    }
    else if ((iTy >= 88) && (iTy < 125)) // Row 3 (ASDF row)
    {
        if ((iTx >= 0) && (iTx < 28))
        {
            kbd_key = 'A';
        }
        else if ((iTx >= 28) && (iTx < 54))
        {
            kbd_key = 'S';
        }
        else if ((iTx >= 54) && (iTx < 80))
        {
            kbd_key = 'D';
        }
        else if ((iTx >= 80) && (iTx < 106))
        {
            kbd_key = 'F';
        }
        else if ((iTx >= 106) && (iTx < 132))
        {
            kbd_key = 'G';
        }
        else if ((iTx >= 132) && (iTx < 148))
        {
            kbd_key = 'H';
        }
        else if ((iTx >= 148) && (iTx < 174))
        {
            kbd_key = 'J';
        }
        else if ((iTx >= 174) && (iTx < 200))
        {
            kbd_key = 'K';
        }
        else if ((iTx >= 200) && (iTx < 226))
        {
            kbd_key = 'L';
        }
        else if ((iTx >= 226) && (iTx < 255))
        {
            kbd_key = KBD_KEY_BS;
        }
    }
    else if ((iTy >= 125) && (iTy < 160)) // Row 4 (ZXCV row)
    {
        if ((iTx >= 0) && (iTx < 28))
        {
            kbd_key = 'Z';
        }
        else if ((iTx >= 28) && (iTx < 54))
        {
            kbd_key = 'X';
        }
        else if ((iTx >= 54) && (iTx < 80))
        {
            kbd_key = 'C';
        }
        else if ((iTx >= 80) && (iTx < 106))
        {
            kbd_key = 'V';
        }
        else if ((iTx >= 106) && (iTx < 132))
        {
            kbd_key = 'B';
        }
        else if ((iTx >= 132) && (iTx < 148))
        {
            kbd_key = 'N';
        }
        else if ((iTx >= 148) && (iTx < 174))
        {
            kbd_key = 'M';
        }
        else if ((iTx >= 174) && (iTx < 200))
        {
            kbd_key = (key_shift ? ',' : '.');
        }
        else if ((iTx >= 200) && (iTx < 255))
        {
            kbd_key = KBD_KEY_RET;
        }
    }
    else if ((iTy >= 160) && (iTy < 192)) // Row 5 (SPACE BAR and icons row)
    {
        if ((iTx >= 1) && (iTx < 202))
        {
            kbd_key = ' ';
        }
        else if ((iTx >= 202) && (iTx < 255))
        {
            return MENU_CHOICE_MENU;
        }
    }

    return MENU_CHOICE_NONE;
}

u8 handle_debugger_overlay(u16 iTx, u16 iTy)
{
    if ((iTy >= 175) && (iTy < 192)) // Bottom row is where the debugger keys are...
    {
        if ((iTx >= 1) && (iTx < 125))
        {
            kbd_key = ' ';
        }
        if ((iTx >= 125) && (iTx < 158))
        {
            return MENU_CHOICE_MENU;
        }
        else if ((iTx >= 192) && (iTx < 255))
        {
            return MENU_CHOICE_MENU;
        }
    }
    else
    {
        kbd_key = 0;
        last_kbd_key = 0;
    }

    return MENU_CHOICE_NONE;
}

// ----------------------------------------------------------------------------
// Slide-n-Glide D-pad keeps moving in the last known direction for a few more
// frames to help make those hairpin turns up and off ladders much easier...
// ----------------------------------------------------------------------------
u8 slide_n_glide_key_up = 0;
u8 slide_n_glide_key_down = 0;
u8 slide_n_glide_key_left = 0;
u8 slide_n_glide_key_right = 0;

// ------------------------------------------------------------------------
// The main emulation loop is here... call into the Z80, VDP and PSG
// ------------------------------------------------------------------------
void HachibittoRunEmu(void)
{
    u16 iTx, iTy;
    u16 SaveNow = 0, LoadNow = 0;
    u32 JoyStickMap;
    static u32 lastUN = 0;
    static u8 dampenClick = 0;
    u8 meta_key = 0;
    u8 screen_position_dampen = 0;
    u8 save_config = 0;

    // Setup the debug buffer for DSi use
    debug_init();

    // Returns when  user has asked for a game to run...
    BottomScreenOptions();

    // Clear top screen
    BG_PALETTE[0] = RGB15(0, 0, 0);
    memset((u8 *)0x06000000, 0x00, 0x40000); // Ensure screen is clear... (LCD_A and LCD_B)

    // Get the MSX Machine Emulator ready
    msxInit();

    Z80_Interface_Reset(); // Reset the Z80 Interface module
    ResetZ80(&CPU);        // Reset the CZ80 core CPU
    BottomScreenKeypad();  // Show the game-related screen with keypad / keyboard

    // Frame-to-frame timing...
    TIMER1_CR = 0;
    TIMER1_DATA = 0;
    TIMER1_CR = TIMER_ENABLE | TIMER_DIV_1024;

    // Once/second timing...
    TIMER2_CR = 0;
    TIMER2_DATA = 0;
    TIMER2_CR = TIMER_ENABLE | TIMER_DIV_1024;
    timingFrames = 0;

    // Force the sound engine to turn on when we start emulation
    bStartSoundEngine = true;

    // -------------------------------------------------------------------
    // Stay in this loop running the MSX game until the user exits...
    // -------------------------------------------------------------------
    while (1)
    {
        // Take a tour of the Z80 counter and display the screen if necessary
        if (!LoopZ80())
        {
            // If we've been asked to start the sound engine, rock-and-roll!
            if (bStartSoundEngine)
            {
                bStartSoundEngine = false;
                SoundUnPause();
            }

            // -------------------------------------------------------------
            // Stuff to do once/second such as FPS display and Debug Data
            // -------------------------------------------------------------
            if (TIMER1_DATA >= 32728) //  1000MS (1 sec)
            {
                TIMER1_CR = 0;
                TIMER1_DATA = 0;
                TIMER1_CR = TIMER_ENABLE | TIMER_DIV_1024;
                u16 emuFps = emuActFrames;
                if (myGlobalConfig.showFPS)
                {
                    if (myGlobalConfig.showFPS != 2) // If not Full Speed... round towards 60
                    {
                        if (emuFps == 61)
                        {
                            emuFps = 60;
                        }
                        else if (emuFps == 59)
                        {
                            emuFps = 60;
                        }
                    }
                    DSPrint_fps(emuFps);
                }
                emuActFrames = 0;
            }

            emuActFrames++; // This one can go above or below 60... used for FPS calculation

            // ---------------------------------------------
            // We only support NTSC 60 frames per second...
            // ---------------------------------------------
            if (++timingFrames == 60)
            {
                TIMER2_CR = 0;
                TIMER2_DATA = 0;
                TIMER2_CR = TIMER_ENABLE | TIMER_DIV_1024;
                timingFrames = 0;
                DisplayStatusLine(); // Every half second
            }
            else if (timingFrames == 30) // Every half second
            {
                DisplayStatusLine();
            }

            // -----------------------------------------------------
            // Vertical Sync reduces tearing but costs CPU time...
            // -----------------------------------------------------
            while (dsVSyncCount == last_vsync_count)
            {
                if (myGlobalConfig.showFPS == 2)
                {
                    break; // If Full Speed, break out...
                }
            }
            last_vsync_count = dsVSyncCount;

            // And copy out XBuf[] to the DS VRAM!
            msxUpdateScreen();

            // If the Z80 Debugger is enabled, call it every frame. Expensive but we need the debug!
            if (myGlobalConfig.debugger)
            {
                ShowDebugZ80();
            }

            // Screen 7 is 512px wide and we have the ability to render it in Zoom mode
            if (ScrMode == 7)
            {
                if (!zoom_screen)
                {
                    zoom_screen = 1;
                }
                render_blended = 1; // We Alpha Blend the A/B pixels in Screen 7
            }
            else if (ScrMode == 9) // 80 Column Mode (480px)
            {
                if (!zoom_screen)
                {
                    zoom_screen = 1;
                }
            }
            else
            {
                zoom_screen = 0;
            }

            // ---------------------------------------------------------------------------------
            // Hold the key press for a brief instant... some machines take longer than others
            // (eg MSX needs to see the keypress for many tens of milliseconds)... This allows
            // us to 'hold' the keypress in memory for several frames which is good enough.
            // ---------------------------------------------------------------------------------
            if (key_debounce > 0)
            {
                key_debounce--;
            }
            else
            {
                // -----------------------------------------------------------
                // This is where we accumulate the keys pressed... up to 12!
                // -----------------------------------------------------------
                kbd_keys_pressed = 0;
                memset(kbd_keys, 0x00, sizeof(kbd_keys));
                kbd_key = 0;

                // ------------------------------------------
                // Handle any screen touch events
                // ------------------------------------------
                if (keysCurrent() & KEY_TOUCH)
                {
                    // ------------------------------------------------------------------------------------------------
                    // Just a tiny bit of touch debounce so ensure touch screen is pressed for a fraction of a second.
                    // ------------------------------------------------------------------------------------------------
                    if (++touch_debounce > 1)
                    {
                        touchPosition touch;
                        touchRead(&touch);
                        iTx = touch.px;
                        iTy = touch.py;

                        if (myGlobalConfig.debugger)
                        {
                            meta_key = handle_debugger_overlay(iTx, iTy);
                        }
                        // ------------------------------------------------------------
                        // Test the touchscreen for various full keyboard handlers...
                        // ------------------------------------------------------------
                        else
                        {
                            if (myConfig.keyboard == OVL_ALPHAKBD)
                            {
                                meta_key = handle_alpha_keyboard_press(iTx, iTy);
                            }
                            else
                            {
                                meta_key = handle_msx_keyboard_press(iTx, iTy);
                            }
                        }

                        if (kbd_key != 0)
                        {
                            kbd_keys[kbd_keys_pressed++] = kbd_key;
                            key_debounce = 2;
                        }

                        // If the special menu key indicates we should show the choice menu, do so here...
                        if (meta_key == MENU_CHOICE_MENU)
                        {
                            meta_key = MiniMenu();
                        }

                        // -------------------------------------------------------------------
                        // If one of the special meta keys was picked, we handle that here...
                        // -------------------------------------------------------------------
                        switch (meta_key)
                        {
                        case MENU_CHOICE_RESET_GAME:
                            SoundPause();
                            // Ask for verification
                            if (showMessage("DO YOU REALLY WANT TO", "RESET THE CURRENT GAME ?") == ID_SHM_YES)
                            {
                                memset((u8 *)0x06000000, 0x00, 0x40000); // Ensure screen is clear...
                                ResetMSX();
                            }
                            BottomScreenKeypad();
                            SoundUnPause();
                            break;

                        case MENU_CHOICE_END_GAME:
                            SoundPause();
                            //  Ask for verification
                            if (showMessage("DO YOU REALLY WANT TO", "QUIT THE CURRENT GAME ?") == ID_SHM_YES)
                            {
                                memset((u8 *)0x06000000, 0x00, 0x40000); // Reset VRAM to 0x00 to clear any potential display garbage on way out
                                return;
                            }
                            BottomScreenKeypad();
                            DisplayStatusLine();
                            SoundUnPause();
                            break;

                        case MENU_CHOICE_HI_SCORE:
                            SoundPause();
                            highscore_display(GetMasterCRC());
                            DisplayStatusLine();
                            SoundUnPause();
                            break;

                        case MENU_CHOICE_GAME_OPTIONS:
                            SoundPause();
                            BottomScreenOptions();
                            HachibittoGameOptions(false);
                            BottomScreenKeypad();
                            DisplayStatusLine();
                            SoundUnPause();
                            break;

                        case MENU_CHOICE_DEFINE_KEYS:
                            SoundPause();
                            BottomScreenOptions();
                            HachibittoChangeKeymap();
                            BottomScreenKeypad();
                            DisplayStatusLine();
                            SoundUnPause();
                            break;

                        case MENU_CHOICE_SAVE_GAME:
                            if (!SaveNow)
                            {
                                SoundPause();
                                if (showMessage("DO YOU REALLY WANT TO", "SAVE GAME STATE ?") == ID_SHM_YES)
                                {
                                    SaveNow = 1;
                                    msxSaveState();
                                }
                                BottomScreenKeypad();
                                SoundUnPause();
                            }
                            break;

                        case MENU_CHOICE_LOAD_GAME:
                            if (!LoadNow)
                            {
                                SoundPause();
                                if (showMessage("DO YOU REALLY WANT TO", "LOAD GAME STATE ?") == ID_SHM_YES)
                                {
                                    LoadNow = 1;
                                    msxLoadState();
                                }
                                BottomScreenKeypad();
                                SoundUnPause();
                            }
                            break;

                        case MENU_CHOICE_SWAP_DISK:
                            SoundPause();
                            BottomScreenOptions();
                            HachibittoChooseFile(MEDIA_DISK, 0); // Swap disk but do NOT allow directory change
                            if (ucGameChoice >= 0)               // Did the user select a game?
                            {
                                BottomScreenOptions();
                                DSPrint(11, 13, 6, "LOADING...");
                                // -------------------------------------------------------------------------
                                // Do NOT save this new disk/filename into MyMedia[].filename as it would
                                // overwrite the master. Same for filecrc. Instead we save it into filename2
                                // which always represents the current file for the disk swapped in.
                                // -------------------------------------------------------------------------
                                strcpy(MyMedia[MEDIA_DISK].currdisk, gpFic[ucGameChoice].szName);
                                MyMedia[MEDIA_DISK].filesize = ReadFileCarefully(MyMedia[MEDIA_DISK].currdisk, DISK_Memory, (MAX_DISK_SIZE_KB * 1024), 0, NULL);
                                fdc_init(1, (MyMedia[MEDIA_DISK].filesize / 1024 == 360) ? 1 : 2, 80, 9, 512, 1, DISK_Memory, NULL);
                                fdc_reset(false);
                            }
                            BottomScreenKeypad();
                            SoundUnPause();
                            break;

                        default:
                            SaveNow = 0;
                            LoadNow = 0;
                        }

                        if (++dampenClick > 0) // Make sure the key is pressed for an appreciable amount of time...
                        {
                            if ((kbd_key != 0) && (lastUN == 0))
                            {
                                mmEffect(SFX_KEYCLICK); // Play short key click for feedback...
                            }
                            lastUN = kbd_key;
                        }
                    }
                } //  SCR_TOUCH
                else
                {
                    touch_debounce = 0;
                    SaveNow = LoadNow = 0;
                    lastUN = 0;
                    dampenClick = 0;
                    last_kbd_key = 0;
                    if (dampen_zoom)
                    {
                        dampen_zoom--;
                    }
                }
            }

            // ------------------------------------------------------------------------
            //  Test DS keypresses (ABXY, L/R) and map to corresponding MSX keys
            // ------------------------------------------------------------------------
            key_shift = false;
            key_ctrl = false;
            key_kana = false;
            key_graph = false;

            JoyStickMap = 0;
            nds_key = keysCurrent(); // Get any current keys pressed on the NDS

            // ----------------------------------------------------------------------
            // If we are in Screen 7 'Zoom' mode, the L/R keys take on new meaning...
            // ----------------------------------------------------------------------
            if ((zoom_screen == 2) && (nds_key & (KEY_L | KEY_R)) && ((nds_key & KEY_Y) == 0))
            {
                if (nds_key & KEY_R)
                {
                    if (screenPanX < 256)
                    {
                        screenPanX += 4;
                    }
                }
                if (nds_key & KEY_L)
                {
                    if (screenPanX > 0)
                    {
                        screenPanX -= 4;
                    }
                }
                nds_key &= ~(KEY_L | KEY_R); // We lose the ability for normal handling of these keys
            }

            if ((nds_key & KEY_L) && (nds_key & KEY_R) && (nds_key & KEY_X))
            {
                // Screen swap top/bottom
                lcdSwap();
                WAITVBL; WAITVBL; WAITVBL; WAITVBL; WAITVBL; WAITVBL;
            }
            else if ((nds_key & KEY_L) && (nds_key & KEY_R) && (nds_key & KEY_Y))
            {
                // Snapshot - show the little camera icon for a second or so...
                DSPrint(20, 0, 2, "9:;");
                DSPrint(20, 1, 2, "YZ[");
                screenshot();
                debug_save();
                WAITVBL; WAITVBL; WAITVBL; WAITVBL; WAITVBL; WAITVBL;
                DSPrint(20, 0, 6, "   ");
                DSPrint(20, 1, 6, "   ");
            }
            else if ((nds_key & KEY_R) && (nds_key & (KEY_UP | KEY_DOWN))) // Move screen
            {
                if (!screen_position_dampen)
                {
                    if (nds_key & KEY_UP)
                    {
                        if (myConfig.yOffset < (myConfig.overscan ? 32 : 20))
                        {
                            myConfig.yOffset++;
                        }
                    }
                    else if (nds_key & KEY_DOWN)
                    {
                        if (myConfig.yOffset > 0)
                        {
                            myConfig.yOffset--;
                        }
                    }
                    screen_position_dampen = 8;
                    save_config = 0;
                }
            }
            else if (nds_key & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_START | KEY_SELECT | KEY_R | KEY_L | KEY_X | KEY_Y))
            {
                if (myConfig.dpad == DPAD_ARKANOID) // This is a paddle controller and we nudge the position with left/right
                {
                    if (nds_key & KEY_LEFT)
                    {
                        update_arkanoid_paddle_position(0, (nds_key & KEY_B) ? 10 : 5);
                    }
                    if (nds_key & KEY_RIGHT)
                    {
                        update_arkanoid_paddle_position(1, (nds_key & KEY_B) ? 10 : 5);
                    }
                }
                else if (myConfig.dpad == DPAD_SLIDE_N_GLIDE) // CHUCKIE-EGG Style... hold left/right or up/down for a few frames
                {
                    if (nds_key & KEY_UP)
                    {
                        slide_n_glide_key_up = 12;
                        slide_n_glide_key_down = 0;
                    }
                    if (nds_key & KEY_DOWN)
                    {
                        slide_n_glide_key_down = 12;
                        slide_n_glide_key_up = 0;
                    }
                    if (nds_key & KEY_LEFT)
                    {
                        slide_n_glide_key_left = 12;
                        slide_n_glide_key_right = 0;
                    }
                    if (nds_key & KEY_RIGHT)
                    {
                        slide_n_glide_key_right = 12;
                        slide_n_glide_key_left = 0;
                    }

                    if (slide_n_glide_key_up)
                    {
                        slide_n_glide_key_up--;
                        nds_key |= KEY_UP;
                    }

                    if (slide_n_glide_key_down)
                    {
                        slide_n_glide_key_down--;
                        nds_key |= KEY_DOWN;
                    }

                    if (slide_n_glide_key_left)
                    {
                        slide_n_glide_key_left--;
                        nds_key |= KEY_LEFT;
                    }

                    if (slide_n_glide_key_right)
                    {
                        slide_n_glide_key_right--;
                        nds_key |= KEY_RIGHT;
                    }
                }

                // --------------------------------------------------------------------------------------------------
                // There are 12 NDS buttons (D-Pad, XYAB, L/R and Start+Select) - we allow mapping of any of these.
                // --------------------------------------------------------------------------------------------------
                for (u8 i = 0; i < 12; i++)
                {
                    if (nds_key & NDS_keyMap[i])
                    {
                        if (keyCoresp[myConfig.keymap[i]] < 0xFFFE0000) // Normal Joystick-type key map
                        {
                            JoyStickMap |= keyCoresp[myConfig.keymap[i]];
                        }
                        else // This is a keyboard maping... handle that here... just set the appropriate kbd_key
                        {
                            if ((keyCoresp[myConfig.keymap[i]] >= META_KBD_A) && (keyCoresp[myConfig.keymap[i]] <= META_KBD_Z))
                            {
                                kbd_key = ('A' + (keyCoresp[myConfig.keymap[i]] - META_KBD_A));
                            }
                            else if ((keyCoresp[myConfig.keymap[i]] >= META_KBD_0) && (keyCoresp[myConfig.keymap[i]] <= META_KBD_9))
                            {
                                kbd_key = ('0' + (keyCoresp[myConfig.keymap[i]] - META_KBD_0));
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SPACE)
                            {
                                kbd_key = ' ';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_RETURN)
                            {
                                kbd_key = KBD_KEY_RET;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_ESC)
                            {
                                kbd_key = KBD_KEY_ESC;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SHIFT)
                            {
                                key_shift = 1;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_CTRL)
                            {
                                key_ctrl = 1;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_KANA)
                            {
                                key_kana = 1;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_GRAPH)
                            {
                                key_graph = 1;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_HOME)
                            {
                                kbd_key = KBD_KEY_HOME;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_UP)
                            {
                                kbd_key = KBD_KEY_UP;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_DOWN)
                            {
                                kbd_key = KBD_KEY_DOWN;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_LEFT)
                            {
                                kbd_key = KBD_KEY_LEFT;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_RIGHT)
                            {
                                kbd_key = KBD_KEY_RIGHT;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_PERIOD)
                            {
                                kbd_key = '.';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_COMMA)
                            {
                                kbd_key = ',';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_COLON)
                            {
                                kbd_key = ':';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SEMI)
                            {
                                kbd_key = ';';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_QUOTE)
                            {
                                kbd_key = KBD_KEY_QUOTE;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SLASH)
                            {
                                kbd_key = '/';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_CARET)
                            {
                                kbd_key = '^';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_MINUS)
                            {
                                kbd_key = '-';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_LBRACKET)
                            {
                                kbd_key = '[';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_RBRACKET)
                            {
                                kbd_key = ']';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_BS)
                            {
                                kbd_key = KBD_KEY_BS;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_ATSIGN)
                            {
                                kbd_key = '@';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_YEN)
                            {
                                kbd_key = '|';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_TAB)
                            {
                                kbd_key = KBD_KEY_TAB;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_INS)
                            {
                                kbd_key = KBD_KEY_INS;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_DEL)
                            {
                                kbd_key = KBD_KEY_DEL;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_STOP_BRK)
                            {
                                kbd_key = KBD_KEY_STOP;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F1)
                            {
                                kbd_key = KBD_KEY_F1;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F2)
                            {
                                kbd_key = KBD_KEY_F2;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F3)
                            {
                                kbd_key = KBD_KEY_F3;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F4)
                            {
                                kbd_key = KBD_KEY_F4;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F5)
                            {
                                kbd_key = KBD_KEY_F5;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_PANUP)
                            {
                                if ((VDP[9] & 0x80) || myConfig.overscan)
                                {
                                    temp_offset = -myConfig.yOffset;
                                    slide_dampen = 15;
                                }
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_PANDN)
                            {
                                if ((VDP[9] & 0x80) || myConfig.overscan)
                                {
                                    temp_offset = ((myConfig.overscan ? 32 : 20) - myConfig.yOffset);
                                    slide_dampen = 15;
                                }
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SHOWTOP)
                            {
                                myConfig.yOffset = 0;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SHOWBOT)
                            {
                                myConfig.yOffset = (myConfig.overscan ? 32 : 20);
                            }

                            if (kbd_key != 0)
                            {
                                kbd_keys[kbd_keys_pressed++] = kbd_key;
                            }
                        }
                    }
                }
            }
            else // Nothing pressed... let slide-n-glide counters drift down
            {
                if (slide_n_glide_key_up)
                {
                    slide_n_glide_key_up--;
                }
                if (slide_n_glide_key_down)
                {
                    slide_n_glide_key_down--;
                }
                if (slide_n_glide_key_left)
                {
                    slide_n_glide_key_left--;
                }
                if (slide_n_glide_key_right)
                {
                    slide_n_glide_key_right--;
                }
                last_mapped_key = 0;
            }

            // -----------------------------------------------------------------------------
            // If the user has changed the screen position using the LEFT shoulder key,
            // we need to save that out. But we do so only if they still aren't doing that.
            // -----------------------------------------------------------------------------
            if (save_config)
            {
                if ((nds_key & KEY_L) == 0)
                {
                    if (--save_config == 0)
                    {
                        SaveConfig(FALSE);
                    }
                }
            }

            if (screen_position_dampen)
            {
                if (--screen_position_dampen == 0)
                {
                    save_config = 60; // Save config in 1 second
                }
                else
                {
                    save_config = 0;
                }
            }

            // ------------------------------------------------------------------------------------------
            // Finally, check if there are any buffered keys that need to go into the keyboard handling.
            // ------------------------------------------------------------------------------------------
            ProcessBufferedKeys();

            // ---------------------------------------------------------
            // Accumulate all bits above into the Joystick State var...
            // ---------------------------------------------------------
            JoyState = JoyStickMap;

            // --------------------------------------------------
            // Handle Auto-Fire if enabled in configuration...
            // --------------------------------------------------
            static u8 autoFireTimer[2] = {0, 0};
            if ((myConfig.autoFire & 0x01) && (JoyState & JST_FIRE1)) // Fire Button 1
            {
                if ((++autoFireTimer[0] & 7) > 4)
                {
                    JoyState &= ~JST_FIRE1;
                }
            }
            if ((myConfig.autoFire & 0x02) && (JoyState & JST_FIRE2)) // Fire Button 2
            {
                if ((++autoFireTimer[1] & 7) > 4)
                {
                    JoyState &= ~JST_FIRE2;
                }
            }
        }
    }
}

// ----------------------------------------------------------------------------------------
// We can use this VRAM as semi-fast RAM if needed... right now, no actual need.
// ----------------------------------------------------------------------------------------
void useVRAM(void)
{
    vramSetBankD(VRAM_D_LCD); // Not using this for video but 128K of faster RAM always useful!  Mapped at 0x06860000 -   Used as temp swap area when saving/loading state (256K total)
    vramSetBankE(VRAM_E_LCD); // Not using this for video but 64K of faster RAM always useful!   Mapped at 0x06880000 -   ..
    vramSetBankF(VRAM_F_LCD); // Not using this for video but 16K of faster RAM always useful!   Mapped at 0x06890000 -   ..
    vramSetBankG(VRAM_G_LCD); // Not using this for video but 16K of faster RAM always useful!   Mapped at 0x06894000 -   ..
    vramSetBankH(VRAM_H_LCD); // Not using this for video but 32K of faster RAM always useful!   Mapped at 0x06898000 -   ..
    vramSetBankI(VRAM_I_LCD); // Not using this for video but 16K of faster RAM always useful!   Mapped at 0x068A0000 -   16K available for general use...
}

void ShowInstructions(void)
{
    swiWaitForVBlank();

    bg0b = bgInitSub(0, BgType_Text8bpp, BgSize_T_256x256, 31, 0);
    bg1b = bgInitSub(1, BgType_Text8bpp, BgSize_T_256x256, 29, 0);
    bgSetPriority(bg0b, 1);
    bgSetPriority(bg1b, 0);
    decompress(instructionsTiles, bgGetGfxPtr(bg0b), LZ77Vram);
    decompress(instructionsMap, (void *)bgGetMapPtr(bg0b), LZ77Vram);
    dmaCopy((void *)instructionsPal, (void *)BG_PALETTE_SUB, 256 * 2);
    unsigned short dmaVal = *(bgGetMapPtr(bg1b) + 24 * 32);
    dmaFillWords(dmaVal | (dmaVal << 16), (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);

    WAITVBL; WAITVBL; WAITVBL; WAITVBL; WAITVBL;
    WAITVBL; WAITVBL; WAITVBL; WAITVBL; WAITVBL;

    while ((keysCurrent() & (KEY_START | KEY_A | KEY_B)) == 0)
    {
        ShowRandomPreviewSnaps();
        swiWaitForVBlank();
    }
    // Wait for key to be released before returning
    while ((keysCurrent() & (KEY_TOUCH | KEY_START | KEY_SELECT | KEY_A | KEY_B | KEY_R | KEY_L | KEY_UP | KEY_DOWN)) != 0)
        ;
}

void ShowLoading(void)
{
    swiWaitForVBlank();

    bg0b = bgInitSub(0, BgType_Text8bpp, BgSize_T_256x256, 31, 0);
    bg1b = bgInitSub(1, BgType_Text8bpp, BgSize_T_256x256, 29, 0);
    bgSetPriority(bg0b, 1);
    bgSetPriority(bg1b, 0);
    decompress(loadingTiles, bgGetGfxPtr(bg0b), LZ77Vram);
    decompress(loadingMap, (void *)bgGetMapPtr(bg0b), LZ77Vram);
    dmaCopy((void *)loadingPal, (void *)BG_PALETTE_SUB, 256 * 2);
    unsigned short dmaVal = *(bgGetMapPtr(bg1b) + 24 * 32);
    dmaFillWords(dmaVal | (dmaVal << 16), (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);
}

void restore_lcd(void)
{
    //  Init graphic mode (bitmap mode)
    videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_SPR_1D_LAYOUT | DISPLAY_SPR_ACTIVE);
    videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_SPR_1D_LAYOUT | DISPLAY_SPR_ACTIVE);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankB(VRAM_B_MAIN_SPRITE); // Once emulation of game starts, we steal this back for an additional 128K of VRAM at 0x6820000
    vramSetBankC(VRAM_C_SUB_BG);

    //  Stop blending effect of intro
    REG_BLDCNT = 0;
    REG_BLDCNT_SUB = 0;
    REG_BLDY = 0;
    REG_BLDY_SUB = 0;

    swiWaitForVBlank();

    //  Render the top screen
    bg0 = bgInit(0, BgType_Text8bpp, BgSize_T_256x512, 31, 0);
    bg1 = bgInit(1, BgType_Text8bpp, BgSize_T_256x512, 29, 0);
    bgSetPriority(bg0, 1);
    bgSetPriority(bg1, 0);
    decompress(topscreenTiles, bgGetGfxPtr(bg0), LZ77Vram);
    decompress(topscreenMap, (void *)bgGetMapPtr(bg0), LZ77Vram);
    dmaCopy((void *)topscreenPal, (void *)BG_PALETTE, 256 * 2);
    unsigned short dmaVal = *(bgGetMapPtr(bg0) + 51 * 32);
    dmaFillWords(dmaVal | (dmaVal << 16), (void *)bgGetMapPtr(bg1), 32 * 24 * 2);
}

/*********************************************************************************
 * Init DS Emulator - setup VRAM banks and background screen rendering banks
 ********************************************************************************/
void HachibittoInit(void)
{
    restore_lcd();

    // Clear out media bays
    memset(MyMedia, 0x00, sizeof(MyMedia));

    // Show brief instructions to the user
    if (myGlobalConfig.bShowInstructions)
    {
        ShowInstructions();
        myGlobalConfig.bShowInstructions = 0;
    }
}

// ---------------------------------------------------------------------------
// Setup the bottom screen - mostly for menu, high scores, options, etc.
// ---------------------------------------------------------------------------
void BottomScreenOptions(void)
{
    swiWaitForVBlank();

    bg0b = bgInitSub(0, BgType_Text8bpp, BgSize_T_256x256, 31, 0);
    bg1b = bgInitSub(1, BgType_Text8bpp, BgSize_T_256x256, 29, 0);
    bgSetPriority(bg0b, 1);
    bgSetPriority(bg1b, 0);
    decompress(optionsTiles, bgGetGfxPtr(bg0b), LZ77Vram);
    decompress(optionsMap, (void *)bgGetMapPtr(bg0b), LZ77Vram);
    dmaCopy((void *)optionsPal, (void *)BG_PALETTE_SUB, 256 * 2);

    unsigned short dmaVal = *(bgGetMapPtr(bg1b) + 24 * 32);
    dmaFillWords(dmaVal | (dmaVal << 16), (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);
}

// ---------------------------------------------------------------------------
// Setup the bottom screen for the main menu icons (carts, disk, play button)
// ---------------------------------------------------------------------------
void BottomScreenMainMenu(void)
{
    swiWaitForVBlank();

    bg0b = bgInitSub(0, BgType_Text8bpp, BgSize_T_256x256, 31, 0);
    bg1b = bgInitSub(1, BgType_Text8bpp, BgSize_T_256x256, 29, 0);
    bgSetPriority(bg0b, 1);
    bgSetPriority(bg1b, 0);
    decompress(mainmenuTiles, bgGetGfxPtr(bg0b), LZ77Vram);
    decompress(mainmenuMap, (void *)bgGetMapPtr(bg0b), LZ77Vram);
    dmaCopy((void *)mainmenuPal, (void *)BG_PALETTE_SUB, 256 * 2);

    unsigned short dmaVal = *(bgGetMapPtr(bg1b) + 24 * 32);
    dmaFillWords(dmaVal | (dmaVal << 16), (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);
}

// -----------------------------------------------------------------------------
// Setup the bottom screen for the mini MSX menu (with all the in-game options)
// -----------------------------------------------------------------------------
void BottomScreenMiniMenu(void)
{
    swiWaitForVBlank();

    bg0b = bgInitSub(0, BgType_Text8bpp, BgSize_T_256x256, 31, 0);
    bg1b = bgInitSub(1, BgType_Text8bpp, BgSize_T_256x256, 29, 0);
    bgSetPriority(bg0b, 1);
    bgSetPriority(bg1b, 0);
    decompress(minimenuTiles, bgGetGfxPtr(bg0b), LZ77Vram);
    decompress(minimenuMap, (void *)bgGetMapPtr(bg0b), LZ77Vram);
    dmaCopy((void *)minimenuPal, (void *)BG_PALETTE_SUB, 256 * 2);

    unsigned short dmaVal = *(bgGetMapPtr(bg1b) + 24 * 32);
    dmaFillWords(dmaVal | (dmaVal << 16), (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);
}

// ---------------------------------------------------------------------------
// Keyboard decompression buffers in main RAM.
// Keep these static; do not put them on the stack.
// ---------------------------------------------------------------------------
static u16 kbdMapBuf[32 * 64] __attribute__((aligned(4))); // The two maps are 2K in size

// ---------------------------------------------------------------------------
// Setup the bottom screen with the correct virtual keyboard.
// ---------------------------------------------------------------------------
void BottomScreenKeypad(void)
{
    if (myGlobalConfig.debugger) // Full Z80 Debug overrides things... put up the debugger overlay
    {
        //  Init bottom screen
        decompress(debug_ovlTiles, bgGetGfxPtr(bg0b), LZ77Vram);
        decompress(debug_ovlMap, (void *)bgGetMapPtr(bg0b), LZ77Vram);
        dmaCopy((void *)bgGetMapPtr(bg0b) + 32 * 30 * 2, (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);
        dmaCopy((void *)debug_ovlPal, (void *)BG_PALETTE_SUB, 256 * 2);
    }
    else if (myConfig.keyboard == OVL_ALPHAKBD) // Alpha Simplified Keyboard
    {
        //  Init bottom screen
        decompress(alpha_kbdTiles, bgGetGfxPtr(bg0b), LZ77Vram);
        decompress(alpha_kbdMap, (void *)bgGetMapPtr(bg0b), LZ77Vram);
        dmaCopy((void *)bgGetMapPtr(bg0b) + 32 * 30 * 2, (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);
        dmaCopy((void *)alpha_kbdPal, (void *)BG_PALETTE_SUB, 256 * 2);
    }
    else // Must be OVL_FULLKBD
    {
        const void *tiles;
        const void *map;
        const u16 *pal;

        if (msx_kana_lock)
        {
            tiles = msx_kbd_jaTiles;
            map = msx_kbd_jaMap;
            pal = msx_kbd_jaPal;
        }
        else
        {
            tiles = msx_kbdTiles;
            map = msx_kbdMap;
            pal = msx_kbdPal;
        }

        // --------------------------------------------------------------
        // Decompress into RAM and then do the copy... this prevents the
        // graphics from flashing/garbage while the decompress happens.
        // --------------------------------------------------------------
        memcpy((u8 *)0x06880000, SCC_Memory, 0x10000); // Save RAM so we can reuse the buffer for decompression
        decompress(tiles, SCC_Memory, LZ77);
        decompress(map, kbdMapBuf, LZ77);

        // Get uncompressed byte sizes from the LZ77 headers.
        const u8 *t = (const u8 *)tiles;
        const u8 *m = (const u8 *)map;

        u32 tileSize = t[1] | ((u32)t[2] << 8) | ((u32)t[3] << 16);
        u32 mapSize = m[1] | ((u32)m[2] << 8) | ((u32)m[3] << 16);

        DC_FlushRange(SCC_Memory, tileSize);
        DC_FlushRange(kbdMapBuf, mapSize);

        swiWaitForVBlank(); // Draw during VBLANK to avoid artifacts

        bgHide(bg0b);
        bgHide(bg1b);

        // Copy only the actual decompressed data.
        dmaCopy(SCC_Memory, bgGetGfxPtr(bg0b), tileSize);
        dmaCopy(kbdMapBuf, bgGetMapPtr(bg0b), mapSize);

        // Preserve the original map offset and copy size.
        dmaCopy((void *)bgGetMapPtr(bg0b) + 32 * 30 * 2,
                (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);

        dmaCopy((void *)pal, (void *)BG_PALETTE_SUB, 256 * 2);

        bgShow(bg0b);
        bgShow(bg1b);

        memcpy(SCC_Memory, (u8 *)0x06880000, 0x10000); // Restore previous RAM buffer
    }

    unsigned short dmaVal = *(bgGetMapPtr(bg1b) + 24 * 32);
    dmaFillWords(dmaVal | (dmaVal << 16), (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);

    DisplayStatusLine();
}

/*********************************************************************************
 * Init CPU for the current game
 ********************************************************************************/
void HachibittoInitCPU(void)
{
    //  -----------------------------------------
    //  Init Main Memory and VDP Video Memory
    //  -----------------------------------------
    memset(RAM_Memory, 0x00, sizeof(RAM_Memory));
    memset(VDP_Memory, 0x00, sizeof(VDP_Memory));

    // -----------------------------------------------
    // Init bottom screen do display correct overlay
    // -----------------------------------------------
    BottomScreenKeypad();

    // -----------------------------------------------------
    //  Load the correct Bios ROM for the given machine
    // -----------------------------------------------------
    msx_restore_bios();
}

// -----------------------------------------------------------------
// This is where we handle our pan/scan up and down as well as
// some basic vSYNC timing so that we lock the DS screen updates
// and the MSX screen updates to avoid tearing as much as possible.
// -----------------------------------------------------------------
ITCM_CODE void irqVBlank(void)
{
    int ydyBG = 0x0100; // Default to no screen scale
    int shifty = 0;
    int cyBG = 0;

    // Manage time and true vSync on display output to reduce tearing...
    vusCptVBL++;
    dsVSyncCount++;

    if (!bMainMenuActive) // If main menu active, don't shift anything...
    {
        // ---------------------------------------------------------------------
        // Are we in 192 scanline mode? If so, we don't need ANY screen panning.
        // ---------------------------------------------------------------------
        if (!(VDP[9] & 0x80) && !myConfig.overscan) // High bit set if we are in 212 mode (we ignore this if overscan enabled)
        {
            cyBG = 0;   // No shift
            shifty = 0; // No shift
        }
        else
        {
            cyBG = ((s16)myConfig.yOffset + temp_offset) << 8;

            // --------------------------------------------------------------------
            // Compress screen (yuck!). We do a little bit of DS magic here to
            // shift one of the two DS video pointers so that we end up with a
            // tiny bit of vertical blurring that helps games that have static
            // screens so that some lines repeat a pixel rather than just losing
            // it completely. This produces squatter/fatter text but generally
            // looks better than a line of missing pixels. It's not perfect.
            // --------------------------------------------------------------------
            if (myConfig.scaleScreen)
            {
                ydyBG = (((myConfig.overscan ? 224 : 212) / 192) << 8) | ((myConfig.overscan ? 224 : 212) % 192);
                if (myConfig.yOffset + temp_offset == 0)
                {
                    cyBG = (1 << 8);
                }
                shifty = 20;
            }
        }
    }

    REG_BG2Y = cyBG + shifty;
    REG_BG3Y = cyBG;

    REG_BG2PD = ydyBG;
    REG_BG3PD = ydyBG;

    if (temp_offset)
    {
        if (slide_dampen == 0)
        {
            if (temp_offset > 0)
            {
                temp_offset--;
            }
            else
            {
                temp_offset++;
            }
        }
        else
        {
            slide_dampen--;
        }
    }
}

/*********************************************************************************
 * Program entry point - check if an argument has been passed in probably from TWL++
 ********************************************************************************/
int main(int argc, char **argv)
{
    // Init sound so we get an intro jingle...
    consoleDemoInit();

    // Initialize the old FAT16 library for use with the SD card
    if (!fatInitDefault())
    {
        iprintf("Unable to initialize libfat!\n");
        return -1;
    }

    // -----------------------------------------------------------------
    // Allocate the Large DSi buffer for expanded RAM banking...
    // -----------------------------------------------------------------
    if (isDSiMode())
    {
        MAX_CART_SIZE_KB = 4096; // 4MB is the max MSX cart size without tom-foolery
        ROM_Memory = malloc(MAX_CART_SIZE_KB * 1024);
        BoxArtBuffer = malloc(512 * 1024);
    }
    else // For older DS units... 1MB max
    {
        MAX_CART_SIZE_KB = 1024;
        ROM_Memory = malloc(MAX_CART_SIZE_KB * 1024);
    }

    // Clear out the ROM memory with all 0xFF values...
    memset(ROM_Memory, 0xFF, (MAX_CART_SIZE_KB * 1024));

    // ------------------------------------------------------
    // Both DS-Lite and DSi get a single Disk buffer of 720K
    // ------------------------------------------------------
    DISK_Memory = malloc(MAX_DISK_SIZE_KB * 1024);
    memset(DISK_Memory, 0xFF, (MAX_DISK_SIZE_KB * 1024));

    // Make sure the DISK memory is reset...
    memset(DISK_Memory, 0xFF, (MAX_DISK_SIZE_KB * 1024));

    // Make sure the SRAM is reset... This small buffer is pre-allocated.
    memset(SRAM_Memory, 0xFF, sizeof(SRAM_Memory));

    // ------------------------------------------
    // Load the High Score table into memory...
    // ------------------------------------------
    highscore_init();

    // ---------------------------------------------------------------
    // The main game action is on the top screen, keyboard on bottom
    // ---------------------------------------------------------------
    lcdMainOnTop();

    //  Init timer for frame management
    TIMER2_DATA = 0;
    TIMER2_CR = TIMER_ENABLE | TIMER_DIV_1024;

    // Install the sound driver for emulated AY/SCC sound
    SoundPause();
    dsInstallSoundEmuFIFO();

    //  Show the fade-away intro logo...
    intro_logo();

    SetYtrigger(180); // trigger 12 lines before true DS vsync

    irqSet(IRQ_VBLANK, irqVBlank);
    irqEnable(IRQ_VBLANK);

    // -------------------------------------------
    // Setup our DS VRAM - most banks go unused.
    // -------------------------------------------
    useVRAM();

    // -----------------------------------------------------------------
    // And do an initial load of configuration... We'll match it up
    // with the game that was selected later... Mostly need globals.
    // -----------------------------------------------------------------
    LoadConfigDatabase();

    // -----------------------------------------
    // Do an initial load of the Favorites file
    // -----------------------------------------
    LoadFavorites();

    // ----------------------------------------
    // Get into the best directory possible...
    // ----------------------------------------
    chdir("/roms"); // Try to start in roms area... doesn't matter if it fails
    chdir("msx");   // And try to start in the subdir /msx... doesn't matter if it fails.

    // -----------------------------------------------------
    // Make sure we have true-ish random number generation
    // -----------------------------------------------------
    srand(time(NULL));

    //  ------------------------------------------------------------
    //  We run this loop forever until game exit is selected...
    //  ------------------------------------------------------------
    while (1)
    {
        HachibittoInit();

        while (1)
        {
            SoundPause();

            //  Choose carts and disks and options
            HachibittoMainMenu();

            //  Run Machine
            HachibittoInitCPU();
            HachibittoRunEmu();
        }
    }
    return (0);
}

// -----------------------------------------------------------------------------------------------------------
// Used by the MSX handler to point to different 8K segments of memory as RAM and Carts are swapped in/out.
// This is one of the most important data structures in our system as it universally maps what to read/write.
// Please note: to gain an almost 5% speed-up, wherever these are written, we offset them by the appropriate
// amount. So MemoryMap[0] is offset by -0x0000 and MemoryMap[3] is offset by -0x6000, etc. This way we don't
// need to do any masking when we fetch bytes in the Z80 and those fetches happen a million times per second.
// -----------------------------------------------------------------------------------------------------------
u8 *MemoryMap[8] __attribute__((section(".dtcm"))) = {0, 0, 0, 0, 0, 0, 0, 0};

// ---------------------------------------
// The main Peripheral Interface IO Ports
// ---------------------------------------
u8 Port_PPI_A __attribute__((section(".dtcm"))) = 0x00;
u8 Port_PPI_B __attribute__((section(".dtcm"))) = 0x00;
u8 Port_PPI_C __attribute__((section(".dtcm"))) = 0x00;

// -------------------------------------
// Our venerable Z80 CPU structure!
// -------------------------------------
Z80 CPU __attribute__((section(".dtcm"))); // Put the entire CPU state into fast memory for speed!

// -----------------------------------------
// Some keyboard/joystick mapping vars...
// -----------------------------------------
u8 JoyMode __attribute__((section(".dtcm"))) = 0;   // Joystick Mode (1=Keypad, 0=Joystick)
u32 JoyState __attribute__((section(".dtcm"))) = 0; // Joystick State for P1 and P2

/*********************************************************************************
 * Keyboard Key Buffering Engine...
 ********************************************************************************/
u8 BufferedKeys[32];
u8 BufferedKeysWriteIdx = 0;
u8 BufferedKeysReadIdx = 0;
void BufferKey(u8 key)
{
    BufferedKeys[BufferedKeysWriteIdx] = key;
    BufferedKeysWriteIdx = (BufferedKeysWriteIdx + 1) % 32;
}

// Buffer a whole string worth of characters...
void BufferKeys(char *str)
{
    for (int i = 0; i < strlen(str); i++)
    {
        BufferKey((u8)str[i]);
    }
}

// ---------------------------------------------------------------------------------------
// Called every frame... so 1/50th or 1/60th of a second. We will virtually 'press' and
// hold the key for roughly a tenth of a second and be smart about shift keys...
// ---------------------------------------------------------------------------------------
void ProcessBufferedKeys(void)
{
    static u8 next_dampen_time = 5;
    static u8 dampen = 0;
    static u8 buf_held = 0;
    static u8 buf_shift = 0;
    static u8 buf_ctrl = 0;

    if (++dampen >= next_dampen_time) // Roughly 50ms... experimentally good enough for all systems.
    {
        if (BufferedKeysReadIdx != BufferedKeysWriteIdx)
        {
            buf_held = BufferedKeys[BufferedKeysReadIdx];
            BufferedKeysReadIdx = (BufferedKeysReadIdx + 1) % 32;
            if (buf_held == KBD_KEY_SHIFT)
            {
                buf_shift = 2;
            }
            else
            {
                if (buf_shift)
                {
                    buf_shift--;
                }
            }
            if (buf_held == KBD_KEY_CTRL)
            {
                buf_ctrl = 6;
            }
            else
            {
                if (buf_ctrl)
                {
                    buf_ctrl--;
                }
            }
            if (buf_held == 255)
            {
                buf_held = 0;
                next_dampen_time = 60;
            }
            else
            {
                next_dampen_time = 5;
            }
        }
        else
        {
            buf_held = 0;
        }
        dampen = 0;
    }

    // See if the shift key should be virtually pressed along with this buffered key...
    if (buf_held)
    {
        kbd_keys[kbd_keys_pressed++] = buf_held;
        if (buf_shift)
        {
            key_shift = 1;
        }
        if (buf_ctrl)
        {
            key_ctrl = 1;
        }
    }
}

/*********************************************************************************
 * Init MSX Emulation Engine for that game
 ********************************************************************************/
void msxInit(void)
{
    // We've got some debug data we can use for development... reset these.
    memset(debug, 0x00, sizeof(debug));
    DX = DY = 0;

    // Put up the proper bottom keyboard overlay/graphic
    BottomScreenKeypad();

    // -----------------------------------------------------------------
    // Change graphic mode to initiate emulation.
    // Here we can claim back 128K of VRAM which is otherwise unused
    // but we can use it for fast memory swaps and look-up-tables.
    // -----------------------------------------------------------------
    videoSetMode(MODE_5_2D | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE);
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000); // Bank A starts at 0x06000000
    vramSetBankB(VRAM_B_MAIN_BG_0x06020000); // Bank B starts at 0x06020000

    REG_BG2CNT = BG_BMP8_256x256 | BG_BMP_BASE(0); // Uses Bank A
    REG_BG3CNT = BG_BMP8_256x256 | BG_BMP_BASE(8); // Uses Bank B

    // Alpha blending configuration
    REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG2 | BLEND_DST_BG3;

    // EVA = 8, EVB = 8 (50/50 blend)
    REG_BLDALPHA = (8) | (8 << 8);

    REG_BG2PA = (1 << 8);
    REG_BG2PB = 0;
    REG_BG2PC = 0;
    REG_BG2PD = (1 << 8);
    REG_BG2X = 0;
    REG_BG2Y = 0;

    REG_BG3PA = (1 << 8);
    REG_BG3PB = 0;
    REG_BG3PC = 0;
    REG_BG3PD = (1 << 8);
    REG_BG3X = 0;
    REG_BG3Y = 0;

    memset((u8 *)0x06000000, 0x00, 0x40000); // Ensure screen is clear... (LCD_A and LCD_B)

    // Wipe RAM area for the MSX
    msxWipeRAM();

    // Perform a standard system RESET
    ResetMSX();
}

/*********************************************************************************
 * Update the screen for the current cycle. On the DSi this will generally
 * be called right after swiWaitForVBlank() in VDP9938a.c which will help
 * reduce visual tearing and other artifacts. It's not strictly necessary
 * and that does slow down the loop a bit... but DSi can handle it.
 ********************************************************************************/
void msxUpdateScreen(void)
{
    if (!skip_render)
    {
        // If we are set for overscan but the overscan trick wasn't used, clear out the bottom of the XBuf[] buffer
        if (myConfig.overscan && !overscan_trick)
        {
            memset(XBuf[(drawn_frame_number + 1) & 1] + 256 * ((VDP[9] & 0x80) ? 212 : 192), 0x00, 256 * ((VDP[9] & 0x80) ? 12 : 32));
        }
        overscan_trick = 0;

        // -----------------------------------------------------------------------------------------
        // Alternate frame buffers except for aggressive skip (then just render from the A buffer).
        // The main reason we use this is that the async render does not understand cached memory
        // and if there is any memory that is not yet committed, it would not render properly. By
        // rendering the LAST frame (not the current frame) we virtually ensure that the memory is
        // stale enough to be rendered properly. Yes, this means that the output is 1 frame delayed
        // but nobody notices 1/60th of a second and it's all butter smooth to the player.
        // -----------------------------------------------------------------------------------------
        if (drawn_frame_number & render_blended)
        {
            dmaCopyWordsAsynch(2, (u32 *)XBuf[(drawn_frame_number + 1) & 1], (u32 *)DS_LCD_VRAM_2, (myConfig.overscan ? (256 * 224) : (256 * 212)));
        }
        else
        {
            dmaCopyWordsAsynch(2, (u32 *)XBuf[(drawn_frame_number + 1) & 1], (u32 *)DS_LCD_VRAM_1, (myConfig.overscan ? (256 * 224) : (256 * 212)));
        }
    }

    skip_render = 0;
}

// -------------------------------------------------------------------------
// Return the master CRC32 used for high scores and Configuration. This is
// always the Cart 1 unless that's not loaded in which case it's the Disk.
// We never use Cart 2 as the master slot (only used for cart-combos).
// -------------------------------------------------------------------------
u32 GetMasterCRC(void)
{
    if (MyMedia[MEDIA_CART1].filecrc)
    {
        return MyMedia[MEDIA_CART1].filecrc;
    }
    else if (MyMedia[MEDIA_DISK].filecrc)
    {
        return MyMedia[MEDIA_DISK].filecrc;
    }

    return 0x00000000;
}

// -------------------------------------------------------------------------
// Return the master filename needed as the base filename for .sav and .srm
// Always the Cart 1 unless that's not loaded in which case it's the Disk.
// We never use Cart 2 as the master slot (only used for cart-combos).
// -------------------------------------------------------------------------
char *GetMasterFilename(void)
{
    if (MyMedia[MEDIA_CART1].filecrc)
    {
        return MyMedia[MEDIA_CART1].filename;
    }
    else if (MyMedia[MEDIA_DISK].filecrc)
    {
        return MyMedia[MEDIA_DISK].filename;
    }

    return "No File";
}

// -------------------------------------------------------------------------
// Return the master path needed as the directory needed for .sav and .srm
// Always the Cart 1 unless that's not loaded in which case it's the Disk.
// We never use Cart 2 as the master slot (only used for cart-combos).
// -------------------------------------------------------------------------
char *GetMasterPath(void)
{
    if (MyMedia[MEDIA_CART1].filecrc)
    {
        return MyMedia[MEDIA_CART1].filepath;
    }
    else if (MyMedia[MEDIA_DISK].filecrc)
    {
        return MyMedia[MEDIA_DISK].filepath;
    }

    return "";
}

/** LoadGameIntoMedia() **************************************************************/
/** Open a rom file from file system and load it into the appropriate buffer...     **/
/** After this call, the filename, file size and CRC32 are set for this media slot. **/
/*************************************************************************************/
void LoadGameIntoMedia(u8 media_id, char *filename)
{
    ShowLoading(); // Put up a loading screen so user is aware...

    // Save the directory in which this file was found...
    getcwd(MyMedia[media_id].filepath, MAX_PATH_NAME_LEN);

    // Save the current game filename. We uppercase it so it's easier to search the title...
    strcpy(MyMedia[media_id].filename, filename);
    for (int i = 0; i < strlen(MyMedia[media_id].filename); i++)
    {
        MyMedia[media_id].filename[i] = toupper(MyMedia[media_id].filename[i]); // Uppercase string
    }

    // Now open and read the file... grab file size and CRC32 as well...
    if (media_id == MEDIA_CART1)
    {
        MyMedia[media_id].filesize = ReadFileCarefully(MyMedia[media_id].filename, ROM_Memory, (MAX_CART_SIZE_KB * 1024), 0, &MyMedia[media_id].filecrc);
    }
    else if (media_id == MEDIA_CART2)
    {
        MyMedia[media_id].filesize = ReadFileCarefully(MyMedia[media_id].filename, ROM_Memory + ((MAX_CART_SIZE_KB * 1024) / 2), (MAX_CART_SIZE_KB * 1024) / 2, 0, &MyMedia[media_id].filecrc);
    }
    else // Must be MEDIA_DISK
    {
        strcpy(MyMedia[MEDIA_DISK].currdisk, MyMedia[media_id].filename); // This is the current disk name - since disks can be wapped.
        MyMedia[media_id].filesize = ReadFileCarefully(MyMedia[media_id].filename, DISK_Memory, (MAX_DISK_SIZE_KB * 1024), 0, &MyMedia[media_id].filecrc);
    }

    // --------------------------------------------------------
    // Load the Configuration for this new cart/disk layout...
    // --------------------------------------------------------
    FindConfig();

    // ----------------------------------------------------------------------
    // Let the Loading Screen sit for a second or two so the user sees it...
    // ----------------------------------------------------------------------
    if (MyMedia[media_id].filesize <= (256 * 1024)) // Smaller files... add some wait on the Loading Screen
    {
        WAITVBL; WAITVBL; WAITVBL; WAITVBL;
        WAITVBL; WAITVBL; WAITVBL; WAITVBL;
    }
    else if (MyMedia[media_id].filesize <= (512 * 1024)) // Slightly larger files... add less wait
    {
        WAITVBL; WAITVBL; WAITVBL; WAITVBL;
    }
    // And for even bigger files, add no artificial delay (the file load above was long enough!)
}

// -------------------------------------------------------------------------
// For various machines, we have patched the BIOS so that we trap calls
// to various I/O routines: namely cassette access. We handle that here.
// -------------------------------------------------------------------------
void PatchZ80(register Z80 *r)
{
    // Not used at this time...
}

/** LoopZ80() *************************************************/
/** Z80 emulation calls this function periodically to run    **/
/** Z80 code for the loaded ROM. It runs code refreshing the **/
/** VDP and checking for interrupt requests.                 **/
/**************************************************************/
int mid_frame_interrupt = 0;
u32 LoopZ80(void)
{
    // Execute 1 scanline worth of CPU instructions
    u32 cycles_to_process = VDP9938_CLOCKS_PER_LINE + (myConfig.cpuBoost * 57) + CPU.CycleDeficit;
    CPU.CycleDeficit = ExecZ80(cycles_to_process);

    if (mid_frame_interrupt)
    {
        if (--mid_frame_interrupt == 0)
        {
            RefereshPreviousLine();
        }
    }

    // Grab audio samples for this scanline
    if (isDSiMode())
    {
        ProcessDirectAudio();
    }

    // Run the FDC engine
    LoopFDC();

    // Run the VDP engine
    LoopVDP();

    // Refresh VDP
    Loop9938();

    // ------------------------------------------------------------------------------------
    // Zanac EX has a graphical glitch on return to main menu due to sensitive timing.
    // For this game only, we drop the FH flag immediately if IE1 is clear. This is a
    // game-specific workaround for an unresolved VDP timing discrepancy. Fix this someday.
    // ------------------------------------------------------------------------------------
    if (zanac_glitch_patch)
    {
        if (!(VDP[0] & 0x10)) // Clear the FH flag if IE1 is explicitly lowered by the CPU.
        {
            VDPStatus[1] &= 0xFE;       // If IE1 (R#0 bit 4) is lowered, FH flag is forced low
            SetVDPIRQ(VDP_IRQ_LINE, 0); // And ensure the IRQ is not set for the Line Interrupt
        }
    }

    // Generate an interrupt if called for...
    if (CPU.IRequest != INT_NONE)
    {
        IntZ80(&CPU, CPU.IRequest);
        CPU.NumInts++;                                                                   // Track Interrupt Requests
        if (((CurLine - 1) >= VDP9938_START_LINE) && ((CurLine - 1) < VDP9938_END_LINE)) // Is this a mid-frame line interrupt?
        {
            mid_frame_interrupt = 3; // Let CPU run for 2 lines then redraw the previous 2...
        }
    }

    // Drop out unless end of screen is reached
    if (CurLine == VDP9938_END_LINE)
    {
        CPU.CycleDeficit = 0; // Start fresh at the VBlank boundary. Keeps frame timing consistent.
        return 0;
    }
    return 1;
}

// -----------------------------------------------------------------------
// A bit of memory reserved to store debug information... Reduce this
// when we are stable and don't have much need for it anymore.
// -----------------------------------------------------------------------

#define MAX_DPRINTF_STR_SIZE 128
#define MAX_DEBUG_BUF_SIZE (16 * 1024)

char DEBUG_BUFFER[MAX_DEBUG_BUF_SIZE];
u32 debug_len = 0;
extern char szName[]; // Reuse buffer which has no other in-game use

void debug_init()
{
    debug_len = 0;
    memset(DEBUG_BUFFER, 0x00, MAX_DEBUG_BUF_SIZE);
}

void debug_printf(const char *str, ...)
{
    if (debug_len >= (MAX_DEBUG_BUF_SIZE - MAX_DPRINTF_STR_SIZE))
    {
        return; // No more room!
    }

    va_list ap = {0};

    va_start(ap, str);
    vsnprintf(szName, MAX_DPRINTF_STR_SIZE, str, ap);
    va_end(ap);

    strcat(DEBUG_BUFFER + debug_len, szName);
    debug_len += strlen(szName);
}

void debug_save()
{
    if (debug_len > 0) // Only if we have debug data to write...
    {
        FILE *fp = fopen("debug.log", "w");
        if (fp)
        {
            fwrite(DEBUG_BUFFER, 1, debug_len, fp);
            fclose(fp);
        }
    }
}

// End of file
