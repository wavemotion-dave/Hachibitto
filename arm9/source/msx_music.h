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
//  Melodic channels use a simplified two-operator FM path with separate
//  modulator and carrier envelope states.
//
//  Current melodic synthesis:
//    - Two operators are synthesized per channel: the modulator (a sine, or a
//      half-sine for custom patches with DM set) changes the carrier's phase.
//      Modulation depth is computed at full precision from the modulator
//      envelope and TL (0.75 dB per step, YM_TLAtten) and scaled by
//      YM_MOD_INDEX_SHIFT. The real chip swings the carrier by about +/-4pi
//      (shift 14); the value in use is deliberately much lower and was chosen
//      by ear (see the aliasing note below).
//    - Feedback (FB) modulates the modulator's own PHASE, using a smoothed
//      copy of its output as a stand-in for the chip's two-sample average
//      (YM_FM_FBDepth, scaled by YM_FB_SHIFT).
//    - MUL, TL, FB and AR/DR/SL/RR contribute to the model; block/KSR affect
//      the approximate envelope-rate calculation. KSL and EG-TYP are decoded
//      but not modeled (every note holds at its sustain level while the key
//      is down). Custom patches honor DC/DM waveform selection using full- or
//      half-sine operator waveforms; preset waveform behavior is unchanged.
//    - Vibrato: operators whose VIB bit is set get a ~6.4 Hz, 8-step LFO
//      applied to their phase increment (YM_VIB_SHIFT; about +/-7 cents).
//    - Tremolo (AM): operators whose AM bit is set (the Vibraphone preset and
//      some custom patches) get a ~3.8 Hz triangle LFO that scales their level
//      by up to YM_AM_DEPTH/256. The default is about 2.5 dB, half the real
//      chip's ~4.8 dB (108), because an earlier all-instrument LFO attempt
//      gave far too much shimmer.
//    - Envelopes are 32-bit fixed point (16.16) and advance once every
//      (1 << YM_ENV_DIV_SHIFT) output samples to save CPU. Decay and release
//      speeds follow the hardware's exponential rate scaling (speed doubles
//      with each rate step, rate 0 never decays, release always terminates).
//      The ramp is LINEAR in amplitude rather than the OPLL's logarithmic (dB)
//      curve, so YM_RATE_BASE is tuned by ear to compensate. Attack is a
//      simpler approximation and is not hardware-accurate.
//    - Carrier envelope and channel volume are applied in a single multiply
//      and one shift, with the YM output level (ymScale) folded into that
//      shift. This avoids intermediate truncation, which turned fading notes
//      into coarse, grating stair-steps.
//    - Melodic FM is rendered at half the output sample rate, with oscillator
//      phases advancing by two sample steps. A simple linear midpoint between
//      adjacent rendered samples softens the skipped sample. This intentionally
//      trades some temporal detail and sound quality for lower mixer CPU use.
//      Rhythm and output filtering continue at the full output rate.
//    - ALIASING: because of the half-rate render (Nyquist ~7.7 kHz), strong FM
//      sidebands fold back as rough, gritty noise, worst on high notes. Four
//      controls are in place, all precomputed when a channel changes:
//        1. Global modulator and feedback depth are kept well below the real
//           chip (YM_MOD_INDEX_SHIFT, YM_FB_SHIFT).
//        2. Both shift by one per block around YM_FM_FULL_BLOCK: high notes get
//           less FM, low notes get more, so bass keeps its overtones (a bare
//           low sine is nearly inaudible on the DS speaker). Modulator depth
//           is floored at shift 16 (the real chip is 14, but that made bass
//           rumbly) and feedback at 7 (real chip).
//        3. YM_PresetFMExtra trims modulator depth per ROM preset. Harpsichord
//           (preset 11, TL=7, the strongest modulator and one that never
//           decays) needed it: an opening jingle plays it on channels 4 and 5
//           and sounded gritty without the trim.
//        4. Entry 0 of the same table trims custom patches (set to 3; each step
//           halves the depth, 8 turns custom FM off). A game's ending jingle
//           stacks one custom patch (FB 7, half-wave modulator) as a tight
//           three-note cluster on channels 0-2, and full FM made it harsh.
//           3 and 4 sounded clean; 2 showed a hint of harshness.
//    - Sustain levels (SL) come from YM_FM_SustainAtten; channel volume uses
//      YM_SustainGain (3 dB per step). Preset carriers use a soft-square
//      lookup; custom carriers use the selected sine waveform.
//
//  Current rhythm synthesis (shared in concept, with separate mixer code):
//    - Bass Drum and Tom-Tom use tonal oscillators with the same soft-square
//      lookup as preset carriers. A plain sine at drum frequencies is nearly
//      inaudible on the DS speaker; the extra harmonics give the kick punch.
//    - Hi-Hat, Snare Drum and Top Cymbal use inexpensive phase/noise-based
//      approximations and simplified gain/release behavior.
//
//  Implementation notes (CPU and memory):
//    - YMMixer and YM_RenderChannel2FM live in ITCM. YM_EnvStep is one shared
//      noinline ITCM function (inlining it twice overflowed ITCM).
//    - Redundant $30-$38 writes that don't change the instrument only refresh
//      the channel volume instead of recomputing frequency (games rewrite
//      these registers every frame).
//    - Per-sample data (the two sine tables, YM_FM[] and a few state bytes)
//      lives in DTCM, which is nearly full. Anything only read on a register
//      write should stay out of it.
//
//  The implementation omits cycle-accurate timing and several parts of the
//  YM2413 model. Comments describe the current approximation, not a claim of
//  equivalence with a hardware OPLL. Performance measurements belong in notes.
//
//  Register map (verified against the Yamaha OPLL Application Manual):
//  (unchanged from here on)

#ifndef YM_H
#define YM_H

#include <nds.h>

#define YM_NUM_CHANNELS      9

//@----------------------------------------------------------------------------
//@ Register-level instrument parameters. Both custom-instrument registers and
//@ ROM presets are decoded and stored. The FM mixer uses a subset of
//@ these fields; unsupported parameters remain available but are not all modeled.
//@----------------------------------------------------------------------------
typedef struct
{
    u8 mulMod, mulCar;             // $00/$01 D3-0 - operator multipliers (FM path)
    u8 amMod,  amCar;              // $00/$01 D7 - decoded; AM not currently modeled
    u8 vibMod, vibCar;             // $00/$01 D6 - enables the vibrato LFO on that operator
    u8 egTypeMod, egTypeCar;       // $00/$01 D5 - decoded; EG-TYP behavior not modeled
    u8 ksrMod, ksrCar;             // $00/$01 D4 - used by approximate FM rate calculation
    u8 kslMod, kslCar;             // $02/$03 D7-6 - decoded; KSL not currently applied
    u8 tl;                         // $02 D5-0 - modulator total level, used by FM path
    u8 dm, dc;                     // $03 D3,D4 - modulator/carrier waveform select
    u8 fb;                         // $03 D2-0 - feedback amount used by FM path
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
    u32 envelopeLevel;       // Reserved; not used by current synthesis.
    u32 envelopeAccum;
    u8  envelopeState;
    u8  previousKeyOn;
    u8  sustainCounter;
    u8  sustainGain;
    u8  gain;

    /* Melodic two-operator FM runtime state. */
    u32 modEnv, carEnv;     // 0 = full level, YM_ENV_SILENT = silent (was u8)
    u8  modState, carState;
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
    s32 rhythmPrevNoise;                // Reserved; currently unused.
    YM_Oscillator rhythmSD;             // rhythm mode only: channel 7's SECOND voice's envelope (HH uses
                                        // channels[7].osc's envelope; both derive their actual waveform
                                        // from channels 7 & 8's phase, not their own - see YMMixer)
    YM_Oscillator rhythmTCY;            // rhythm mode only: channel 8's SECOND voice's envelope (TOM uses
                                        // channels[8].osc directly, both for envelope and waveform)
    s32 outputFilterState;              // one-pole output-smoothing state; included in save states
} YM;


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

extern YM_FMParams YM_FM[YM_NUM_CHANNELS];
extern u8  YM_EnvDiv;
extern u8  YM_HalfRatePhase;
extern s32 YM_HeldMelodicSample;
extern u8  YM_LiteEnvDiv;
extern u8  YM_LitePhase;
extern u8  YM_LiteRhythmTick;
extern s32 YM_LiteLast;
extern s32 YM_LiteCur;
extern s32 YM_LiteStep;
extern s32 YM_LiteRhythmHeld;
extern s8  YM_VibNow;
extern u8  YM_VibPos;
extern u16 YM_VibCnt;
extern s8  YM_VibTable[8];


//@----------------------------------------------------------------------------
//@ Public interface - same shape as the SCC driver.
//@----------------------------------------------------------------------------
void YMReset(YM *chip);
void YMWrite(u8 value, u8 address, YM *chip);     // address = resolved register 0x00-0x38, not a Z80 address
void YMMixer(int len, s16 *dest, YM *chip);       // accumulates into dest - see msx_music.c (DSi version)
void YMMixer_Lite(int len, s16 *dest, YM *chip);  // accumulates into dest - see msx_music.c (DS-Lite version)

#endif // YM_H
