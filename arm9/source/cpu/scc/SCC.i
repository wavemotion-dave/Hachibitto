;@
;@  SCC.i
;@  Konami SCC+/K052539 sound chip emulator for arm32.
;@
;@  Created by Fredrik Ahlström on 2006-04-01.
;@  Copyright © 2006-2024 Fredrik Ahlström. All rights reserved.
;@
;@  SCC+ update: Ch3 and Ch4 now have independent 32-byte waveforms
;@  (real SCC/K051649 shares one waveform between them). This shifts
;@  the freq/volume register block from I/O offset 0x80 to 0xA0, with
;@  its mirror moving from 0x90 to 0xB0, matching real SCC+ hardware.
;@
;@ ASM header for the Konami SCC+ emulator
;@

#if !__ASSEMBLER__
	#error This header file is only for use in assembly files!
#endif

							;@ SCC.s
	.struct 0
sccCh0Freq:		.short 0
sccCh0Addr:		.short 0
sccCh1Freq:		.short 0
sccCh1Addr:		.short 0
sccCh2Freq:		.short 0
sccCh2Addr:		.short 0
sccCh3Freq:		.short 0
sccCh3Addr:		.short 0
sccCh4Freq:		.short 0
sccCh4Addr:		.short 0

sccStateStart:
sccCh0Wave:		.space 32	;@ 0x00-0x1F
sccCh1Wave:		.space 32	;@ 0x20-0x3F
sccCh2Wave:		.space 32	;@ 0x40-0x5F
sccCh3Wave:		.space 32	;@ 0x60-0x7F
sccCh4Wave:		.space 32	;@ 0x80-0x9F Independent from Ch3 (SCC+)
sccCh0Frq:		.short 0	;@ 0xA0/0xB0
sccCh1Frq:		.short 0	;@ 0xA2/0xB2
sccCh2Frq:		.short 0	;@ 0xA4/0xB4
sccCh3Frq:		.short 0	;@ 0xA6/0xB6
sccCh4Frq:		.short 0	;@ 0xA8/0xB8
sccCh0Volume:	.byte 0		;@ 0xAA/0xBA
sccCh1Volume:	.byte 0		;@ 0xAB/0xBB
sccCh2Volume:	.byte 0		;@ 0xAC/0xBC
sccCh3Volume:	.byte 0		;@ 0xAD/0xBD
sccCh4Volume:	.byte 0		;@ 0xAE/0xBE
sccChControl:	.byte 0		;@ 0xAF/0xBF

sccTestReg:		.byte 0		;@ 0xC0-0xFF (deformation reg - stored only, not implemented)
sccPadding:		.space 3
sccStateEnd:

sccSize:

;@----------------------------------------------------------------------------
