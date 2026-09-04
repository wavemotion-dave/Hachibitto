#ifndef _Z80_INTERFACE_H_
#define _Z80_INTERFACE_H_

#include <nds.h>

#include "./cz80/Z80.h"

#define word u16
#define byte u8

extern Z80 CPU;

extern void ClearCPUInterrupt(void);

extern void cpu_writeport16(register unsigned short Port,register unsigned char Value);
extern unsigned char cpu_readport16(register unsigned short Port);

extern void cpu_writeport_msx(register unsigned short Port,register unsigned char Value);
extern unsigned char cpu_readport_msx(register unsigned short Port);

extern void Trap_Bad_Ops(char *prefix, byte I, word W);

#endif
