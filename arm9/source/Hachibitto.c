// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave (Phoenix-Edition),
// Alekmaul (original port) and Marat Fayzullin (ColEM core) are thanked profusely.
//
// The Hachibitto emulator is offered as-is, without any warranty. Please see readme.md
// =====================================================================================
#include <nds.h>
#include <nds/fifomessages.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <fat.h>
#include <maxmod9.h>

#include "Hachibitto.h"
#include "highscore.h"
#include "MSX_generic.h"
#include "cpu/vdp9938/vdp9938.h"
#include "intro.h"
#include "msx_kbd.h"
#include "alpha_kbd.h"
#include "debug_ovl.h"
#include "options.h"
#include "topscreen.h"
#include "fdc.h"
#include "V9938.h"
#include "CRC32.h"

#include "soundbank.h"
#include "soundbank_bin.h"
#include "screenshot.h"
#include "cpu/z80/Z80_interface.h"
#include "cpu/scc/SCC.h"

#include "printf.h"

u32 debug[0x10]={0};
u32 DX = 0;
u32 DY = 0;

volatile u32 dsVSyncCount = 0;
u32 last_vsync_count = 0xFEEDBEEF;
s16 temp_offset   __attribute__((section(".dtcm"))) = 0;
u16 slide_dampen  __attribute__((section(".dtcm"))) = 0;
u16 DelayFirstOutput __attribute__((section(".dtcm"))) = 0;

// -------------------------------------------------------------------------------------------
// All emulated systems have ROM, RAM and possibly BIOS or SRAM. So we create generic buffers
// for all this here... these are sized big enough to handle the largest memory necessary
// to render games playable. There are a few MSX games that are larger than 512k but they
// are mostly demos or foreign-language adventures... not enough interest to try to squeeze
// in a larger ROM buffer to include them - we are still trying to keep compatible with the
// smaller memory model of the original DS/DS-LITE.
//
// These memory buffers will be pointed to by the MemoryMap[] array. This array contains 8
// pointers that can break down the Z80 memory into 8k chunks.
// -------------------------------------------------------------------------------------------

u32 MAX_CART_SIZE = 1280;                                     // 1.25MB of ROM Cart... for DSi we will bump this up to 4MB
u8 *ROM_Memory;                                               // ROM Carts up to 1MB/4MB (that's pretty huge in the Z80 world!)
u8 RAM_Memory[0x10000]                ALIGN(32) = {0};        // RAM is 64K for the MSX2 (this is the minimum spec)
u8 BIOS_Memory[0x10000]               ALIGN(32) = {0};        // To hold our BIOS and related OS memory (64K as the BIOS  for various machines ends up in different spots)
u8 SRAM_Memory[0x4000]                ALIGN(32) = {0};        // SRAM up to 16K for the few carts which use it (e.g. MSX Deep Dungeon II, Hydlide II, etc)

u8 io_show_status = 0;  // Used to indicate a RD/WR status for various disk/tape activities

static char cmd_line_file[256];
char initial_file[MAX_ROM_NAME] = "";
char initial_path[MAX_ROM_NAME] = "";

u8 msx_caps_lock        = 0;
u8 msx_kana_lock        = 0;
u8 write_NV_counter     = 0;

u8   disk_unsaved_data[3]      = {0,0,0};
u32  disk_last_size[3]         = {0,0,0};
char disk_last_file[3][256]    = {"","",""};
char disk_last_path[3][256]    = {"","",""};

// --------------------------------------------------------------------------
// For machines that have a full keybaord, we use the Left and Right
// shoulder buttons on the NDS to emulate the SHIFT and CTRL keys...
// --------------------------------------------------------------------------
u8 key_shift __attribute__((section(".dtcm"))) = false;
u8 key_ctrl  __attribute__((section(".dtcm"))) = false;
u8 key_code  __attribute__((section(".dtcm"))) = false;
u8 key_graph __attribute__((section(".dtcm"))) = false;
u8 key_dia   __attribute__((section(".dtcm"))) = false;

// ---------------------------------------------------------------------------
// Some timing and frame rate comutations to keep the emulation on pace...
// ---------------------------------------------------------------------------
u16 emuFps          __attribute__((section(".dtcm"))) = 0;
u16 emuActFrames    __attribute__((section(".dtcm"))) = 0;
u16 timingFrames    __attribute__((section(".dtcm"))) = 0;

u8 soundEmuPause     __attribute__((section(".dtcm"))) = 1;       // Set to 1 to pause (mute) sound, 0 is sound unmuted (sound channels active)

// -----------------------------------------------------------------------------
// This set of critical vars is what determines the game type is... ROM vs DSK
// -----------------------------------------------------------------------------
u8 msx_mode          __attribute__((section(".dtcm"))) = 0;       // Set to 1 when a .msx game is loaded for basic MSX support

u8 kbd_key           __attribute__((section(".dtcm"))) = 0;       // 0 if no key pressed, othewise the ASCII key (e.g. 'A', 'B', '3', etc)
u16 nds_key          __attribute__((section(".dtcm"))) = 0;       // 0 if no key pressed, othewise the NDS keys from keysCurrent() or similar
u8 last_mapped_key   __attribute__((section(".dtcm"))) = 0;       // The last mapped key which has been pressed - used for key click feedback
u8 kbd_keys_pressed  __attribute__((section(".dtcm"))) = 0;       // Each frame we check for keys pressed - since we can map keyboard keys to the NDS, there may be several pressed at once
u8 kbd_keys[12]      __attribute__((section(".dtcm")));           // Up to 12 possible keys pressed at the same time (we have 12 NDS physical buttons though it's unlikely that more than 2 or maybe 3 would be pressed)

u8 bStartSoundEngine = false;  // Set to true to unmute sound after 1 frame of rendering...
int bg0, bg1, bg0b, bg1b;      // Some vars for NDS background screen handling
volatile u16 vusCptVBL = 0;    // We use this as a basic timer for the Mario sprite... could be removed if another timer can be utilized
u8 touch_debounce = 0;         // A bit of touch-screen debounce
u8 key_debounce = 0;           // A bit of key debounce
u8 playingSFX = 0;             // To prevent sound effects like disk/tape loading from happening too frequently

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
    JST_FIRE2,
    JST_FIRE1,

    JST_UP      << 16,      // P2 versions of the above...
    JST_DOWN    << 16,
    JST_LEFT    << 16,
    JST_RIGHT   << 16,
    JST_FIRE2   << 16,
    JST_FIRE1   << 16,

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
    META_KBD_CODE,
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
    META_KBD_BACKSLASH,
    META_KBD_PLUS,
    META_KBD_MINUS,
    META_KBD_LBRACKET,
    META_KBD_RBRACKET,
    META_KBD_CARET,
    META_KBD_ASTERISK,
    META_KBD_ATSIGN,
    META_KBD_BS,
    META_KBD_TAB,
    META_KBD_INS,
    META_KBD_DEL,
    META_KBD_CLR,
    META_KBD_STOP_BRK,
    META_KBD_F1,
    META_KBD_F2,
    META_KBD_F3,
    META_KBD_F4,
    META_KBD_F5,
};

static char tmp[64];    // For various sprintf() calls

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
#define sample_rate         (27965)    // To match the AY driver - this is good enough quality for the DS
#define buffer_size         (512+16)   // Enough buffer that we don't have to fill it too often. Must be multiple of 16.

mm_ds_system sys   __attribute__((section(".dtcm")));
mm_stream myStream __attribute__((section(".dtcm")));

s16 mixbuf1[4096+64];      // When we have SN and AY sound we have to mix 3+3 channels
s16 mixbuf2[4096+64];      // into a single output so we render to mix buffers first.

u16 mixer_read      __attribute__((section(".dtcm"))) = 0;
u16 mixer_write     __attribute__((section(".dtcm"))) = 0;
u8 wave_direct_skip __attribute__((section(".dtcm"))) = 0;

// -------------------------------------------------------------------------------------------
// maxmod will call this routine when the buffer is half-empty and requests that
// we fill the sound buffer with more samples. They will request 'len' samples and
// we will fill exactly that many. If the sound is paused, we fill with 'mute' samples.
// -------------------------------------------------------------------------------------------
s16 last_sample __attribute__((section(".dtcm"))) = 0;
int breather    __attribute__((section(".dtcm"))) = 0;
ITCM_CODE mm_word OurSoundMixer(mm_word len, mm_addr dest, mm_stream_formats format)
{
    if (soundEmuPause)  // If paused, just "mix" in mute sound chip... all channels are OFF
    {
        s16 *p = (s16*)dest;
        for (int i=0; i<len*2; i++)
        {
           *p++ = last_sample;      // To prevent pops and clicks... just keep outputting the last sample
        }
    }
    else
    {
        if (msx_scc_enable)   // If SCC is enabled, we need to mix the AY with the SCC chips
        {
            ay38910Mixer(len*2, mixbuf1, &myAY);
            SCCMixer(len*4, mixbuf2, &mySCC);

            s16 *p = (s16*)dest;
            int j=0;
            for (int i=0; i<len*2; i++)
            {
                // ------------------------------------------------------------------------
                // We normalize the samples and mix them carefully to minimize clipping...
                // ------------------------------------------------------------------------
                s32 combined = (mixbuf1[i]) + ((mixbuf2[j] + mixbuf2[j+1])/2) + 32768;
                j+=2;
                if (combined >  32767) combined = 32767;
                *p++ = (s16)combined;
            }
            p--; last_sample = *p;
        }
        else  // Pretty simple... just AY
        {
            ay38910Mixer(len*2, dest, &myAY);
            last_sample = ((s16*)dest)[len*2 - 1];
        }
    }

    return  len;
}

// -------------------------------------------------------------------------------------------
// Setup the maxmod audio stream - this will be a 16-bit Stereo PCM output at 55KHz which
// sounds about right for the MSX audio data stream.
// -------------------------------------------------------------------------------------------
void setupStream(void)
{
  //----------------------------------------------------------------
  //  initialize maxmod with our small 5-effect soundbank
  //----------------------------------------------------------------
  mmInitDefaultMem((mm_addr)soundbank_bin);

  mmLoadEffect(SFX_CLICKNOQUIT);
  mmLoadEffect(SFX_KEYCLICK);
  mmLoadEffect(SFX_MUS_INTRO);
  mmLoadEffect(SFX_FLOPPY);

  //----------------------------------------------------------------
  //  open stream
  //----------------------------------------------------------------
  myStream.sampling_rate  = sample_rate;            // sample_rate for the CV to match the SN/AY drivers
  myStream.buffer_length  = buffer_size;            // buffer length = (512+16)
  myStream.callback       = OurSoundMixer;          // set callback function
  myStream.format         = MM_STREAM_16BIT_STEREO; // format = stereo 16-bit
  myStream.timer          = MM_TIMER0;              // use hardware timer 0
  myStream.manual         = false;                  // use automatic filling
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

void sound_chip_reset()
{
  memset(mixbuf1, 0x00, sizeof(mixbuf1));
  memset(mixbuf2, 0x00, sizeof(mixbuf2));
  mixer_read=0;
  mixer_write=0;

  //  --------------------------------------------------------------------
  //  The AY sound chip is for Super Game Module and MSX sound handling
  //  --------------------------------------------------------------------
  ay38910Reset(&myAY);             // Reset the "AY" sound chip
  ay38910IndexW(0x07, &myAY);      // Register 7 is ENABLE
  ay38910DataW(0x3F, &myAY);       // All OFF (negative logic)
  ay38910Mixer(8, mixbuf2, &myAY); // Do an initial mix conversion to clear the output

  // -----------------------------------------------------------------
  // The SCC sound chip is just for a few select Konami MSX1 games
  // -----------------------------------------------------------------
  SCCReset(&mySCC);

  SCCWrite(0x00, 0x988A, &mySCC);
  SCCWrite(0x00, 0x988B, &mySCC);
  SCCWrite(0x00, 0x988C, &mySCC);
  SCCWrite(0x00, 0x988D, &mySCC);
  SCCWrite(0x00, 0x988E, &mySCC);
  SCCWrite(0x00, 0x988F, &mySCC);

  SCCMixer(16, mixbuf2, &mySCC);     // Do an initial mix conversion to clear the output
}

// -----------------------------------------------------------------------
// We setup the sound chips - disabling all volumes to start.
// -----------------------------------------------------------------------
void dsInstallSoundEmuFIFO(void)
{
  SoundPause();             // Pause any sound output
  sound_chip_reset();       // Reset the SN, AY and SCC chips
  setupStream();            // Setup maxmod stream...
  bStartSoundEngine = true; // Volume will 'unpause' after 1 frame in the main loop.
}

//*****************************************************************************
// Reset the MSX - mostly CPU and memory...
//*****************************************************************************

static u8 last_msx_mode = 0;
static u8 last_msx_scc_enable = 0;

// --------------------------------------------------------------
// When we first load a ROM/CASSETTE or when the user presses
// the RESET button on the touch-screen...
// --------------------------------------------------------------
void ResetMSX(void)
{
  JoyMode=JOYMODE_JOYSTICK;             // Joystick mode key
  JoyState = 0x00000000;                // Nothing pressed to start

  Reset9938();                          // Reset the video chip

  sound_chip_reset();                   // Reset the SN, AY and SCC chips

  Z80_Interface_Reset();                // Reset the Z80 Interface
  ResetZ80(&CPU);                       // Reset the Z80 CPU core

  msx_reset();                          // Reset the MSX specific vars

  disk_unsaved_data[0] = 0;             // No unsaved tape/disk data to start
  disk_unsaved_data[1] = 0;             // No unsaved tape/disk data to start
  msx_caps_lock = 0;                    // MSX CAPS lock off
  msx_kana_lock = 0;                    // MSX KANA lock off

  write_NV_counter=0;                   // Nothing to write for EEPROM yet

  playingSFX = 0;                       // No sound effects playing yet

  msxWipeRAM();                         // Wipe main RAM area (config chooses zero or random)
  msx_restore_bios();                   // Put the BIOS back in place and point to it

  // -----------------------------------------------------------
  // Timer 1 is used to time frame-to-frame of actual emulation
  // -----------------------------------------------------------
  TIMER1_CR = 0;
  TIMER1_DATA=0;
  TIMER1_CR=TIMER_ENABLE  | TIMER_DIV_1024;

  // -----------------------------------------------------------
  // Timer 2 is used to time once per second events
  // -----------------------------------------------------------
  TIMER2_CR=0;
  TIMER2_DATA=0;
  TIMER2_CR=TIMER_ENABLE  | TIMER_DIV_1024;
  timingFrames  = 0;
  emuFps=0;

  last_msx_mode = 0;
  last_msx_scc_enable = 0;
}

//*********************************************************************************
// A mini Z80 debugger of sorts. Put out some Z80, VDP and SGM/Bank info on
// screen every frame to help us debug some of the problem games. This is enabled
// via global configuration for debugger.
//*********************************************************************************
extern u8 *fake_heap_end;     // current heap start
extern u8 *fake_heap_start;   // current heap end

u8* getHeapStart() {return fake_heap_start;}
u8* getHeapEnd()   {return (u8*)sbrk(0);}
u8* getHeapLimit() {return fake_heap_end;}

int getMemUsed() { // returns the amount of used memory in bytes
   struct mallinfo mi = mallinfo();
   return mi.uordblks;
}

int getMemFree() { // returns the amount of free memory in bytes
   struct mallinfo mi = mallinfo();
   return mi.fordblks + (getHeapLimit() - getHeapEnd());
}

void ShowDebugZ80(void)
{
    u8 idx=1;

    if (myGlobalConfig.debugger == 3)
    {
        sprintf(tmp, "VDP: %02X %02X %02X %02X %02X %02X %02X %02X", VDP[0],VDP[1],VDP[2],VDP[3], VDP[4],VDP[5],VDP[6],VDP[7]);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "VDP: %02X %02X %02X %02X %02X %02X %02X %02X", VDP[8],VDP[9],VDP[10],VDP[11], VDP[12],VDP[13],VDP[14],VDP[15]);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "VDP: %02X %02X %02X %02X %02X %02X %02X %02X", VDP[16],VDP[17],VDP[18],VDP[19], VDP[20],VDP[21],VDP[22],VDP[23]);
        DSPrint(0,idx++,7, tmp);

        sprintf(tmp, "VStat %02X %02X %02X %02X Data=%02X", VDPStatus[0], VDPStatus[1], VDPStatus[2], VDPStatus[3], VDPDlatch);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "VAddr %08X", (VDP[14]<<14)+(int)VAddr);
        DSPrint(0,idx++,7, tmp);
        idx++;

        sprintf(tmp, "Z80PC %04X", CPU.PC.W);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "Z80SP %04X", CPU.SP.W);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "Z80AF %04X", CPU.AF.W);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "Z80BC %04X", CPU.BC.W);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "Z80DE %04X", CPU.DE.W);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "IRQ %04X %d", CPU.IRequest, (CPU.NumInts % 99999));
        DSPrint(0,idx++,7, tmp);
        idx++;

        sprintf(tmp, "AY:%02X %02X %02X %02X", myAY.ayRegs[0], myAY.ayRegs[1], myAY.ayRegs[2], myAY.ayRegs[3]);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "AY:%02X %02X %02X %02X", myAY.ayRegs[4], myAY.ayRegs[5], myAY.ayRegs[6], myAY.ayRegs[7]);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "AY:%02X %02X %02X %02X", myAY.ayRegs[8], myAY.ayRegs[9], myAY.ayRegs[10], myAY.ayRegs[11]);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "AY:%02X %02X %02X %02X", myAY.ayRegs[12], myAY.ayRegs[13], myAY.ayRegs[14], myAY.ayRegs[15]);
        DSPrint(0,idx++,7, tmp);

        idx++;
        sprintf(tmp, "Screen %02X", ScrMode); DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "PPI A=%02X B=%02X",Port_PPI_A,Port_PPI_B);    DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "PPI C=%02X       ",Port_PPI_C); DSPrint(0,idx++,7, tmp);

        idx = 6;
        for (u8 i=0; i< 16; i++)
        {
            sprintf(tmp, "D%-2d %-8lu %04X", i, debug[i], (u16)debug[i]); DSPrint(15,idx++,7, tmp);
        }
    }
    else
    {
        idx = 1;
        for (u8 i=0; i<4; i++)
        {
            sprintf(tmp, "D%d %-7ld %04lX  D%d %-7ld %04lX", i, (s32)debug[i], (debug[i] < 0xFFFF ? debug[i]:0xFFFF), 4+i, (s32)debug[4+i], (debug[4+i] < 0xFFFF ? debug[4+i]:0xFFFF));
            DSPrint(0,idx++,7, tmp);
        }

        if (msx_mode == 3)
        {
            sprintf(tmp, "FD.ST=%02X CM=%02X TR=%02X SI=%02X SE=%02X", FDC.status, FDC.command, FDC.track, FDC.side, FDC.sector); DSPrint(0,idx++,7, tmp);
        }
    }
    idx++;
}


// ------------------------------------------------------------
// The status line shows the status of the Super Game Moudle,
// AY sound chip support and MegaCart support.  Game players
// probably don't care, but it's really helpful for devs.
// ------------------------------------------------------------
void DisplayStatusLine(bool bForce)
{
    if (myGlobalConfig.emuText == 0) return;

    if (msx_mode)
    {
        if ((last_msx_mode != msx_mode) || bForce)
        {
            last_msx_mode = msx_mode;
        }
        if (last_msx_scc_enable != msx_scc_enable)
        {
            // SCC has a little cool graphic to go with it!
            DSPrint(20,0, (msx_scc_enable ? 2:0), (msx_scc_enable ? "012":"   "));
            DSPrint(20,1, (msx_scc_enable ? 2:0), (msx_scc_enable ? "PQR":"   "));
            last_msx_scc_enable = msx_scc_enable;
        }
        if (write_NV_counter > 0)
        {
            --write_NV_counter;
            if (write_NV_counter == 0)
            {
                // Save EE now!
                msxSaveEEPROM();
            }
            DSPrint(21,0,6, (write_NV_counter ? "EE":"  "));
        }
        if (msx_mode == 3)
        {
            if (io_show_status)
            {
                if (io_show_status == 5) {DSPrint(21,0,6, "WR"); io_show_status = 3;}
                if (io_show_status == 4) {DSPrint(21,0,6, "RD"); io_show_status = 3;}
                if (io_show_status == 3)
                {
                    if (!myGlobalConfig.diskSfxMute) mmEffect(SFX_FLOPPY);
                }
                io_show_status--;
            }
            else
            {
                DSPrint(8,0,6, "          ");
            }
        }

        if (myConfig.keyboard == OVL_FULLKBD) // Is full keyboard showing?
        {
            // Caps Lock
            DSPrint(1,23,0, (msx_caps_lock ? "@":" "));
            DSPrint(2,23,(msx_caps_lock ? 2:0), (msx_caps_lock ? "@":" "));

            // KANA Lock
            if (msx_japanese_matrix)
            {
                msx_kana_lock = (myAY.ayPortBOut & 0x80) ? 0:1;
                DSPrint(22,23,(msx_kana_lock ? 2:0), (msx_kana_lock ? "^":" "));
            }
        }
    }
}


#define MENU_ACTION_END             255 // Always the last sentinal value
#define MENU_ACTION_EXIT            0   // Exit the menu
#define MENU_ACTION_SAVE            1   // Save Disk or Cassette in primary drive
#define MENU_ACTION_SWAP            2   // Swap Disk or Cassette in primary drive
#define MENU_ACTION_EJECT           3   // Eject Disk or Cassette in primary drive

#define MENU_ACTION_SAVE1           4   // Save Disk or Cassette in secondary drive
#define MENU_ACTION_SWAP1           5   // Swap Disk or Cassette in secondary drive
#define MENU_ACTION_EJECT1          6   // Eject Disk or Cassette in secondary drive

#define MENU_ACTION_SAVE2           7   // Save Disk or Cassette in third drive
#define MENU_ACTION_EJECT2          9   // Eject Disk or Cassette in third drive

#define MENU_ACTION_RESET           98  // Reset the machine
#define MENU_ACTION_SKIP            99  // Skip this MENU choice

typedef struct
{
    char *menu_string;
    u8    menu_action;
} MenuItem_t;

// ------------------------------------------------------------------------
// Show the Mini Menu - highlight the selected row.
// ------------------------------------------------------------------------
u8 mini_menu_items = 0;
void MiniMenuShow(bool bClearScreen, u8 sel)
{
    mini_menu_items = 0;
    if (bClearScreen)
    {
      // ---------------------------------------------------
      // Put up a generic background for this mini-menu...
      // ---------------------------------------------------
      BottomScreenOptions();
    }

    DSPrint(8,7,6,                                           " DS MINI MENU  ");
    DSPrint(8,9+mini_menu_items,(sel==mini_menu_items)?2:0,  " RESET  GAME   ");  mini_menu_items++;
    DSPrint(8,9+mini_menu_items,(sel==mini_menu_items)?2:0,  " QUIT   GAME   ");  mini_menu_items++;
    DSPrint(8,9+mini_menu_items,(sel==mini_menu_items)?2:0,  " HIGH   SCORE  ");  mini_menu_items++;
    DSPrint(8,9+mini_menu_items,(sel==mini_menu_items)?2:0,  " GAME   OPTIONS");  mini_menu_items++;
    DSPrint(8,9+mini_menu_items,(sel==mini_menu_items)?2:0,  " DEFINE KEYS   ");  mini_menu_items++;
    DSPrint(8,9+mini_menu_items,(sel==mini_menu_items)?2:0,  " SAVE   STATE  ");  mini_menu_items++;
    DSPrint(8,9+mini_menu_items,(sel==mini_menu_items)?2:0,  " LOAD   STATE  ");  mini_menu_items++;
    DSPrint(8,9+mini_menu_items,(sel==mini_menu_items)?2:0,  " EXIT   MENU   ");  mini_menu_items++;
}

// ------------------------------------------------------------------------
// Handle mini-menu interface...
// ------------------------------------------------------------------------
u8 MiniMenu(void)
{
  u8 retVal = MENU_CHOICE_NONE;
  u8 menuSelection = 0;

  SoundPause();
  while ((keysCurrent() & (KEY_TOUCH | KEY_LEFT | KEY_RIGHT | KEY_A ))!=0);

  MiniMenuShow(true, menuSelection);

  while (true)
  {
    nds_key = keysCurrent();
    if (nds_key)
    {
        if (nds_key & KEY_UP)
        {
            menuSelection = (menuSelection > 0) ? (menuSelection-1):(mini_menu_items-1);
            MiniMenuShow(false, menuSelection);
        }
        if (nds_key & KEY_DOWN)
        {
            menuSelection = (menuSelection+1) % mini_menu_items;
            MiniMenuShow(false, menuSelection);
        }
        if (nds_key & KEY_A)
        {
            if      (menuSelection == 0) retVal = MENU_CHOICE_RESET_GAME;
            else if (menuSelection == 1) retVal = MENU_CHOICE_END_GAME;
            else if (menuSelection == 2) retVal = MENU_CHOICE_HI_SCORE;
            else if (menuSelection == 3) retVal = MENU_CHOICE_GAME_OPTIONS;
            else if (menuSelection == 4) retVal = MENU_CHOICE_DEFINE_KEYS;
            else if (menuSelection == 5) retVal = MENU_CHOICE_SAVE_GAME;
            else if (menuSelection == 6) retVal = MENU_CHOICE_LOAD_GAME;
            else if (menuSelection == 7) retVal = MENU_CHOICE_NONE;
            else retVal = MENU_CHOICE_NONE;
            break;
        }
        if (nds_key & KEY_B)
        {
            retVal = MENU_CHOICE_NONE;
            break;
        }

        while ((keysCurrent() & (KEY_UP | KEY_DOWN | KEY_A ))!=0);
        WAITVBL;WAITVBL;
    }
  }

  while ((keysCurrent() & (KEY_UP | KEY_DOWN | KEY_A ))!=0);
  WAITVBL;WAITVBL;

  BottomScreenKeypad();  // Could be generic or overlay...

  SoundUnPause();

  return retVal;
}


// ------------------------------------------------------------------------
// Return 1 if we are showing full keyboard... otherwise 0
// ------------------------------------------------------------------------
u8 last_special_key = 0;
u8 last_special_key_dampen = 0;
u8 last_kbd_key = 0;

u8 handle_msx_keyboard_press(u16 iTx, u16 iTy)  // MSX Keyboard
{
    if ((iTx > 212) && (iTy >= 102) && (iTy < 162))  // Triangular Arrow Keys... do our best
    {
        if      (iTy < 120)   kbd_key = KBD_KEY_UP;
        else if (iTy > 145)   kbd_key = KBD_KEY_DOWN;
        else if (iTx < 234)   kbd_key = KBD_KEY_LEFT;
        else                  kbd_key = KBD_KEY_RIGHT;
    }
    else if ((iTy >= 12) && (iTy < 42))    // Row 1 (top row with Function Keys)
    {
        if      ((iTx >= 0)   && (iTx < 22))   kbd_key = KBD_KEY_ESC;
        else if ((iTx >= 22)  && (iTx < 44))   kbd_key = KBD_KEY_HOME;
        else if ((iTx >= 44)  && (iTx < 73))   kbd_key = KBD_KEY_F1;
        else if ((iTx >= 73)  && (iTx < 102))  kbd_key = KBD_KEY_F2;
        else if ((iTx >= 102) && (iTx < 131))  kbd_key = KBD_KEY_F3;
        else if ((iTx >= 131) && (iTx < 160))  kbd_key = KBD_KEY_F4;
        else if ((iTx >= 160) && (iTx < 190))  kbd_key = KBD_KEY_F5;
        else if ((iTx >= 190) && (iTx < 212))  kbd_key = KBD_KEY_BS;
        else if ((iTx >= 212) && (iTx < 235))  kbd_key = KBD_KEY_INS;
        else if ((iTx >= 235) && (iTx < 255))  kbd_key = KBD_KEY_DEL;
    }
    else if ((iTy >= 42) && (iTy < 72))   // Row 2 (number row)
    {
        if      ((iTx >= 0)   && (iTx < 15))   kbd_key = (msx_japanese_matrix ? '[' : '`');
        else if ((iTx >= 15)  && (iTx < 31))   kbd_key = '1';
        else if ((iTx >= 31)  && (iTx < 45))   kbd_key = '2';
        else if ((iTx >= 45)  && (iTx < 61))   kbd_key = '3';
        else if ((iTx >= 61)  && (iTx < 75))   kbd_key = '4';
        else if ((iTx >= 75)  && (iTx < 91))   kbd_key = '5';
        else if ((iTx >= 91)  && (iTx < 106))  kbd_key = '6';
        else if ((iTx >= 106) && (iTx < 121))  kbd_key = '7';
        else if ((iTx >= 121) && (iTx < 135))  kbd_key = '8';
        else if ((iTx >= 135) && (iTx < 151))  kbd_key = '9';
        else if ((iTx >= 151) && (iTx < 165))  kbd_key = '0';
        else if ((iTx >= 165) && (iTx < 181))  kbd_key = '-';
        else if ((iTx >= 181) && (iTx < 195))  kbd_key = '=';
        else if ((iTx >= 195) && (iTx < 210))  kbd_key = '\\';
        else if ((iTx >= 210) && (iTx < 255))  kbd_key = KBD_KEY_SEL;
    }
    else if ((iTy >= 72) && (iTy < 102))  // Row 3 (QWERTY row)
    {
        if      ((iTx >= 0)   && (iTx < 23))   kbd_key = KBD_KEY_TAB;
        else if ((iTx >= 23)  && (iTx < 39))   kbd_key = 'Q';
        else if ((iTx >= 39)  && (iTx < 54))   kbd_key = 'W';
        else if ((iTx >= 54)  && (iTx < 69))   kbd_key = 'E';
        else if ((iTx >= 69)  && (iTx < 83))   kbd_key = 'R';
        else if ((iTx >= 83)  && (iTx < 99))   kbd_key = 'T';
        else if ((iTx >= 99)  && (iTx < 113))  kbd_key = 'Y';
        else if ((iTx >= 113) && (iTx < 129))  kbd_key = 'U';
        else if ((iTx >= 129) && (iTx < 143))  kbd_key = 'I';
        else if ((iTx >= 143) && (iTx < 158))  kbd_key = 'O';
        else if ((iTx >= 158) && (iTx < 174))  kbd_key = 'P';
        else if ((iTx >= 174) && (iTx < 189))  kbd_key = (msx_japanese_matrix ? ']' : '[');
        else if ((iTx >= 189) && (iTx < 203))  kbd_key = (msx_japanese_matrix ? '`' : ']');
        else if ((iTx >= 203) && (iTx < 214))  kbd_key = KBD_KEY_DEAD;
        else if ((iTx >= 214) && (iTx < 255))  kbd_key = KBD_KEY_STOP;
    }
    else if ((iTy >= 102) && (iTy < 132)) // Row 4 (ASDF row)
    {
        if      ((iTx >= 0)   && (iTx < 27))   {kbd_key = KBD_KEY_CTRL; last_special_key = KBD_KEY_CTRL; last_special_key_dampen = 20;}
        else if ((iTx >= 27)  && (iTx < 43))   kbd_key = 'A';
        else if ((iTx >= 43)  && (iTx < 58))   kbd_key = 'S';
        else if ((iTx >= 58)  && (iTx < 72))   kbd_key = 'D';
        else if ((iTx >= 72)  && (iTx < 87))   kbd_key = 'F';
        else if ((iTx >= 87)  && (iTx < 102))  kbd_key = 'G';
        else if ((iTx >= 102) && (iTx < 117))  kbd_key = 'H';
        else if ((iTx >= 117) && (iTx < 132))  kbd_key = 'J';
        else if ((iTx >= 132) && (iTx < 147))  kbd_key = 'K';
        else if ((iTx >= 147) && (iTx < 161))  kbd_key = 'L';
        else if ((iTx >= 161) && (iTx < 178))  kbd_key = (msx_japanese_matrix ? ';' : KBD_KEY_QUOTE);
        else if ((iTx >= 178) && (iTx < 192))  kbd_key = (msx_japanese_matrix ? KBD_KEY_QUOTE : ';');
        else if ((iTx >= 192) && (iTx < 214))  kbd_key = KBD_KEY_RET;
    }
    else if ((iTy >= 132) && (iTy < 162)) // Row 5 (ZXCV row)
    {
        if      ((iTx >= 0)   && (iTx < 33))   {kbd_key = KBD_KEY_SHIFT; last_special_key = KBD_KEY_SHIFT; last_special_key_dampen = 20;}
        else if ((iTx >= 33)  && (iTx < 49))   kbd_key = 'Z';
        else if ((iTx >= 49)  && (iTx < 64))   kbd_key = 'X';
        else if ((iTx >= 64)  && (iTx < 78))   kbd_key = 'C';
        else if ((iTx >= 78)  && (iTx < 94))   kbd_key = 'V';
        else if ((iTx >= 94)  && (iTx < 109))  kbd_key = 'B';
        else if ((iTx >= 109) && (iTx < 123))  kbd_key = 'N';
        else if ((iTx >= 123) && (iTx < 139))  kbd_key = 'M';
        else if ((iTx >= 139) && (iTx < 154))  kbd_key = ',';
        else if ((iTx >= 154) && (iTx < 169))  kbd_key = '.';
        else if ((iTx >= 169) && (iTx < 184))  kbd_key = '/';
        else if ((iTx >= 184) && (iTx < 214))  kbd_key = KBD_KEY_RET;
    }
    else if ((iTy >= 162) && (iTy < 192)) // Row 6 (SPACE BAR and icons row)
    {
        if      ((iTx >= 1)   && (iTx < 30))   kbd_key = KBD_KEY_CAPS;
        else if ((iTx >= 30)  && (iTx < 53))   {kbd_key = KBD_KEY_GRAPH; last_special_key = KBD_KEY_GRAPH; last_special_key_dampen = 20;}
        else if ((iTx >= 53)  && (iTx < 163))  kbd_key = ' ';
        else if ((iTx >= 163) && (iTx < 192))  {kbd_key = KBD_KEY_CODE; if (!msx_japanese_matrix) {last_special_key = KBD_KEY_CODE; last_special_key_dampen = 20;}}
        else if ((iTx >= 192) && (iTx < 255))  return MENU_CHOICE_MENU;
    }

    if ((kbd_key != 0) && (kbd_key != KBD_KEY_CODE))
    {
        DSPrint(4,0,6,"    ");
    }

    return MENU_CHOICE_NONE;
}

u8 handle_alpha_keyboard_press(u16 iTx, u16 iTy)  // Generic and Simplified Alpha-Numeric Keyboard
{
    if ((iTy >= 14) && (iTy < 48))   // Row 1 (number row)
    {
        if      ((iTx >= 0)   && (iTx < 28))   kbd_key = '1';
        else if ((iTx >= 28)  && (iTx < 54))   kbd_key = '2';
        else if ((iTx >= 54)  && (iTx < 80))   kbd_key = '3';
        else if ((iTx >= 80)  && (iTx < 106))  kbd_key = '4';
        else if ((iTx >= 106) && (iTx < 132))  kbd_key = '5';
        else if ((iTx >= 132) && (iTx < 148))  kbd_key = '6';
        else if ((iTx >= 148) && (iTx < 174))  kbd_key = '7';
        else if ((iTx >= 174) && (iTx < 200))  kbd_key = '8';
        else if ((iTx >= 200) && (iTx < 226))  kbd_key = '9';
        else if ((iTx >= 226) && (iTx < 255))  kbd_key = '0';
    }
    else if ((iTy >= 48) && (iTy < 85))  // Row 2 (QWERTY row)
    {
        if      ((iTx >= 0)   && (iTx < 28))   kbd_key = 'Q';
        else if ((iTx >= 28)  && (iTx < 54))   kbd_key = 'W';
        else if ((iTx >= 54)  && (iTx < 80))   kbd_key = 'E';
        else if ((iTx >= 80)  && (iTx < 106))  kbd_key = 'R';
        else if ((iTx >= 106) && (iTx < 132))  kbd_key = 'T';
        else if ((iTx >= 132) && (iTx < 148))  kbd_key = 'Y';
        else if ((iTx >= 148) && (iTx < 174))  kbd_key = 'U';
        else if ((iTx >= 174) && (iTx < 200))  kbd_key = 'I';
        else if ((iTx >= 200) && (iTx < 226))  kbd_key = 'O';
        else if ((iTx >= 226) && (iTx < 255))  kbd_key = 'P';
    }
    else if ((iTy >= 85) && (iTy < 122)) // Row 3 (ASDF row)
    {
        if      ((iTx >= 0)   && (iTx < 28))   kbd_key = 'A';
        else if ((iTx >= 28)  && (iTx < 54))   kbd_key = 'S';
        else if ((iTx >= 54)  && (iTx < 80))   kbd_key = 'D';
        else if ((iTx >= 80)  && (iTx < 106))  kbd_key = 'F';
        else if ((iTx >= 106) && (iTx < 132))  kbd_key = 'G';
        else if ((iTx >= 132) && (iTx < 148))  kbd_key = 'H';
        else if ((iTx >= 148) && (iTx < 174))  kbd_key = 'J';
        else if ((iTx >= 174) && (iTx < 200))  kbd_key = 'K';
        else if ((iTx >= 200) && (iTx < 226))  kbd_key = 'L';
        else if ((iTx >= 226) && (iTx < 255))  kbd_key = KBD_KEY_BS;
    }
    else if ((iTy >= 122) && (iTy < 159)) // Row 4 (ZXCV row)
    {
        if      ((iTx >= 0)   && (iTx < 28))   kbd_key = 'Z';
        else if ((iTx >= 28)  && (iTx < 54))   kbd_key = 'X';
        else if ((iTx >= 54)  && (iTx < 80))   kbd_key = 'C';
        else if ((iTx >= 80)  && (iTx < 106))  kbd_key = 'V';
        else if ((iTx >= 106) && (iTx < 132))  kbd_key = 'B';
        else if ((iTx >= 132) && (iTx < 148))  kbd_key = 'N';
        else if ((iTx >= 148) && (iTx < 174))  kbd_key = 'M';
        else if ((iTx >= 174) && (iTx < 200))  kbd_key = (key_shift ?  KBD_KEY_QUOTE : ',');
        else if ((iTx >= 200) && (iTx < 226))  kbd_key = (key_shift ?  KBD_KEY_F1 : '.');
        else if ((iTx >= 226) && (iTx < 255))  kbd_key = KBD_KEY_RET;
    }
    else if ((iTy >= 159) && (iTy < 192)) // Row 5 (SPACE BAR and icons row)
    {
        if      ((iTx >= 1)   && (iTx < 52))   return MENU_CHOICE_MENU;
        else if ((iTx >= 54)  && (iTx < 202))  kbd_key = ' ';
        else if ((iTx >= 202) && (iTx < 255))  return MENU_CHOICE_MENU;
    }


    return MENU_CHOICE_NONE;
}

u8 handle_debugger_overlay(u16 iTx, u16 iTy)
{
    if ((iTy >= 175) && (iTy < 192)) // Bottom row is where the debugger keys are...
    {
        if      ((iTx >= 1)   && (iTx < 125))  kbd_key = ' ';
        if      ((iTx >= 125) && (iTx < 158))  return MENU_CHOICE_MENU;
        else if ((iTx >= 192) && (iTx < 255))  return MENU_CHOICE_MENU;
    }
    else {kbd_key = 0; last_kbd_key = 0;}

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
void Hachibitto_main(void)
{
  u16 iTx,  iTy;
  u16 SaveNow = 0, LoadNow = 0;
  u32 ucDEUX;
  static u32 lastUN = 0;
  static u8 dampenClick = 0;
  u8 meta_key = 0;

  // Setup the debug buffer for DSi use
  debug_init();

  // Returns when  user has asked for a game to run...
  BottomScreenOptions();

  // Get the MSX Machine Emulator ready
  msxInit(gpFic[ucGameAct].szName);

  msxSetPal();

  msxRun();

  // Frame-to-frame timing...
  TIMER1_CR = 0;
  TIMER1_DATA=0;
  TIMER1_CR=TIMER_ENABLE  | TIMER_DIV_1024;

  // Once/second timing...
  TIMER2_CR=0;
  TIMER2_DATA=0;
  TIMER2_CR=TIMER_ENABLE  | TIMER_DIV_1024;
  timingFrames  = 0;
  emuFps=0;

  // Force the sound engine to turn on when we start emulation
  bStartSoundEngine = true;

  DelayFirstOutput = 145; // Number of frames to skip before first output to the screen (1 second)

  // -------------------------------------------------------------------
  // Stay in this loop running the MSX game until the user exits...
  // -------------------------------------------------------------------
  while(1)
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
        if (TIMER1_DATA >= 32728)   //  1000MS (1 sec)
        {
            char szChai[4];

            TIMER1_CR = 0;
            TIMER1_DATA = 0;
            TIMER1_CR=TIMER_ENABLE | TIMER_DIV_1024;
            emuFps = emuActFrames;
            if (myGlobalConfig.showFPS)
            {
                if (emuFps == 61) emuFps=60;
                else if (emuFps == 59) emuFps=60;
                if (emuFps/100) szChai[0] = '0' + emuFps/100;
                else szChai[0] = ' ';
                szChai[1] = '0' + (emuFps%100) / 10;
                szChai[2] = '0' + (emuFps%100) % 10;
                szChai[3] = 0;
                DSPrint(0,0,6,szChai);
            }
            DisplayStatusLine(false);
            emuActFrames = 0;
        }
        emuActFrames++;

        // -------------------------------------------------------------
        // Vertical Sync reduces tearing but costs CPU time so this
        // is configurable - default to '1' on DSi and '0' on DS-LITE
        // -------------------------------------------------------------
        while (dsVSyncCount == last_vsync_count)
        {
            if (myGlobalConfig.showFPS == 2) break;
        }
        last_vsync_count = dsVSyncCount;
        msxUpdateScreen();

        // -----------------------------------
        // We only support NTSC 60 frames...
        // -----------------------------------
        if (++timingFrames == 60)
        {
            TIMER2_CR=0;
            TIMER2_DATA=0;
            TIMER2_CR=TIMER_ENABLE | TIMER_DIV_1024;
            timingFrames = 0;
        }

        // If the Z80 Debugger is enabled, call it every frame. Expensive but we need the debug!
        if (myGlobalConfig.debugger >= 2)
        {
            ShowDebugZ80();
        }

        // ---------------------------------------------------------------------------------
        // Hold the key press for a brief instant... some machines take longer than others
        // (eg MSX needs to see the keypress for many tens of milliseconds)... This allows
        // us to 'hold' the keypress in memory for several frames which is good enough.
        // ---------------------------------------------------------------------------------
        if (key_debounce > 0) key_debounce--;
        else
        {
            // -----------------------------------------------------------
            // This is where we accumualte the keys pressed... up to 12!
            // -----------------------------------------------------------
            kbd_keys_pressed = 0;
            memset(kbd_keys, 0x00, sizeof(kbd_keys));
            kbd_key = 0;

            // ------------------------------------------
            // Handle any screen touch events
            // ------------------------------------------
            if  (keysCurrent() & KEY_TOUCH)
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

                  if (myGlobalConfig.debugger == 3)
                  {
                      meta_key = handle_debugger_overlay(iTx, iTy);
                  }
                  // ------------------------------------------------------------
                  // Test the touchscreen for various full keyboard handlers...
                  // ------------------------------------------------------------
                  else
                  {
                      meta_key = handle_msx_keyboard_press(iTx, iTy);
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
                              DelayFirstOutput = 145; // Number of frames to skip before first output to the screen (1 second)
                              ResetMSX();
                          }
                          BottomScreenKeypad();
                          SoundUnPause();
                          break;

                      case MENU_CHOICE_END_GAME:
                            SoundPause();
                            //  Ask for verification
                            if  (showMessage("DO YOU REALLY WANT TO","QUIT THE CURRENT GAME ?") == ID_SHM_YES)
                            {
                                memset((u8*)0x06000000, 0x00, 0x20000);    // Reset VRAM to 0x00 to clear any potential display garbage on way out
                                return;
                            }
                            BottomScreenKeypad();
                            DisplayStatusLine(true);
                            SoundUnPause();
                          break;

                      case MENU_CHOICE_HI_SCORE:
                          SoundPause();
                          highscore_display(file_crc);
                          DisplayStatusLine(true);
                          SoundUnPause();
                          break;

                      case MENU_CHOICE_GAME_OPTIONS:
                          SoundPause();
                          BottomScreenOptions();
                          HachibittoGameOptions(false);
                          BottomScreenKeypad();
                          DisplayStatusLine(true);
                          SoundUnPause();
                          break;

                      case MENU_CHOICE_DEFINE_KEYS:
                          SoundPause();
                          BottomScreenOptions();
                          HachibittoChangeKeymap();
                          BottomScreenKeypad();
                          DisplayStatusLine(true);
                          SoundUnPause();
                          break;

                      case MENU_CHOICE_SAVE_GAME:
                          if  (!SaveNow)
                          {
                              SoundPause();
                              if  (showMessage("DO YOU REALLY WANT TO","SAVE GAME STATE ?") == ID_SHM_YES)
                              {
                                SaveNow = 1;
                                msxSaveState();
                              }
                              BottomScreenKeypad();
                              SoundUnPause();
                          }
                          break;

                      case MENU_CHOICE_LOAD_GAME:
                          if  (!LoadNow)
                          {
                              SoundPause();
                              if (showMessage("DO YOU REALLY WANT TO","LOAD GAME STATE ?") == ID_SHM_YES)
                              {
                                LoadNow = 1;
                                msxLoadState();
                              }
                              BottomScreenKeypad();
                              SoundUnPause();
                          }
                          break;

                      default:
                          SaveNow = 0;
                          LoadNow = 0;
                  }


                  if (++dampenClick > 0)  // Make sure the key is pressed for an appreciable amount of time...
                  {
                      if ((kbd_key != 0) && (lastUN == 0))
                      {
                          mmEffect(SFX_KEYCLICK);  // Play short key click for feedback...
                      }
                      lastUN = kbd_key;
                  }
                }
            } //  SCR_TOUCH
            else
            {
              touch_debounce = 0;
              SaveNow=LoadNow = 0;
              lastUN = 0;  dampenClick = 0;
              last_kbd_key = 0;
            }
      }

      // ------------------------------------------------------------------------
      //  Test DS keypresses (ABXY, L/R) and map to corresponding MSX keys
      // ------------------------------------------------------------------------
      key_shift = false;
      key_ctrl = false;
      key_code = false;
      key_graph = false;
      key_dia = false;

      ucDEUX  = 0;
      nds_key  = keysCurrent();     // Get any current keys pressed on the NDS

      if (nds_key & KEY_X)
      {
          temp_offset = -16; slide_dampen = 15;
      }
      if (nds_key & KEY_Y)
      {
          temp_offset = 16; slide_dampen = 15;
      }
      if ((nds_key & KEY_L) && (nds_key & KEY_R) && (nds_key & KEY_X))
      {
            lcdSwap();
            WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;
      }
      else if ((nds_key & KEY_L) && (nds_key & KEY_R) && (nds_key & KEY_Y))
      {
            DSPrint(5,0,0,"SNAPSHOT");
            screenshot();
            debug_save();
            WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;
            DSPrint(5,0,0,"        ");
      }
      else if  (nds_key & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_START | KEY_SELECT | KEY_R | KEY_L | KEY_X | KEY_Y))
      {
          if (myConfig.dpad == DPAD_SLIDE_N_GLIDE) // CHUCKIE-EGG Style... hold left/right or up/down for a few frames
          {
                if (nds_key & KEY_UP)
                {
                    slide_n_glide_key_up    = 12;
                    slide_n_glide_key_down  = 0;
                }
                if (nds_key & KEY_DOWN)
                {
                    slide_n_glide_key_down  = 12;
                    slide_n_glide_key_up    = 0;
                }
                if (nds_key & KEY_LEFT)
                {
                    slide_n_glide_key_left  = 12;
                    slide_n_glide_key_right = 0;
                }
                if (nds_key & KEY_RIGHT)
                {
                    slide_n_glide_key_right = 12;
                    slide_n_glide_key_left  = 0;
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
          for (u8 i=0; i<12; i++)
          {
              if (nds_key & NDS_keyMap[i])
              {
                  if (keyCoresp[myConfig.keymap[i]] < 0xFFFE0000)   // Normal key map
                  {
                      ucDEUX  |= keyCoresp[myConfig.keymap[i]];
                  }
                  else // This is a keyboard maping... handle that here... just set the appopriate kbd_key
                  {
                      if      ((keyCoresp[myConfig.keymap[i]] >= META_KBD_A) && (keyCoresp[myConfig.keymap[i]] <= META_KBD_Z))  kbd_key = ('A' + (keyCoresp[myConfig.keymap[i]] - META_KBD_A));
                      else if ((keyCoresp[myConfig.keymap[i]] >= META_KBD_0) && (keyCoresp[myConfig.keymap[i]] <= META_KBD_9))  kbd_key = ('0' + (keyCoresp[myConfig.keymap[i]] - META_KBD_0));
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SPACE)     kbd_key = ' ';
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_RETURN)    kbd_key = KBD_KEY_RET;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_ESC)       kbd_key = KBD_KEY_ESC;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SHIFT)     key_shift = 1;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_CTRL)      key_ctrl  = 1;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_CODE)      key_code  = 1;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_GRAPH)     key_graph = 1;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_HOME)      kbd_key = KBD_KEY_HOME;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_UP)        kbd_key = KBD_KEY_UP;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_DOWN)      kbd_key = KBD_KEY_DOWN;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_LEFT)      kbd_key = KBD_KEY_LEFT;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_RIGHT)     kbd_key = KBD_KEY_RIGHT;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_PERIOD)    kbd_key = '.';
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_COMMA)     kbd_key = ',';
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_COLON)     kbd_key = ':';
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SEMI)      kbd_key = ';';
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_QUOTE)     kbd_key = KBD_KEY_QUOTE;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SLASH)     kbd_key = '/';
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_BACKSLASH) kbd_key = '\\';
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_PLUS)      kbd_key = '+';
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_MINUS)     kbd_key = '-';
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_LBRACKET)  kbd_key = '[';
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_RBRACKET)  kbd_key = ']';
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_BS)        kbd_key = KBD_KEY_BS;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_CARET)     kbd_key = '^';
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_ASTERISK)  kbd_key = '*';
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_ATSIGN)    kbd_key = '@';
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_TAB)       kbd_key = KBD_KEY_TAB;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_INS)       kbd_key = KBD_KEY_INS;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_DEL)       kbd_key = KBD_KEY_DEL;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_CLR)       kbd_key = KBD_KEY_CLEAR;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_STOP_BRK)  kbd_key = KBD_KEY_STOP;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F1)        kbd_key = KBD_KEY_F1;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F2)        kbd_key = KBD_KEY_F2;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F3)        kbd_key = KBD_KEY_F3;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F4)        kbd_key = KBD_KEY_F4;
                      else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F5)        kbd_key = KBD_KEY_F5;

                      if (kbd_key != 0)
                      {
                          kbd_keys[kbd_keys_pressed++] = kbd_key;
                      }
                  }
              }
          }
      }
      else
      {
          if (slide_n_glide_key_up)    slide_n_glide_key_up--;
          if (slide_n_glide_key_down)  slide_n_glide_key_down--;
          if (slide_n_glide_key_left)  slide_n_glide_key_left--;
          if (slide_n_glide_key_right) slide_n_glide_key_right--;
          last_mapped_key = 0;
      }


      // ------------------------------------------------------------------------------------------
      // Finally, check if there are any buffered keys that need to go into the keyboard handling.
      // ------------------------------------------------------------------------------------------
      ProcessBufferedKeys();

      // ---------------------------------------------------------
      // Accumulate all bits above into the Joystick State var...
      // ---------------------------------------------------------
      JoyState = ucDEUX;

      // --------------------------------------------------
      // Handle Auto-Fire if enabled in configuration...
      // --------------------------------------------------
      static u8 autoFireTimer[2]={0,0};
      if ((myConfig.autoFire & 0x01) && (JoyState & JST_FIRE1))  // Fire Button 1
      {
         if ((++autoFireTimer[0] & 7) > 4)  JoyState &= ~JST_FIRE1;
      }
      if ((myConfig.autoFire & 0x02) && (JoyState & JST_FIRE2))  // Fire Button 2
      {
          if ((++autoFireTimer[1] & 7) > 4) JoyState &= ~JST_FIRE2;
      }
    }
  }
}


// ----------------------------------------------------------------------------------------
// We can use this VRAM as semi-fast RAM if needed... right now, no actual need.
// ----------------------------------------------------------------------------------------
void useVRAM(void)
{
  vramSetBankD(VRAM_D_LCD );        // Not using this for video but 128K of faster RAM always useful!  Mapped at 0x06860000 -   Not currently used...
  vramSetBankE(VRAM_E_LCD );        // Not using this for video but 64K of faster RAM always useful!   Mapped at 0x06880000 -   ..
  vramSetBankF(VRAM_F_LCD );        // Not using this for video but 16K of faster RAM always useful!   Mapped at 0x06890000 -   ..
  vramSetBankG(VRAM_G_LCD );        // Not using this for video but 16K of faster RAM always useful!   Mapped at 0x06894000 -   ..
  vramSetBankH(VRAM_H_LCD );        // Not using this for video but 32K of faster RAM always useful!   Mapped at 0x06898000 -   ..
  vramSetBankI(VRAM_I_LCD );        // Not using this for video but 16K of faster RAM always useful!   Mapped at 0x068A0000 -   16K Used for the VDP Look Up Table
}

/*********************************************************************************
 * Init DS Emulator - setup VRAM banks and background screen rendering banks
 ********************************************************************************/
void HachibittoInit(void)
{
  //  Init graphic mode (bitmap mode)
  videoSetMode(MODE_0_2D  | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_SPR_1D_LAYOUT | DISPLAY_SPR_ACTIVE);
  videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE  | DISPLAY_BG1_ACTIVE | DISPLAY_SPR_1D_LAYOUT | DISPLAY_SPR_ACTIVE);
  vramSetBankA(VRAM_A_MAIN_BG);
  vramSetBankB(VRAM_B_MAIN_SPRITE);          // Once emulation of game starts, we steal this back for an additional 128K of VRAM at 0x6820000 which we will use as a snapshot buffer for taking screen pics
  vramSetBankC(VRAM_C_SUB_BG);

  //  Stop blending effect of intro
  REG_BLDCNT=0; REG_BLDCNT_SUB=0; REG_BLDY=0; REG_BLDY_SUB=0;

  //  Render the top screen
  bg0 = bgInit(0, BgType_Text8bpp,  BgSize_T_256x512, 31,0);
  bg1 = bgInit(1, BgType_Text8bpp,  BgSize_T_256x512, 29,0);
  bgSetPriority(bg0,1);bgSetPriority(bg1,0);
  decompress(topscreenTiles,  bgGetGfxPtr(bg0), LZ77Vram);
  decompress(topscreenMap,  (void*) bgGetMapPtr(bg0), LZ77Vram);
  dmaCopy((void*) topscreenPal,(void*)  BG_PALETTE,256*2);
  unsigned  short dmaVal =*(bgGetMapPtr(bg0)+51*32);
  dmaFillWords(dmaVal | (dmaVal<<16),(void*)  bgGetMapPtr(bg1),32*24*2);

  // Put up the options screen
  BottomScreenOptions();

  //  Find the files
  HachibittoFindFiles();
}


void BottomScreenOptions(void)
{
    swiWaitForVBlank();

    // ---------------------------------------------------
    // Put up the options select screen background...
    // ---------------------------------------------------
    bg0b = bgInitSub(0, BgType_Text8bpp, BgSize_T_256x256, 31,0);
    bg1b = bgInitSub(1, BgType_Text8bpp, BgSize_T_256x256, 29,0);
    bgSetPriority(bg0b,1);bgSetPriority(bg1b,0);
    decompress(optionsTiles, bgGetGfxPtr(bg0b), LZ77Vram);
    decompress(optionsMap, (void*) bgGetMapPtr(bg0b), LZ77Vram);
    dmaCopy((void*) optionsPal,(void*) BG_PALETTE_SUB,256*2);
    unsigned short dmaVal = *(bgGetMapPtr(bg1b)+24*32);
    dmaFillWords(dmaVal | (dmaVal<<16),(void*) bgGetMapPtr(bg1b),32*24*2);
}

// ---------------------------------------------------------------------------
// Setup the bottom screen - mostly for menu, high scores, options, etc.
// ---------------------------------------------------------------------------
void BottomScreenKeypad(void)
{
    if (myGlobalConfig.debugger == 3)  // Full Z80 Debug overrides things... put up the debugger overlay
    {
      //  Init bottom screen
      decompress(debug_ovlTiles, bgGetGfxPtr(bg0b),  LZ77Vram);
      decompress(debug_ovlMap, (void*) bgGetMapPtr(bg0b),  LZ77Vram);
      dmaCopy((void*) bgGetMapPtr(bg0b)+32*30*2,(void*) bgGetMapPtr(bg1b),32*24*2);
      dmaCopy((void*) debug_ovlPal,(void*) BG_PALETTE_SUB,256*2);
    }
    else if (myConfig.keyboard == OVL_FULLKBD) // Full Keyboard (based on machine)
    {
      decompress(msx_kbdTiles, bgGetGfxPtr(bg0b),  LZ77Vram);
      decompress(msx_kbdMap, (void*) bgGetMapPtr(bg0b),  LZ77Vram);
      dmaCopy((void*) bgGetMapPtr(bg0b)+32*30*2,(void*) bgGetMapPtr(bg1b),32*24*2);
      dmaCopy((void*) msx_kbdPal,(void*) BG_PALETTE_SUB,256*2);
    }
    else if (myConfig.keyboard == OVL_ALPHAKBD) // Alpha Simplified Keyboard
    {
      //  Init bottom screen
      decompress(alpha_kbdTiles, bgGetGfxPtr(bg0b),  LZ77Vram);
      decompress(alpha_kbdMap, (void*) bgGetMapPtr(bg0b),  LZ77Vram);
      dmaCopy((void*) bgGetMapPtr(bg0b)+32*30*2,(void*) bgGetMapPtr(bg1b),32*24*2);
      dmaCopy((void*) alpha_kbdPal,(void*) BG_PALETTE_SUB,256*2);
    }
    else // Generic Overlay (overlay == 0)
    {
        //todo
    }

    unsigned  short dmaVal = *(bgGetMapPtr(bg1b)+24*32);
    dmaFillWords(dmaVal | (dmaVal<<16),(void*)  bgGetMapPtr(bg1b),32*24*2);

    last_msx_scc_enable = 99;
    DisplayStatusLine(true);
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

// -------------------------------------------------------------
// Only used for basic timing of moving the Mario sprite...
// -------------------------------------------------------------
void irqVBlank(void)
{
    // Manage time and true vSync on display output to reduce tearing...
    vusCptVBL++;
    dsVSyncCount++;

    int cyBG = ((s16)myConfig.yOffset+temp_offset) << 8;

    REG_BG2Y = cyBG;
    REG_BG3Y = cyBG;

    if (temp_offset)
    {
        if (slide_dampen == 0)
        {
            if (temp_offset > 0) temp_offset--;
            else temp_offset++;
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
  //  Init sound
  consoleDemoInit();

  if  (!fatInitDefault()) {
     iprintf("Unable to initialize libfat!\n");
     return -1;
  }

  // -----------------------------------------------------------------
  // Allocate the Large DSi buffer for expanded RAM banking...
  // -----------------------------------------------------------------
  if (isDSiMode())
  {
      MAX_CART_SIZE = 4096;
      ROM_Memory = malloc(MAX_CART_SIZE * 1024);
  }
  else // For older DS units... 1.25MB max
  {
      MAX_CART_SIZE = 1280;
      ROM_Memory = malloc(MAX_CART_SIZE * 1024);
  }

  highscore_init();

  lcdMainOnTop();

  //  Init timer for frame management
  TIMER2_DATA=0;
  TIMER2_CR=TIMER_ENABLE|TIMER_DIV_1024;
  dsInstallSoundEmuFIFO();

  //  Show the fade-away intro logo...
  intro_logo();

  SetYtrigger(190); //trigger 2 lines before vsync

  irqSet(IRQ_VBLANK,  irqVBlank);
  irqEnable(IRQ_VBLANK);

  // -----------------------------------------------------------------
  // Grab the BIOS before we try to switch any directories around...
  // -----------------------------------------------------------------
  useVRAM();

  // -----------------------------------------------------------------
  // And do an initial load of configuration... We'll match it up
  // with the game that was selected later...
  // -----------------------------------------------------------------
  LoadConfig();

  //  Handle command line argument... mostly for TWL++
  if  (argc > 1)
  {
      //  We want to start in the directory where the file is being launched...
      if  (strchr(argv[1], '/') != NULL)
      {
          static char  path[128];
          strcpy(path,  argv[1]);
          char  *ptr = &path[strlen(path)-1];
          while (*ptr !=  '/') ptr--;
          ptr++;
          strcpy(cmd_line_file,  ptr);
          *ptr=0;
          chdir(path);
      }
      else
      {
          strcpy(cmd_line_file,  argv[1]);
      }
  }
  else
  {
      cmd_line_file[0]=0; // No file passed on command line...
      chdir("/roms");     // Try to start in roms area... doesn't matter if it fails
      chdir("msx");       // And try to start in the subdir /msx... doesn't matter if it fails.
  }

  SoundPause();

  srand(time(NULL));

  //  ------------------------------------------------------------
  //  We run this loop forever until game exit is selected...
  //  ------------------------------------------------------------
  while(1)
  {
    HachibittoInit();

    while(1)
    {
      SoundPause();
      //  Choose option
      if  (cmd_line_file[0] != 0)
      {
          ucGameChoice=0;
          ucGameAct=0;
          strcpy(gpFic[ucGameAct].szName, cmd_line_file);
          cmd_line_file[0] = 0;    // No more initial file...
          ReadFileCRCAndConfig(); // Get CRC32 of the file and read the config/keys
      }
      else
      {
          HachibittoChangeOptions();
      }

      //  Run Machine
      HachibittoInitCPU();
      Hachibitto_main();
    }
  }
  return(0);
}

// --------------------------------------------------------------------------------------------------------
// Used by the MSX handler to point to different 8K segments of memory as RAM and Carts are swapped in/out.
// --------------------------------------------------------------------------------------------------------
u8 *MemoryMap[8]        __attribute__((section(".dtcm"))) = {0,0,0,0,0,0,0,0};

// -------------------------------------
// Some IO Port and Memory Map vars...
// -------------------------------------
u8 key_shift_hold       __attribute__((section(".dtcm"))) = 0;

// -------------------------------------
// Our venerable Z80 CPU structure!
// -------------------------------------
Z80 CPU __attribute__((section(".dtcm")));      // Put the entire CPU state into fast memory for speed!

// --------------------------------------------------
// Some special ports for the MSX machine emu
// --------------------------------------------------
u8 Port_PPI_A __attribute__((section(".dtcm"))) = 0x00;
u8 Port_PPI_B __attribute__((section(".dtcm"))) = 0x00;
u8 Port_PPI_C __attribute__((section(".dtcm"))) = 0x00;

u8 romBankMask          __attribute__((section(".dtcm"))) = 0x00;

u8  JoyMode        __attribute__((section(".dtcm"))) = 0;           // Joystick Mode (1=Keypad, 0=Joystick)
u32 JoyState       __attribute__((section(".dtcm"))) = 0;           // Joystick State for P1 and P2

// ------------------------------------------------------------
// Some global vars to track what kind of cart/rom we have...
// ------------------------------------------------------------
u32 file_crc __attribute__((section(".dtcm")))  = 0x00000000;  // Our global file CRC32 to uniquiely identify this game

// --------------------------------------------------------------
// The master AY sound chip for the MSX. We might also have SCC.
// --------------------------------------------------------------
AY38910 myAY   __attribute__((section(".dtcm")));

/*********************************************************************************
 * Keyboard Key Buffering Engine...
 ********************************************************************************/
u8 BufferedKeys[32];
u8 BufferedKeysWriteIdx=0;
u8 BufferedKeysReadIdx=0;
void BufferKey(u8 key)
{
    BufferedKeys[BufferedKeysWriteIdx] = key;
    BufferedKeysWriteIdx = (BufferedKeysWriteIdx+1) % 32;
}

// Buffer a whole string worth of characters...
void BufferKeys(char *str)
{
    for (int i=0; i<strlen(str); i++)  BufferKey((u8)str[i]);
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
            BufferedKeysReadIdx = (BufferedKeysReadIdx+1) % 32;
            if (buf_held == KBD_KEY_SHIFT) buf_shift = 2; else {if (buf_shift) buf_shift--;}
            if (buf_held == KBD_KEY_CTRL)  buf_ctrl = 6; else {if (buf_ctrl) buf_ctrl--;}
            if (buf_held == 255) {buf_held = 0; next_dampen_time=60;} else next_dampen_time = 5;
        } else buf_held = 0;
        dampen = 0;
    }

    // See if the shift key should be virtually pressed along with this buffered key...
    if (buf_held) {kbd_keys[kbd_keys_pressed++] = buf_held; if (buf_shift) key_shift=1; if (buf_ctrl) key_ctrl=1;}
}


/*********************************************************************************
 * Init MSX Emulation Engine for that game
 ********************************************************************************/
u8 msxInit(char *szGame)
{
  extern u8 bForceMSXLoad;
  u8 RetFct,uBcl;
  u16 uVide;

  // We've got some debug data we can use for development... reset these.
  memset(debug, 0x00, sizeof(debug));

  // See if we have forced any specific modes on loading...
  if (bForceMSXLoad) msx_mode = 1;
  if (msx_mode) BottomScreenKeypad();  // Could Need to ensure the MSX layout is shown

  // -----------------------------------------------------------------
  // Change graphic mode to initiate emulation.
  // Here we can claim back 128K of VRAM which is otherwise unused
  // but we can use it for fast memory swaps and look-up-tables.
  // -----------------------------------------------------------------
  videoSetMode(MODE_5_2D | DISPLAY_BG3_ACTIVE);
  vramSetBankA(VRAM_A_MAIN_BG_0x06000000);      // This is our top emulation screen (where the game is played)
  vramSetBankB(VRAM_B_LCD);                     // 128K of Video Memory mapped at 0x6820000
  REG_BG3CNT = BG_BMP8_256x256;
  REG_BG3PA = (1<<8);
  REG_BG3PB = 0;
  REG_BG3PC = 0;
  REG_BG3PD = (1<<8);
  REG_BG3X = 0;
  REG_BG3Y = 0;

  // Init the page flipping buffer...
  for (uBcl=0;uBcl<255;uBcl++)
  {
     uVide=0;
     dmaFillWords(uVide | (uVide<<16),pVidFlipBuf+uBcl*128,256);
  }

  write_NV_counter=0;

  // loadrom() will figure out how big and where to load it... the 0x8000 here is meaningless.
  RetFct = loadrom(szGame,RAM_Memory+0x8000);

  // Wipe RAM area for the MSX
  msxWipeRAM();

  if (RetFct)
  {
    // Perform a standard system RESET
    ResetMSX();
  }

  // Return with result
  return (RetFct);
}

/*********************************************************************************
 * Run the emul
 ********************************************************************************/
void msxRun(void)
{
  Z80_Interface_Reset();                // Reset the Z80 Interface module
  ResetZ80(&CPU);                       // Reset the CZ80 core CPU
  BottomScreenKeypad();                 // Show the game-related screen with keypad / keyboard
}

/*********************************************************************************
 * Set MSX legacy Palette (MSX2 can override)
 ********************************************************************************/
void msxSetPal(void)
{
  u16 uBcl;
  u8 r,g,b;

  // -----------------------------------------------------------------------
  // The MSX has a 16 color pallette... we set that up. MSX2 expands this.
  // We always use the standard NTSC color palette which is fine for now
  // but maybe in the future we add the PAL color palette for a bit more
  // authenticity.
  // -----------------------------------------------------------------------
  for (uBcl=0;uBcl<16;uBcl++)
  {
    r = (u8) ((float) VDP9938A_palette[uBcl*3+0]*0.121568f);
    g = (u8) ((float) VDP9938A_palette[uBcl*3+1]*0.121568f);
    b = (u8) ((float) VDP9938A_palette[uBcl*3+2]*0.121568f);
    SPRITE_PALETTE[uBcl] = RGB15(r,g,b);
    BG_PALETTE[uBcl] = RGB15(r,g,b);
  }
  BG_PALETTE[16] = RGB15(0,0,0);
  BG_PALETTE[17] = RGB15(0,0,0);

  for (uBcl=18; uBcl < 256; uBcl++)
  {
      //Green (G)3 bitsBits 7, 6, 5 (MSB)8 levels0 to 7Red (R)3 bitsBits 4, 3, 28 levels0 to 7Blue (B)2 bitsBits 1, 0 (LSB)4 levels0 to 3
      b = uBcl & 3;
      r = (uBcl >> 2) & 7;
      g = (uBcl >> 5) & 7;
      BG_PALETTE[uBcl] = RGB15(r<<3,g<<3,b<<3);
  }
}


/*********************************************************************************
 * Update the screen for the current cycle. On the DSi this will generally
 * be called right after swiWaitForVBlank() in VDP9938a.c which will help
 * reduce visual tearing and other artifacts. It's not strictly necessary
 * and that does slow down the loop a bit... but DSi can handle it.
 ********************************************************************************/
u8 skip_render = 0;
ITCM_CODE void msxUpdateScreen(void)
{
    if (DelayFirstOutput)
    {
        DelayFirstOutput--;
        return;
    }

    if (!skip_render)
    {
        dmaCopyWordsAsynch(2, (u32*)XBuf, (u32*)pVidFlipBuf, 256*212);
    }
    skip_render=0;
}


/*******************************************************************************
 * Compute the file CRC - this will be our unique identifier for the game
 * for saving HI SCORES and Configuration / Key Mapping data.
 *******************************************************************************/
void getfile_crc(const char *filename)
{
    DSPrint(11,13,6, "LOADING...");

    file_crc = getFileCrc(filename);        // The CRC is used as a unique ID to save out High Scores and Configuration...

    DSPrint(11,13,6, "          ");

    // ------------------------------------------------------------------------------
    // And a handful of games require SRAM which is a special case-by-case basis...
    // ------------------------------------------------------------------------------
    msx_sram_enabled = 0;
    if (file_crc == 0x92943e5b) msx_sram_enabled = 0x10;       // MSX Hydlide 2 - Shine Of Darkness (EN)
    if (file_crc == 0xb29edaec) msx_sram_enabled = 0x10;       // MSX Hydlide 2 - Shine Of Darkness (EN)
    if (file_crc == 0xa0fd57cf) msx_sram_enabled = 0x10;       // MSX Hydlide 2 - Shine Of Darkness (EN)
    if (file_crc == 0xd640deaf) msx_sram_enabled = 0x20;       // MSX Dragon Slayer 2 - Xanadu (EN)
    if (file_crc == 0x119b7ba8) msx_sram_enabled = 0x20;       // MSX Dragon Slayer 2 - Xanadu (JP)
    if (file_crc == 0x27fd8f9a) msx_sram_enabled = 0x10;       // MSX Deep Dungeon I (JP)
    if (file_crc == 0x213da247) msx_sram_enabled = 0x10;       // MSX Deep Dungeon II (EN)
    if (file_crc == 0x101db19c) msx_sram_enabled = 0x10;       // MSX Deep Dungeon II (JP)
    if (file_crc == 0x96b7faca) msx_sram_enabled = 0x10;       // MSX Harry Fox Special (JP)
    if (file_crc == 0xb8fc19a4) msx_sram_enabled = 0x20;       // MSX Cosmic Soldier 2 - Psychic War
    if (file_crc == 0x4ead5098) msx_sram_enabled = 0x20;       // MSX Ghengis Khan
    if (file_crc == 0x3aa33a30) msx_sram_enabled = 0x20;       // MSX Nobunaga no Yabou - Zenkokuhan
}


/** loadrom() ******************************************************************/
/* Open a rom file from file system and load it into the ROM_Memory[] buffer   */
/*******************************************************************************/
u8 loadrom(const char *filename, u8 * ptr)
{
  u8 bOK = 0;
  int romSize = 0;

  FILE* handle = fopen(filename, "rb");
  if (handle != NULL)
  {
    // Save the initial filename and file - we need it for save/restore of state
    strcpy(initial_file, filename);
    getcwd(initial_path, MAX_ROM_NAME);

    // Get file size the 'fast' way - use fstat() instead of fseek() or ftell()
    struct stat stbuf;
    (void)fstat(fileno(handle), &stbuf);
    romSize = stbuf.st_size;

    if (romSize <= (MAX_CART_SIZE * 1024))  // Max size cart is 1MB/4MB - that's pretty huge...
    {
        fclose(handle); // We only need to close the file - the game ROM is now sitting in ROM_Memory[] from the getFileCrc() handler

        romBankMask = 0x00;         // No bank mask until proven otherwise
        mapperMask = 0x00;          // No MSX mapper mask

        // Cache the first 256K of the ROM into fast VRAM for possible use...
        u8 *fastROM = (u8*) (0x06860000);
        memcpy(fastROM, ROM_Memory, (256 * 1024));

        // ------------------------------------------------------------------------------
        // For the MSX emulation, we setup the initial memory map based on ROM size
        // ------------------------------------------------------------------------------
        if (msx_mode)
        {
            MSX_InitialMemoryLayout(romSize);
        }
        bOK = 1;
    }
    else fclose(handle);
  }

  return bOK;
}

// --------------------------------------------------------------------------
// Based on writes to Port53 and Port60 we configure the SGM handling of
// memory... this includes 24K vs 32K of RAM (the latter is BIOS disabled).
// --------------------------------------------------------------------------
__attribute__ ((noinline)) void SetupSGM(void)
{
    return;
}

// -------------------------------------------------------------------------
// For arious machines, we have patched the BIOS so that we trap calls
// to various I/O routines: namely cassette access. We handle that here.
// -------------------------------------------------------------------------
void PatchZ80(register Z80 *r)
{

}


/** LoopZ80() *************************************************/
/** Z80 emulation calls this function periodically to run    **/
/** Z80 code for the loaded ROM. It runs code refreshing the **/
/** VDP and checking for interrupt requests.                 **/
/**************************************************************/
int mid_frame_interrupt=0;
ITCM_CODE u32 LoopZ80()
{
  // Execute 1 scanline worth of CPU instructions
  u32 cycles_to_process = VDP9938_CLOCKS_PER_LINE + CPU.CycleDeficit;
  CPU.CycleDeficit = ExecZ80(cycles_to_process);

  if (mid_frame_interrupt)
  {
      if (--mid_frame_interrupt == 0)
      {
          RefereshPreviousLines();
      }
  }

  // Run the VDP engine
  LoopVDP();

  // Refresh VDP
  Loop9938();

  // Generate an interrupt if called for...
  if(CPU.IRequest!=INT_NONE)
  {
      IntZ80(&CPU, CPU.IRequest);
      CPU.NumInts++;   // Track Interrupt Requests
      if (((CurLine-1) >= VDP9938_START_LINE) && ((CurLine-1) < VDP9938_END_LINE)) // Is this a mid-frame line interrupt?
      {
          CPU.CycleDeficit = 0;
          mid_frame_interrupt=3; // Let CPU run 3 lines then redraw the previous 2...
      }
  }

  // Drop out unless end of screen is reached
  if (CurLine == VDP9938_END_LINE)
  {
      return 0;
  }
  return 1;
}

// End of file


// -----------------------------------------------------------------------
// The code below is a handy set of debug tools that allows us to
// write printf() like strings out to a file. Basically we accumulate
// the strings into a large RAM buffer and then when the L+R shoulder
// buttons are pressed and held, we will snapshot out the debug.log file.
// The DS-Lite only gets a small 16K debug buffer but the DSi gets 4MB!
// -----------------------------------------------------------------------

#define MAX_DPRINTF_STR_SIZE  256
u32     MAX_DEBUG_BUF_SIZE  = 0;

char *debug_buffer = 0;
u32  debug_len = 0;
extern char szName[]; // Reuse buffer which has no other in-game use

void debug_init()
{
    if (!debug_buffer)
    {
        if (isDSiMode())
        {
            MAX_DEBUG_BUF_SIZE = (1024*1024*2); // 2MB!!
            debug_buffer = malloc(MAX_DEBUG_BUF_SIZE);
        }
        else
        {
            MAX_DEBUG_BUF_SIZE = (1024*16);     // 16K only
            debug_buffer = (char*)SRAM_Memory;  // Steal the SRAM_Memory[] for debug
        }
    }
    memset(debug_buffer, 0x00, MAX_DEBUG_BUF_SIZE);
    DX=DY=0;
    debug_len = 0;
}

void debug_printf(const char * str, ...)
{
    va_list ap = {0};

    va_start(ap, str);
    vsnprintf(szName, MAX_DPRINTF_STR_SIZE, str, ap);
    va_end(ap);

    strcat(debug_buffer, szName);
    debug_len += strlen(szName);
}

void debug_save()
{
    if (debug_len > 0) // Only if we have debug data to write...
    {
        FILE *fp = fopen("debug.log", "w");
        if (fp)
        {
            fwrite(debug_buffer, 1, debug_len, fp);
            fclose(fp);
        }
    }
}

// End of file
