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

//  msx_music.c
//  MSX-MUSIC (Yamaha YM2413 / OPLL) sound chip emulator - music only.
//  The mixer uses a simplified two-operator FM path with per-operator
//  attack/decay/sustain/release states. This is not a cycle-accurate OPLL core.
//
//  Please note: this file was largely generated with Claude.AI and ChatGPT with a lot
//  of patience (and frustration) by the developer. Sound drivers are not my specialty...
//  in fact, we are not really on speaking terms. So if you happen to understand basic
//  sound sampling principles and can figure out FM Synthesis and want to help improve
//  this - please be my guest!

#include "msx_music.h"
#include "MSX_generic.h"
#include <string.h> // memcpy, memset

//@----------------------------------------------------------------------------
//@ Tunables.
//@----------------------------------------------------------------------------
#define YM_SAMPLE_RATE 31000            // confirmed value, matches the AY driver's rate
#define YM_MASTER_CLOCK 3579545         // MSX standard clock, same as SCC's
#define YM_SIN_SHIFT 24                 // phase>>24 -> 8-bit (256 entry) table index
#define YM_GAIN_RAMP_STEP 16            // Gain ramp toward full level on key-on.
#define YM_PERCUSSION_RELEASE_STEP 3500 // PERCUSSION release rate: higher = faster decay for more distinct drum hits.


/* FM envelope: 32-bit fixed point, 16 fractional bits.
   0 = full level, YM_ENV_SILENT = silent.  Amplitude = 255 - (env >> 16). */
#define YM_ENV_SILENT (255u << 16)

/* Decay/release speed. Per-sample envelope step for rate R (1..15) is
   YM_RATE_BASE << (R-1): speed doubles with every rate step like the real OPLL.
   170 gives ~3.2s full-scale at R=1, ~25ms at R=8, ~1.5ms at R=12
   is a LINEAR amplitude ramp, so it needs to run much faster than the real
   chip's dB-domain times to sound the same. Tune by ear. */
#define YM_RATE_BASE 170u

/* The envelopes are only advanced once every (1 << YM_ENV_DIV_SHIFT) output
   samples (all steps are scaled up to compensate).  Saves a lot of CPU and
   is inaudible at these rates. 2 = every 4th sample (~7.7 kHz). */
#define YM_ENV_DIV_SHIFT 2

/* Linear amplitude factors for the OPLL's 3 dB-per-step channel volume. */
static u8 YM_SustainGain[16] __attribute__((section(".dtcm"))) =
{
    255, 181, 128, 90, 64, 45, 32, 22, 16, 11, 8, 5, 4, 2, 1, 0
};

#define YM_SUSTAIN_TICK_SAMPLES 64 // Samples between sustain-level gain reductions

#define YM_OUT_SHIFT 8 // Output scaling for the mixer and rhythm paths.
                       // The FM path scales its output separately.

/* Extra modulator-depth shift per ROM preset (0 = global depth). Index 0 = custom patch. */
static u8 YM_PresetFMExtra[16] __attribute__((section(".dtcm"))) = { 3,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0 };   // 11 = Harpsichord (dampen for Aleste)

//@----------------------------------------------------------------------------
//@ 256-entry soft-square-like carrier waveform table, amplitude -127..127.
//@ The modulator uses a separate sine table.
//@----------------------------------------------------------------------------
static s8 YM_SineTable[256] __attribute__((section(".dtcm"))) =
{
       0,   13,   27,   39,   52,   64,   75,   85,   94,  102,  109,  115,  119,  123,  125,  127,
     127,  127,  125,  124,  121,  119,  116,  113,  110,  107,  104,  101,   99,   98,   97,   96,
      96,   96,   97,   98,   99,  101,  102,  104,  106,  108,  110,  112,  114,  115,  116,  116,
     116,  116,  116,  115,  114,  112,  111,  109,  107,  106,  104,  103,  101,  100,   99,   99,
      99,   99,   99,  100,  101,  103,  104,  106,  107,  109,  111,  112,  114,  115,  116,  116,
     116,  116,  116,  115,  114,  112,  110,  108,  106,  104,  102,  101,   99,   98,   97,   96,
      96,   96,   97,   98,   99,  101,  104,  107,  110,  113,  116,  119,  121,  124,  125,  127,
     127,  127,  125,  123,  119,  115,  109,  102,   94,   85,   75,   64,   52,   39,   27,   13,
       0,  -13,  -27,  -39,  -52,  -64,  -75,  -85,  -94, -102, -109, -115, -119, -123, -125, -127,
    -127, -127, -125, -124, -121, -119, -116, -113, -110, -107, -104, -101,  -99,  -98,  -97,  -96,
     -96,  -96,  -97,  -98,  -99, -101, -102, -104, -106, -108, -110, -112, -114, -115, -116, -116,
    -116, -116, -116, -115, -114, -112, -111, -109, -107, -106, -104, -103, -101, -100,  -99,  -99,
     -99,  -99,  -99, -100, -101, -103, -104, -106, -107, -109, -111, -112, -114, -115, -116, -116,
    -116, -116, -116, -115, -114, -112, -110, -108, -106, -104, -102, -101,  -99,  -98,  -97,  -96,
     -96,  -96,  -97,  -98,  -99, -101, -104, -107, -110, -113, -116, -119, -121, -124, -125, -127,
    -127, -127, -125, -123, -119, -115, -109, -102,  -94,  -85,  -75,  -64,  -52,  -39,  -27,  -13,
};

/*
 * Sine lookup used by the normal FM path's modulator and by the tonal rhythm
 * voices. It is separate from YM_SineTable (the carrier waveform); the
 * tables are not blended. Stored in DTCM for fast audio-rate access.
 */
static s8 YM_CarrierSineTable[256] __attribute__((section(".dtcm"))) =
{
      0,   3,   6,   9,  12,  16,  19,  22,  25,  28,  31,  34,  37,  40,  43,  46,
     49,  51,  54,  57,  60,  63,  65,  68,  71,  73,  76,  78,  81,  83,  85,  88,
     90,  92,  94,  96,  98, 100, 102, 104, 106, 108, 109, 111, 112, 114, 115, 117,
    118, 119, 120, 121, 122, 123, 124, 125, 125, 126, 126, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 126, 126, 125, 125, 124, 123, 122, 121, 120, 119, 118,
    117, 115, 114, 112, 111, 109, 108, 106, 104, 102, 100,  98,  96,  94,  92,  90,
     88,  85,  83,  81,  78,  76,  73,  71,  68,  65,  63,  60,  57,  54,  51,  49,
     46,  43,  40,  37,  34,  31,  28,  25,  22,  19,  16,  12,   9,   6,   3,   0,
      0,  -3,  -6,  -9, -12, -16, -19, -22, -25, -28, -31, -34, -37, -40, -43, -46,
    -49, -51, -54, -57, -60, -63, -65, -68, -71, -73, -76, -78, -81, -83, -85, -88,
    -90, -92, -94, -96, -98,-100,-102,-104,-106,-108,-109,-111,-112,-114,-115,-117,
   -118,-119,-120,-121,-122,-123,-124,-125,-125,-126,-126,-127,-127,-127,-127,-127,
   -127,-127,-127,-127,-127,-126,-126,-125,-125,-124,-123,-122,-121,-120,-119,-118,
   -117,-115,-114,-112,-111,-109,-108,-106,-104,-102,-100, -98, -96, -94, -92, -90,
    -88, -85, -83, -81, -78, -76, -73, -71, -68, -65, -63, -60, -57, -54, -51, -49,
    -46, -43, -40, -37, -34, -31, -28, -25, -22, -19, -16, -12,  -9,  -6,  -3,   0
};

//@----------------------------------------------------------------------------
//@ Real Yamaha MUL table, doubled (so index 0's real x0.5 is a whole number).
//@----------------------------------------------------------------------------
static   u8 YM_MulTableX2[16] __attribute__((section(".dtcm"))) =
{
    1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
};

//@----------------------------------------------------------------------------
//@ 16-entry instrument table containing Yamaha ROM preset data. The FM path
//@ uses MUL, TL, FB and the operator envelope rates/levels; other parameters
//@ are decoded but are not all modeled yet.
//@----------------------------------------------------------------------------
YM_Instrument YM_InstrumentROM[16] =
{
    /*  0 unused/custom */ { 9,12, 0,0, 1,1, 0,0, 0,0, 1,0, 12, 0,1, 2, 0,0,0,0,   0,0,0,0  },
    /*  1 Violin        */ { 1,1,  0,0, 1,1, 1,1, 0,0, 0,0, 30, 0,1, 7, 15,0,0,0,  7,8,1,7  },
    /*  2 Guitar        */ { 3,1,  0,0, 0,1, 0,0, 1,0, 0,0, 30, 1,0, 5, 13,7,1,3,  15,7,1,3 },
    /*  3 Piano         */ { 3,1,  0,0, 0,0, 0,0, 1,0, 2,0, 25, 0,0, 4, 15,2,1,1,  15,4,2,3 },
    /*  4 Flute         */ { 1,1,  0,0, 0,1, 1,1, 0,0, 0,0, 27, 0,0, 7, 10,15,4,0, 6,4,2,7  },
    /*  5 Clarinet      */ { 2,1,  0,0, 0,0, 1,1, 0,0, 0,0, 30, 0,0, 6, 15,0,0,8,  7,5,1,8  },
    /*  6 Oboe          */ { 1,2,  0,0, 0,0, 1,1, 1,0, 0,0, 22, 0,0, 5, 9,0,0,0,   7,1,1,3  },
    /*  7 Trumpet       */ { 1,1,  0,0, 0,1, 1,1, 0,0, 0,0, 29, 0,0, 7, 8,2,1,0,   8,0,1,7  },
    /*  8 Organ         */ { 3,1,  0,0, 0,0, 1,1, 0,0, 0,0, 45, 0,1, 6, 12,0,0,7,  7,0,0,7  },
    /*  9 Horn          */ { 1,1,  0,0, 1,1, 1,1, 0,0, 0,0, 27, 0,0, 6, 6,4,1,0,   6,5,1,7  },
    /* 10 Synthesizer   */ { 1,1,  0,0, 1,1, 1,1, 0,0, 0,0, 12, 1,1, 0, 8,5,7,0,   15,0,0,7 },
    /* 11 Harpsichord   */ { 3,1,  0,0, 0,0, 1,0, 0,0, 0,0, 7,  0,1, 1, 15,0,0,0,  10,4,2,2 },
    /* 12 Vibraphone    */ { 7,1,  1,1, 0,1, 0,0, 1,0, 0,0, 36, 0,0, 7, 15,15,2,2, 15,8,1,2 },
    /* 13 Synth Bass    */ { 1,0,  0,0, 1,0, 1,0, 0,1, 0,0, 12, 0,0, 5, 15,2,4,0,  15,4,4,4 },
    /* 14 Acoustic Bass */ { 1,1,  0,0, 0,0, 0,0, 0,0, 1,0, 21, 0,0, 3, 15,3,15,3, 9,2,15,3 },
    /* 15 Elec Guitar   */ { 1,1, 0,0,  1,1, 1,0, 0,0, 2,0, 9,  0,0, 3, 15,1,15,0, 15,4,1,3 },
};

/* Hot FM parameters. Keep these in DTCM: they are touched for every active
   melodic channel on every synthesized sample pair.  All instrument/block/
   volume-derived values are calculated when the channel configuration changes. */
typedef struct
{
    u8  arMod, arCar;       // attack: small per-sample step (<< 16 when applied)
    u8  modAtten;           // TL-derived linear amplitude factor from YM_TLAtten[]
    u16 fbDepth;            // YM_FM_FBDepth[FB] (phase units at full-scale feedback)
    u8  vibMod, vibCar;     // VIB bits: apply the vibrato LFO to that operator
    u8  modShift, fbShift;  // per-channel FM depth shifts (higher notes get less FM: less aliasing)
    u8  volume;             // 15 - channel volume
    u8  waveMod, waveCar;   // Custom waveform modes; 0=legacy/default, 1=sine, 2=half-sine
    u32 drMod, rrMod;       // decay/release per-sample envelope steps (16.16 fixed)
    u32 drCar, rrCar;       // Carrier decay/release per-sample envelope steps (16.16 fixed)
    u32 slMod, slCar;       // sustain target levels, already << 16
    u32 susMod, susCar;     // extra decay while key held: 0 = hold (EG-TYP=1), else RR step (EG-TYP=0)
} YM_FMParams;

static YM_FMParams YM_FM[YM_NUM_CHANNELS] __attribute__((section(".dtcm")));
static u8 YM_EnvDiv __attribute__((section(".dtcm")));   // envelope clock divider counter
static u8 YM_HalfRatePhase __attribute__((section(".dtcm")));
static s32 YM_HeldMelodicSample __attribute__((section(".dtcm")));

/* Modulator depth. The real chip swings the carrier phase by about +/-4pi at
   TL=0. 14 = real chip, 15 = half (default), 16 = quarter. If FM sounds gritty
   (melodic FM renders at half rate, so strong sidebands alias), raise it. */
#define YM_MOD_INDEX_SHIFT 17

/* Feedback depth. 7 = real chip, 9 = quarter (default), 20 = effectively off. */
#define YM_FB_SHIFT 9

/* Blocks above this get progressively less FM depth (1 shift per block). 7 = off. */
#define YM_FM_FULL_BLOCK 4

/* Vibrato LFO (~6.4 Hz at the half-rate render), applied only to operators
   with the VIB bit set. Depth = rate >> YM_VIB_SHIFT per LFO unit (LFO is
   -2..+2): 9 = about +/-7 cents, 10 = about +/-3.5 cents. */
#define YM_VIB_SHIFT   9
#define YM_VIB_PERIOD  303   // render samples per LFO step (8 steps per cycle)
static s8  YM_VibTable[8] __attribute__((section(".dtcm"))) = { 0, 1, 2, 1, 0, -1, -2, -1 };
static s8  YM_VibNow  __attribute__((section(".dtcm")));
static u8  YM_VibPos  __attribute__((section(".dtcm")));
static u16 YM_VibCnt  __attribute__((section(".dtcm")));

// This should be roughly 0.75dB per step... approx.
static u8 YM_TLAtten[64] __attribute__((section(".dtcm"))) = {
    255,234,215,197,181,166,152,139,128,117,108,99,90,83,76,70,
    64,59,54,49,45,42,38,35,32,29,27,25,23,21,19,18,
    16,15,14,12,11,10,10,9,8,7,7,6,6,5,5,4,
    4,4,3,3,3,3,2,2,2,2,2,2,1,1,1,1
};

//@----------------------------------------------------------------------------
//@ Small helpers
//@----------------------------------------------------------------------------
static YM_Instrument *YM_GetInstrument(YM *chip, u8 instrument)
{
    if (instrument == 0)
    {
        return &chip->customInstrument;
    }
    return &YM_InstrumentROM[instrument & 0x0F];
}

static inline void YM_PrecomputeFMRates(YM *chip, int ch);

/* Phase increment divisor. 72 = real chip (A4 = 437 Hz at fNum 288/block 4).
   36 = old behaviour (one octave sharp). 51 = half octave down, for A/B only. */
#define YM_PHASE_DIV 72
    
static u32 YM_ComputePhaseIncrement(u16 fNumber, u8 block, u8 mulNibble)
{
    // Integer-only: inc = F * 2^block * mulX2 * (masterClock << 12) / (72 * sampleRate)
    // See the development notes for the derivation. This rearrangement avoids
    // floating point entirely; both operands are integers.
    unsigned long long num = (unsigned long long)fNumber * (unsigned long long)YM_MulTableX2[mulNibble & 0x0F];
    num <<= block;
    num *= (unsigned long long)YM_MASTER_CLOCK << 12;

    unsigned long long inc = num / ((unsigned long long)YM_PHASE_DIV * YM_SAMPLE_RATE);
    
    if (inc >= 0x40000000ULL)
    {
        return 0; // would alias: freeze the operator
    }
    return (u32)inc;
}

static void YM_UpdateChannelFreq(YM *chip, int ch)
{
    YM_Channel *c = &chip->channels[ch];
    const YM_Instrument *inst = c->instPtr;

    c->osc.phaseIncrement =
        YM_ComputePhaseIncrement(c->fNumber, c->block, inst->mulCar);

    c->osc.modPhaseIncrement =
        YM_ComputePhaseIncrement(c->fNumber, c->block, inst->mulMod);

    YM_PrecomputeFMRates(chip, ch);
}

static void YM_UpdateCustomUsers(YM *chip)
{
    int ch;
    for (ch = 0; ch < YM_NUM_CHANNELS; ch++)
    {
        if (chip->channels[ch].instrument == 0)
        {
            YM_UpdateChannelFreq(chip, ch);
        }
    }
}

//@----------------------------------------------------------------------------
//@ Gain/envelope helper used by rhythm voices.
//@----------------------------------------------------------------------------
static inline void YM_UpdateGain(YM_Oscillator *osc, u8 keyOn, u32 releaseStep)
{
    if (keyOn)
    {
        osc->releaseAccum = 0;

        if (osc->gain < 255)
        {
            u16 g = osc->gain + (YM_GAIN_RAMP_STEP << 1);
            osc->gain = (g >= 255) ? 255 : (u8)g;
            osc->sustainCounter = 0;
        }
        else if (osc->gain > osc->sustainGain)
        {
            osc->sustainCounter += 2;

            if (osc->sustainCounter >= YM_SUSTAIN_TICK_SAMPLES)
            {
                osc->sustainCounter = 0;
                osc->gain--;
            }
        }
        else
        {
            osc->sustainCounter = 0;
        }
    }
    else if (osc->gain > 0)
    {
        osc->sustainCounter = 0;

        osc->releaseAccum += releaseStep << 1;

        if (osc->releaseAccum >= 0x10000)
        {
            osc->releaseAccum -= 0x10000;
            osc->gain--;
        }
    }
}

//@----------------------------------------------------------------------------
//@ Public interface
//@----------------------------------------------------------------------------
void YMReset(YM *chip)
{
    memset(chip, 0, sizeof(YM));
    chip->noiseLFSR = 1; // must not be seeded with 0, or the LFSR locks up
    YM_EnvDiv = 0;
    YM_HalfRatePhase = 0;
    YM_HeldMelodicSample = 0;
    YM_VibNow = 0;
    YM_VibPos = 0;
    YM_VibCnt = 0;
    int ch;
    for (ch = 0; ch < YM_NUM_CHANNELS; ch++)
    {
        chip->channels[ch].instPtr = &chip->customInstrument;
        chip->channels[ch].osc.sustainGain = 255;
        YM_UpdateChannelFreq(chip, ch);
    }
}

static void YM_RhythmRetrigger(YM_Oscillator *osc, int resetPhase)
{
    /* YM2413 rhythm bits are trigger/key-on controls, not sustained
       oscillator gates.  A 0->1 write starts a new percussion envelope. */
    osc->gain = 255;
    osc->releaseAccum = 0;
    osc->sustainCounter = 0;
    if (resetPhase)
    {
        osc->phase = 0;
    }
}

void YMWrite(u8 value, u8 address, YM *chip)
{
    if (address <= 0x07)
    {
        YM_Instrument *ci = &chip->customInstrument;
        switch (address)
        {
        case 0x00:
            ci->amMod = (value & YM_REG_AM_BIT) ? 1 : 0;
            ci->vibMod = (value & YM_REG_VIB_BIT) ? 1 : 0;
            ci->egTypeMod = (value & YM_REG_EGTYPE_BIT) ? 1 : 0;
            ci->ksrMod = (value & YM_REG_KSR_BIT) ? 1 : 0;
            ci->mulMod = value & YM_REG_MUL_MASK;
            break;
        case 0x01:
            ci->amCar = (value & YM_REG_AM_BIT) ? 1 : 0;
            ci->vibCar = (value & YM_REG_VIB_BIT) ? 1 : 0;
            ci->egTypeCar = (value & YM_REG_EGTYPE_BIT) ? 1 : 0;
            ci->ksrCar = (value & YM_REG_KSR_BIT) ? 1 : 0;
            ci->mulCar = value & YM_REG_MUL_MASK;
            break;
        case 0x02:
            ci->kslMod = value >> YM_REG_KSL_SHIFT;
            ci->tl = value & YM_REG_TL_MASK;
            break;
        case 0x03:
            ci->kslCar = value >> YM_REG_KSL_SHIFT;
            ci->dc = (value & YM_REG_DC_BIT) ? 1 : 0;
            ci->dm = (value & YM_REG_DM_BIT) ? 1 : 0;
            ci->fb = value & YM_REG_FB_MASK;
            break;
        case 0x04:
            ci->arMod = value >> YM_REG_AR_SHIFT;
            ci->drMod = value & YM_REG_DR_MASK;
            break;
        case 0x05:
            ci->arCar = value >> YM_REG_AR_SHIFT;
            ci->drCar = value & YM_REG_DR_MASK;
            break;
        case 0x06:
            ci->slMod = value >> YM_REG_SL_SHIFT;
            ci->rrMod = value & YM_REG_RR_MASK;
            break;
        case 0x07:
            ci->slCar = value >> YM_REG_SL_SHIFT;
            ci->rrCar = value & YM_REG_RR_MASK;
            break;
        }
        YM_UpdateCustomUsers(chip);
    }
    else if (address == 0x0E)
    {
        u8 old = chip->rhythmReg;
        chip->rhythmReg = value;

        /*
         * The rhythm bits are write-controlled percussion key-ons.  Aleste
         * explicitly does the expected trigger sequence:
         *
         *     0E=20   ; all rhythm voices off
         *     0E=28   ; SD + TOM on
         *
         * The mixer cannot reliably observe the intervening state because the
         * two CPU writes can occur between audio samples.  Therefore the
         * 0->1 transition must be handled here, at write time.
         *
         * Once triggered, a percussion voice decays on its own.  A rhythm bit
         * remaining at 1 must NOT hold the simplified oscillator/noise source
         * at full gain, otherwise the repeated 0E=20/28 sequences in real
         * music lose their attack/decay behavior.
         */
        if (value & YM_RHYTHM_ENABLE_BIT)
        {
            if (!(old & YM_RHYTHM_ENABLE_BIT))
            {
                /* Entering rhythm mode: channels 6-8 stop being melodic voices. */
                for (int c6 = YM_CHANNEL_BD; c6 <= YM_CHANNEL_TOMTCY; c6++)
                {
                    chip->channels[c6].keyOn = 0;
                    chip->channels[c6].osc.gain = 0; // drop any leftover melodic gain
                }

                /* Entering rhythm mode: any selected percussion voice is a new hit. */
                if (value & YM_RHYTHM_BD_BIT)
                {
                    YM_RhythmRetrigger(&chip->channels[YM_CHANNEL_BD].osc, 1);
                }
                if (value & YM_RHYTHM_SD_BIT)
                {
                    YM_RhythmRetrigger(&chip->rhythmSD, 1);
                }
                if (value & YM_RHYTHM_TOM_BIT)
                {
                    YM_RhythmRetrigger(&chip->channels[YM_CHANNEL_TOMTCY].osc, 1);
                }
                if (value & YM_RHYTHM_TCY_BIT)
                {
                    YM_RhythmRetrigger(&chip->rhythmTCY, 0);
                }
                if (value & YM_RHYTHM_HH_BIT)
                {
                    YM_RhythmRetrigger(&chip->channels[YM_CHANNEL_HHSD].osc, 0);
                }
            }
            else
            {
                /* Normal drum trigger: only newly asserted bits re-attack. */
                if ((value & YM_RHYTHM_BD_BIT) && !(old & YM_RHYTHM_BD_BIT))
                {
                    YM_RhythmRetrigger(&chip->channels[YM_CHANNEL_BD].osc, 1);
                }
                if ((value & YM_RHYTHM_SD_BIT) && !(old & YM_RHYTHM_SD_BIT))
                {
                    YM_RhythmRetrigger(&chip->rhythmSD, 1);
                }
                if ((value & YM_RHYTHM_TOM_BIT) && !(old & YM_RHYTHM_TOM_BIT))
                {
                    YM_RhythmRetrigger(&chip->channels[YM_CHANNEL_TOMTCY].osc, 1);
                }
                if ((value & YM_RHYTHM_TCY_BIT) && !(old & YM_RHYTHM_TCY_BIT))
                {
                    YM_RhythmRetrigger(&chip->rhythmTCY, 0);
                }
                if ((value & YM_RHYTHM_HH_BIT) && !(old & YM_RHYTHM_HH_BIT))
                {
                    YM_RhythmRetrigger(&chip->channels[YM_CHANNEL_HHSD].osc, 0);
                }
            }
        }
        else
        {
            /* Leaving rhythm mode mutes the dedicated rhythm voices. */
            chip->channels[YM_CHANNEL_BD].osc.gain = 0;
            chip->channels[YM_CHANNEL_TOMTCY].osc.gain = 0;
            chip->channels[YM_CHANNEL_HHSD].osc.gain = 0;
            chip->rhythmSD.gain = 0;
            chip->rhythmTCY.gain = 0;
        }
    }
    else if (address == 0x0F)
    {
        chip->testReg = value;
    }
    else if (address >= 0x10 && address <= 0x18)
    {
        int ch = address - 0x10;
        chip->channels[ch].fNumber = (chip->channels[ch].fNumber & 0x100) | value;
        YM_UpdateChannelFreq(chip, ch);
    }
    else if (address >= 0x20 && address <= 0x28)
    {
        int ch = address - 0x20;
        YM_Channel *c = &chip->channels[ch];
        c->sustain = (value & YM_REG_SUS_BIT) ? 1 : 0;
        c->block = (value >> YM_REG_BLOCK_SHIFT) & YM_REG_BLOCK_MASK;
        c->fNumber = (c->fNumber & 0x0FF) | ((value & YM_REG_FNUM_MSB_BIT) ? 0x100 : 0);
        YM_UpdateChannelFreq(chip, ch);

        if (!(chip->rhythmReg & YM_RHYTHM_ENABLE_BIT) || ch < YM_CHANNEL_BD)
        {
            u8 newKey = (value & YM_REG_KEY_BIT) ? 1 : 0;
            if (newKey && !c->keyOn)
            {
                c->osc.previousKeyOn = 0; /* make the mixer see a fresh key-on edge */
                c->osc.gain = 0;          /* clear stale activity before key-on     */
            }
            c->keyOn = newKey;
        }
    }
    else if (address >= 0x30 && address <= 0x38)
    {
        int ch = address - 0x30;
        u8 oldInst = chip->channels[ch].instrument;

        chip->channels[ch].instrument = value >> YM_REG_INST_SHIFT;
        chip->channels[ch].volume = value & YM_REG_VOL_MASK;
        chip->channels[ch].instPtr = YM_GetInstrument(chip, chip->channels[ch].instrument);
        chip->channels[ch].osc.sustainGain =
            ((chip->channels[ch].instPtr->slCar & 0x0F) == 15)
                ? 255
                : YM_SustainGain[chip->channels[ch].instPtr->slCar & 0x0F];
        if (ch == YM_CHANNEL_BD)
        {
            chip->rhythmVolBD = value & YM_REG_VOL_MASK;
        }
        if (ch == YM_CHANNEL_HHSD)
        {
            chip->rhythmVolHH = value >> YM_REG_INST_SHIFT;
            chip->rhythmVolSD = value & YM_REG_VOL_MASK;
        }
        if (ch == YM_CHANNEL_TOMTCY)
        {
            chip->rhythmVolTOM = value >> YM_REG_INST_SHIFT;
            chip->rhythmVolTCY = value & YM_REG_VOL_MASK;
        }

        if (chip->channels[ch].instrument != oldInst)
            YM_UpdateChannelFreq(chip, ch);   // new MUL values -> new phase increments
        else
            YM_PrecomputeFMRates(chip, ch);   // volume-only change
    }
}

// =====================================================================================
// Simplified melodic 2-operator FM synthesis.
//
// This integer-only approximation is not a cycle-accurate YM2413 core. It models
// two operators, simplified envelopes, MUL, TL and feedback. Block/KSR influence
// the approximate envelope-rate calculation; VIB drives a gentle vibrato LFO. KSL, AM
// and EG-TYP are decoded but not currently applied. Rhythm synthesis is handled separately.
// =====================================================================================

enum
{
    YM_FM_ENV_ATTACK = 0,
    YM_FM_ENV_DECAY,
    YM_FM_ENV_SUSTAIN,
    YM_FM_ENV_RELEASE
};

static u8 YM_FM_SustainAtten[16] __attribute__((section(".dtcm"))) = {
    0, 74, 128, 165, 191, 210, 223, 232, 239, 243, 247, 249, 251, 252, 253, 255
};

/* Feedback in phase units (256 per cycle) at full-scale modulator output.
   Real chip: 0, pi/16, pi/8, pi/4, pi/2, pi, 2pi, 4pi. */
static u16 YM_FM_FBDepth[8] __attribute__((section(".dtcm"))) = {
    0, 8, 16, 32, 64, 128, 256, 512
};

/* Attack still uses the old small per-sample step (applied << 16). */
static inline u8 YM_FMRateStepPrecompute(u8 rate, u8 block, u8 ksr)
{
    u32 r = rate & 0x0F;
    if (r == 0)
    {
        return 1;
    }
    if (ksr)
    {
        r += block >> 1;
    }
    else
    {
        r += block >> 2;
    }
    if (r > 15)
    {
        r = 15;
    }
    return (u8)(1 + (r * r >> 3));
}

/* Decay/release step: exponential in the rate nibble (speed doubles per step),
   16.16 fixed point per output sample.  Rate 0 = frozen (real OPLL behavior)
   unless mustMove is set (release must always terminate so the channel can
   go inactive). */
static inline u32 YM_FMRateInc(u8 rate, u8 block, u8 ksr, int mustMove)
{
    u32 r = rate & 0x0F;
    if (r == 0)
    {
        if (!mustMove)
        {
            return 0;
        }
        r = 1;
    }
    r += ksr ? (block >> 1) : (block >> 2);
    if (r > 15)
    {
        r = 15;
    }
    return (YM_RATE_BASE << (r - 1)) << YM_ENV_DIV_SHIFT; // per envelope update
}

static inline void YM_PrecomputeFMRates(YM *chip, int ch)
{
    const YM_Instrument *inst = chip->channels[ch].instPtr;
    u8 block = chip->channels[ch].block;
    YM_FMParams *p = &YM_FM[ch];

    p->arMod = YM_FMRateStepPrecompute(inst->arMod, block, inst->ksrMod);
    p->arCar = YM_FMRateStepPrecompute(inst->arCar, block, inst->ksrCar);

    p->drMod = YM_FMRateInc(inst->drMod, block, inst->ksrMod, 0);
    p->rrMod = YM_FMRateInc(inst->rrMod, block, inst->ksrMod, 1);
    p->drCar = YM_FMRateInc(inst->drCar, block, inst->ksrCar, 0);
    p->rrCar = YM_FMRateInc(inst->rrCar, block, inst->ksrCar, 1);

    p->slMod = (u32)YM_FM_SustainAtten[inst->slMod & 0x0F] << 16;
    p->slCar = (u32)YM_FM_SustainAtten[inst->slCar & 0x0F] << 16;

    /* Percussive (EG-TYP=0) sustain decay is OFF: every note holds at its
       sustain level while the key is down. To experiment, use:
           p->susMod = inst->egTypeMod ? 0 : p->rrMod;
           p->susCar = inst->egTypeCar ? 0 : p->rrCar;                       */
#define YM_EGTYP0_SHIFT 2  // 1 = half the RR speed; raise to fade slower
        p->susMod = 0;
        p->susCar = (chip->channels[ch].instrument && !inst->egTypeCar)
                    ? (p->rrCar >> YM_EGTYP0_SHIFT) : 0;
                    
    /* YM2413 TL is 0.75 dB per step: precompute the linear amplitude factor.
       Modulation depth is applied at full precision in the renderer. */
    p->modAtten = YM_TLAtten[inst->tl & 0x3F];
    /* Melodic FM renders at half rate, so FM sidebands on high notes fold back
       (alias) into rough, gritty noise. Back the depth off by one shift for
       every block above YM_FM_FULL_BLOCK. */
    {
        s32 d  = (s32)block - YM_FM_FULL_BLOCK;     // > 0 high notes (less FM), < 0 low notes (more FM)
        s32 ms = YM_MOD_INDEX_SHIFT + d;
        s32 fs = YM_FB_SHIFT + d;
        if (ms < 16) ms = 16;                       // 14 = real chip depth. We use 16 to raise the mod floor to avoid rumbling noise.
        if (fs < 7)  fs = 7;                        // 7 = real chip feedback
        p->modShift = (u8)ms;
        p->fbShift  = (u8)fs;

        p->modShift += YM_PresetFMExtra[chip->channels[ch].instrument & 15];
    }
    p->vibMod = inst->vibMod;
    p->vibCar = inst->vibCar;

    p->fbDepth = YM_FM_FBDepth[inst->fb & 7];
    p->volume = YM_SustainGain[chip->channels[ch].volume & 0x0F]; /* 3 dB/step like the OPLL */
    p->waveMod = chip->channels[ch].instrument == 0 && inst->dm;
    p->waveCar = chip->channels[ch].instrument == 0 ? (inst->dc ? 2 : 1) : 0;
}

/* One operator's envelope, advanced one output sample.
   keyEdge = fresh key-on this sample (computed once by the caller so both
   operators restart together). */
static __attribute__((noinline)) void YM_EnvStep(
    u32 *env, u8 *state, u8 keyOn, u8 keyEdge,
    u8 arStep, u32 drInc, u32 slTarget, u32 susInc, u32 rrInc)
{
    if (keyEdge)
    {
        *env = YM_ENV_SILENT;
        *state = YM_FM_ENV_ATTACK;
    }
    else if (!keyOn)
    {
        *state = YM_FM_ENV_RELEASE;
    }

    /* Ordered by how often each state is hit. No switch: the ARM9 has no
       branch predictor and a jump table is slow. */
    u8 st = *state;

    if (st == YM_FM_ENV_SUSTAIN)
    {
        // Hold at the sustain level while the key is down (susInc is 0),
        // or keep decaying for percussive (EG-TYP=0) instruments.
        if (susInc)
        {
            u32 e = *env + susInc;
            *env = (e > YM_ENV_SILENT) ? YM_ENV_SILENT : e;
        }
    }
    else if (st == YM_FM_ENV_RELEASE)
    {
        u32 e = *env + rrInc;
        *env = (e > YM_ENV_SILENT) ? YM_ENV_SILENT : e;
    }
    else if (st == YM_FM_ENV_DECAY)
    {
        u32 e = *env + drInc;
        if (e >= slTarget)
        {
            e = slTarget;
            *state = YM_FM_ENV_SUSTAIN;
        }
        *env = e;
    }
    else // YM_FM_ENV_ATTACK
    {
        // Faster as the envelope approaches zero, like a log-domain EG.
        // (scaled for the divided envelope clock)
        u32 e = *env;
        u32 delta = ((u32)arStep << (16 + YM_ENV_DIV_SHIFT)) +
                    ((YM_ENV_SILENT - e) >> (5 - YM_ENV_DIV_SHIFT));
        if (delta >= e)
        {
            e = 0;
            *state = YM_FM_ENV_DECAY;
        }
        else
        {
            e -= delta;
        }
        *env = e;
    }
}

ITCM_CODE static inline s32 YM_RenderChannel2FM(YM_Oscillator *osc, u8 keyOn, int ch, u8 doEnv)
{
    YM_FMParams *p = &YM_FM[ch];
    u8 keyEdge = (keyOn && !osc->previousKeyOn);

    if (keyEdge)
    {
        osc->phase = 0;
        osc->modPhase = 0;
        osc->feedback = 0;
    }

    /* Envelopes advance on the divided clock, but a key-on edge is always
       handled immediately so notes start on time. A key-off is picked up on
       the next envelope tick. Both operators restart together. */
    if (doEnv | keyEdge)
    {
        if (keyEdge || !keyOn ||
            osc->modState != YM_FM_ENV_SUSTAIN || p->susMod)
        {
            YM_EnvStep(&osc->modEnv, &osc->modState, keyOn, keyEdge,
                       p->arMod, p->drMod, p->slMod, p->susMod, p->rrMod);
        }

        if (keyEdge || !keyOn ||
            osc->carState != YM_FM_ENV_SUSTAIN || p->susCar)
        {
            YM_EnvStep(&osc->carEnv, &osc->carState, keyOn, keyEdge,
                       p->arCar, p->drCar, p->slCar, p->susCar, p->rrCar);
        }
    }

    osc->previousKeyOn = keyOn;

    /* The carrier alone decides audibility. When it reaches silence the
       channel goes inactive, which lets YMMixer() stop visiting it. (The
       modulator is not checked: some presets have RR=0 on the modulator.) */
    if (osc->carEnv >= YM_ENV_SILENT)
    {
        osc->gain = 0;
        return 0;
    }

    /* While the voice is audible, keep the active flag asserted.  It is
       deliberately not used for amplitude here. */
    osc->gain = 255;

    /* Per-operator increments, with optional vibrato (VIB bit). */
    u32 pi = osc->phaseIncrement;
    u32 mi = osc->modPhaseIncrement;
    if (p->vibCar)
    {
        pi += (pi >> YM_VIB_SHIFT) * YM_VibNow;
    }
    if (p->vibMod)
    {
        mi += (mi >> YM_VIB_SHIFT) * YM_VibNow;
    }

    osc->modPhase += mi;
    osc->phase += pi;

    /* Feedback modulates the modulator's own PHASE (as on the real chip). */
    u32 modIdx = osc->modPhase >> YM_SIN_SHIFT;
    if (p->fbDepth)
    {
        modIdx += (osc->feedback * (s32)p->fbDepth) >> p->fbShift;
    }
    s32 mod = YM_CarrierSineTable[modIdx & 0xFF];
    if (p->waveMod && mod < 0)
    {
        mod = 0;
    }

    /* Full precision: sine(+/-127) * env(0..255) * TL(0..255) = +/-8.2M, no
       early truncation. KSL is not applied. */
    s32 modFull = mod * ((255 - (s32)(osc->modEnv >> 16)) * p->modAtten);

    /* Feedback source = smoothed modulator output (stands in for the chip's
       two-sample average). */
    osc->feedback = (s16)((osc->feedback + (modFull >> 16)) >> 1);

    s32 modIndex = modFull >> p->modShift;

    u32 carrierIndex = ((osc->phase >> YM_SIN_SHIFT) + modIndex) & 0xFF;
    s32 carrier;
    if (p->waveCar == 0)
    {
        carrier = YM_SineTable[carrierIndex];
    }
    else
    {
        carrier = YM_CarrierSineTable[carrierIndex];
        if (p->waveCar == 2 && carrier < 0)
        {
            carrier = 0;
        }
    }

    // Carrier envelope and channel volume in one step: +/-8.2M, no early truncation.
    carrier = carrier * (255 - (s32)(osc->carEnv >> 16)) * (s32)p->volume;

    osc->modPhase += mi;
    osc->phase += pi;

    // Apply the configured YM output level after channel-volume attenuation.
    // Peak per channel is about +/-8.26M (127 * 255 * 255) before this shift.
    // (>>12) is +6 dB over the old (>>13), restoring loudness lost when the
    // sustain-level table and percussive decay were made more accurate.
    // Equivalent to (>>14) then << (ymScale+2). If dense chords crackle at high
    // ymScale, check the final mix for overflow or go back to (>>13).
    return carrier >> (12 - myConfig.ymScale);
}

// -------------------------------------------------------------------------
// And finally the mixer - this is a real hot-spot of emulation given the
// number of music channels that need to be handled. In ITCM to help speed.
//
// Melodic FM is rendered at half the output rate; rhythm and filtering
// remain at the full output rate.
// -------------------------------------------------------------------------
ITCM_CODE void YMMixer(int len, s16 *dest, YM *chip)
{
    int i;
    int rhythmOn = chip->rhythmReg & YM_RHYTHM_ENABLE_BIT;
    int lastMelodic = rhythmOn ? YM_CHANNEL_BD : YM_NUM_CHANNELS;

    for (i = 0; i < len; i++)
    {
        s32 sample = YM_HeldMelodicSample;

        if (!YM_HalfRatePhase)
        {
            s32 melodicSample = 0;
            u8 doEnv = ((++YM_EnvDiv) & ((1 << (YM_ENV_DIV_SHIFT - 1)) - 1)) == 0;

            /* Vibrato LFO step (one tick per rendered sample). */
            if (++YM_VibCnt >= YM_VIB_PERIOD)
            {
                YM_VibCnt = 0;
                YM_VibPos = (YM_VibPos + 1) & 7;
                YM_VibNow = YM_VibTable[YM_VibPos];
            }

            /* Melodic 2-op FM */
            for (int ch = 0; ch < lastMelodic; ch++)
            {
                YM_Channel *cc = &chip->channels[ch];

                if (!cc->keyOn && cc->osc.gain == 0)
                {
                    continue;
                }

                melodicSample += YM_RenderChannel2FM(
                    &cc->osc,
                    cc->keyOn,
                    ch,
                    doEnv);
            }

            sample = (YM_HeldMelodicSample + melodicSample) >> 1;
            YM_HeldMelodicSample = melodicSample;
        }
        YM_HalfRatePhase ^= 1;

        /*
         * Rhythm
         */
        if (rhythmOn)
        {
            YM_Channel *bd = &chip->channels[YM_CHANNEL_BD];
            YM_Channel *hs = &chip->channels[YM_CHANNEL_HHSD];
            YM_Channel *tt = &chip->channels[YM_CHANNEL_TOMTCY];

            /* Bass Drum */
            if (bd->osc.gain != 0)
            {
                YM_UpdateGain(
                    &bd->osc,
                    0,
                    YM_PERCUSSION_RELEASE_STEP);

                if (bd->osc.gain != 0)
                {
                    bd->osc.phase += bd->osc.phaseIncrement;

                    s32 s = YM_SineTable[(bd->osc.phase >> YM_SIN_SHIFT) & 0xFF];

                    sample +=
                        (s *
                         (15 - chip->rhythmVolBD) *
                         bd->osc.gain) >>
                        YM_OUT_SHIFT;
                }
            }

            /* Tom-Tom */
            if (tt->osc.gain != 0)
            {
                YM_UpdateGain(
                    &tt->osc,
                    0,
                    YM_PERCUSSION_RELEASE_STEP);

                if (tt->osc.gain != 0)
                {
                    tt->osc.phase += tt->osc.phaseIncrement;

                    s32 s = YM_SineTable[(tt->osc.phase >> YM_SIN_SHIFT) & 0xFF];

                    sample +=
                        (s *
                         (15 - chip->rhythmVolTOM) *
                         tt->osc.gain) >>
                        YM_OUT_SHIFT;
                }
            }

            /*
             * Noise percussion: one LFSR step per output sample.
             */
            if (hs->osc.gain != 0 ||
                chip->rhythmSD.gain != 0 ||
                chip->rhythmTCY.gain != 0)
            {
                if (chip->noiseLFSR & 1)
                {
                    chip->noiseLFSR ^= 0x800200;
                }

                chip->noiseLFSR >>= 1;
                chip->noiseLFSR &= 0x7FFFFF;

                u32 noiseBit = chip->noiseLFSR & 1;

                u32 hhPhase =
                    (chip->channels[YM_CHANNEL_HHSD].osc.phase >> 22) &
                    0x3FF;

                u32 cymPhase =
                    (chip->channels[YM_CHANNEL_TOMTCY].osc.phase >> 22) &
                    0x3FF;

                u32 shortNoise =
                    (((hhPhase >> 2) & 1) ^ ((hhPhase >> 7) & 1)) |
                    (((hhPhase >> 3) & 1) ^ ((cymPhase >> 5) & 1)) |
                    (((cymPhase >> 3) & 1) ^ ((cymPhase >> 5) & 1));

#define YM_RHYTHM_WAVE(p) \
    YM_SineTable[((p) >> 2) & 0xFF]

                /* Hi-Hat */
                if (hs->osc.gain != 0)
                {
                    YM_UpdateGain(
                        &hs->osc,
                        0,
                        YM_PERCUSSION_RELEASE_STEP);

                    if (hs->osc.gain != 0)
                    {
                        u32 phase;

                        if (shortNoise)
                        {
                            phase = noiseBit ? 0x2D0 : 0x234;
                        }
                        else
                        {
                            phase = noiseBit ? 0x034 : 0x0D0;
                        }

                        s32 s = YM_RHYTHM_WAVE(phase);

                        sample +=
                            (s *
                             (15 - chip->rhythmVolHH) *
                             hs->osc.gain) >>
                            YM_OUT_SHIFT;
                    }
                }

                /* Snare Drum: preserve existing tuning */
                if (chip->rhythmSD.gain != 0)
                {
                    YM_UpdateGain(
                        &chip->rhythmSD,
                        0,
                        2000);

                    if (chip->rhythmSD.gain != 0)
                    {
                        u32 phase;

                        if (hhPhase & 0x100)
                        {
                            phase = noiseBit ? 0x300 : 0x200;
                        }
                        else
                        {
                            phase = noiseBit ? 0x000 : 0x100;
                        }

                        s32 s = YM_RHYTHM_WAVE(phase);

                        sample +=
                            (s *
                             (15 - chip->rhythmVolSD) *
                             chip->rhythmSD.gain * 19) >>
                            (YM_OUT_SHIFT + 4);
                    }
                }

                /* Top Cymbal */
                if (chip->rhythmTCY.gain != 0)
                {
                    YM_UpdateGain(
                        &chip->rhythmTCY,
                        0,
                        YM_PERCUSSION_RELEASE_STEP);

                    if (chip->rhythmTCY.gain != 0)
                    {
                        u32 phase = shortNoise ? 0x300 : 0x100;
                        s32 s = YM_RHYTHM_WAVE(phase);

                        sample +=
                            (s *
                             (15 - chip->rhythmVolTCY) *
                             chip->rhythmTCY.gain) >>
                            YM_OUT_SHIFT;
                    }
                }

#undef YM_RHYTHM_WAVE
            }
        }

        /*
         * Output filter: one update per output sample. Experimentally this sounds best at >> 5.
         */
        sample += (chip->outputFilterState - sample) >> 5;
        chip->outputFilterState = sample;

        /* Mix one synthesized sample into the existing output buffer. */
        s32 mixed = ((s32)dest[i] + 32768) + sample;

        // Clamp final mixed result...
        if (mixed > 32767) mixed = 32767;
        else if (mixed < -32768) mixed = -32768;

        dest[i] = (s16)mixed;
    }
}

// End of file
