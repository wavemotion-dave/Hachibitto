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
//  ACCURACY LEVEL - deliberately simplified OPLL emulation.
//
//  This is not a cycle-accurate or complete YM2413/OPLL implementation.
//  The normal/DSi mixer uses a simplified two-operator FM path with separate
//  modulator and carrier envelope states. The DS-Lite fast mixer retains a
//  cheaper single-oscillator model. Both trade chip accuracy for performance.
//
//  Current normal/DSi melodic synthesis:
//    - Two operators are synthesized per channel: the modulator changes the
//      carrier's phase, with simplified feedback and TL-derived modulation.
//    - MUL, TL, FB and AR/DR/SL/RR contribute to the model; block/KSR affect
//      the approximate envelope-rate calculation. KSL, AM, VIB and EG-TYP are
//      decoded but are not currently fully modeled.
//    - The envelope generators are simplified approximations, not the OPLL's
//      hardware rate tables or exact logarithmic envelope behavior.
//
//  Current DS-Lite fast-mixer synthesis:
//    - Melodic channels use a single oscillator and a gain ramp, without the
//      normal mixer's two-operator FM timbre generation. Carrier MUL affects
//      pitch; the legacy gain model approximates sustain and release.
//    - This lower-cost path intentionally sacrifices timbral fidelity for speed.
//
//  Current rhythm synthesis (shared in concept, with separate mixer code):
//    - Bass Drum and Tom-Tom use tonal oscillators.
//    - Hi-Hat, Snare Drum and Top Cymbal use inexpensive phase/noise-based
//      approximations and simplified gain/release behavior.
//
//  The implementation omits cycle-accurate timing and several parts of the
//  YM2413 model. Comments describe the current approximation, not a claim of
//  equivalence with a hardware OPLL. Performance measurements belong in notes.
//
//
//  Register map (verified against the Yamaha OPLL Application Manual,
//  not reconstructed from memory - see chat for the source):
//    $00/$01   mod/car: AM(D7) VIB(D6) EG-TYP(D5) KSR(D4) MUL(D3-0)
//    $02       mod:     KSL(D7-6) TL(D5-0)
//    $03       car:     KSL(D7-6) DC(D4) DM(D3) FB(D2-0)
//    $04/$05   mod/car: AR(D7-4) DR(D3-0)
//    $06/$07   mod/car: SL(D7-4) RR(D3-0)
//    $0E       D5=RHYTHM D4=BD D3=SD D2=TOM D1=TOP-CY D0=HH
//    $0F       TEST, normally 0
//    $10-$18   F-Number low byte, one per channel (ch = reg-0x10)
//    $20-$28   D5=SUS D4=KEY D3-1=BLOCK D0=F-Number bit8
//    $30-$38   D7-4=INST(0-15) D3-0=VOL
//
//  Rhythm mode (D5 of $0E set) repurposes channels 6/7/8 (zero-indexed):
//    ch6 = Bass Drum (tonal oscillator, key-on = D4/$0E)
//    ch7 = Hi-Hat (noise, key-on = D0/$0E) + Snare Drum (noise, D3/$0E)
//    ch8 = Tom-Tom (tonal oscillator, key-on = D2/$0E) + Top Cymbal (noise, D1/$0E)
//
//  F-number/block registers for channels 6-8 are still decoded while rhythm
//  mode is active; $36-$38 supply the individual rhythm-voice volume fields.
//

#ifndef YM_H
#define YM_H

#include <nds.h>

#define YM_NUM_CHANNELS      9

//@----------------------------------------------------------------------------
//@ Register-level instrument parameters. Both custom-instrument registers and
//@ ROM presets are decoded and stored. The normal/DSi mixer uses a subset of
//@ these fields; unsupported parameters remain available but are not all modeled.
//@----------------------------------------------------------------------------
typedef struct
{
    u8 mulMod, mulCar;              // $00/$01 D3-0 - operator multipliers (normal FM path)
    u8 amMod,  amCar;              // $00/$01 D7 - decoded; AM not currently modeled
    u8 vibMod, vibCar;             // $00/$01 D6 - decoded; VIB not currently modeled
    u8 egTypeMod, egTypeCar;       // $00/$01 D5 - decoded; EG-TYP behavior not modeled
    u8 ksrMod, ksrCar;             // $00/$01 D4 - used by approximate normal-FM rate calculation
    u8 kslMod, kslCar;             // $02/$03 D7-6 - decoded; KSL not currently applied
    u8 tl;                         // $02 D5-0 - modulator total level, used by normal FM path
    u8 dm, dc;                     // $03 D3,D4 - decoded rhythm-related bits; not fully modeled
    u8 fb;                         // $03 D2-0 - feedback amount used by normal FM path
    u8 arMod, drMod, slMod, rrMod; // $04/$06 - modulator envelope parameters (approximate)
    u8 arCar, drCar, slCar, rrCar; // $05/$07 - carrier envelope parameters (approximate)
} YM_Instrument;

//@----------------------------------------------------------------------------
//@ Runtime (audio-rate) state for one channel's oscillator.
//@----------------------------------------------------------------------------
typedef struct
{
    u32 phase;
    u32 phaseIncrement;
    u32 modPhase;
    u32 modPhaseIncrement;
    u32 releaseAccum;
    u32 envelopeLevel;       // legacy envelope fields; not used by current renderers
    u32 envelopeAccum;
    u8  envelopeState;
    u8  previousKeyOn;
    u8  sustainCounter;
    u8  sustainGain;
    u8  gain;

    /* Normal/DSi mixer: true two-operator FM runtime state. */
    u8  modEnv;
    u8  carEnv;
    u8  modState;
    u8  carState;
    s16 feedback;
} YM_Oscillator;

//@----------------------------------------------------------------------------
//@ Register-level (saved) state for one channel, plus its oscillator's
//@ runtime state. In rhythm mode, channels 6-8's fields below are
//@ reinterpreted per the register map notes above rather than unused.
//@----------------------------------------------------------------------------
typedef struct
{
    u8  instrument;         // $3x D7-4 - 0 = custom (YM.customInstrument), 1-15 = ROM preset
    u8  volume;             // $3x D3-0 - attenuation, 0 (loudest) - 15 (quietest)
    u16 fNumber;            // $1x + $2x D0 - 9-bit F-Number
    u8  block;              // $2x D3-1 - octave, 0-7
    u8  keyOn;              // $2x D4 (melodic) or the matching $0E bit (rhythm ch6-8)
    u8  sustain;            // $2x D5 - stored; not currently applied by the mixer

    const YM_Instrument *instPtr;  // cached &customInstrument or &InstrumentROM[instrument] -
                                   // re-pointed only on a $3x write, not re-derived every sample

    YM_Oscillator osc;
} YM_Channel;

//@----------------------------------------------------------------------------
//@ Whole-chip state. This is the struct pointer passed around exactly like
//@ SCC's SCCptr, and the whole thing is what YMSaveState/YMLoadState
//@ round-trip.
//@----------------------------------------------------------------------------
typedef struct
{
    YM_Channel channels[YM_NUM_CHANNELS];
    YM_Instrument customInstrument;  // regs $00-$07, used by any channel with instrument==0
    u8 rhythmReg;                       // $0E raw byte
    u8 rhythmVolBD;                     // $36 D3-0
    u8 rhythmVolHH, rhythmVolSD;        // $37 D7-4, D3-0
    u8 rhythmVolTOM, rhythmVolTCY;      // $38 D7-4, D3-0
    u8 testReg;                         // $0F, storage only
    u8 addressLatch;                    // last value written to the address-select port (caller's convenience)
    u32 noiseLFSR;                      // noise source for HH/SD/TOP-CY; must never be seeded 0
    s32 rhythmPrevNoise;                // legacy previous-noise state; not used by current mixer
    YM_Oscillator rhythmSD;             // rhythm mode only: channel 7's SECOND voice's envelope (HH uses
                                        // channels[7].osc's envelope; both derive their actual waveform
                                        // from channels 7 & 8's phase, not their own - see YMMixer)
    YM_Oscillator rhythmTCY;            // rhythm mode only: channel 8's SECOND voice's envelope (TOM uses
                                        // channels[8].osc directly, both for envelope and waveform)
    s32 outputFilterState;              // one-pole output-smoothing state; included in save states
} YM;

//@----------------------------------------------------------------------------
//@ Register bit masks/shifts.
//@----------------------------------------------------------------------------
#define YM_REG_AM_BIT        0x80
#define YM_REG_VIB_BIT       0x40
#define YM_REG_EGTYPE_BIT    0x20
#define YM_REG_KSR_BIT       0x10
#define YM_REG_MUL_MASK      0x0F

#define YM_REG_KSL_SHIFT     6       // $02/$03 D7-6
#define YM_REG_TL_MASK       0x3F    // $02 D5-0
#define YM_REG_DC_BIT        0x10    // $03 D4
#define YM_REG_DM_BIT        0x08    // $03 D3
#define YM_REG_FB_MASK       0x07    // $03 D2-0

#define YM_REG_AR_SHIFT      4       // $04/$05 D7-4
#define YM_REG_DR_MASK       0x0F    // $04/$05 D3-0
#define YM_REG_SL_SHIFT      4       // $06/$07 D7-4
#define YM_REG_RR_MASK       0x0F    // $06/$07 D3-0

#define YM_REG_SUS_BIT       0x20    // $20-$28 D5
#define YM_REG_KEY_BIT       0x10    // $20-$28 D4
#define YM_REG_BLOCK_SHIFT   1       // $20-$28 D3-1
#define YM_REG_BLOCK_MASK    0x07
#define YM_REG_FNUM_MSB_BIT  0x01    // $20-$28 D0

#define YM_REG_INST_SHIFT    4       // $30-$38 D7-4
#define YM_REG_VOL_MASK      0x0F    // $30-$38 D3-0

#define YM_RHYTHM_ENABLE_BIT 0x20    // $0E D5
#define YM_RHYTHM_BD_BIT     0x10    // $0E D4
#define YM_RHYTHM_SD_BIT     0x08    // $0E D3
#define YM_RHYTHM_TOM_BIT    0x04    // $0E D2
#define YM_RHYTHM_TCY_BIT    0x02    // $0E D1
#define YM_RHYTHM_HH_BIT     0x01    // $0E D0

#define YM_CHANNEL_BD        6       // zero-indexed - real-world "channel 7"
#define YM_CHANNEL_HHSD      7       // real-world "channel 8"
#define YM_CHANNEL_TOMTCY    8       // real-world "channel 9"

//@----------------------------------------------------------------------------
//@ Public interface - same shape as the SCC driver.
//@----------------------------------------------------------------------------
void YMReset(YM *chip);
void YMWrite(u8 value, u8 address, YM *chip);     // address = resolved register 0x00-0x38, not a Z80 address
void YMMixer(int len, s16 *dest, YM *chip);       // accumulates into dest - see msx_music.c
void YMMixerFast(int len, s16 *dest, YM *chip);   // accumulates into dest - see msx_music.c (DS-Lite version)

#endif // YM_H
