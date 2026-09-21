//
//  FMPAC.c
//  Konami FM-PAC (Yamaha YM2413 / OPLL) sound chip emulator - music only.
//  See FMPAC.h for the accuracy-level disclaimer: this is a drastically
//  simplified, no-FM, no-ADSR design chosen purely for speed.
//

#include "FMPAC.h"
#include <string.h>	// memcpy, memset

//@----------------------------------------------------------------------------
//@ Tunables.
//@----------------------------------------------------------------------------
#define FMPAC_SAMPLE_RATE		27965		// confirmed value, matches the AY driver's rate
#define FMPAC_MASTER_CLOCK		3579545		// MSX standard clock, same as SCC's
#define FMPAC_SIN_SHIFT			24		// phase>>24 -> 8-bit (256 entry) table index
#define FMPAC_GAIN_RAMP_STEP		16		// ATTACK rate: gain moves this much per sample toward
							// full on key-on - ~16 samples (~0.6ms), fast/click-free
#define FMPAC_RELEASE_STEP		3984		// MELODIC release rate: 16.16 fixed-point step targeting a
							// ~150ms fade to silence on key-off, not an instant cutoff.
							// This is the fix for FM music sounding "thin"/"cut" - real FM
							// pieces lean on overlapping decay tails for their fullness
							// (unlike AY music, which doesn't use per-note envelopes at
							// all), and cutting every note off in <1ms removed exactly
							// that. Retune this constant if it still isn't right - up
							// for a lusher/longer tail, down if notes start blurring
							// together too much.
#define FMPAC_PERCUSSION_RELEASE_STEP	19920		// PERCUSSION release rate: ~30ms, NOT the melodic 150ms.
							// Real drums (hi-hat especially) decay in tens of ms, not
							// hundreds - using the melodic rate here made consecutive
							// hits (fired every 100-150ms in a normal rhythm pattern)
							// overlap and blend continuously instead of sounding like
							// distinct hits. Applies to BD/TOM and HH/SD/TOP-CY alike.
#define FMPAC_OUT_SHIFT			8		// output headroom for everything - melodic channels AND
							// percussion now both go through FMPAC_SinTable via real
							// phase-selection logic (see FMPACMixer), not a separate
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
static const s8 FMPAC_SinTable[256] =
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
//@ Plain sine table, used ONLY by the HH/SD/TOP-CY phase-selection algorithm
//@ in FMPACMixer (see there). That algorithm picks between a small set of
//@ FIXED phase positions expecting them to land at genuinely different
//@ amplitudes (roughly +-40 and +-122 on a real sine) - real hardware's
//@ own hi-hat "shimmer" depends on that variety. FMPAC_SinTable above is a
//@ square-wave-ish shape (added for melodic brightness) that saturates
//@ near its extremes much faster than a sine does, so those same four
//@ positions land at ~+-114/123 on it - basically two values differing
//@ only in sign. That flattened the entire selection algorithm into a
//@ crude two-level flip-flop, which is what was producing a "wall of
//@ noise" sound instead of a real hi-hat tick. Keeping this separate
//@ rather than changing FMPAC_SinTable itself, since that table's
//@ brightness was a deliberate, separately-validated fix for melodic
//@ channels sounding muffled.
//@----------------------------------------------------------------------------
static const s8 FMPAC_RhythmSinTable[256] =
{
	   0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,   37,   40,   43,   46,
	  49,   51,   54,   57,   60,   63,   65,   68,   71,   73,   76,   78,   81,   83,   85,   88,
	  90,   92,   94,   96,   98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
	 117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,  126,  127,  127,  127,
	 127,  127,  127,  127,  126,  126,  126,  125,  125,  124,  123,  122,  122,  121,  120,  118,
	 117,  116,  115,  113,  112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
	  90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,   60,   57,   54,   51,
	  49,   46,   43,   40,   37,   34,   31,   28,   25,   22,   19,   16,   12,    9,    6,    3,
	   0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
	 -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
	 -90,  -92,  -94,  -96,  -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
	-117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
	-127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
	-117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100,  -98,  -96,  -94,  -92,
	 -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
	 -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3,
};

//@----------------------------------------------------------------------------
//@ Real Yamaha MUL table, doubled (so index 0's real x0.5 is a whole number).
//@----------------------------------------------------------------------------
static const u8 FMPAC_MulTableX2[16] =
{
	1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
};

//@----------------------------------------------------------------------------
//@ 16-entry instrument table - real Yamaha ROM data. Only mulMod/mulCar are
//@ read by the mixer right now; everything else is stored for later use.
//@----------------------------------------------------------------------------
const FMPAC_Instrument FMPAC_InstrumentROM[16] =
{
	/*  0 unused/custom */ { 9,12, 0,0, 1,1, 0,0, 0,0, 1,0, 12, 0,1, 2, 0,0,0,0, 0,0,0,0 },
	/*  1 Violin        */ { 1,1, 0,0, 1,1, 1,1, 0,0, 0,0, 30, 0,1, 7, 15,0,0,0, 7,8,1,7 },
	/*  2 Guitar        */ { 3,1, 0,0, 0,1, 0,0, 1,0, 0,0, 30, 1,0, 5, 13,7,1,3, 15,7,1,3 },
	/*  3 Piano         */ { 3,1, 0,0, 0,0, 0,0, 1,0, 2,0, 25, 0,0, 4, 15,2,1,1, 15,4,2,3 },
	/*  4 Flute         */ { 1,1, 0,0, 0,1, 1,1, 0,0, 0,0, 27, 0,0, 7, 10,15,4,0, 6,4,2,7 },
	/*  5 Clarinet      */ { 2,1, 0,0, 0,0, 1,1, 0,0, 0,0, 30, 0,0, 6, 15,0,0,8, 7,5,1,8 },
	/*  6 Oboe          */ { 1,2, 0,0, 0,0, 1,1, 1,0, 0,0, 22, 0,0, 5, 9,0,0,0, 7,1,1,3 },
	/*  7 Trumpet       */ { 1,1, 0,0, 0,1, 1,1, 0,0, 0,0, 29, 0,0, 7, 8,2,1,0, 8,0,1,7 },
	/*  8 Organ         */ { 3,1, 0,0, 0,0, 1,1, 0,0, 0,0, 45, 0,1, 6, 12,0,0,7, 7,0,0,7 },
	/*  9 Horn          */ { 1,1, 0,0, 1,1, 1,1, 0,0, 0,0, 27, 0,0, 6, 6,4,1,0, 6,5,1,7 },
	/* 10 Synthesizer   */ { 1,1, 0,0, 1,1, 1,1, 0,0, 0,0, 12, 1,1, 0, 8,5,7,0, 15,0,0,7 },
	/* 11 Harpsichord   */ { 3,1, 0,0, 0,0, 1,0, 0,0, 0,0, 7, 0,1, 1, 15,0,0,0, 10,4,2,2 },
	/* 12 Vibraphone    */ { 7,1, 1,1, 0,1, 0,0, 1,0, 0,0, 36, 0,0, 7, 15,15,2,2, 15,8,1,2 },
	/* 13 Synth Bass    */ { 1,0, 0,0, 1,0, 1,0, 0,1, 0,0, 12, 0,0, 5, 15,2,4,0, 15,4,4,4 },
	/* 14 Acoustic Bass */ { 1,1, 0,0, 0,0, 0,0, 0,0, 1,0, 21, 0,0, 3, 15,3,15,3, 9,2,15,3 },
	/* 15 Elec Guitar   */ { 1,1, 0,0, 1,1, 1,0, 0,0, 2,0, 9, 0,0, 3, 15,1,15,0, 15,4,1,3 },
};

//@----------------------------------------------------------------------------
//@ Small helpers
//@----------------------------------------------------------------------------
static const FMPAC_Instrument *FMPAC_GetInstrument(const FMPAC *chip, u8 instrument)
{
	if (instrument == 0)
		return &chip->customInstrument;
	return &FMPAC_InstrumentROM[instrument & 0x0F];
}

static u32 FMPAC_ComputePhaseIncrement(u16 fNumber, u8 block, u8 mulNibble)
{
	// Integer-only: inc = F * 2^block * mulX2 * (masterClock << 12) / (72 * sampleRate)
	// See chat history for the derivation - matches the Yamaha application manual's
	// fmus formula, just rearranged to avoid floating point entirely. Both constants
	// are plain integers now (no lingering compile-time-folded double literals).
	unsigned long long num = (unsigned long long)fNumber * (unsigned long long)FMPAC_MulTableX2[mulNibble & 0x0F];
	num <<= block;
	num *= (unsigned long long)FMPAC_MASTER_CLOCK << 12;
	unsigned long long inc = num / ((unsigned long long)72 * (unsigned long long)FMPAC_SAMPLE_RATE);
	if (inc > 0xFFFFFFFFULL) inc = 0xFFFFFFFFULL;
	return (u32)inc;
}

static void FMPAC_UpdateChannelFreq(FMPAC *chip, int ch)
{
	FMPAC_Channel *c = &chip->channels[ch];
	const FMPAC_Instrument *inst = c->instPtr;
	c->osc.phaseIncrement = FMPAC_ComputePhaseIncrement(c->fNumber, c->block, inst->mulCar);
}

static void FMPAC_UpdateCustomUsers(FMPAC *chip)
{
	int ch;
	for (ch = 0; ch < FMPAC_NUM_CHANNELS; ch++)
		if (chip->channels[ch].instrument == 0)
			FMPAC_UpdateChannelFreq(chip, ch);
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
static void FMPAC_UpdateGain(FMPAC_Oscillator *osc, u8 keyOn, u32 releaseStep)
{
	if (keyOn)
	{
		osc->releaseAccum = 0;
		if (osc->gain < 255)
		{
			u16 g = osc->gain + FMPAC_GAIN_RAMP_STEP;
			osc->gain = (g >= 255) ? 255 : (u8)g;
		}
	}
	else if (osc->gain > 0)
	{
		osc->releaseAccum += releaseStep;
		while (osc->releaseAccum >= 0x10000)
		{
			osc->releaseAccum -= 0x10000;
			osc->gain--;
			if (osc->gain == 0) break;
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
//@ isMelodic selects which release rate applies - see FMPAC_RELEASE_STEP
//@ vs FMPAC_PERCUSSION_RELEASE_STEP.
//@----------------------------------------------------------------------------
static s32 FMPAC_RenderChannel(FMPAC_Oscillator *osc, u8 keyOn, u8 volume, int isMelodic)
{
	FMPAC_UpdateGain(osc, keyOn, isMelodic ? FMPAC_RELEASE_STEP : FMPAC_PERCUSSION_RELEASE_STEP);
	if (osc->gain == 0) return 0;	// still idle/silent - skip the phase/table work

	osc->phase += osc->phaseIncrement;
	s32 s = FMPAC_SinTable[(osc->phase >> FMPAC_SIN_SHIFT) & 0xFF];
	return (s * (15 - volume) * osc->gain) >> FMPAC_OUT_SHIFT;
}

//@----------------------------------------------------------------------------
//@ Advances the shared noise LFSR by one bit per sample. Used only as a
//@ single tie-breaker bit by the real HH/SD phase-selection algorithm in
//@ FMPACMixer (see there) - real hardware does exactly this, reading
//@ (noise_rng>>0)&1 fresh each sample, not a "held"/bandwidth-reduced
//@ value. No separate noise waveform synthesis needed anymore.
//@----------------------------------------------------------------------------
static void FMPAC_AdvanceNoise(FMPAC *chip)
{
	u32 lfsr = chip->noiseLFSR;
	u32 bit = ((lfsr >> 0) ^ (lfsr >> 3)) & 1;
	lfsr = (lfsr >> 1) | (bit << 16);
	chip->noiseLFSR = lfsr;
}

//@----------------------------------------------------------------------------
//@ Public interface
//@----------------------------------------------------------------------------
void FMPACReset(FMPAC *chip)
{
	memset(chip, 0, sizeof(FMPAC));
	chip->noiseLFSR = 1;	// must not be seeded with 0, or the LFSR locks up
	int ch;
	for (ch = 0; ch < FMPAC_NUM_CHANNELS; ch++)
	{
		chip->channels[ch].instPtr = &chip->customInstrument;
		FMPAC_UpdateChannelFreq(chip, ch);
	}
}

void FMPACWrite(u8 value, u8 address, FMPAC *chip)
{
	if (address <= 0x07)
	{
		FMPAC_Instrument *ci = &chip->customInstrument;
		switch (address)
		{
			case 0x00:
				ci->amMod = (value & FMPAC_REG_AM_BIT) ? 1 : 0;
				ci->vibMod = (value & FMPAC_REG_VIB_BIT) ? 1 : 0;
				ci->egTypeMod = (value & FMPAC_REG_EGTYPE_BIT) ? 1 : 0;
				ci->ksrMod = (value & FMPAC_REG_KSR_BIT) ? 1 : 0;
				ci->mulMod = value & FMPAC_REG_MUL_MASK;
				break;
			case 0x01:
				ci->amCar = (value & FMPAC_REG_AM_BIT) ? 1 : 0;
				ci->vibCar = (value & FMPAC_REG_VIB_BIT) ? 1 : 0;
				ci->egTypeCar = (value & FMPAC_REG_EGTYPE_BIT) ? 1 : 0;
				ci->ksrCar = (value & FMPAC_REG_KSR_BIT) ? 1 : 0;
				ci->mulCar = value & FMPAC_REG_MUL_MASK;
				break;
			case 0x02:
				ci->kslMod = value >> FMPAC_REG_KSL_SHIFT;
				ci->tl = value & FMPAC_REG_TL_MASK;
				break;
			case 0x03:
				ci->kslCar = value >> FMPAC_REG_KSL_SHIFT;
				ci->dc = (value & FMPAC_REG_DC_BIT) ? 1 : 0;
				ci->dm = (value & FMPAC_REG_DM_BIT) ? 1 : 0;
				ci->fb = value & FMPAC_REG_FB_MASK;
				break;
			case 0x04: ci->arMod = value >> FMPAC_REG_AR_SHIFT; ci->drMod = value & FMPAC_REG_DR_MASK; break;
			case 0x05: ci->arCar = value >> FMPAC_REG_AR_SHIFT; ci->drCar = value & FMPAC_REG_DR_MASK; break;
			case 0x06: ci->slMod = value >> FMPAC_REG_SL_SHIFT; ci->rrMod = value & FMPAC_REG_RR_MASK; break;
			case 0x07: ci->slCar = value >> FMPAC_REG_SL_SHIFT; ci->rrCar = value & FMPAC_REG_RR_MASK; break;
		}
		FMPAC_UpdateCustomUsers(chip);
	}
	else if (address == 0x0E)
	{
		chip->rhythmReg = value;
		if (value & FMPAC_RHYTHM_ENABLE_BIT)
		{
			chip->channels[FMPAC_CHANNEL_BD].keyOn    = (value & FMPAC_RHYTHM_BD_BIT)  ? 1 : 0;
			// HH/SD/TOM/TOP-CY key-on state is read directly from rhythmReg at mix time
			// (see FMPACMixer) rather than mirrored into a channel field here.
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
		FMPAC_UpdateChannelFreq(chip, ch);
	}
	else if (address >= 0x20 && address <= 0x28)
	{
		int ch = address - 0x20;
		FMPAC_Channel *c = &chip->channels[ch];
		c->sustain = (value & FMPAC_REG_SUS_BIT) ? 1 : 0;
		c->block = (value >> FMPAC_REG_BLOCK_SHIFT) & FMPAC_REG_BLOCK_MASK;
		c->fNumber = (c->fNumber & 0x0FF) | ((value & FMPAC_REG_FNUM_MSB_BIT) ? 0x100 : 0);
		FMPAC_UpdateChannelFreq(chip, ch);

		if (!(chip->rhythmReg & FMPAC_RHYTHM_ENABLE_BIT) || ch < FMPAC_CHANNEL_BD)
			c->keyOn = (value & FMPAC_REG_KEY_BIT) ? 1 : 0;
	}
	else if (address >= 0x30 && address <= 0x38)
	{
		int ch = address - 0x30;
		chip->channels[ch].instrument = value >> FMPAC_REG_INST_SHIFT;
		chip->channels[ch].volume = value & FMPAC_REG_VOL_MASK;
		chip->channels[ch].instPtr = FMPAC_GetInstrument(chip, chip->channels[ch].instrument);
		if (ch == FMPAC_CHANNEL_BD)  chip->rhythmVolBD = value & FMPAC_REG_VOL_MASK;
		if (ch == FMPAC_CHANNEL_HHSD)  { chip->rhythmVolHH = value >> FMPAC_REG_INST_SHIFT; chip->rhythmVolSD = value & FMPAC_REG_VOL_MASK; }
		if (ch == FMPAC_CHANNEL_TOMTCY) { chip->rhythmVolTOM = value >> FMPAC_REG_INST_SHIFT; chip->rhythmVolTCY = value & FMPAC_REG_VOL_MASK; }
		FMPAC_UpdateChannelFreq(chip, ch);
	}
}

u8 FMPACRead(u8 address, FMPAC *chip)
{
	(void)address;
	(void)chip;
	return 0xFF;
}

void FMPACMixer(int len, s16 *dest, FMPAC *chip)
{
	int i;
	int rhythmOn = chip->rhythmReg & FMPAC_RHYTHM_ENABLE_BIT;
	int lastMelodic = rhythmOn ? FMPAC_CHANNEL_BD : FMPAC_NUM_CHANNELS;

	for (i = 0; i < len; i++)
	{
		s32 sample = 0;
		int ch;

		if (rhythmOn) FMPAC_AdvanceNoise(chip);

		for (ch = 0; ch < lastMelodic; ch++)
		{
			FMPAC_Channel *cc = &chip->channels[ch];
			if (!cc->keyOn && cc->osc.gain == 0) continue;	// fully idle - skip entirely
			sample += FMPAC_RenderChannel(&cc->osc, cc->keyOn, cc->volume, 1);
		}

		if (rhythmOn)
		{
			FMPAC_Channel *bd = &chip->channels[FMPAC_CHANNEL_BD];
			FMPAC_Channel *hs = &chip->channels[FMPAC_CHANNEL_HHSD];
			FMPAC_Channel *tt = &chip->channels[FMPAC_CHANNEL_TOMTCY];

			u8 hhOn  = (chip->rhythmReg & FMPAC_RHYTHM_HH_BIT)  ? 1 : 0;
			u8 sdOn  = (chip->rhythmReg & FMPAC_RHYTHM_SD_BIT)  ? 1 : 0;
			u8 tomOn = (chip->rhythmReg & FMPAC_RHYTHM_TOM_BIT) ? 1 : 0;
			u8 tcyOn = (chip->rhythmReg & FMPAC_RHYTHM_TCY_BIT) ? 1 : 0;

			if (bd->keyOn || bd->osc.gain != 0)
				sample += FMPAC_RenderChannel(&bd->osc, bd->keyOn, chip->rhythmVolBD, 0);

			// Real HH/SD/TOM/TOP-CY are NOT generic noise - they're built from specific
			// bits of channels 7 & 8's own frequency phase (verified against a real
			// reference OPLL core), which is why real percussion has a "tick"/metallic
			// character rather than a plain "shhh". The phase generators for channels 7
			// & 8 must keep running every sample regardless of their own key state, since
			// HH/TOP-CY derive their sound from them even when TOM/the tonal part isn't
			// itself sounding.
			hs->osc.phase += hs->osc.phaseIncrement;
			tt->osc.phase += tt->osc.phaseIncrement;

			u32 idx7 = (hs->osc.phase >> FMPAC_SIN_SHIFT) & 0xFF;
			u32 idx8 = (tt->osc.phase >> FMPAC_SIN_SHIFT) & 0xFF;
			// Bit positions below are the reference's (7,3,2,8,5,3), each shifted down by
			// 2 to account for our table being 256 entries instead of the reference's 1024.
			u32 hbit7 = (idx7 >> 5) & 1, hbit3 = (idx7 >> 1) & 1, hbit2 = idx7 & 1;
			u32 res1 = (hbit2 ^ hbit7) | hbit3;
			u32 gbit5 = (idx8 >> 3) & 1, gbit3 = (idx8 >> 1) & 1;
			u32 res2 = gbit3 | gbit5;
			u32 highBranch = res2 ? 1 : res1;
			u32 noiseBit = chip->noiseLFSR & 1;

			if (hhOn || hs->osc.gain != 0)
			{
				FMPAC_UpdateGain(&hs->osc, hhOn, FMPAC_PERCUSSION_RELEASE_STEP);
				if (hs->osc.gain != 0)
				{
					u8 phase = highBranch ? (noiseBit ? 180 : 141) : (noiseBit ? 13 : 52);
					s32 s = FMPAC_RhythmSinTable[phase];
					sample += (s * (15 - chip->rhythmVolHH) * hs->osc.gain) >> FMPAC_OUT_SHIFT;
				}
			}
			if (sdOn || chip->rhythmSD.gain != 0)
			{
				FMPAC_UpdateGain(&chip->rhythmSD, sdOn, FMPAC_PERCUSSION_RELEASE_STEP);
				if (chip->rhythmSD.gain != 0)
				{
					u32 hbit8 = (idx7 >> 6) & 1;
					u8 phase = (u8)((hbit8 ? 128 : 64) ^ (noiseBit ? 64 : 0));
					s32 s = FMPAC_RhythmSinTable[phase];
					sample += (s * (15 - chip->rhythmVolSD) * chip->rhythmSD.gain) >> FMPAC_OUT_SHIFT;
				}
			}

			if (tomOn || tt->osc.gain != 0)
				sample += FMPAC_RenderChannel(&tt->osc, tomOn, chip->rhythmVolTOM, 0);

			if (tcyOn || chip->rhythmTCY.gain != 0)
			{
				FMPAC_UpdateGain(&chip->rhythmTCY, tcyOn, FMPAC_PERCUSSION_RELEASE_STEP);
				if (chip->rhythmTCY.gain != 0)
				{
					u8 phase = (u8)(highBranch ? 192 : 64);
					s32 s = FMPAC_RhythmSinTable[phase];
					sample += (s * (15 - chip->rhythmVolTCY) * chip->rhythmTCY.gain) >> FMPAC_OUT_SHIFT;
				}
			}
		}

		// AY driver outputs unsigned-centered PCM (silence = 32768) but writes it into
		// this shared s16 buffer via raw reinterpretation rather than converting to
		// signed first - so AY's "silence" actually lands at -32768 (the bit pattern
		// for unsigned 32768 read back as signed) instead of 0. Without this offset,
		// FM-PAC's own correctly-centered output gets added on top of that heavily
		// negative baseline and any negative half of FM-PAC's waveform clips away
		// instantly against the -32768 floor. This offset cancels that bias back out.
		// FM-PAC is only ever paired with this specific AY driver (confirmed), so this
		// is safe to leave here rather than fixing it at the AY driver's own output -
		// but if that ever changes, this is the first place to look.
		s32 mixed = ((s32)dest[i] + 32767) + sample;
		if (mixed > 32767) mixed = 32767;
		if (mixed < -32768) mixed = -32768;
		dest[i] = (s16)mixed;
	}
}

u32 FMPACGetStateSize(void)
{
	return sizeof(FMPAC);
}

void FMPACSaveState(u8 *dest, FMPAC *chip)
{
	memcpy(dest, chip, sizeof(FMPAC));
}

void FMPACLoadState(FMPAC *chip, const u8 *src)
{
	memcpy(chip, src, sizeof(FMPAC));
}
