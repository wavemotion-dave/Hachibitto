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

//
//  msx_music.c
//  MSX-MUSIC (Yamaha YM2413 / OPLL) sound chip emulator - music only.
//  The normal/DSi mixer uses a simplified two-operator FM path with per-operator
//  attack/decay/sustain/release states. The DS-Lite fast mixer retains a cheaper
//  single-oscillator gain model. Neither path is a cycle-accurate OPLL core.
//
//  Please note: this file was largely generated with Claude.AI and ChatGPT with a lot
//  of patience (and frustration) by the author. Sound drivers are not my specialty...
//  in fact, we are not really on speaking terms. So if you happen to understand basic
//  sound sampling principles and can figure out FM Synthesis and want to help improve
//  this - please be my guest!

#include "msx_music.h"
#include <string.h>    // memcpy, memset

//@----------------------------------------------------------------------------
//@ Tunables.
//@----------------------------------------------------------------------------
#define YM_SAMPLE_RATE             27965     // confirmed value, matches the AY driver's rate
#define YM_MASTER_CLOCK            3579545   // MSX standard clock, same as SCC's
#define YM_SIN_SHIFT               24        // phase>>24 -> 8-bit (256 entry) table index
#define YM_GAIN_RAMP_STEP          16        // Legacy fast-mixer/rhythm gain ramp toward full
                                             // level on key-on; the normal FM path has operator envelopes.
#define YM_PERCUSSION_RELEASE_STEP 5500      // PERCUSSION release rate: higher = faster decay
                                             // for more distinct drum hits.
                                             
/* Legacy fast-mixer carrier sustain approximation: map OPLL SL to gain levels
   and move toward the target slowly. The normal FM path uses operator envelopes. */
static   u8 YM_SustainGain[16] __attribute__((section(".dtcm"))) =
{
    255, 181, 128, 90, 64, 45, 32, 22,
     16,  11,   8,  5,  4,  2,  1,  0
};
#define YM_SUSTAIN_TICK_SAMPLES 64  // Samples between sustain-level gain reductions

#define YM_OUT_SHIFT  8     // Output scaling for the legacy fast-mixer and rhythm paths.
                            // The normal/DSi two-operator FM path scales its output separately.

//@----------------------------------------------------------------------------
//@ 256-entry soft-square-like waveform table, amplitude -127..127. It supplies
//@ additional harmonics for the carrier in the normal FM path and the legacy
//@ single-oscillator path. The normal FM renderer also uses a separate sine
//@ table for its modulator, so this is not the only per-sample table lookup.
//@----------------------------------------------------------------------------
static   s8 YM_SinTable[256] __attribute__((section(".dtcm"))) =
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
 * voices. It is separate from YM_SinTable (the carrier/legacy waveform); the
 * tables are not blended. Stored in DTCM for fast audio-rate access.
 */
static   s8 YM_CarrierSineTable[256] __attribute__((section(".dtcm"))) =
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

// Exact 3-step advance of the YM noise LFSR.
// Indexed by the original low 3 bits.
// Must be in DTCM/fast RAM if possible.
static   u32 YM_NoiseAdvance3[8] __attribute__((section(".dtcm")))
=
{
    0x000000,
    0x200080,
    0x400100,
    0x600180,
    0x800200,
    0xA00280,
    0xC00300,
    0xE00380
};

//@----------------------------------------------------------------------------
//@ 16-entry instrument table containing Yamaha ROM preset data. The normal
//@ FM path uses MUL, TL, FB and the operator envelope rates/levels; other
//@ parameters are decoded but are not all modeled yet. The DS-Lite fast path
//@ uses a smaller subset.
//@----------------------------------------------------------------------------
YM_Instrument YM_InstrumentROM[16] __attribute__((section(".dtcm"))) =
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
    u8 arMod, drMod, rrMod;
    u8 arCar, drCar, rrCar;
    u8 modAtten;       // TL-derived linear amplitude factor from YM_TLAtten[]
    u8 modDepth;       // TL-derived phase-deviation scale (see YM_PrecomputeFMRates)
    u8 fbDepth;        // YM_FM_FBDepth[FB]
    u8 slMod;          // sustain attenuation target
    u8 slCar;
    u8 volume;         // 15 - channel volume
} YM_FMParams;

static YM_FMParams YM_FM[YM_NUM_CHANNELS] __attribute__((section(".dtcm")));

static u8 YM_TLAtten[64] __attribute__((section(".dtcm"))) = {
    255,234,215,197,181,166,152,139,128,117,108,99,90,83,76,70,
    64,59,54,49,45,42,38,35,32,29,27,25,23,21,19,18,
    16,15,14,12,11,10,10,9,8,7,7,6,6,5,5,4,
    4,4,3,3,3,3,2,2,2,2,2,2,1,1,1,1
};


//@----------------------------------------------------------------------------
//@ Small helpers
//@----------------------------------------------------------------------------
static YM_Instrument *YM_GetInstrument(  YM *chip, u8 instrument)
{
    if (instrument == 0)
        return &chip->customInstrument;
    return &YM_InstrumentROM[instrument & 0x0F];
}

static inline void YM_PrecomputeFMRates(YM *chip, int ch);

static u32 YM_ComputePhaseIncrement(u16 fNumber, u8 block, u8 mulNibble)
{
    // Integer-only: inc = F * 2^block * mulX2 * (masterClock << 12) / (72 * sampleRate)
    // See the development notes for the derivation. This rearrangement avoids
    // floating point entirely; both operands are integers.
    unsigned long long num = (unsigned long long)fNumber * (unsigned long long)YM_MulTableX2[mulNibble & 0x0F];
    num <<= block;
    num *= (unsigned long long)YM_MASTER_CLOCK << 12;
    unsigned long long inc = num / ((unsigned long long)72 * (unsigned long long)YM_SAMPLE_RATE);
    if (inc > 0xFFFFFFFFULL) inc = 0xFFFFFFFFULL;
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
        if (chip->channels[ch].instrument == 0)
            YM_UpdateChannelFreq(chip, ch);
}

//@----------------------------------------------------------------------------
//@ Legacy single-oscillator gain/envelope helper used by the fast mixer and rhythm voices.
//@----------------------------------------------------------------------------
//@ The legacy fast-mixer/rhythm path uses a quick gain ramp on key-on and a
//@ fractional release accumulator. The normal/DSi FM path uses separate
//@ modulator and carrier envelope state machines instead.
//@----------------------------------------------------------------------------
static inline void YM_UpdateGain2(YM_Oscillator *osc, u8 keyOn, u32 releaseStep)
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
//@ Legacy single-oscillator renderer for the DS-Lite fast mixer. It uses a
//@ gain ramp rather than the normal mixer's two-operator FM envelopes. The
//@ rhythm voices use the related gain helper above.
//@----------------------------------------------------------------------------
/* Legacy fast-mixer release table. The mapping is an approximation rather
   than a hardware-rate table; larger RR values produce faster release here. */

static   u16 YM_RRReleaseStepTest[16] __attribute__((section(".dtcm"))) =
    {900,1000,1100,1200,1350,1500,1700,1900,2150,2400,2700,3000,3300,3550,3780,3984};

//@----------------------------------------------------------------------------
//@ Public interface
//@----------------------------------------------------------------------------
void YMReset(YM *chip)
{
    memset(chip, 0, sizeof(YM));
    chip->noiseLFSR = 1;    // must not be seeded with 0, or the LFSR locks up
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
        osc->phase = 0;
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
            case 0x04: ci->arMod = value >> YM_REG_AR_SHIFT; ci->drMod = value & YM_REG_DR_MASK; break;
            case 0x05: ci->arCar = value >> YM_REG_AR_SHIFT; ci->drCar = value & YM_REG_DR_MASK; break;
            case 0x06: ci->slMod = value >> YM_REG_SL_SHIFT; ci->rrMod = value & YM_REG_RR_MASK; break;
            case 0x07: ci->slCar = value >> YM_REG_SL_SHIFT; ci->rrCar = value & YM_REG_RR_MASK; break;
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
                /* Entering rhythm mode: any selected percussion voice is a new hit. */
                if (value & YM_RHYTHM_BD_BIT)
                    YM_RhythmRetrigger(&chip->channels[YM_CHANNEL_BD].osc, 1);
                if (value & YM_RHYTHM_SD_BIT)
                    YM_RhythmRetrigger(&chip->rhythmSD, 1);
                if (value & YM_RHYTHM_TOM_BIT)
                    YM_RhythmRetrigger(&chip->channels[YM_CHANNEL_TOMTCY].osc, 1);
                if (value & YM_RHYTHM_TCY_BIT)
                    YM_RhythmRetrigger(&chip->rhythmTCY, 0);
                if (value & YM_RHYTHM_HH_BIT)
                    YM_RhythmRetrigger(&chip->channels[YM_CHANNEL_HHSD].osc, 0);
            }
            else
            {
                /* Normal drum trigger: only newly asserted bits re-attack. */
                if ((value & YM_RHYTHM_BD_BIT) && !(old & YM_RHYTHM_BD_BIT))
                    YM_RhythmRetrigger(&chip->channels[YM_CHANNEL_BD].osc, 1);
                if ((value & YM_RHYTHM_SD_BIT) && !(old & YM_RHYTHM_SD_BIT))
                    YM_RhythmRetrigger(&chip->rhythmSD, 1);
                if ((value & YM_RHYTHM_TOM_BIT) && !(old & YM_RHYTHM_TOM_BIT))
                    YM_RhythmRetrigger(&chip->channels[YM_CHANNEL_TOMTCY].osc, 1);
                if ((value & YM_RHYTHM_TCY_BIT) && !(old & YM_RHYTHM_TCY_BIT))
                    YM_RhythmRetrigger(&chip->rhythmTCY, 0);
                if ((value & YM_RHYTHM_HH_BIT) && !(old & YM_RHYTHM_HH_BIT))
                    YM_RhythmRetrigger(&chip->channels[YM_CHANNEL_HHSD].osc, 0);
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

        /* The melodic key-on field is not used for rhythm voices. */
        chip->channels[YM_CHANNEL_BD].keyOn = 0;
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
            c->keyOn = (value & YM_REG_KEY_BIT) ? 1 : 0;
    }
    else if (address >= 0x30 && address <= 0x38)
    {
        int ch = address - 0x30;
        chip->channels[ch].instrument = value >> YM_REG_INST_SHIFT;
        chip->channels[ch].volume = value & YM_REG_VOL_MASK;
        chip->channels[ch].instPtr = YM_GetInstrument(chip, chip->channels[ch].instrument);
        chip->channels[ch].osc.sustainGain =
            ((chip->channels[ch].instPtr->slCar & 0x0F) == 15)
            ? 255
            : YM_SustainGain[chip->channels[ch].instPtr->slCar & 0x0F];
        if (ch == YM_CHANNEL_BD)  chip->rhythmVolBD = value & YM_REG_VOL_MASK;
        if (ch == YM_CHANNEL_HHSD)  { chip->rhythmVolHH = value >> YM_REG_INST_SHIFT; chip->rhythmVolSD = value & YM_REG_VOL_MASK; }
        if (ch == YM_CHANNEL_TOMTCY) { chip->rhythmVolTOM = value >> YM_REG_INST_SHIFT; chip->rhythmVolTCY = value & YM_REG_VOL_MASK; }
        YM_UpdateChannelFreq(chip, ch);
    }
}

// =====================================================================================
// Simplified melodic 2-operator FM path used by the normal/DSi mixer.
//
// This integer-only approximation is not a cycle-accurate YM2413 core. It models
// two operators, simplified envelopes, MUL, TL and feedback. Block/KSR influence
// the approximate envelope-rate calculation; KSL, AM, VIB and EG-TYP are decoded
// but not currently applied. Rhythm synthesis and the DS-Lite fast mixer remain
// separate paths.
// =====================================================================================

enum
{
    YM_FM_ENV_ATTACK = 0,
    YM_FM_ENV_DECAY,
    YM_FM_ENV_SUSTAIN,
    YM_FM_ENV_RELEASE
};

static   u8 YM_FM_SustainAtten[16] =
{
      0,  8, 16, 24, 32, 40, 48, 56,
     64, 72, 80, 88, 96,104,112,127
};

static   u8 YM_FM_FBDepth[8] =
{
    0, 2, 4, 8, 16, 32, 64, 96
};

static inline u8 YM_FMRateStepPrecompute(u8 rate, u8 block, u8 ksr)
{
    u32 r = rate & 0x0F;
    if (r == 0)
        return 1;
    if (ksr)
        r += block >> 1;
    else
        r += block >> 2;
    if (r > 15)
        r = 15;
    return (u8)(1 + (r * r >> 3));
}

static inline void YM_PrecomputeFMRates(YM *chip, int ch)
{
    const YM_Instrument *inst = chip->channels[ch].instPtr;
    u8 block = chip->channels[ch].block;
    YM_FMParams *p = &YM_FM[ch];
    p->arMod = YM_FMRateStepPrecompute(inst->arMod, block, inst->ksrMod);
    p->drMod = YM_FMRateStepPrecompute(inst->drMod, block, inst->ksrMod);
    p->rrMod = YM_FMRateStepPrecompute(inst->rrMod, block, inst->ksrMod);
    p->arCar = YM_FMRateStepPrecompute(inst->arCar, block, inst->ksrCar);
    p->drCar = YM_FMRateStepPrecompute(inst->drCar, block, inst->ksrCar);
    p->rrCar = YM_FMRateStepPrecompute(inst->rrCar, block, inst->ksrCar);
    /* YM2413 TL is 0.75 dB per step, not a linear 4/step attenuation.
       Precompute the corresponding linear amplitude factor.  This is
       particularly important for patches with a heavily attenuated
       modulator: the old approximation could effectively erase it. */
    p->modAtten = YM_TLAtten[inst->tl & 0x3F];
    /* TL sets modulator level; do not apply the same attenuation a second
       time to FM deviation.  Scale the surviving modulator into a useful
       phase deviation range. */

    p->modDepth = (u8)(8 + ((63 - (inst->tl & 0x3F)) >> 1));
        
    p->fbDepth = YM_FM_FBDepth[inst->fb & 7];
    p->slMod = YM_FM_SustainAtten[inst->slMod & 0x0F];
    p->slCar = YM_FM_SustainAtten[inst->slCar & 0x0F];
    p->volume = (u8)(15 - (chip->channels[ch].volume & 0x0F));
}

ITCM_CODE static inline void YM_FMEnvelopeStep(
    YM_Oscillator *osc, u8 *env, u8 *state, u8 keyOn,
    u8 arStep, u8 drStep, u8 sl, u8 rrStep)
{
    if (keyOn && !osc->previousKeyOn)
    {
        *env = 255;
        *state = YM_FM_ENV_ATTACK;
    }
    else if (!keyOn && osc->previousKeyOn)
    {
        *state = YM_FM_ENV_RELEASE;
    }

    if (keyOn)
    {
        if (*state == YM_FM_ENV_ATTACK)
        {
            u8 step = arStep;
            if (step >= *env)
            {
                *env = 0;
                *state = YM_FM_ENV_DECAY;
            }
            else
            {
                // Faster as the envelope approaches zero, like a log-domain EG.
                u16 delta = step + ((255 - *env) >> 5);
                *env = (delta >= *env) ? 0 : (u8)(*env - delta);
            }
        }
        else if (*state == YM_FM_ENV_DECAY)
        {
            u8 target = sl;
            u8 step = drStep;
            u16 e = *env + step;

            if (e >= target)
            {
                *env = target;
                *state = YM_FM_ENV_SUSTAIN;
            }
            else
            {
                *env = (u8)e;
            }
        }
        else if (*state == YM_FM_ENV_SUSTAIN)
        {
            // Hold the operator at its sustain level while the key is down.
            // The previous approximation continued changing the envelope here,
            // which caused audible level/timbre "waffling" on sustained notes.
        }
    }
    else if (*state == YM_FM_ENV_RELEASE)
    {
        u8 step = rrStep;
        u16 e = *env + step;
        *env = (e >= 255) ? 255 : (u8)e;
    }

    osc->previousKeyOn = keyOn;
}

ITCM_CODE static inline s32 YM_RenderChannel2FM(YM_Oscillator *osc, u8 keyOn, int ch)
{
      YM_FMParams *p = &YM_FM[ch];
    u8 keyTransition = (keyOn && !osc->previousKeyOn);

    /* The normal mixer still uses gain as the cheap channel-active flag.
       Keep it synchronized with the new two-operator envelope so a released
       voice can actually disappear from the mix. */
    if (keyTransition)
        osc->gain = 255;

    YM_FMEnvelopeStep(osc, &osc->modEnv, &osc->modState, keyOn,
                      p->arMod, p->drMod, p->slMod, p->rrMod);

    // Use a separate carrier envelope state while sharing the oscillator's
    // previous-key flag only as the channel gate.
    // The two state machines are intentionally advanced from the same key event.
    // The modulator call updated previousKeyOn. Use the saved transition for
    // the carrier so both operators restart together on every key-on.
    if (keyTransition)
    {
        osc->carEnv = 255;
        osc->carState = YM_FM_ENV_ATTACK;
        osc->phase = 0;
        osc->modPhase = 0;
        osc->feedback = 0;
    }
    else if (!keyOn && osc->carState != YM_FM_ENV_RELEASE)
    {
        osc->carState = YM_FM_ENV_RELEASE;
    }

    if (keyOn)
    {
        if (osc->carState == YM_FM_ENV_ATTACK)
        {
            u8 step = p->arCar;
            if (step >= osc->carEnv)
            {
                osc->carEnv = 0;
                osc->carState = YM_FM_ENV_DECAY;
            }
            else
            {
                u16 delta = step + ((255 - osc->carEnv) >> 5);
                osc->carEnv = (delta >= osc->carEnv) ? 0 : (u8)(osc->carEnv - delta);
            }
        }
        else if (osc->carState == YM_FM_ENV_DECAY)
        {
            u8 target = p->slCar;
            u8 step = p->drCar;
            u16 e = osc->carEnv + step;
            if (e >= target)
            {
                osc->carEnv = target;
                osc->carState = YM_FM_ENV_SUSTAIN;
            }
            else
                osc->carEnv = (u8)e;
        }
        else if (osc->carState == YM_FM_ENV_SUSTAIN)
        {
            // Hold the carrier at its sustain level while the key is down.
            // Do not artificially ramp it here; that was the source of the
            // audible volume/timbre wavering in sustained notes.
        }
    }
    else if (osc->carState == YM_FM_ENV_RELEASE)
    {
        u8 step = p->rrCar;
        u16 e = osc->carEnv + step;
        osc->carEnv = (e >= 255) ? 255 : (u8)e;
    }

    if (osc->modEnv == 255 && osc->carEnv == 255)
    {
        /* Both operators have reached true release/silence.  This is also
           what allows YMMixer() to stop visiting the channel. */
        osc->gain = 0;
        return 0;
    }

    /* While a released voice is still audible, keep the legacy active flag
       asserted.  It is deliberately not used for amplitude here. */
    osc->gain = 255;

    // Two samples are synthesized at once, matching the existing normal mixer.
    osc->modPhase += osc->modPhaseIncrement << 1;
    osc->phase += osc->phaseIncrement << 1;

    s32 mod = YM_CarrierSineTable[(osc->modPhase >> YM_SIN_SHIFT) & 0xFF];
    s32 modGain = 255 - osc->modEnv;
    mod = (mod * modGain) >> 8;

    // TL-derived modulator attenuation, feedback depth and FM deviation are
    // precomputed per channel. KSL is not currently applied by this approximation.
    mod = (mod * p->modAtten) >> 8;

    // Feedback is deliberately kept small.  The carrier phase table has 256
    // entries, so feeding a raw +/-127 sample back into the index produces
    // nearly a full-cycle phase jump and sounds like broadband grit.
    s32 feedback = osc->feedback;
    if (p->fbDepth)
        mod += (feedback * p->fbDepth) >> 10;
    osc->feedback = (s16)mod;

    // TL-derived modulation depth is also precomputed.
    s32 modIndex = (mod * p->modDepth) >> 8;

    u32 carrierIndex = ((osc->phase >> YM_SIN_SHIFT) + modIndex) & 0xFF;
    s32 carrier = YM_SinTable[carrierIndex];

    // Carrier envelope followed by channel volume attenuation.
    carrier = (carrier * (255 - osc->carEnv)) >> 7;
    carrier = (carrier * p->volume) >> 3;

    // Match the baseline YMMixer amplitude.  The carrier is already scaled
    // by (15-volume) above; *16 here is equivalent to the old >>8 path
    // and restores the roughly 2x level lost in the previous FM renderer.
    return carrier * 6;
}

// -------------------------------------------------------------------------
// And finally the mixer - this is a real hot-spot of emulation given the
// number of music channels that need to be handled. In ITCM to help speed.
// -------------------------------------------------------------------------
ITCM_CODE void YMMixer(int len, s16 *dest, YM *chip)
{
    int i;
    int rhythmOn = chip->rhythmReg & YM_RHYTHM_ENABLE_BIT;
    int lastMelodic = rhythmOn ? YM_CHANNEL_BD : YM_NUM_CHANNELS;

    for (i = 0; i < len; i += 2)
    {
        s32 sample = 0;

        /* Melodic 2-op FM - run through all possible channels */
        for (int ch = 0; ch < lastMelodic; ch++)
        {
            YM_Channel *cc = &chip->channels[ch];

            if (!cc->keyOn && cc->osc.gain == 0)
                continue;

            sample += YM_RenderChannel2FM(&cc->osc, cc->keyOn, ch);
        }

        /*
         * -----------------------------------------------------------------
         * Rhythm
         * -----------------------------------------------------------------
         */
        if (rhythmOn)
        {
            YM_Channel *bd = &chip->channels[YM_CHANNEL_BD];
            YM_Channel *hs = &chip->channels[YM_CHANNEL_HHSD];
            YM_Channel *tt = &chip->channels[YM_CHANNEL_TOMTCY];

            /*
             * Bass Drum
             */
            if (bd->osc.gain != 0)
            {
                YM_UpdateGain2(
                    &bd->osc,
                    0,
                    YM_PERCUSSION_RELEASE_STEP
                );

                if (bd->osc.gain != 0)
                {
                    bd->osc.phase += bd->osc.phaseIncrement << 1;

                    s32 s = YM_CarrierSineTable[(bd->osc.phase >> YM_SIN_SHIFT) & 0xFF];

                    sample +=
                        (s *
                         (15 - chip->rhythmVolBD) *
                         bd->osc.gain) >>
                        YM_OUT_SHIFT;
                }
            }

            /*
             * Tom-Tom
             */
            if (tt->osc.gain != 0)
            {
                YM_UpdateGain2(
                    &tt->osc,
                    0,
                    YM_PERCUSSION_RELEASE_STEP
                );

                if (tt->osc.gain != 0)
                {
                    tt->osc.phase += tt->osc.phaseIncrement << 1;

                    s32 s = YM_CarrierSineTable[(tt->osc.phase >> YM_SIN_SHIFT) & 0xFF];

                    sample +=
                        (s *
                         (15 - chip->rhythmVolTOM) *
                         tt->osc.gain) >>
                        YM_OUT_SHIFT;
                }
            }

            /*
             * -----------------------------------------------------------------
             * Noise percussion
             *
             * Advance the YM2413 LFSR TWO steps, since this synthesis sample
             * represents two output samples.
             * -----------------------------------------------------------------
             */
            if (hs->osc.gain != 0 ||
                chip->rhythmSD.gain != 0 ||
                chip->rhythmTCY.gain != 0)
            {
                /*
                 * LFSR step #1
                 */
                if (chip->noiseLFSR & 1)
                    chip->noiseLFSR ^= 0x800200;

                chip->noiseLFSR >>= 1;
                chip->noiseLFSR &= 0x7FFFFF;

                /*
                 * LFSR step #2
                 */
                if (chip->noiseLFSR & 1)
                    chip->noiseLFSR ^= 0x800200;

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

#define YM_RHYTHM_WAVE_2(p) \
                    YM_SinTable[((p) >> 2) & 0xFF]

                /*
                 * Hi-Hat
                 */
                if (hs->osc.gain != 0)
                {
                    YM_UpdateGain2(
                        &hs->osc,
                        0,
                        YM_PERCUSSION_RELEASE_STEP
                    );

                    if (hs->osc.gain != 0)
                    {
                        u32 phase;

                        if (shortNoise)
                            phase = noiseBit ? 0x2D0 : 0x234;
                        else
                            phase = noiseBit ? 0x034 : 0x0D0;

                        s32 s = YM_RHYTHM_WAVE_2(phase);

                        sample +=
                            (s *
                             (15 - chip->rhythmVolHH) *
                             hs->osc.gain) >>
                            YM_OUT_SHIFT;
                    }
                }

                /*
                 * Snare Drum
                 *
                 * Keep the tuned DSi version exactly as-is:
                 * 2000 release and 19/16 gain.
                 */
                if (chip->rhythmSD.gain != 0)
                {
                    YM_UpdateGain2(
                        &chip->rhythmSD,
                        0,
                        2000
                    );

                    if (chip->rhythmSD.gain != 0)
                    {
                        u32 phase;

                        if (hhPhase & 0x100)
                            phase = noiseBit ? 0x300 : 0x200;
                        else
                            phase = noiseBit ? 0x000 : 0x100;

                        s32 s = YM_RHYTHM_WAVE_2(phase);

                        sample +=
                            (s *
                             (15 - chip->rhythmVolSD) *
                             chip->rhythmSD.gain * 19) >>
                            (YM_OUT_SHIFT + 4);
                    }
                }

                /*
                 * Top Cymbal
                 */
                if (chip->rhythmTCY.gain != 0)
                {
                    YM_UpdateGain2(
                        &chip->rhythmTCY,
                        0,
                        YM_PERCUSSION_RELEASE_STEP
                    );

                    if (chip->rhythmTCY.gain != 0)
                    {
                        u32 phase =
                            shortNoise ? 0x300 : 0x100;

                        s32 s = YM_RHYTHM_WAVE_2(phase);

                        sample +=
                            (s *
                             (15 - chip->rhythmVolTCY) *
                             chip->rhythmTCY.gain) >>
                            YM_OUT_SHIFT;
                    }
                }

#undef YM_RHYTHM_WAVE_2
            }
        }

        /*
         * Output filter.
         *
         * One low-pass smoothing update per synthesized sample, consistent
         * with the reduced-rate synthesis.
         */
        sample += (chip->outputFilterState - sample) >> 4;

        chip->outputFilterState = sample;

        /*
         * Output sample 0 and repeat for sample 1
         */
        s32 mixed = ((s32)dest[i] + 32767) + sample;
        if ((u32)(mixed + 32768) > 65535) mixed = (mixed < 0) ? -32768 : 32767;
        dest[i] = (s16)mixed;

        mixed = ((s32)dest[i + 1] + 32767) + sample;
        if ((u32)(mixed + 32768) > 65535) mixed = (mixed < 0) ? -32768 : 32767;
        dest[i + 1] = (s16)mixed;
    }
}

// =====================================================================================
// DS-Lite fast mixer
//
// Generates one FM sample for every two output samples.
//
// IMPORTANT:
//   State advances at 2x the normal per-output-sample increment, so pitch and
//   envelope timing remain approximately correct.
//
// This deliberately trades audio fidelity for CPU speed by using a single
// oscillator per melodic channel and holding each synthesized sample across
// multiple output samples. DSi/XL/LL should use the normal YMMixer().
// =====================================================================================

static inline s32 YM_RenderChannel2(YM_Oscillator *osc, u8 keyOn, u8 volume, u32 releaseStep)
{
    if (!(keyOn && osc->gain == osc->sustainGain))
        YM_UpdateGain2(osc, keyOn, releaseStep);

    if (osc->gain == 0)
        return 0;

    // Advance TWO audio samples at once.
    osc->phase += osc->phaseIncrement << 1;

    // Only ONE waveform lookup/multiply for the two output samples.
    s32 s = YM_SinTable[(osc->phase >> YM_SIN_SHIFT) & 0xFF];

    return (s * (15 - volume) * osc->gain) >> YM_OUT_SHIFT;
}


// =====================================================================================
// DS-Lite ultra-fast mixer helpers
//
// One synthesis sample is generated for every THREE output samples.
//
// State advances by three samples at a time, but only one waveform lookup/multiply
// is performed.
//
// This is intentionally aggressive.  The resulting audio has a ~9.3 kHz effective
// sample-and-hold rate, but FM-PAC music should remain recognizable and pitched
// correctly.
// =====================================================================================

static inline void YM_UpdateGain3(YM_Oscillator *osc, u8 keyOn, u32 releaseStep)
{
    if (keyOn)
    {
        osc->releaseAccum = 0;

        if (osc->gain < 255)
        {
            u16 g = osc->gain + (YM_GAIN_RAMP_STEP * 3);
            osc->gain = (g >= 255) ? 255 : (u8)g;
            osc->sustainCounter = 0;
        }
        else if (osc->gain > osc->sustainGain)
        {
            osc->sustainCounter += 3;

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

        osc->releaseAccum += releaseStep * 3;

        if (osc->releaseAccum >= 0x10000)
        {
            osc->releaseAccum -= 0x10000;
            osc->gain--;
        }
    }
}


static inline s32 YM_RenderChannel3(YM_Oscillator *osc, u8 keyOn, u8 attenuation, u32 releaseStep)
{
    if (!(keyOn && osc->gain == osc->sustainGain))
        YM_UpdateGain3(osc, keyOn, releaseStep);

    if (osc->gain == 0)
        return 0;

    // Advance THREE audio samples at once.
    osc->phase += osc->phaseIncrement * 3;

    // Only ONE waveform lookup/multiply for all three output samples.
    s32 s = YM_SinTable[(osc->phase >> YM_SIN_SHIFT) & 0xFF];

    return (s * attenuation * osc->gain) >> YM_OUT_SHIFT;
}


// -----------------------------------------------------------------------
// Call this one from DS-Lite/Phat only... it's deliberately simplified.
// -----------------------------------------------------------------------
void YMMixerFast(int len, s16 *dest, YM *chip)
{
    int i;
    int rhythmOn = chip->rhythmReg & YM_RHYTHM_ENABLE_BIT;
    int lastMelodic = rhythmOn ? YM_CHANNEL_BD : YM_NUM_CHANNELS;

      int volBD  = 15 - chip->rhythmVolBD;
      int volHH  = 15 - chip->rhythmVolHH;
      int volTOM = 15 - chip->rhythmVolTOM;
      int volTCY = 15 - chip->rhythmVolTCY;

    for (i = 0; i < len; i += 3)
    {
        s32 sample = 0;
        int ch;

        for (ch = 0; ch < lastMelodic; ch++)
        {
            YM_Channel *cc = &chip->channels[ch];

            if (!cc->keyOn && cc->osc.gain == 0)
                continue;

            int attenuation = 15 - cc->volume;

            sample += YM_RenderChannel3(
                &cc->osc,
                cc->keyOn,
                attenuation,
                (u32)YM_RRReleaseStepTest[cc->instPtr->rrCar & 0x0F] << 2);

            // Acoustic Bass fundamental.
            if (ch == 3 &&
                cc->instrument == 14 &&
                cc->osc.gain != 0)
            {
                s32 fundamental =
                    YM_SinTable[(cc->osc.phase >> YM_SIN_SHIFT) & 0xFF];

                sample +=
                    (fundamental * attenuation * cc->osc.gain) >>
                    (YM_OUT_SHIFT + 1);
            }
        }

        if (rhythmOn)
        {
            YM_Channel *bd = &chip->channels[YM_CHANNEL_BD];
            YM_Channel *hs = &chip->channels[YM_CHANNEL_HHSD];
            YM_Channel *tt = &chip->channels[YM_CHANNEL_TOMTCY];

            // Bass Drum
            if (bd->osc.gain != 0)
            {
                YM_UpdateGain3(&bd->osc, 0, YM_PERCUSSION_RELEASE_STEP);

                if (bd->osc.gain != 0)
                {
                    bd->osc.phase += bd->osc.phaseIncrement * 3;

                    s32 s =
                        YM_SinTable[
                            (bd->osc.phase >> YM_SIN_SHIFT) & 0xFF];

                    sample +=
                        (s * volBD * bd->osc.gain) >>
                        YM_OUT_SHIFT;
                }
            }

            // Tom-Tom
            if (tt->osc.gain != 0)
            {
                YM_UpdateGain3(&tt->osc, 0, YM_PERCUSSION_RELEASE_STEP);

                if (tt->osc.gain != 0)
                {
                    tt->osc.phase += tt->osc.phaseIncrement * 3;

                    s32 s =
                        YM_SinTable[
                            (tt->osc.phase >> YM_SIN_SHIFT) & 0xFF];

                    sample +=
                        (s * volTOM * tt->osc.gain) >>
                        YM_OUT_SHIFT;
                }
            }

            // Noise percussion
            if (hs->osc.gain != 0 ||
                chip->rhythmSD.gain != 0 ||
                chip->rhythmTCY.gain != 0)
            {
                /*
                 * Advance the LFSR THREE steps at once.
                 *
                 * This is mathematically identical to:
                 *
                 *   step
                 *   step
                 *   step
                 *
                 * in the old code.
                 */
                u32 noise = chip->noiseLFSR;

                chip->noiseLFSR =
                    (noise >> 3) ^ YM_NoiseAdvance3[noise & 7];

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

                #define YM_RHYTHM_WAVE_FAST(p) \
                    YM_SinTable[((p) >> 2) & 0xFF]

                // Hi-Hat
                if (hs->osc.gain != 0)
                {
                    YM_UpdateGain3(
                        &hs->osc,
                        0,
                        YM_PERCUSSION_RELEASE_STEP);

                    if (hs->osc.gain != 0)
                    {
                        u32 phase;

                        if (shortNoise)
                            phase = noiseBit ? 0x2D0 : 0x234;
                        else
                            phase = noiseBit ? 0x034 : 0x0D0;

                        s32 s = YM_RHYTHM_WAVE_FAST(phase);

                        sample +=
                            (s * volHH * hs->osc.gain) >>
                            YM_OUT_SHIFT;
                    }
                }

                // Snare Drum
                if (chip->rhythmSD.gain != 0)
                {
                    YM_UpdateGain3(&chip->rhythmSD,0,2000);

                    if (chip->rhythmSD.gain != 0)
                    {
                        u32 phase;

                        if (hhPhase & 0x100)
                            phase = noiseBit ? 0x300 : 0x200;
                        else
                            phase = noiseBit ? 0x000 : 0x100;

                        s32 s = YM_RHYTHM_WAVE_FAST(phase);

                        sample +=
                            (s * (15 - chip->rhythmVolSD) * chip->rhythmSD.gain * 19) >>
                            (YM_OUT_SHIFT + 4);
                    }
                }

                // Top Cymbal
                if (chip->rhythmTCY.gain != 0)
                {
                    YM_UpdateGain3(
                        &chip->rhythmTCY,
                        0,
                        YM_PERCUSSION_RELEASE_STEP);

                    if (chip->rhythmTCY.gain != 0)
                    {
                        u32 phase =
                            shortNoise ? 0x300 : 0x100;

                        s32 s = YM_RHYTHM_WAVE_FAST(phase);

                        sample +=
                            (s * volTCY * chip->rhythmTCY.gain) >>
                            YM_OUT_SHIFT;
                    }
                }

                #undef YM_RHYTHM_WAVE_FAST
            }
        }

        sample += (chip->outputFilterState - sample) >> 4;
        chip->outputFilterState = sample;

        // Output sample 0
        s32 mixed = ((s32)dest[i] + 32767) + sample;

        if (mixed > 32767)
            mixed = 32767;

        dest[i] = (s16)mixed;

        // Last partial block / samples 1 and 2
        if (i + 2 < len)
        {
            mixed = ((s32)dest[i + 1] + 32767) + sample;

            if (mixed > 32767)
                mixed = 32767;

            dest[i + 1] = (s16)mixed;

            mixed = ((s32)dest[i + 2] + 32767) + sample;

            if (mixed > 32767)
                mixed = 32767;

            dest[i + 2] = (s16)mixed;
        }
        else if (i + 1 < len)
        {
            mixed = ((s32)dest[i + 1] + 32767) + sample;

            if (mixed > 32767)
                mixed = 32767;

            dest[i + 1] = (s16)mixed;
        }
    }
}

// End of file
