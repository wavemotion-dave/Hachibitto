//
//  FMPAC.c
//  Konami FM-PAC (Yamaha YM2413 / OPLL) sound chip emulator - music only.
//  No SRAM/user-instrument-storage handling here - that's cartridge/mapper
//  territory (same split as SCC.s not touching Konami mapper banking).
//
//  See FMPAC.h for the accuracy-level disclaimer and verified register map.
//

#include "FMPAC.h"
#include <string.h>	// memcpy, memset

//@----------------------------------------------------------------------------
//@ Tunables. All of these are "sounds reasonable" approximations, not
//@ derived from real chip timing - safe to retune by ear later.
//@----------------------------------------------------------------------------
#define FMPAC_SAMPLE_RATE		27965.0		// confirmed value, matches the AY driver's rate
#define FMPAC_MASTER_CLOCK		3579545.0	// MSX standard clock, same as SCC's
#define FMPAC_SIN_SHIFT			24		// phase>>24 -> 8-bit (256 entry) table index
#define FMPAC_MOD_INDEX_SHIFT		15		// modulator-output-to-phase-offset scale (reduced from 20:
							// log-domain samples run 0..4095 vs the old linear table's
							// 0..127, ~32x bigger, so the shift dropped by ~5 bits to
							// keep the same actual modulation depth)
#define FMPAC_OUT_SHIFT			1		// FMPAC_LogSynth already returns the fully-combined
							// magnitude (envelope+volume folded in via the attenuation
							// add, range 0-4095) - this is just a modest headroom
							// rescale, NOT dividing out separate multiplied factors
							// like the old linear approach needed. (Was wrongly set to
							// 9 initially - compared this against the old approach's
							// max PRODUCT of three separate factors, which doesn't
							// apply here since LogSynth's output is already final.)
#define FMPAC_NOISE_SHIFT		8		// separate from FMPAC_OUT_SHIFT: the noise drums still do
							// one real multiply (noise -128..127 times gain 0..4095),
							// which needs its own, larger shift to fit the same headroom

//@----------------------------------------------------------------------------
//@ Log-domain synthesis tables, replacing the earlier linear-sine-table +
//@ multiply approach. This is how real OPLL (and basically every serious
//@ FM/OPL-family emulator) actually combines sine shape and envelope: one
//@ table lookup for the sine's log-magnitude, added to the envelope/TL/
//@ volume attenuation (all in the same log units), then one more table
//@ lookup to convert the combined attenuation back to a linear sample.
//@ That's 2 table lookups + adds per operator, and ZERO multiplies - a real
//@ win on an FPU-less ARM9 where multiplies aren't free, and also more
//@ correct: attenuations are supposed to combine additively in log space,
//@ not by multiplying linear fractions together like the old approach did.
//@
//@ Units: 256 = one octave (6.02dB) of amplitude change. Envelope's 0-255
//@ envLevel (255=loudest) is rescaled to attenuation via (255-envLevel)<<3
//@ (0-2040 units, deliberately reaching just under FMPAC_EXP_TABLE_SIZE so
//@ envLevel==0 always clamps to true silence regardless of TL/volume -
//@ reuses the existing, already-tuned envelope timing
//@ unchanged - only the final combination step changed). TL (0-63) scales
//@ by *8 (0-504 units, ~48dB, matching real TL's range). Channel volume
//@ (0-15) scales by *32 (0-480 units, ~45dB, matching real volume's range).
//@----------------------------------------------------------------------------
#define FMPAC_ATTEN_UNIT_BITS	8		// 256 units = 1 octave
#define FMPAC_EXP_TABLE_SIZE	2048		// combined attenuation beyond this = silence
#define FMPAC_EXP_MAXLIN	4095		// ExpTable[0] value (zero attenuation = full linear scale)

static const u16 FMPAC_LogSinTable[256] =
{
	1625, 1220, 1031,  907,  814,  741,  679,  627,
	 582,  541,  505,  472,  442,  415,  389,  366,
	 344,  324,  304,  286,  269,  253,  238,  224,
	 210,  198,  185,  174,  163,  152,  142,  133,
	 124,  115,  107,   99,   91,   84,   78,   71,
	  65,   59,   54,   49,   44,   39,   35,   31,
	  27,   24,   21,   18,   15,   12,   10,    8,
	   6,    5,    3,    2,    1,    1,    0,    0,
	   0,    0,    1,    1,    2,    3,    5,    6,
	   8,   10,   12,   15,   18,   21,   24,   27,
	  31,   35,   39,   44,   49,   54,   59,   65,
	  71,   78,   84,   91,   99,  107,  115,  124,
	 133,  142,  152,  163,  174,  185,  198,  210,
	 224,  238,  253,  269,  286,  304,  324,  344,
	 366,  389,  415,  442,  472,  505,  541,  582,
	 627,  679,  741,  814,  907, 1031, 1220, 1625,
	1625, 1220, 1031,  907,  814,  741,  679,  627,
	 582,  541,  505,  472,  442,  415,  389,  366,
	 344,  324,  304,  286,  269,  253,  238,  224,
	 210,  198,  185,  174,  163,  152,  142,  133,
	 124,  115,  107,   99,   91,   84,   78,   71,
	  65,   59,   54,   49,   44,   39,   35,   31,
	  27,   24,   21,   18,   15,   12,   10,    8,
	   6,    5,    3,    2,    1,    1,    0,    0,
	   0,    0,    1,    1,    2,    3,    5,    6,
	   8,   10,   12,   15,   18,   21,   24,   27,
	  31,   35,   39,   44,   49,   54,   59,   65,
	  71,   78,   84,   91,   99,  107,  115,  124,
	 133,  142,  152,  163,  174,  185,  198,  210,
	 224,  238,  253,  269,  286,  304,  324,  344,
	 366,  389,  415,  442,  472,  505,  541,  582,
	 627,  679,  741,  814,  907, 1031, 1220, 1625,
};

// FMPAC_ExpOctaveTable[frac] = round(4095 * 2^(-frac/256)), frac 0..255 -
// i.e. one octave's worth of the exponential curve, embedded as plain
// integer data (generated at build time, not runtime - see chat for the
// generator). A full attenuation value (0..2047, 8 octaves) is split into
// an octave count (atten>>8) and a fraction (atten&0xFF): look up the
// fraction here, then shift right by the octave count. No runtime math.h
// functions needed at all - this avoids relying on this platform's libm
// having pow()/exp() linkable, which it apparently doesn't (undefined
// reference to 'exp' when this was computed at startup instead).
static const u16 FMPAC_ExpOctaveTable[256] =
{
	4095, 4084, 4073, 4062, 4051, 4040, 4029, 4018,
	4007, 3996, 3986, 3975, 3964, 3953, 3943, 3932,
	3921, 3911, 3900, 3890, 3879, 3869, 3858, 3848,
	3837, 3827, 3817, 3806, 3796, 3786, 3776, 3765,
	3755, 3745, 3735, 3725, 3715, 3705, 3695, 3685,
	3675, 3665, 3655, 3645, 3635, 3625, 3615, 3606,
	3596, 3586, 3577, 3567, 3557, 3548, 3538, 3528,
	3519, 3509, 3500, 3490, 3481, 3472, 3462, 3453,
	3443, 3434, 3425, 3416, 3406, 3397, 3388, 3379,
	3370, 3361, 3351, 3342, 3333, 3324, 3315, 3306,
	3297, 3289, 3280, 3271, 3262, 3253, 3244, 3236,
	3227, 3218, 3209, 3201, 3192, 3183, 3175, 3166,
	3158, 3149, 3141, 3132, 3124, 3115, 3107, 3098,
	3090, 3082, 3073, 3065, 3057, 3048, 3040, 3032,
	3024, 3016, 3007, 2999, 2991, 2983, 2975, 2967,
	2959, 2951, 2943, 2935, 2927, 2919, 2911, 2903,
	2896, 2888, 2880, 2872, 2864, 2857, 2849, 2841,
	2834, 2826, 2818, 2811, 2803, 2795, 2788, 2780,
	2773, 2765, 2758, 2750, 2743, 2736, 2728, 2721,
	2713, 2706, 2699, 2691, 2684, 2677, 2670, 2662,
	2655, 2648, 2641, 2634, 2627, 2620, 2612, 2605,
	2598, 2591, 2584, 2577, 2570, 2563, 2557, 2550,
	2543, 2536, 2529, 2522, 2515, 2509, 2502, 2495,
	2488, 2481, 2475, 2468, 2461, 2455, 2448, 2442,
	2435, 2428, 2422, 2415, 2409, 2402, 2396, 2389,
	2383, 2376, 2370, 2363, 2357, 2351, 2344, 2338,
	2332, 2325, 2319, 2313, 2307, 2300, 2294, 2288,
	2282, 2276, 2269, 2263, 2257, 2251, 2245, 2239,
	2233, 2227, 2221, 2215, 2209, 2203, 2197, 2191,
	2185, 2179, 2173, 2167, 2161, 2156, 2150, 2144,
	2138, 2132, 2127, 2121, 2115, 2109, 2104, 2098,
	2092, 2087, 2081, 2075, 2070, 2064, 2059, 2053,
};

// Converts a full attenuation value (log units, 0=loudest, >=2048=silent)
// into a linear magnitude 0..4095, via one table lookup and one shift.
static s32 FMPAC_ExpLookup(s32 atten)
{
	if (atten >= FMPAC_EXP_TABLE_SIZE || atten < 0) return 0;
	return (s32)FMPAC_ExpOctaveTable[atten & 0xFF] >> (atten >> 8);
}

// Combines a phase index (0-255) and a total attenuation (log units, 0=loudest)
// into a signed linear sample. This IS the per-operator "synthesis" step.
static s32 FMPAC_LogSynth(u8 phaseIdx, s32 atten)
{
	atten += FMPAC_LogSinTable[phaseIdx];
	s32 mag = FMPAC_ExpLookup(atten);
	return (phaseIdx < 128) ? mag : -mag;
}

//@----------------------------------------------------------------------------
//@ Real OPLL MUL table (this part IS the documented real value - it's a
//@ fixed hardware constant, not something the "approximate" disclaimer
//@ applies to). Stored doubled so index 0 (the real chip's x0.5) is a
//@ whole number; divide by 2 wherever this is consumed.
//@----------------------------------------------------------------------------
static const u8 FMPAC_MulTableX2[16] =
{
	1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
};

//@----------------------------------------------------------------------------
//@ Approximate envelope step table, 16.16 fixed-point units-of-255-per-sample,
//@ indexed by the 4-bit AR/DR/RR register value. Rate 0 = frozen (matches
//@ real HW: rate 0 means "this stage never completes"). Calibrated to roughly
//@ 2ms (fastest, rate 15) through 3s (slowest useful rate, rate 1) for a full
//@ 0-255 sweep at FMPAC_SAMPLE_RATE - NOT derived from real chip timing,
//@ retune by ear if a game's attacks/decays feel off.
//@----------------------------------------------------------------------------
static const u32 FMPAC_EnvStepTable[16] =
{
	       0,      199,      332,      543,
	     919,     1494,     2490,     4121,
	    6791,    11275,    18675,    29880,
	   49799,    85370,   149398,   298796,
};

//@----------------------------------------------------------------------------
//@ 16-entry instrument table - decoded from the real Yamaha YM2413 factory
//@ ROM data (verified against Jarek Burczynski's reference YM2413 core,
//@ the same numbers also appear in FluBBaOfWard's ARM attempt independently -
//@ this is the genuine chip data, not an approximation). Index 0 is never
//@ read (channels with instrument==0 use chip->customInstrument instead)
//@ but is filled in here anyway so the array stays index-aligned with the
//@ register field (avoids off-by-one mistakes later).
//@ Field order: mulMod,mulCar, amMod,amCar, vibMod,vibCar,
//@              egTypeMod,egTypeCar, ksrMod,ksrCar, kslMod,kslCar,
//@              tl, dm,dc, fb, arMod,drMod,slMod,rrMod, arCar,drCar,slCar,rrCar
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
//@ Percussion voices' parameters, also decoded from the real ROM data (the
//@ same reference's drum table, rows 16/18 - row 17/HH-SD has no tonal
//@ component so its rates are applied directly at the FMPAC_RenderNoiseDrum
//@ call sites below instead of through an Instrument struct). The synthesis
//@ *method* for the non-tonal drums (HH/SD/TOP-CY) is still a simplification -
//@ real HW derives their tone partly from channels 7/8's own phase generators
//@ rather than a free-running LFSR - but the ROM-accurate envelope timing is
//@ no longer a guess.
//@----------------------------------------------------------------------------
static const FMPAC_Instrument FMPAC_RhythmBD  = { 1,1, 0,0,0,0, 0,0, 0,0,0,0, 22, 0,0,0, 15,13,2,15, 15,8,6,13 };
static const FMPAC_Instrument FMPAC_RhythmTOM = { 5,1, 0,0,0,0, 0,0, 0,0,0,0, 0,  0,0,0, 15,8,4,9,   11,10,5,5 };
// HH/SD/TOP-CY don't use an Instrument struct - their real ROM rates (13,8,15,9 / 13,8,15,8 / 11,10,5,5)
// are passed directly to FMPAC_RenderNoiseDrum() at the call sites in FMPACMixer.

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
	// fmus = F * 2^(block-1) * masterClock / (72 * 2^18)   [Yamaha application manual formula]
	// then scale by this operator's own MUL factor (stored doubled -> /2 here) and convert
	// to our own fixed-point phase units (2^32 = one full 256-entry table cycle).
	double fmus = (double)fNumber * (double)(1u << block) * FMPAC_MASTER_CLOCK / (72.0 * (1u << 19));
	double opFreq = fmus * (FMPAC_MulTableX2[mulNibble & 0x0F] * 0.5);
	double inc = (opFreq / FMPAC_SAMPLE_RATE) * 4294967296.0; // * 2^32
	if (inc < 0.0) inc = 0.0;
	if (inc > 4294967295.0) inc = 4294967295.0;
	return (u32)inc;
}

static void FMPAC_UpdateChannelFreq(FMPAC *chip, int ch)
{
	FMPAC_Channel *c = &chip->channels[ch];
	const FMPAC_Instrument *inst = FMPAC_GetInstrument(chip, c->instrument);
	c->mod.phaseIncrement = FMPAC_ComputePhaseIncrement(c->fNumber, c->block, inst->mulMod);
	c->car.phaseIncrement = FMPAC_ComputePhaseIncrement(c->fNumber, c->block, inst->mulCar);
}

// Recompute every channel currently using the custom instrument (0) - called
// after any $00-$07 write, since MUL/etc. may have just changed under them.
static void FMPAC_UpdateCustomUsers(FMPAC *chip)
{
	int ch;
	for (ch = 0; ch < FMPAC_NUM_CHANNELS; ch++)
		if (chip->channels[ch].instrument == 0)
			FMPAC_UpdateChannelFreq(chip, ch);
}

static void FMPAC_KeyOn(FMPAC_Operator *op)
{
	op->phase = 0;			// approximation: real HW behavior here is debated/varies; resetting phase
	op->envAccum = 0;
	op->envLevel = 0;		// gives predictable, click-free attacks, which is the priority for now.
	op->envStage = FMPAC_ENV_ATTACK;
	op->feedbackHist[0] = 0;
	op->feedbackHist[1] = 0;
}

static void FMPAC_KeyOff(FMPAC_Operator *op)
{
	if (op->envStage != FMPAC_ENV_IDLE)
		op->envStage = FMPAC_ENV_RELEASE;
}

//@----------------------------------------------------------------------------
//@ Advances envLevel by a fractional (16.16) step, in the given direction.
//@ Separated out because every envelope stage below needs the same
//@ accumulate-and-carry logic, just with a different step/direction.
//@----------------------------------------------------------------------------
static void FMPAC_EnvAdvance(FMPAC_Operator *op, int rising, u32 step)
{
	op->envAccum += step;
	while (op->envAccum >= 0x10000)
	{
		op->envAccum -= 0x10000;
		if (rising) { if (op->envLevel < 255) op->envLevel++; }
		else        { if (op->envLevel > 0)   op->envLevel--; }
	}
}

//@----------------------------------------------------------------------------
//@ Envelope generator - one call per operator per sample. Approximate linear
//@ ADSR, NOT the real chip's logarithmic-rate/key-scaled envelope. See
//@ FMPAC.h.
//@----------------------------------------------------------------------------
static void FMPAC_StepEnvelope(FMPAC_Operator *op, u8 ar, u8 dr, u8 sl, u8 rr, u8 egType)
{
	u8 sustainLevel = 255 - (sl * 17);	// sl 0-15 -> approx 255..0

	switch (op->envStage)
	{
		case FMPAC_ENV_ATTACK:
		{
			u32 step = FMPAC_EnvStepTable[ar & 0x0F];
			if (step == 0) break;		// AR=0: never attacks, real HW stays silent forever too
			FMPAC_EnvAdvance(op, 1, step);
			if (op->envLevel >= 255)
			{
				op->envLevel = 255;
				op->envAccum = 0;
				op->envStage = FMPAC_ENV_DECAY;
			}
			break;
		}
		case FMPAC_ENV_DECAY:
		{
			u32 step = FMPAC_EnvStepTable[dr & 0x0F];
			if (op->envLevel > sustainLevel && step != 0)
			{
				FMPAC_EnvAdvance(op, 0, step);
				if (op->envLevel < sustainLevel) op->envLevel = sustainLevel;
			}
			if (op->envLevel <= sustainLevel)
			{
				if (egType)
				{
					op->envStage = FMPAC_ENV_SUSTAIN;	// sustained: hold here until key-off
					op->envAccum = 0;
				}
				else
				{
					// percussive: real HW keeps decaying past the sustain line, using RR,
					// even while the key is still held - approximated by just continuing
					// to bleed level away here instead of holding.
					u32 rstep = FMPAC_EnvStepTable[rr & 0x0F];
					if (rstep != 0)
					{
						FMPAC_EnvAdvance(op, 0, rstep);
						if (op->envLevel == 0) op->envStage = FMPAC_ENV_IDLE;
					}
				}
			}
			break;
		}
		case FMPAC_ENV_SUSTAIN:
			// egType==1 channels park here (level fixed at sustainLevel) until key-off
			// moves them to RELEASE; nothing to step per-sample.
			break;
		case FMPAC_ENV_RELEASE:
		{
			u32 step = FMPAC_EnvStepTable[rr & 0x0F];
			if (step == 0) break;
			FMPAC_EnvAdvance(op, 0, step);
			if (op->envLevel == 0) op->envStage = FMPAC_ENV_IDLE;
			break;
		}
		case FMPAC_ENV_IDLE:
		default:
			break;
	}
}

//@----------------------------------------------------------------------------
//@ One melodic (2-operator FM) channel, one sample.
//@----------------------------------------------------------------------------
static s32 FMPAC_RenderFMChannel(FMPAC *chip, int ch)
{
	FMPAC_Channel *c = &chip->channels[ch];
	const FMPAC_Instrument *inst = c->instPtr;

	FMPAC_StepEnvelope(&c->mod, inst->arMod, inst->drMod, inst->slMod, inst->rrMod, inst->egTypeMod);
	FMPAC_StepEnvelope(&c->car, inst->arCar, inst->drCar, inst->slCar, inst->rrCar, inst->egTypeCar);

	// Modulator
	c->mod.phase += c->mod.phaseIncrement;
	s32 fbTerm = 0;
	if (inst->fb) fbTerm = ((s32)c->mod.feedbackHist[0] + (s32)c->mod.feedbackHist[1]) >> (14 - inst->fb);
	u8 modIdx = (u8)(((c->mod.phase >> FMPAC_SIN_SHIFT) + (u32)(s32)fbTerm) & 0xFF);
	s32 modAtten = ((255 - c->mod.envLevel) << 3) + (inst->tl << 3);	// envelope + TL, both in log units
	s32 modOut = FMPAC_LogSynth(modIdx, modAtten);
	c->mod.feedbackHist[1] = c->mod.feedbackHist[0];
	c->mod.feedbackHist[0] = (s16)modOut;

	// Carrier, phase-modulated by the modulator's output
	c->car.phase += c->car.phaseIncrement + ((u32)modOut << FMPAC_MOD_INDEX_SHIFT);
	u8 carIdx = (u8)((c->car.phase >> FMPAC_SIN_SHIFT) & 0xFF);
	s32 carAtten = ((255 - c->car.envLevel) << 3) + (c->volume << 5);	// envelope + channel volume
	s32 carOut = FMPAC_LogSynth(carIdx, carAtten) >> FMPAC_OUT_SHIFT;

	return carOut;
}

//@----------------------------------------------------------------------------
//@ One non-tonal rhythm voice (HH/SD/TOP-CY), one sample. Real HW derives
//@ these partly from an LFSR fed by channels 7/8's own phase generators -
//@ this driver just uses a single shared LFSR, which is simpler and close
//@ enough for "sounds percussive", not bit-accurate.
//@----------------------------------------------------------------------------
static s32 FMPAC_RenderNoiseDrum(FMPAC *chip, FMPAC_Operator *op, u8 ar, u8 dr, u8 sl, u8 rr, u8 volume)
{
	FMPAC_StepEnvelope(op, ar, dr, sl, rr, /*egType=*/0);	// percussion is always percussive-type in practice

	// 17-bit Fibonacci LFSR, taps 17/14 - cheap, adequate "hiss" for this purpose.
	u32 lfsr = chip->noiseLFSR;
	u32 bit = ((lfsr >> 0) ^ (lfsr >> 3)) & 1;
	lfsr = (lfsr >> 1) | (bit << 16);
	chip->noiseLFSR = lfsr;

	s32 noise = (s32)(lfsr & 0xFF) - 128;	// -128..127 - no natural "sine" component to log-combine here,
						// so this stays one multiply (fine - only 3 voices, not the 9-channel hot path)
	s32 atten = ((255 - op->envLevel) << 3) + ((volume & 0x0F) << 5);
	s32 gain = FMPAC_ExpLookup(atten);
	return (noise * gain) >> FMPAC_NOISE_SHIFT;
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
		chip->channels[ch].instPtr = &chip->customInstrument;	// instrument==0 after memset
		FMPAC_UpdateChannelFreq(chip, ch);
	}
}

void FMPACWrite(u8 value, u8 address, FMPAC *chip)
{
	if (address <= 0x07)
	{
		// Custom/user instrument registers - two bytes per field-pair (mod=even offset within
		// the $00/$01, $02/$03, ... pairing except $02/$03 and $06/$07 which differ per side).
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

		if (value & FMPAC_RHYTHM_ENABLE_BIT)
		{
			// Edge-detect each drum's own key-on bit independently.
			if ((value & FMPAC_RHYTHM_BD_BIT)  && !(old & FMPAC_RHYTHM_BD_BIT))  { FMPAC_KeyOn(&chip->channels[FMPAC_CHANNEL_BD].mod); FMPAC_KeyOn(&chip->channels[FMPAC_CHANNEL_BD].car); }
			if (!(value & FMPAC_RHYTHM_BD_BIT)  && (old & FMPAC_RHYTHM_BD_BIT))  { FMPAC_KeyOff(&chip->channels[FMPAC_CHANNEL_BD].mod); FMPAC_KeyOff(&chip->channels[FMPAC_CHANNEL_BD].car); }
			if ((value & FMPAC_RHYTHM_HH_BIT)   && !(old & FMPAC_RHYTHM_HH_BIT))   FMPAC_KeyOn(&chip->channels[FMPAC_CHANNEL_HHSD].mod);
			if (!(value & FMPAC_RHYTHM_HH_BIT)  && (old & FMPAC_RHYTHM_HH_BIT))    FMPAC_KeyOff(&chip->channels[FMPAC_CHANNEL_HHSD].mod);
			if ((value & FMPAC_RHYTHM_SD_BIT)   && !(old & FMPAC_RHYTHM_SD_BIT))   FMPAC_KeyOn(&chip->channels[FMPAC_CHANNEL_HHSD].car);
			if (!(value & FMPAC_RHYTHM_SD_BIT)  && (old & FMPAC_RHYTHM_SD_BIT))    FMPAC_KeyOff(&chip->channels[FMPAC_CHANNEL_HHSD].car);
			if ((value & FMPAC_RHYTHM_TOM_BIT)  && !(old & FMPAC_RHYTHM_TOM_BIT))  FMPAC_KeyOn(&chip->channels[FMPAC_CHANNEL_TOMTCY].mod);
			if (!(value & FMPAC_RHYTHM_TOM_BIT) && (old & FMPAC_RHYTHM_TOM_BIT))   FMPAC_KeyOff(&chip->channels[FMPAC_CHANNEL_TOMTCY].mod);
			if ((value & FMPAC_RHYTHM_TCY_BIT)  && !(old & FMPAC_RHYTHM_TCY_BIT))  FMPAC_KeyOn(&chip->channels[FMPAC_CHANNEL_TOMTCY].car);
			if (!(value & FMPAC_RHYTHM_TCY_BIT) && (old & FMPAC_RHYTHM_TCY_BIT))   FMPAC_KeyOff(&chip->channels[FMPAC_CHANNEL_TOMTCY].car);
		}
	}
	else if (address == 0x0F)
	{
		chip->testReg = value;	// storage only
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
		u8 wasKeyOn = c->keyOn;

		c->sustain = (value & FMPAC_REG_SUS_BIT) ? 1 : 0;
		c->keyOn = (value & FMPAC_REG_KEY_BIT) ? 1 : 0;
		c->block = (value >> FMPAC_REG_BLOCK_SHIFT) & FMPAC_REG_BLOCK_MASK;
		c->fNumber = (c->fNumber & 0x0FF) | ((value & FMPAC_REG_FNUM_MSB_BIT) ? 0x100 : 0);
		FMPAC_UpdateChannelFreq(chip, ch);

		// Melodic key-on/off only applies outside rhythm mode for channels 6-8 -
		// in rhythm mode those three channels' key-on comes from $0E instead, and
		// this register's KEY bit (D4) is expected to stay 0 per the setup values.
		if (!(chip->rhythmReg & FMPAC_RHYTHM_ENABLE_BIT) || ch < FMPAC_CHANNEL_BD)
		{
			if (c->keyOn && !wasKeyOn) { FMPAC_KeyOn(&c->mod); FMPAC_KeyOn(&c->car); }
			else if (!c->keyOn && wasKeyOn) { FMPAC_KeyOff(&c->mod); FMPAC_KeyOff(&c->car); }
		}
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
		FMPAC_UpdateChannelFreq(chip, ch);	// instrument change -> MUL may have changed
	}
	// anything else (e.g. mirrors outside $00-$38): real HW ignores it, so do we
}

u8 FMPACRead(u8 address, FMPAC *chip)
{
	(void)address;
	(void)chip;
	return 0xFF;	// real OPLL is write-only for music registers - no meaningful readback exists
}

void FMPACMixer(int len, s16 *dest, FMPAC *chip)
{
	// NOTE: unlike SCC's mixer, this ACCUMULATES into dest rather than overwriting it,
	// so it can be layered on top of other already-mixed chips in one shared buffer.
	// Zero the buffer first (or route every source the same way) if that's not what
	// your integration wants.
	int i;
	int rhythmOn = chip->rhythmReg & FMPAC_RHYTHM_ENABLE_BIT;

	for (i = 0; i < len; i++)
	{
		s32 sample = 0;
		int ch;
		int lastMelodic = rhythmOn ? FMPAC_CHANNEL_BD : FMPAC_NUM_CHANNELS;

		for (ch = 0; ch < lastMelodic; ch++)
		{
			FMPAC_Channel *cc = &chip->channels[ch];
			// A channel that's not keyed on AND both operators are IDLE contributes exactly 0
			// either way (envLevel sits at 0 in IDLE) - skip the whole render for it. Channels
			// in DECAY/SUSTAIN/RELEASE still need rendering even with keyOn==0 (that's a note
			// still ringing out), so this only catches genuinely untouched/finished channels.
			if (!cc->keyOn && cc->mod.envStage == FMPAC_ENV_IDLE && cc->car.envStage == FMPAC_ENV_IDLE)
				continue;
			sample += FMPAC_RenderFMChannel(chip, ch);
		}

		if (rhythmOn)
		{
			FMPAC_Channel *bd  = &chip->channels[FMPAC_CHANNEL_BD];
			FMPAC_Channel *hs  = &chip->channels[FMPAC_CHANNEL_HHSD];
			FMPAC_Channel *tt  = &chip->channels[FMPAC_CHANNEL_TOMTCY];

			// BD: tonal 2-op FM, fixed patch, uses its own channel's freq registers
			FMPAC_StepEnvelope(&bd->mod, FMPAC_RhythmBD.arMod, FMPAC_RhythmBD.drMod, FMPAC_RhythmBD.slMod, FMPAC_RhythmBD.rrMod, FMPAC_RhythmBD.egTypeMod);
			FMPAC_StepEnvelope(&bd->car, FMPAC_RhythmBD.arCar, FMPAC_RhythmBD.drCar, FMPAC_RhythmBD.slCar, FMPAC_RhythmBD.rrCar, FMPAC_RhythmBD.egTypeCar);
			bd->mod.phase += bd->mod.phaseIncrement;
			u8 bdModIdx = (u8)((bd->mod.phase >> FMPAC_SIN_SHIFT) & 0xFF);
			s32 bdModAtten = ((255 - bd->mod.envLevel) << 3) + (FMPAC_RhythmBD.tl << 3);
			s32 bdMod = FMPAC_LogSynth(bdModIdx, bdModAtten);
			bd->car.phase += bd->car.phaseIncrement + ((u32)bdMod << FMPAC_MOD_INDEX_SHIFT);
			u8 bdCarIdx = (u8)((bd->car.phase >> FMPAC_SIN_SHIFT) & 0xFF);
			s32 bdCarAtten = ((255 - bd->car.envLevel) << 3) + (chip->rhythmVolBD << 5);
			sample += FMPAC_LogSynth(bdCarIdx, bdCarAtten) >> FMPAC_OUT_SHIFT;

			// HH (noise) + SD (noise) share channel 7's operator slots
			sample += FMPAC_RenderNoiseDrum(chip, &hs->mod, 13,8,15,9, chip->rhythmVolHH);
			sample += FMPAC_RenderNoiseDrum(chip, &hs->car, 13,8,15,8, chip->rhythmVolSD);

			// TOM (tonal, fixed patch) + TOP-CY (noise) share channel 8's operator slots
			FMPAC_StepEnvelope(&tt->mod, FMPAC_RhythmTOM.arMod, FMPAC_RhythmTOM.drMod, FMPAC_RhythmTOM.slMod, FMPAC_RhythmTOM.rrMod, FMPAC_RhythmTOM.egTypeMod);
			tt->mod.phase += tt->mod.phaseIncrement;
			u8 tomIdx = (u8)((tt->mod.phase >> FMPAC_SIN_SHIFT) & 0xFF);
			s32 tomAtten = ((255 - tt->mod.envLevel) << 3) + (chip->rhythmVolTOM << 5);
			sample += FMPAC_LogSynth(tomIdx, tomAtten) >> FMPAC_OUT_SHIFT;
			sample += FMPAC_RenderNoiseDrum(chip, &tt->car, 11,10,5,5, chip->rhythmVolTCY);
		}

		// Clamp before accumulating - dest[] may already hold another chip's samples.
		s32 mixed = (s32)dest[i] + sample;
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
