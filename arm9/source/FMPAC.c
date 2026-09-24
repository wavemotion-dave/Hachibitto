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
#define FMPAC_PERCUSSION_RELEASE_STEP	7500		// PERCUSSION release rate: ~30ms, NOT the melodic 150ms.
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

static void FMPAC_RhythmRetrigger(FMPAC_Oscillator *osc, int resetPhase)
{
	/* YM2413 rhythm bits are trigger/key-on controls, not sustained
	   oscillator gates.  A 0->1 write starts a new percussion envelope. */
	osc->gain = 255;
	osc->releaseAccum = 0;
	if (resetPhase)
		osc->phase = 0;
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
		if (value & FMPAC_RHYTHM_ENABLE_BIT)
		{
			if (!(old & FMPAC_RHYTHM_ENABLE_BIT))
			{
				/* Entering rhythm mode: any selected percussion voice is a new hit. */
				if (value & FMPAC_RHYTHM_BD_BIT)
					FMPAC_RhythmRetrigger(&chip->channels[FMPAC_CHANNEL_BD].osc, 1);
				if (value & FMPAC_RHYTHM_SD_BIT)
					FMPAC_RhythmRetrigger(&chip->rhythmSD, 1);
				if (value & FMPAC_RHYTHM_TOM_BIT)
					FMPAC_RhythmRetrigger(&chip->channels[FMPAC_CHANNEL_TOMTCY].osc, 1);
				if (value & FMPAC_RHYTHM_TCY_BIT)
					FMPAC_RhythmRetrigger(&chip->rhythmTCY, 0);
				if (value & FMPAC_RHYTHM_HH_BIT)
					FMPAC_RhythmRetrigger(&chip->channels[FMPAC_CHANNEL_HHSD].osc, 0);
			}
			else
			{
				/* Normal drum trigger: only newly asserted bits re-attack. */
				if ((value & FMPAC_RHYTHM_BD_BIT) && !(old & FMPAC_RHYTHM_BD_BIT))
					FMPAC_RhythmRetrigger(&chip->channels[FMPAC_CHANNEL_BD].osc, 1);
				if ((value & FMPAC_RHYTHM_SD_BIT) && !(old & FMPAC_RHYTHM_SD_BIT))
					FMPAC_RhythmRetrigger(&chip->rhythmSD, 1);
				if ((value & FMPAC_RHYTHM_TOM_BIT) && !(old & FMPAC_RHYTHM_TOM_BIT))
					FMPAC_RhythmRetrigger(&chip->channels[FMPAC_CHANNEL_TOMTCY].osc, 1);
				if ((value & FMPAC_RHYTHM_TCY_BIT) && !(old & FMPAC_RHYTHM_TCY_BIT))
					FMPAC_RhythmRetrigger(&chip->rhythmTCY, 0);
				if ((value & FMPAC_RHYTHM_HH_BIT) && !(old & FMPAC_RHYTHM_HH_BIT))
					FMPAC_RhythmRetrigger(&chip->channels[FMPAC_CHANNEL_HHSD].osc, 0);
			}
		}
		else
		{
			/* Leaving rhythm mode mutes the dedicated rhythm voices. */
			chip->channels[FMPAC_CHANNEL_BD].osc.gain = 0;
			chip->channels[FMPAC_CHANNEL_TOMTCY].osc.gain = 0;
			chip->channels[FMPAC_CHANNEL_HHSD].osc.gain = 0;
			chip->rhythmSD.gain = 0;
			chip->rhythmTCY.gain = 0;
		}

		/* The melodic key-on field is not used for rhythm voices. */
		chip->channels[FMPAC_CHANNEL_BD].keyOn = 0;
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

			/*
			 * Rhythm voices are one-shot envelopes.  FMPAC_RhythmRetrigger()
			 * starts them at full gain when the corresponding 0E bit rises;
			 * after that they decay regardless of whether the bit remains 1.
			 *
			 * This is important for Aleste: its trace repeatedly writes 0E=28
			 * between frame updates, but only occasionally writes 0E=20 followed
			 * immediately by 0E=28 to create the actual re-trigger.
			 */
			if (bd->osc.gain != 0)
			{
				FMPAC_UpdateGain(&bd->osc, 0, FMPAC_PERCUSSION_RELEASE_STEP);
				if (bd->osc.gain != 0)
				{
					bd->osc.phase += bd->osc.phaseIncrement;
					s32 s = FMPAC_SinTable[(bd->osc.phase >> FMPAC_SIN_SHIFT) & 0xFF];
					sample += (s * (15 - chip->rhythmVolBD) * bd->osc.gain) >> FMPAC_OUT_SHIFT;
				}
			}

			if (tt->osc.gain != 0)
			{
				FMPAC_UpdateGain(&tt->osc, 0, FMPAC_PERCUSSION_RELEASE_STEP);
				if (tt->osc.gain != 0)
				{
					tt->osc.phase += tt->osc.phaseIncrement;
					s32 s = FMPAC_SinTable[(tt->osc.phase >> FMPAC_SIN_SHIFT) & 0xFF];
					sample += (s * (15 - chip->rhythmVolTOM) * tt->osc.gain) >> FMPAC_OUT_SHIFT;
				}
			}

			/*
			 * YM2413 rhythm mode is NOT a generic white-noise generator.
			 *
			 * The OPLL combines the noise bit with selected phase bits from the
			 * HH and top-cymbal phase generators. SD, HH and TCY then select one
			 * of several fixed phase positions from their waveform. This is the
			 * important missing ingredient in the previous versions: feeding the
			 * LFSR amplitude directly to the DAC produces a sharp broadband tick,
			 * whereas the real chip produces a much denser, phase-shaped drum
			 * waveform.
			 *
			 * This follows the compact rhythm equations used by emu2413:
			 *   SD:  phase bit 8 + noise bit
			 *   CYM: short-noise bit
			 *   HH:  short-noise bit + noise bit
			 * The existing fast 256-entry waveform table is used for the selected
			 * phase positions, so this adds no large table or expensive FM path.
			 */
			if (hs->osc.gain != 0 || chip->rhythmSD.gain != 0 || chip->rhythmTCY.gain != 0)
			{
				/*
				 * Keep the YM2413-style 23-bit noise generator running continuously.
				 * emu2413 clocks it by:
				 *
				 *     if (noise & 1) noise ^= 0x800200;
				 *     noise >>= 1;
				 *
				 * This is deliberately different from the old 17-bit LFSR.
				 */
				if (chip->noiseLFSR & 1)
					chip->noiseLFSR ^= 0x800200;
				chip->noiseLFSR >>= 1;
				chip->noiseLFSR &= 0x7FFFFF;
				u32 noiseBit = chip->noiseLFSR & 1;

				/* 10-bit phase outputs corresponding to the OPLL PG. */
				u32 hhPhase = (chip->channels[FMPAC_CHANNEL_HHSD].osc.phase >> 22) & 0x3FF;
				u32 cymPhase = (chip->channels[FMPAC_CHANNEL_TOMTCY].osc.phase >> 22) & 0x3FF;

				/*
				 * Short-noise equation from the OPLL rhythm section:
				 * (HH bit2 xor bit7) | (HH bit3 xor CYM bit5) |
				 * (CYM bit3 xor CYM bit5)
				 */
				u32 shortNoise =
					(((hhPhase >> 2) & 1) ^ ((hhPhase >> 7) & 1)) |
					(((hhPhase >> 3) & 1) ^ ((cymPhase >> 5) & 1)) |
					(((cymPhase >> 3) & 1) ^ ((cymPhase >> 5) & 1));

				/* Convert a 10-bit phase position to our 256-entry waveform. */
#define FMPAC_RHYTHM_WAVE(p) FMPAC_SinTable[((p) >> 2) & 0xFF]

				if (hs->osc.gain != 0)
				{
					FMPAC_UpdateGain(&hs->osc, 0, FMPAC_PERCUSSION_RELEASE_STEP);
					if (hs->osc.gain != 0)
					{
						/*
						 * YM2413 HH:
						 * short_noise ? {2D0,234} : {034,0D0},
						 * selected by the noise bit.
						 */
						u32 phase;
						if (shortNoise)
							phase = noiseBit ? 0x2D0 : 0x234;
						else
							phase = noiseBit ? 0x034 : 0x0D0;

						s32 s = FMPAC_RHYTHM_WAVE(phase);
						sample += (s * (15 - chip->rhythmVolHH) * hs->osc.gain) >> FMPAC_OUT_SHIFT;
					}
				}

				if (chip->rhythmSD.gain != 0)
				{
					FMPAC_UpdateGain(&chip->rhythmSD, 0, FMPAC_PERCUSSION_RELEASE_STEP);
					if (chip->rhythmSD.gain != 0)
					{
						/*
						 * YM2413 SD:
						 * if carrier phase bit 8 is set, select 300/200;
						 * otherwise select 000/100; noise chooses within the pair.
						 */
						u32 phase;
						if (hhPhase & 0x100)
							phase = noiseBit ? 0x300 : 0x200;
						else
							phase = noiseBit ? 0x000 : 0x100;

						s32 s = FMPAC_RHYTHM_WAVE(phase);
						sample += (s * (15 - chip->rhythmVolSD) * chip->rhythmSD.gain) >> FMPAC_OUT_SHIFT;
					}
				}

				if (chip->rhythmTCY.gain != 0)
				{
					FMPAC_UpdateGain(&chip->rhythmTCY, 0, FMPAC_PERCUSSION_RELEASE_STEP);
					if (chip->rhythmTCY.gain != 0)
					{
						/* YM2413 top cymbal: short-noise selects 300 or 100. */
						u32 phase = shortNoise ? 0x300 : 0x100;
						s32 s = FMPAC_RHYTHM_WAVE(phase);
						sample += (s * (15 - chip->rhythmVolTCY) * chip->rhythmTCY.gain) >> FMPAC_OUT_SHIFT;
					}
				}

#undef FMPAC_RHYTHM_WAVE
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
