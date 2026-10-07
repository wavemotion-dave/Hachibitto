#ifndef _Z80_INTERFACE_H_
#define _Z80_INTERFACE_H_

#include <nds.h>

#include "./cz80/Z80.h"

#define word u16
#define byte u8

extern Z80 CPU;

extern u8 sram_write_enabled_a;
extern u8 sram_write_enabled_b;
extern u8 special_memory_access;
extern u8 FMPAC_SRAM_in_view;

extern void ClearCPUInterrupt(void);

extern void cpu_writeport_msx(register u8 Port,register unsigned char Value);
extern unsigned char cpu_readport_msx(register u8 Port);

extern void cpu_writemem16 (u16 address, u8 value);
extern byte cpu_readmem16 (u16 address);

extern void Trap_Bad_Ops(char *prefix, byte I, word W);

#endif
