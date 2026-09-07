;@
;@  SCC.s
;@  Konami SCC+/K052539 sound chip emulator for arm32.
;@
;@  Created by Fredrik Ahlström on 2006-04-01.
;@  Copyright © 2006-2024 Fredrik Ahlström. All rights reserved.
;@
;@  SCC+ update: see SCC.i for the address-map rationale. Summary of
;@  what changed here vs. the original SCC driver:
;@    - SCCMixer: Ch4 now reads its own waveform (r12 is transiently
;@      nudged +0x20 to reach it, then restored) instead of reusing
;@      Ch3's pointer.
;@    - SCCWrite: wave RAM now spans 0x00-0x9F (was 0x00-0x7F), the
;@      register block moves to 0xA0-0xAF mirrored at 0xB0-0xBF (was
;@      0x80-0x8F mirrored at 0x90-0x9F), everything from 0xC0 up is
;@      still the catch-all "test/deform" byte.
;@    - SCCRead: wave read range extends to 0x00-0x9F; 0xA0-0xFF
;@      returns 0xFF (write-only registers), matching real SCC+
;@      "Mode::Plus" peek behavior. This needed an actual compare
;@      instead of the original's single-shift bit-7 test, because
;@      0xA0 isn't a power-of-two boundary the way 0x80 was.
;@    - SCCLoadState: register replay loop now targets I/O addresses
;@      0xA0-0xAF instead of 0x80-0x8F.
;@    - No Real/Compatible mode switching and no deformation/rotate
;@      register behavior are implemented, same as the original
;@      driver - sccTestReg is storage only, matching the prior
;@      sccTestReg's level of support.
;@    - Per-channel volume is no longer applied by self-modifying the
;@      mixer's own instructions (the old vol0-vol4 code-patch trick).
;@      SCCWrite now stores the precomputed volume into the existing
;@      (previously unused) sccCh0Volume-sccCh4Volume struct fields,
;@      and SCCMixer reads them as plain data - the same pattern
;@      AY38910.s already uses for its ayCalculatedVolumes table.
;@      SCCMixer can be invoked asynchronously by the audio engine
;@      while SCCWrite runs from the Z80 core, so patching live
;@      instruction immediates was a genuine (if narrow) race: a
;@      write could land while the mixer's pipeline had already
;@      fetched the old instruction. Plain byte loads/stores are
;@      atomic, so this removes that hazard entirely.
;@
#ifdef __arm__

#include "SCC.i"

#if !defined(SCCMULT)
	#define SCCMULT 16
#endif
#define SCCADDITION 0x00004000*SCCMULT

	.global SCCReset
	.global SCCSaveState
	.global SCCLoadState
	.global SCCGetStateSize
	.global SCCMixer
	.global SCCWrite
	.global SCCRead


	.syntax unified
	.arm

	.section .itcm
	.align 2
;@----------------------------------------------------------------------------
;@ r0  = Mix length.
;@ r1  = Mixerbuffer.
;@ r2  = sccptr.
;@ r3 -> r7 = pos+freq.
;@ r8  = Sample reg/volume.
;@ r9  = Mixer reg.
;@ r12 = Ch3 wave base (nudged transiently to reach Ch4 wave, then restored).
;@ lr  = Scrap.
;@----------------------------------------------------------------------------
//IIIIIVCCCCCCCCCCCC10FFFFFFFFFFFF
//I=sampleindex, V=overflow, C=counter, F=frequency
;@----------------------------------------------------------------------------
SCCMixer:					;@ r0=len, r1=dest, r2=SCCptr
	.type   SCCMixer STT_FUNC
;@----------------------------------------------------------------------------
	stmfd sp!,{r4-r11,lr}
	ldmia r2!,{r3-r7}			;@ r2 now points to Ch0 wave.
	add r10,r2,#0x20			;@ Ch1 Wave
	add r11,r2,#0x40			;@ Ch2 Wave
	add r12,r2,#0x60			;@ Ch3 Wave
;@----------------------------------------------------------------------------
sccMixLoop:
	add r3,r3,#SCCADDITION
	movs lr,r3,lsr#27
	mov r8,r3,lsl#18
	subcs r3,r3,r8,asr#4
	ldrb r9,[r2,#sccCh0Volume-sccStateStart]	;@ Volume (plain data - safe under concurrent SCCWrite)
	cmp r9,#0
	ldrsbne lr,[r2,lr]			;@ Channel 0
	mulne r9,lr,r9


	add r4,r4,#SCCADDITION
	movs lr,r4,lsr#27
	mov r8,r4,lsl#18
	subcs r4,r4,r8,asr#4
	ldrb r8,[r2,#sccCh1Volume-sccStateStart]	;@ Volume (plain data)
	cmp r8,#0
	ldrsbne lr,[r10,lr]			;@ Channel 1
	mlane r9,r8,lr,r9


	add r5,r5,#SCCADDITION
	movs lr,r5,lsr#27
	mov r8,r5,lsl#18
	subcs r5,r5,r8,asr#4
	ldrb r8,[r2,#sccCh2Volume-sccStateStart]	;@ Volume (plain data)
	cmp r8,#0
	ldrsbne lr,[r11,lr]			;@ Channel 2
	mlane r9,r8,lr,r9


	add r6,r6,#SCCADDITION
	movs lr,r6,lsr#27
	mov r8,r6,lsl#18
	subcs r6,r6,r8,asr#4
	ldrb r8,[r2,#sccCh3Volume-sccStateStart]	;@ Volume (plain data)
	cmp r8,#0
	ldrsbne lr,[r12,lr]			;@ Channel 3
	mlane r9,r8,lr,r9


	add r7,r7,#SCCADDITION
	movs lr,r7,lsr#27
	mov r8,r7,lsl#18
	subcs r7,r7,r8,asr#4
	add r12,r12,#0x20			;@ Ch3 Wave base -> Ch4 Wave base (SCC+, independent)
	ldrb r8,[r2,#sccCh4Volume-sccStateStart]	;@ Volume (plain data)
	cmp r8,#0
	ldrsbne lr,[r12,lr]			;@ Channel 4, own waveform (SCC+)
	mlane r9,r8,lr,r9
	sub r12,r12,#0x20			;@ restore Ch3 Wave base for next sample


	subs r0,r0,#1
	strhpl r9,[r1],#2
	bhi sccMixLoop

	stmdb r2!,{r3-r7}
	ldmfd sp!,{r4-r11,lr}
	bx lr
;@----------------------------------------------------------------------------

	.section .text
	.align 2
;@----------------------------------------------------------------------------
SCCReset:					;@ r0=SCCptr
	.type   SCCReset STT_FUNC
;@----------------------------------------------------------------------------
	stmfd sp!,{r0,lr}
	mov r1,#0
	mov r2,#sccSize
	bl memset					;@ clear variables
	ldmfd sp!,{r0,lr}
	mov r1,#0x20
	strb r1,[r0,#sccCh0Freq+1]	;@ counters
	strb r1,[r0,#sccCh1Freq+1]	;@ counters
	strb r1,[r0,#sccCh2Freq+1]	;@ counters
	strb r1,[r0,#sccCh3Freq+1]	;@ counters
	strb r1,[r0,#sccCh4Freq+1]	;@ counters
	bx lr
;@----------------------------------------------------------------------------
SCCSaveState:				;@ In r0=destination, r1=SCCptr. Out r0=state size.
	.type   SCCSaveState STT_FUNC
;@----------------------------------------------------------------------------
	add r1,r1,#sccStateStart
	mov r2,#sccStateEnd-sccStateStart
	stmfd sp!,{r2,lr}
	bl memcpy
	ldmfd sp!,{r0,lr}
	bx lr
;@----------------------------------------------------------------------------
SCCLoadState:				;@ In r0=SCCptr, r1=source. Out r0=state size.
	.type   SCCLoadState STT_FUNC
;@----------------------------------------------------------------------------
	stmfd sp!,{r4,r5,lr}
	mov r4,r0
	add r0,r0,#sccStateStart
	mov r2,#sccStateEnd-sccStateStart
	bl memcpy
	mov r5,#0xF
stateLoop:
	add r2,r4,#sccStateStart
	add r1,r5,#0xA0				;@ SCC+ register block starts at 0xA0 (was 0x80)
	ldrb r0,[r2,r1]
	mov r2,r4
	bl SCCWrite
	subs r5,r5,#1
	bpl stateLoop
	ldmfd sp!,{r4,r5,lr}
;@----------------------------------------------------------------------------
SCCGetStateSize:			;@ Out r0=state size.
	.type   SCCGetStateSize STT_FUNC
;@----------------------------------------------------------------------------
	mov r0,#sccStateEnd-sccStateStart
	bx lr
;@----------------------------------------------------------------------------
SCCVolume:
	.byte 0,3,7,10,14,17,20,24,27,31,34,37,41,44,48,51
;@----------------------------------------------------------------------------
SCCRead:					;@ 0x9800-0x9FFF, r0=adr, r1=SCCptr
	.type   SCCRead STT_FUNC
;@----------------------------------------------------------------------------
	and r0,r0,#0xFF				;@ 0xA0 isn't a power-of-two boundary, so this
	cmp r0,#0xA0				;@ needs an actual compare (old code tested bit 7
	ldrblo r0,[r1,r0]			;@ via a shift, which only worked for a 0x80 split)
	movhs r0,#0xFF				;@ 0xA0-0xFF: freq/vol/deform block, write only
	bx lr
;@----------------------------------------------------------------------------
SCCWrite:					;@ 0x9800-0x9FFF, r0=val, r1=adr, r2=SCCptr
	.type   SCCWrite STT_FUNC
;@----------------------------------------------------------------------------
	and r1,r1,#0xFF				;@ 0x00-0x9F wave ram (Ch0-Ch4, SCC+).
	cmp r1,#0xB0				;@ 0xA0-0xAF registers, 0xB0-0xBF mirror.
	subpl r1,r1,#0x10			;@ 0xC0-0xFF test/deform register, all mirrors.
	cmp r1,#0xB0
	add r3,r2,#sccCh0Wave
	strbmi r0,[r3,r1]
	strbpl r0,[r2,#sccTestReg]
	bxpl lr
	subs r1,r1,#0xA0
	ldrpl pc,[pc,r1,lsl#2]
	bx lr
	.long sccCh0FreqLW			;@ 0xA0
	.long sccCh0FreqHW			;@ 0xA1
	.long sccCh1FreqLW			;@ 0xA2
	.long sccCh1FreqHW			;@ 0xA3
	.long sccCh2FreqLW			;@ 0xA4
	.long sccCh2FreqHW			;@ 0xA5
	.long sccCh3FreqLW			;@ 0xA6
	.long sccCh3FreqHW			;@ 0xA7
	.long sccCh4FreqLW			;@ 0xA8
	.long sccCh4FreqHW			;@ 0xA9
	.long sccCh0VolW			;@ 0xAA
	.long sccCh1VolW			;@ 0xAB
	.long sccCh2VolW			;@ 0xAC
	.long sccCh3VolW			;@ 0xAD
	.long sccCh4VolW			;@ 0xAE
	.long sccKeyOnW				;@ 0xAF

;@----------------------------------------------------------------------------
sccCh0FreqLW:
;@----------------------------------------------------------------------------
	strb r0,[r2,#sccCh0Freq]
	bx lr
;@----------------------------------------------------------------------------
sccCh0FreqHW:
;@----------------------------------------------------------------------------
	ldrb r1,[r2,#sccCh0Freq+1]
	and r0,r0,#0x0F
	bic r1,r1,#0x0F
	orr r1,r1,r0
	strb r1,[r2,#sccCh0Freq+1]
	bx lr
;@----------------------------------------------------------------------------
sccCh1FreqLW:
;@----------------------------------------------------------------------------
	strb r0,[r2,#sccCh1Freq]
	bx lr
;@----------------------------------------------------------------------------
sccCh1FreqHW:
;@----------------------------------------------------------------------------
	ldrb r1,[r2,#sccCh1Freq+1]
	and r0,r0,#0x0F
	bic r1,r1,#0x0F
	orr r1,r1,r0
	strb r1,[r2,#sccCh1Freq+1]
	bx lr
;@----------------------------------------------------------------------------
sccCh2FreqLW:
;@----------------------------------------------------------------------------
	strb r0,[r2,#sccCh2Freq]
	bx lr
;@----------------------------------------------------------------------------
sccCh2FreqHW:
;@----------------------------------------------------------------------------
	ldrb r1,[r2,#sccCh2Freq+1]
	and r0,r0,#0x0F
	bic r1,r1,#0x0F
	orr r1,r1,r0
	strb r1,[r2,#sccCh2Freq+1]
	bx lr
;@----------------------------------------------------------------------------
sccCh3FreqLW:
;@----------------------------------------------------------------------------
	strb r0,[r2,#sccCh3Freq]
	bx lr
;@----------------------------------------------------------------------------
sccCh3FreqHW:
;@----------------------------------------------------------------------------
	ldrb r1,[r2,#sccCh3Freq+1]
	and r0,r0,#0x0F
	bic r1,r1,#0x0F
	orr r1,r1,r0
	strb r1,[r2,#sccCh3Freq+1]
	bx lr
;@----------------------------------------------------------------------------
sccCh4FreqLW:
;@----------------------------------------------------------------------------
	strb r0,[r2,#sccCh4Freq]
	bx lr
;@----------------------------------------------------------------------------
sccCh4FreqHW:
;@----------------------------------------------------------------------------
	ldrb r1,[r2,#sccCh4Freq+1]
	and r0,r0,#0x0F
	bic r1,r1,#0x0F
	orr r1,r1,r0
	strb r1,[r2,#sccCh4Freq+1]
	bx lr
;@----------------------------------------------------------------------------
sccCh0VolW:
;@----------------------------------------------------------------------------
	ands r0,r0,#0x0F
	ldrbne r1,[r2,#sccChControl]
	andsne r1,r1,#0x01
	adrne r1,SCCVolume
	ldrbne r0,[r1,r0]
	strb r0,[r2,#sccCh0Volume]	;@ plain data write - mixer reads it live
	bx lr
;@----------------------------------------------------------------------------
sccCh1VolW:
;@----------------------------------------------------------------------------
	ands r0,r0,#0x0F
	ldrbne r1,[r2,#sccChControl]
	andsne r1,r1,#0x02
	adrne r1,SCCVolume
	ldrbne r0,[r1,r0]
	strb r0,[r2,#sccCh1Volume]	;@ plain data write - mixer reads it live
	bx lr
;@----------------------------------------------------------------------------
sccCh2VolW:
;@----------------------------------------------------------------------------
	ands r0,r0,#0x0F
	ldrbne r1,[r2,#sccChControl]
	andsne r1,r1,#0x04
	adrne r1,SCCVolume
	ldrbne r0,[r1,r0]
	strb r0,[r2,#sccCh2Volume]	;@ plain data write - mixer reads it live
	bx lr
;@----------------------------------------------------------------------------
sccCh3VolW:
;@----------------------------------------------------------------------------
	ands r0,r0,#0x0F
	ldrbne r1,[r2,#sccChControl]
	andsne r1,r1,#0x08
	adrne r1,SCCVolume
	ldrbne r0,[r1,r0]
	strb r0,[r2,#sccCh3Volume]	;@ plain data write - mixer reads it live
	bx lr
;@----------------------------------------------------------------------------
sccCh4VolW:
;@----------------------------------------------------------------------------
	ands r0,r0,#0x0F
	ldrbne r1,[r2,#sccChControl]
	andsne r1,r1,#0x10
	adrne r1,SCCVolume
	ldrbne r0,[r1,r0]
	strb r0,[r2,#sccCh4Volume]	;@ plain data write - mixer reads it live
;@----------------------------------------------------------------------------
sccKeyOnW:
;@----------------------------------------------------------------------------
	bx lr
;@----------------------------------------------------------------------------
	.end
#endif // #ifdef __arm__
