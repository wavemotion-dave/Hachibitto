;@
;@  SCC.i
;@  Konami SCC+/K052539 sound chip emulator for arm32.
;@
;@  Assembly offsets matching the actual SCC C structure in SCC.h.
;@
;@  C layout:
;@
;@    0x00  ch0Wave[32]
;@    0x20  ch1Wave[32]
;@    0x40  ch2Wave[32]
;@    0x60  ch3Wave[32]
;@    0x80  ch4Wave[32]
;@    0xA0  ch0Frq
;@    0xA2  ch1Frq
;@    0xA4  ch2Frq
;@    0xA6  ch3Frq
;@    0xA8  ch4Frq
;@    0xAA  ch0Volume
;@    0xAB  ch1Volume
;@    0xAC  ch2Volume
;@    0xAD  ch3Volume
;@    0xAE  ch4Volume
;@    0xAF  chControl
;@    0xB0  testReg
;@    0xB1  padding[3]
;@
;@    0xB4  ch0Freq   runtime phase accumulator
;@    0xB6  ch0Addr   runtime address/unused
;@    0xB8  ch1Freq
;@    0xBA  ch1Addr
;@    0xBC  ch2Freq
;@    0xBE  ch2Addr
;@    0xC0  ch3Freq
;@    0xC2  ch3Addr
;@    0xC4  ch4Freq
;@    0xC6  ch4Addr
;@
;@  State saved/loaded = 0xB4 bytes, excluding runtime fields.
;@  Total SCC structure size = 0xC8 bytes.
;@

#if !__ASSEMBLER__
	#error This header file is only for use in assembly files!
#endif

	.struct 0

;@----------------------------------------------------------------------------
;@ SCC wave RAM
;@----------------------------------------------------------------------------

sccCh0Wave:
	.space 32				;@ 0x00-0x1F

sccCh1Wave:
	.space 32				;@ 0x20-0x3F

sccCh2Wave:
	.space 32				;@ 0x40-0x5F

sccCh3Wave:
	.space 32				;@ 0x60-0x7F

sccCh4Wave:
	.space 32				;@ 0x80-0x9F

;@----------------------------------------------------------------------------
;@ SCC register block
;@----------------------------------------------------------------------------

sccCh0Frq:
	.short 0				;@ 0xA0

sccCh1Frq:
	.short 0				;@ 0xA2

sccCh2Frq:
	.short 0				;@ 0xA4

sccCh3Frq:
	.short 0				;@ 0xA6

sccCh4Frq:
	.short 0				;@ 0xA8

sccCh0Volume:
	.byte 0					;@ 0xAA

sccCh1Volume:
	.byte 0					;@ 0xAB

sccCh2Volume:
	.byte 0					;@ 0xAC

sccCh3Volume:
	.byte 0					;@ 0xAD

sccCh4Volume:
	.byte 0					;@ 0xAE

sccChControl:
	.byte 0					;@ 0xAF

sccTestReg:
	.byte 0					;@ 0xB0

sccPadding:
	.space 3				;@ 0xB1-0xB3

;@----------------------------------------------------------------------------
;@ Runtime mixer state
;@----------------------------------------------------------------------------

sccCh0Freq:
	.short 0				;@ 0xB4
sccCh0Addr:
	.short 0				;@ 0xB6

sccCh1Freq:
	.short 0				;@ 0xB8
sccCh1Addr:
	.short 0				;@ 0xBA

sccCh2Freq:
	.short 0				;@ 0xBC
sccCh2Addr:
	.short 0				;@ 0xBE

sccCh3Freq:
	.short 0				;@ 0xC0
sccCh3Addr:
	.short 0				;@ 0xC2

sccCh4Freq:
	.short 0				;@ 0xC4
sccCh4Addr:
	.short 0				;@ 0xC6

;@ End of C SCC structure.
sccSize:

;@ State begins at the actual C struct base.
sccStateStart = sccCh0Wave

;@ State ends immediately before runtime mixer state.
sccStateEnd = sccCh0Freq
