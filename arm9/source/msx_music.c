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
//  See YM.h for the accuracy-level disclaimer: this is a drastically
//  simplified, minimal-FM, no-ADSR design chosen purely for speed.
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
#define YM_GAIN_RAMP_STEP          16        // ATTACK rate: gain moves this much per sample toward
                                             // full on key-on - ~16 samples (~0.6ms), fast/click-free
#define YM_RELEASE_STEP            3984      // MELODIC release rate: 16.16 fixed-point step targeting a
                                             // ~150ms fade to silence on key-off, not an instant cutoff.
                                             // This is the fix for FM music sounding "thin"/"cut" - real FM
                                             // pieces lean on overlapping decay tails for their fullness
                                             // (unlike AY music, which doesn't use per-note envelopes at
                                             // all), and cutting every note off in <1ms removed exactly
                                             // that. Retune this constant if it still isn't right - up
                                             // for a lusher/longer tail, down if notes start blurring
                                             // together too much.
#define YM_PERCUSSION_RELEASE_STEP 7500      // PERCUSSION release rate: ~30ms, NOT the melodic 150ms.
                                             // Real drums (hi-hat especially) decay in tens of ms, not
                                             // hundreds - using the melodic rate here made consecutive
                                             // hits overlap instead of sounding like distinct hits.

/* Carrier sustain level test: map OPLL SL to the same approximate gain
   levels used by the earlier envelope experiment, but move toward the target
   very slowly.  V0 key-on/release behavior remains otherwise unchanged. */
static const u8 YM_SustainGain[16] __attribute__((section(".dtcm"))) =
{
    255, 181, 128, 90, 64, 45, 32, 22,
     16,  11,   8,  5,  4,  2,  1,  0
};
#define YM_SUSTAIN_TICK_SAMPLES 64
                            // PERCUSSION release rate: ~30ms, NOT the melodic 150ms.
                            // Real drums (hi-hat especially) decay in tens of ms, not
                            // hundreds - using the melodic rate here made consecutive
                            // hits (fired every 100-150ms in a normal rhythm pattern)
                            // overlap and blend continuously instead of sounding like
                            // distinct hits. Applies to BD/TOM and HH/SD/TOP-CY alike.

#define YM_OUT_SHIFT  8     // Output headroom for everything - melodic channels AND
                            // percussion now both go through YM_SinTable via real
                            // phase-selection logic (see YMMixer), not a separate
                            // noise path, so one shared shift is enough. The earlier
                            // bump to 10 (and a separate, further-attenuated shift just
                            // for percussion) were both compensating for problems that
                            // turned out to have other causes (an AY sign-bias bug, and
                            // generic-noise percussion overlapping continuously) -
                            // neither issue exists anymore, so this is back to a single
                            // plain constant.

//@----------------------------------------------------------------------------
//@ 256-entry waveform table, amplitude -127..127. One lookup per active
//@ channel per sample - this is now the ONLY per-sample table lookup, so
//@ this is free real estate to make the waveform itself richer at ZERO
//@ extra cost. This was a pure sine originally, which is the single most
//@ harmonic-free waveform possible - that's exactly why every channel
//@ sounded "muffled"/"distant" (a pure tone has no overtones at all, no
//@ amount of envelope tuning fixes that). This is now a soft square-wave
//@ approximation (fundamental + 1/3 3rd harmonic + 1/5 5th + 1/7 7th),
//@ giving real harmonic content/brightness for the same one-lookup cost.
//@----------------------------------------------------------------------------
static const s8 YM_SinTable[256] __attribute__((section(".dtcm"))) =
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

//@----------------------------------------------------------------------------
//@ Real Yamaha MUL table, doubled (so index 0's real x0.5 is a whole number).
//@----------------------------------------------------------------------------
static const u8 YM_MulTableX2[16] __attribute__((section(".dtcm"))) =
{
    1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
};

// Exact 3-step advance of the YM noise LFSR.
// Indexed by the original low 3 bits.
// Must be in DTCM/fast RAM if possible.
static const u32 YM_NoiseAdvance3[8] __attribute__((section(".dtcm")))
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
//@ 16-entry instrument table - real Yamaha ROM data. Only mulMod/mulCar are
//@ read by the mixer right now; everything else is stored for later use.
//@----------------------------------------------------------------------------
const YM_Instrument YM_InstrumentROM[16] __attribute__((section(".dtcm"))) =
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

//@----------------------------------------------------------------------------
//@ Small helpers
//@----------------------------------------------------------------------------
static const YM_Instrument *YM_GetInstrument(const YM *chip, u8 instrument)
{
    if (instrument == 0)
        return &chip->customInstrument;
    return &YM_InstrumentROM[instrument & 0x0F];
}

static u32 YM_ComputePhaseIncrement(u16 fNumber, u8 block, u8 mulNibble)
{
    // Integer-only: inc = F * 2^block * mulX2 * (masterClock << 12) / (72 * sampleRate)
    // See chat history for the derivation - matches the Yamaha application manual's
    // fmus formula, just rearranged to avoid floating point entirely. Both constants
    // are plain integers now (no lingering compile-time-folded double literals).
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
}

static void YM_UpdateCustomUsers(YM *chip)
{
    int ch;
    for (ch = 0; ch < YM_NUM_CHANNELS; ch++)
        if (chip->channels[ch].instrument == 0)
            YM_UpdateChannelFreq(chip, ch);
}

//@----------------------------------------------------------------------------
//@ One oscillator, one sample. No FM, no ADSR - just a tone with a gain
//@----------------------------------------------------------------------------
//@ Shared by both render functions below. Attack is instant-ish (whole-unit
//@ steps, same as before). Release is a proper fractional ramp - a whole-
//@ unit-per-sample step can't express a slow enough rate for a musically
//@ reasonable fade, so this uses the same 16.16 accumulator technique as
//@ the earlier ADSR version, just for one rate instead of a per-instrument
//@ table.
//@----------------------------------------------------------------------------
ITCM_CODE static inline void YM_UpdateGain2(YM_Oscillator *osc, u8 keyOn, u32 releaseStep)
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
//@ One oscillator, one sample. No FM, no ADSR - just a tone with a gain
//@ that ramps toward its key-on/off target. Takes keyOn/volume explicitly
//@ (rather than reading a channel struct's fields directly) so the same
//@ function serves both ordinary melodic channels and the tonal rhythm
//@ voices (BD, TOM), which need their key-on state computed fresh from
//@ the rhythm register each sample rather than stored on the channel.
//@ isMelodic selects the release rate: melodic voices use the instrument's
//@ actual carrier RR value; percussion keeps the fixed fast release.
//@----------------------------------------------------------------------------
/* RR-dependent release, normalized so RR=15 is about the old
   150 ms V0 release, while lower RR values release faster. */
/*
 * Simplified carrier envelope model
 *
 * The original V0 renderer used a simple gain ramp: notes reached full
 * volume while held and then decayed at a fixed rate after key-off.
 *
 * Two cheap OPLL-inspired additions are retained here:
 *
 *   - SL (sustain level) sets the level a held note settles toward.
 *     SL=15 is treated as full level.  This is important for sounds that
 *     change channel volume while a key remains held (for example Gaiden
 *     footsteps); treating SL=15 as silence breaks those sounds.
 *
 *   - RR (release rate) controls how quickly the gain falls after key-off.
 *     The table below is deliberately normalized to the V0 gain model
 *     rather than attempting to reproduce the OPLL envelope generator
 *     literally.  RR=15 uses the original V0 release rate, while lower RR
 *     values release progressively more slowly.
 *
 * These approximations give noticeably better note blending and fuller
 * melodic lines at a much lower CPU cost than a full OPLL envelope model.
 */
static const u16 YM_RRReleaseStepTest[16] __attribute__((section(".dtcm"))) =
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

static inline s32 YM_RenderChannel2FM(
    YM_Oscillator *osc,
    u8 keyOn,
    u8 volume,
    u32 releaseStep,
    const YM_Instrument *inst)
{
    /*
     * Advance the envelope by TWO audio samples.
     */
    if (!(keyOn && osc->gain == osc->sustainGain))
        YM_UpdateGain2(osc, keyOn, releaseStep);

    if (osc->gain == 0)
        return 0;

    /*
     * Advance both FM operators by TWO samples.
     *
     * We still calculate the modulator and carrier, unlike the
     * DS-Lite fast mixer.
     */
    osc->modPhase += osc->modPhaseIncrement << 1;

    s32 mod = YM_SinTable[
        (osc->modPhase >> YM_SIN_SHIFT) & 0xFF
    ];

    s32 depth = (s32)(63 - (inst->tl & 0x3F)) >> 2;

    osc->phase += osc->phaseIncrement << 1;

    s32 modIndex = (mod * depth) >> 8;

    s32 carrier = YM_SinTable[
        ((osc->phase >> YM_SIN_SHIFT) + modIndex) & 0xFF
    ];

    return (carrier * (15 - volume) * osc->gain) >> YM_OUT_SHIFT;
}

ITCM_CODE void YMMixer(int len, s16 *dest, YM *chip)
{
    int i;
    int rhythmOn = chip->rhythmReg & YM_RHYTHM_ENABLE_BIT;
    int lastMelodic = rhythmOn ? YM_CHANNEL_BD : YM_NUM_CHANNELS;

    for (i = 0; i < len; i += 2)
    {
        s32 sample = 0;
        int ch;

        /*
         * -----------------------------------------------------------------
         * Melodic channels
         * -----------------------------------------------------------------
         */
        for (ch = 0; ch < lastMelodic; ch++)
        {
            YM_Channel *cc = &chip->channels[ch];

            if (!cc->keyOn && cc->osc.gain == 0)
                continue;

            sample += YM_RenderChannel2FM(
                &cc->osc,
                cc->keyOn,
                cc->volume,
                (u32)YM_RRReleaseStepTest[cc->instPtr->rrCar & 0x0F] << 2,
                cc->instPtr
            );

            /*
             * Acoustic Bass fundamental.
             */
            if (ch == 3 &&
                cc->instrument == 14 &&
                cc->osc.gain != 0)
            {
                s32 fundamental =
                    YM_SinTable[
                        (cc->osc.phase >> YM_SIN_SHIFT) & 0xFF
                    ];

                sample +=
                    (fundamental *
                     (15 - cc->volume) *
                     cc->osc.gain) >>
                    (YM_OUT_SHIFT + 1);
            }
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

                    s32 s =
                        YM_SinTable[
                            (bd->osc.phase >> YM_SIN_SHIFT) & 0xFF
                        ];

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

                    s32 s =
                        YM_SinTable[
                            (tt->osc.phase >> YM_SIN_SHIFT) & 0xFF
                        ];

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
         * One filter update per synthesized sample, which is consistent
         * with the reduced-rate synthesis.
         */
        sample +=
            (chip->outputFilterState - sample) >> 4;

        chip->outputFilterState = sample;

        /*
         * Output sample 0
         */
        s32 mixed =
            ((s32)dest[i] + 32767) + sample;

        if (mixed > 32767)
            mixed = 32767;

        if (mixed < -32768)
            mixed = -32768;

        dest[i] = (s16)mixed;

        /*
         * Hold the synthesized sample for output sample 1.
         */
        if (i + 1 < len)
        {
            mixed =
                ((s32)dest[i + 1] + 32767) + sample;

            if (mixed > 32767)
                mixed = 32767;

            if (mixed < -32768)
                mixed = -32768;

            dest[i + 1] = (s16)mixed;
        }
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
// This deliberately trades high-frequency audio fidelity for CPU speed.
// DSi/XL/LL should continue using the normal YMMixer().
// =====================================================================================

static inline s32 YM_RenderChannel2(
    YM_Oscillator *osc,
    u8 keyOn,
    u8 volume,
    u32 releaseStep)
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


static inline s32 YM_RenderChannel3(
    YM_Oscillator *osc,
    u8 keyOn,
    u8 attenuation,
    u32 releaseStep)
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


void YMMixerFast(int len, s16 *dest, YM *chip)
{
    int i;
    int rhythmOn = chip->rhythmReg & YM_RHYTHM_ENABLE_BIT;
    int lastMelodic = rhythmOn ? YM_CHANNEL_BD : YM_NUM_CHANNELS;

    const int volBD  = 15 - chip->rhythmVolBD;
    const int volHH  = 15 - chip->rhythmVolHH;
    const int volTOM = 15 - chip->rhythmVolTOM;
    const int volTCY = 15 - chip->rhythmVolTCY;

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
