//
//  FMPAC.h
//  Konami FM-PAC (Yamaha YM2413 / OPLL) sound chip emulator.
//
//  Interface shape deliberately mirrors SCC.s/SCC.i: FMPACReset,
//  FMPACWrite, FMPACRead, FMPACMixer, FMPACGetStateSize, FMPACSaveState,
//  FMPACLoadState. First pass is plain C; the mixer is the candidate for
//  a hand-written ARM asm port later, same as SCC's history.
//
//  ACCURACY LEVEL - read this before assuming behavior matches real HW:
//    This is the "simplified/approximate" synthesis option, not a
//    cycle/bit-accurate OPLL core. Specifically:
//      - Real OPLL does FM synthesis in the log domain (phase -> logsin
//        table -> add attenuation -> exp table -> linear sample). This
//        driver instead uses a single plain linear sine table and scales
//        the result by envelope/volume directly. Sounds "FM-shaped" but
//        will not timbre-match real hardware exactly.
//      - The envelope generator is a simplified linear-ish ADSR (four
//        stages, level 0-255, fixed step tables indexed by rate), not
//        the chip's real logarithmic-rate envelope with key-scaling.
//      - The 15 preset instruments below are hand-approximated (roughly
//        categorized by brightness/feedback/envelope shape to be
//        recognizable as "that kind of sound"), NOT the real YM2413
//        factory ROM values. Swap FMPAC_InstrumentROM[] for the real
//        table later if exact-timbre compatibility ever matters.
//      - Vibrato/AM (registers' VIB/AM bits) are decoded and stored but
//        not yet applied to the signal - flagged TODO at the field.
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
//  Rhythm mode (D5 of $0E set) repurposes channels 6/7/8 (zero-indexed):
//    ch6 = Bass Drum (both operators, melodic-style, own key-on = D4/$0E)
//    ch7 = Hi-Hat (modulator, key-on = D0/$0E) + Snare Drum (carrier, D3/$0E)
//    ch8 = Tom-Tom (modulator, key-on = D2/$0E) + Top Cymbal (carrier, D1/$0E)
//  In rhythm mode $26/$27/$28 key-on bits (D4) must stay 0 - the chip
//  uses $0E's dedicated bits instead - and Yamaha's recommended fixed
//  setup values are $16=0x20 $17=0x50 $18=0xC0 $26=0x05 $27=0x05 $28=0x01.
//  Rhythm volumes: $36=BD(D3-0) $37=HH(D7-4)+SD(D3-0) $38=TOM(D7-4)+TOP-CY(D3-0).
//

#ifndef FMPAC_H
#define FMPAC_H

#include <nds.h>

#define FMPAC_NUM_CHANNELS   9
#define FMPAC_NUM_OPERATORS  2		// 0 = modulator, 1 = carrier

//@----------------------------------------------------------------------------
//@ One operator's register-level parameters (this exact 8-byte layout is
//@ shared between the mutable "custom" instrument (regs $00-$07) and each
//@ of the 15 preset ROM-equivalent entries below, so both can be decoded
//@ by the same code path).
//@----------------------------------------------------------------------------
typedef struct
{
	u8 mulMod, mulCar;		// $00/$01 D3-0  - frequency multiplier, 0-15 (see MUL table)
	u8 amMod,  amCar;		// $00/$01 D7    - amplitude modulation enable (TODO: not yet applied)
	u8 vibMod, vibCar;		// $00/$01 D6    - vibrato enable (TODO: not yet applied)
	u8 egTypeMod, egTypeCar;	// $00/$01 D5    - 0=percussive (decay past sustain), 1=sustained (hold)
	u8 ksrMod, ksrCar;		// $00/$01 D4    - key scale rate (TODO: not yet applied to envelope speed)
	u8 kslMod, kslCar;		// $02/$03 D7-6  - key scale level (TODO: not yet applied as attenuation)
	u8 tl;				// $02     D5-0  - modulator total level, 0-63 (carrier's level = channel VOL)
	u8 dm, dc;			// $03     D3,D4 - half-wave rectify modulator/carrier (TODO: not yet applied)
	u8 fb;				// $03     D2-0  - modulator self-feedback amount, 0-7
	u8 arMod, drMod, slMod, rrMod;	// $04/$06 - modulator attack/decay/sustain-level/release rates
	u8 arCar, drCar, slCar, rrCar;	// $05/$07 - carrier attack/decay/sustain-level/release rates
} FMPAC_Instrument;

//@----------------------------------------------------------------------------
//@ Runtime (audio-rate) state for one operator - not saved register content,
//@ this is where the operator "currently is" in its phase/envelope cycle.
//@----------------------------------------------------------------------------
typedef enum
{
	FMPAC_ENV_IDLE = 0,
	FMPAC_ENV_ATTACK,
	FMPAC_ENV_DECAY,
	FMPAC_ENV_SUSTAIN,
	FMPAC_ENV_RELEASE
} FMPAC_EnvelopeStage;

typedef struct
{
	u32 phase;			// fixed-point phase accumulator (see FMPAC.c for the fixed-point format)
	u32 phaseIncrement;		// cached per-sample phase step - recomputed only when freq/block/mul/instrument change, not every sample
	u32 envAccum;			// 16.16 fixed-point fractional accumulator - envLevel only moves in whole
					// units, but real envelope times need sub-1-unit-per-sample steps at
					// audio rate, hence this rather than stepping envLevel directly
	u8  envLevel;			// 0 (silent) - 255 (full) current envelope amplitude
	u8  envStage;			// FMPAC_EnvelopeStage
	s16 feedbackHist[2];		// modulator's last two output samples, for self-feedback (FB); unused on carrier
} FMPAC_Operator;

//@----------------------------------------------------------------------------
//@ Register-level (saved) state for one channel, plus its two operators'
//@ runtime state. In rhythm mode, channels 6-8's fields below are
//@ reinterpreted per the register map notes above rather than unused.
//@----------------------------------------------------------------------------
typedef struct
{
	u8  instrument;			// $3x D7-4 - 0 = custom (FMPAC.customInstrument), 1-15 = ROM preset
	u8  volume;			// $3x D3-0 - carrier attenuation, 0 (loudest) - 15 (quietest)
	u16 fNumber;			// $1x + $2x D0 - 9-bit F-Number
	u8  block;			// $2x D3-1 - octave, 0-7
	u8  keyOn;			// $2x D4 (melodic) or the matching $0E bit (rhythm ch6-8)
	u8  sustain;			// $2x D5 - extends release rate when key is off

	const FMPAC_Instrument *instPtr;	// cached &customInstrument or &InstrumentROM[instrument] -
						// re-pointed only on a $3x write, not re-derived every sample

	FMPAC_Operator mod;
	FMPAC_Operator car;
} FMPAC_Channel;

//@----------------------------------------------------------------------------
//@ Whole-chip state. This is the struct pointer passed around exactly like
//@ SCC's SCCptr, and the whole thing (or a clearly-marked sub-range of it,
//@ if we later split "live" vs "saved" the way SCC.i does) is what
//@ FMPACSaveState/FMPACLoadState round-trip.
//@----------------------------------------------------------------------------
typedef struct
{
	FMPAC_Channel channels[FMPAC_NUM_CHANNELS];
	FMPAC_Instrument customInstrument;	// regs $00-$07, used by any channel with instrument==0
	u8 rhythmReg;				// $0E raw byte, decoded on demand (see FMPAC_RHYTHM_* masks)
	u8 rhythmVolBD;				// $36 D3-0
	u8 rhythmVolHH, rhythmVolSD;		// $37 D7-4, D3-0
	u8 rhythmVolTOM, rhythmVolTCY;		// $38 D7-4, D3-0
	u8 testReg;				// $0F, storage only - real HW says this should stay 0
	u8 addressLatch;			// last value written to the address-select port (see FMPAC.c)
	u32 noiseLFSR;				// shared noise generator feeding HH/SD/TOP-CY - must never be seeded 0
} FMPAC;

//@----------------------------------------------------------------------------
//@ Register bit masks/shifts (for the few registers whose fields don't
//@ split cleanly along nibble/byte lines).
//@----------------------------------------------------------------------------
#define FMPAC_REG_AM_BIT		0x80
#define FMPAC_REG_VIB_BIT		0x40
#define FMPAC_REG_EGTYPE_BIT		0x20
#define FMPAC_REG_KSR_BIT		0x10
#define FMPAC_REG_MUL_MASK		0x0F

#define FMPAC_REG_KSL_SHIFT		6	// $02/$03 D7-6
#define FMPAC_REG_TL_MASK		0x3F	// $02 D5-0
#define FMPAC_REG_DC_BIT		0x10	// $03 D4
#define FMPAC_REG_DM_BIT		0x08	// $03 D3
#define FMPAC_REG_FB_MASK		0x07	// $03 D2-0

#define FMPAC_REG_AR_SHIFT		4	// $04/$05 D7-4
#define FMPAC_REG_DR_MASK		0x0F	// $04/$05 D3-0
#define FMPAC_REG_SL_SHIFT		4	// $06/$07 D7-4
#define FMPAC_REG_RR_MASK		0x0F	// $06/$07 D3-0

#define FMPAC_REG_SUS_BIT		0x20	// $20-$28 D5
#define FMPAC_REG_KEY_BIT		0x10	// $20-$28 D4
#define FMPAC_REG_BLOCK_SHIFT		1	// $20-$28 D3-1
#define FMPAC_REG_BLOCK_MASK		0x07
#define FMPAC_REG_FNUM_MSB_BIT		0x01	// $20-$28 D0

#define FMPAC_REG_INST_SHIFT		4	// $30-$38 D7-4
#define FMPAC_REG_VOL_MASK		0x0F	// $30-$38 D3-0

#define FMPAC_RHYTHM_ENABLE_BIT		0x20	// $0E D5
#define FMPAC_RHYTHM_BD_BIT		0x10	// $0E D4
#define FMPAC_RHYTHM_SD_BIT		0x08	// $0E D3
#define FMPAC_RHYTHM_TOM_BIT		0x04	// $0E D2
#define FMPAC_RHYTHM_TCY_BIT		0x02	// $0E D1
#define FMPAC_RHYTHM_HH_BIT		0x01	// $0E D0

#define FMPAC_CHANNEL_BD		6	// zero-indexed - real-world "channel 7"
#define FMPAC_CHANNEL_HHSD		7	// real-world "channel 8"
#define FMPAC_CHANNEL_TOMTCY		8	// real-world "channel 9"

//@----------------------------------------------------------------------------
//@ Approximate instrument table. Index 0 is never read from here - a
//@ channel with instrument==0 always uses FMPAC.customInstrument instead.
//@ Indices 1-15 are hand-picked stand-ins for Violin/Guitar/Piano/Flute/
//@ Clarinet/Oboe/Trumpet/Organ/Horn/Synthesizer/Harpsichord/Vibraphone/
//@ SynthBass/AcousticBass/ElectricGuitar - NOT the real ROM data (see the
//@ file-level comment). Defined in FMPAC.c.
//@----------------------------------------------------------------------------
extern const FMPAC_Instrument FMPAC_InstrumentROM[16];

//@----------------------------------------------------------------------------
//@ Public interface - same shape as the SCC driver.
//@----------------------------------------------------------------------------
void FMPACReset(FMPAC *chip);
void FMPACWrite(u8 value, u8 address, FMPAC *chip);	// address = resolved register 0x00-0x38, not a Z80 address
u8   FMPACRead(u8 address, FMPAC *chip);
void FMPACMixer(int len, s16 *dest, FMPAC *chip);	// dest = mono sample buffer, chip mixed in additively? see FMPAC.c

u32  FMPACGetStateSize(void);
void FMPACSaveState(u8 *dest, FMPAC *chip);
void FMPACLoadState(FMPAC *chip, const u8 *src);

#endif // FMPAC_H
