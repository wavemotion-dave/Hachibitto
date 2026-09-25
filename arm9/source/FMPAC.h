//
//  ACCURACY LEVEL - deliberately simplified OPLL emulation.
//
//  This is NOT a cycle-accurate or full YM2413/OPLL implementation.
//  The mixer is designed around the CPU budget of the target hardware,
//  trading synthesis accuracy for a substantial reduction in per-sample
//  computation.
//
//  A full 2-operator FM implementation with a more complete ADSR model was
//  measured to be far too expensive on this hardware.  In particular,
//  moving lookup tables to faster memory produced only a small improvement,
//  indicating that the dominant cost was the per-sample synthesis work.
//
//  Current melodic synthesis:
//    - Each channel uses a single oscillator rather than the OPLL's
//      modulator + carrier FM pair.  There is no carrier phase modulation,
//      operator feedback, or full FM timbre synthesis.
//    - The oscillator waveform is intentionally inexpensive.
//    - Instrument selection and the custom/ROM instrument registers are
//      still decoded and retained.  The carrier MUL value contributes to
//      the oscillator frequency.
//    - Carrier SL (sustain level) is approximated by making a held note's
//      gain settle toward an instrument-dependent sustain level.  SL=15 is
//      treated as full level; this is important for software that changes
//      channel volume while leaving a key held.
//    - Carrier RR (release rate) is approximated by making the key-off
//      release rate instrument-dependent.  The RR mapping is normalized to
//      this simplified gain model rather than attempting to reproduce the
//      OPLL envelope generator exactly.
//    - Attack/decay behavior remains deliberately simple.  This avoids the
//      cost and complexity of a full OPLL envelope generator while retaining
//      some of the per-instrument character that is audible in real music.
//
//  Current rhythm synthesis:
//    - Bass Drum and Tom-Tom use tonal oscillators.
//    - Hi-Hat, Snare Drum, and Top Cymbal use inexpensive noise-based
//      approximations.
//    - Rhythm voices use simplified gain/release behavior rather than the
//      complete OPLL rhythm envelope.
//
//  The result intentionally omits several pieces of the YM2413 synthesis
//  model, including full 2-operator FM, operator feedback, KSL, AM, VIB,
//  KSR, and the complete per-operator ADSR behavior.
//
//  These omissions are deliberate.  The goal is to obtain convincing
//  musical behavior at a frame rate suitable for the target hardware,
//  rather than to reproduce every detail of the original chip.
//
//  Performance history is intentionally kept out of this header; measured
//  FPS and experiment results change as the emulator evolves.  The source
//  implementation and comments should describe the current model, while
//  benchmark results belong in development notes.
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
//  Setup values ($16-$18/$26-$28) and rhythm volumes ($36-$38) are still
//  decoded the same as before - only the synthesis method changed.
//

#ifndef FMPAC_H
#define FMPAC_H

#include <nds.h>

#define FMPAC_NUM_CHANNELS      9

//@----------------------------------------------------------------------------
//@ Register-level instrument parameters. Fully decoded and stored (both the
//@ mutable custom instrument and the 15 ROM presets) even though the mixer
//@ currently only reads mulCar - kept complete so richer synthesis can be
//@ dialed back in later without redoing the register decode.
//@----------------------------------------------------------------------------
typedef struct
{
    u8 mulMod, mulCar;              // $00/$01 D3-0  - frequency multiplier, 0-15 (see MUL table). Only
                                    // mulCar is currently read by the mixer (free per-instrument pitch variety).
    u8 amMod,  amCar;               // $00/$01 D7    - unused by the mixer currently
    u8 vibMod, vibCar;              // $00/$01 D6    - unused by the mixer currently
    u8 egTypeMod, egTypeCar;        // $00/$01 D5    - unused (no ADSR right now)
    u8 ksrMod, ksrCar;              // $00/$01 D4    - unused by the mixer currently
    u8 kslMod, kslCar;              // $02/$03 D7-6  - unused by the mixer currently
    u8 tl;                          // $02     D5-0  - unused by the mixer currently (no FM = no modulator level)
    u8 dm, dc;                      // $03     D3,D4 - unused by the mixer currently
    u8 fb;                          // $03     D2-0  - unused (no feedback without a modulator)
    u8 arMod, drMod, slMod, rrMod;  // $04/$06 - unused (no ADSR right now)
    u8 arCar, drCar, slCar, rrCar;  // $05/$07 - unused (no ADSR right now)
} FMPAC_Instrument;

//@----------------------------------------------------------------------------
//@ Runtime (audio-rate) state for one channel's oscillator.
//@----------------------------------------------------------------------------
typedef struct
{
    u32 phase;
    u32 phaseIncrement;
    u32 releaseAccum;
    u32 envelopeLevel;
    u32 envelopeAccum;
    u8  envelopeState;
    u8  previousKeyOn;
    u8  sustainCounter;
    u8  sustainGain;
    u8  gain;
} FMPAC_Oscillator;

//@----------------------------------------------------------------------------
//@ Register-level (saved) state for one channel, plus its oscillator's
//@ runtime state. In rhythm mode, channels 6-8's fields below are
//@ reinterpreted per the register map notes above rather than unused.
//@----------------------------------------------------------------------------
typedef struct
{
    u8  instrument;         // $3x D7-4 - 0 = custom (FMPAC.customInstrument), 1-15 = ROM preset
    u8  volume;             // $3x D3-0 - attenuation, 0 (loudest) - 15 (quietest)
    u16 fNumber;            // $1x + $2x D0 - 9-bit F-Number
    u8  block;              // $2x D3-1 - octave, 0-7
    u8  keyOn;              // $2x D4 (melodic) or the matching $0E bit (rhythm ch6-8)
    u8  sustain;            // $2x D5 - stored, currently unused by the mixer

    const FMPAC_Instrument *instPtr;  // cached &customInstrument or &InstrumentROM[instrument] -
                                      // re-pointed only on a $3x write, not re-derived every sample

    FMPAC_Oscillator osc;
} FMPAC_Channel;

//@----------------------------------------------------------------------------
//@ Whole-chip state. This is the struct pointer passed around exactly like
//@ SCC's SCCptr, and the whole thing is what FMPACSaveState/FMPACLoadState
//@ round-trip.
//@----------------------------------------------------------------------------
typedef struct
{
    FMPAC_Channel channels[FMPAC_NUM_CHANNELS];
    FMPAC_Instrument customInstrument;  // regs $00-$07, used by any channel with instrument==0
    u8 rhythmReg;                       // $0E raw byte
    u8 rhythmVolBD;                     // $36 D3-0
    u8 rhythmVolHH, rhythmVolSD;        // $37 D7-4, D3-0
    u8 rhythmVolTOM, rhythmVolTCY;      // $38 D7-4, D3-0
    u8 testReg;                         // $0F, storage only
    u8 addressLatch;                    // last value written to the address-select port (caller's convenience)
    u32 noiseLFSR;                      // feeds HH/SD/TOP-CY's high-pass-filtered-noise texture -
                                        // must never be seeded 0
    s32 rhythmPrevNoise;                // previous raw noise sample - the one-sample delay used
                                        // for the high-pass filter (output = current - previous)
    FMPAC_Oscillator rhythmSD;          // rhythm mode only: channel 7's SECOND voice's envelope (HH uses
                                        // channels[7].osc's envelope; both derive their actual waveform
                                        // from channels 7 & 8's phase, not their own - see FMPACMixer)
    FMPAC_Oscillator rhythmTCY;         // rhythm mode only: channel 8's SECOND voice's envelope (TOM uses
                                        // channels[8].osc directly, both for envelope and waveform)
    s32 outputFilterState;              // one-sample treble-damping state; included in save states
} FMPAC;

//@----------------------------------------------------------------------------
//@ Register bit masks/shifts.
//@----------------------------------------------------------------------------
#define FMPAC_REG_AM_BIT        0x80
#define FMPAC_REG_VIB_BIT       0x40
#define FMPAC_REG_EGTYPE_BIT    0x20
#define FMPAC_REG_KSR_BIT       0x10
#define FMPAC_REG_MUL_MASK      0x0F

#define FMPAC_REG_KSL_SHIFT     6       // $02/$03 D7-6
#define FMPAC_REG_TL_MASK       0x3F    // $02 D5-0
#define FMPAC_REG_DC_BIT        0x10    // $03 D4
#define FMPAC_REG_DM_BIT        0x08    // $03 D3
#define FMPAC_REG_FB_MASK       0x07    // $03 D2-0

#define FMPAC_REG_AR_SHIFT      4       // $04/$05 D7-4
#define FMPAC_REG_DR_MASK       0x0F    // $04/$05 D3-0
#define FMPAC_REG_SL_SHIFT      4       // $06/$07 D7-4
#define FMPAC_REG_RR_MASK       0x0F    // $06/$07 D3-0

#define FMPAC_REG_SUS_BIT       0x20    // $20-$28 D5
#define FMPAC_REG_KEY_BIT       0x10    // $20-$28 D4
#define FMPAC_REG_BLOCK_SHIFT   1       // $20-$28 D3-1
#define FMPAC_REG_BLOCK_MASK    0x07
#define FMPAC_REG_FNUM_MSB_BIT  0x01    // $20-$28 D0

#define FMPAC_REG_INST_SHIFT    4       // $30-$38 D7-4
#define FMPAC_REG_VOL_MASK      0x0F    // $30-$38 D3-0

#define FMPAC_RHYTHM_ENABLE_BIT 0x20    // $0E D5
#define FMPAC_RHYTHM_BD_BIT     0x10    // $0E D4
#define FMPAC_RHYTHM_SD_BIT     0x08    // $0E D3
#define FMPAC_RHYTHM_TOM_BIT    0x04    // $0E D2
#define FMPAC_RHYTHM_TCY_BIT    0x02    // $0E D1
#define FMPAC_RHYTHM_HH_BIT     0x01    // $0E D0

#define FMPAC_CHANNEL_BD        6       // zero-indexed - real-world "channel 7"
#define FMPAC_CHANNEL_HHSD      7       // real-world "channel 8"
#define FMPAC_CHANNEL_TOMTCY    8       // real-world "channel 9"

//@----------------------------------------------------------------------------
//@ Instrument table - real Yamaha ROM data (decoded from a verified
//@ reference core), though the mixer currently only reads mulCar/mulMod
//@ from each entry. Defined in FMPAC.c.
//@----------------------------------------------------------------------------
extern const FMPAC_Instrument FMPAC_InstrumentROM[16];

//@----------------------------------------------------------------------------
//@ Public interface - same shape as the SCC driver.
//@----------------------------------------------------------------------------
void FMPACReset(FMPAC *chip);
void FMPACWrite(u8 value, u8 address, FMPAC *chip);   // address = resolved register 0x00-0x38, not a Z80 address
void FMPACMixer(int len, s16 *dest, FMPAC *chip);    // accumulates into dest - see FMPAC.c

#endif // FMPAC_H
