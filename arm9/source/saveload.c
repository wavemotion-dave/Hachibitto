// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave (Phoenix-Edition),
// Alekmaul (original port) and Marat Fayzullin (ColEM core) are thanked profusely.
//
// The Hachibitto emulator is offered as-is, without any warranty. Please see readme.md
// =====================================================================================
#include <nds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fat.h>
#include <dirent.h>

#include "Hachibitto.h"
#include "CRC32.h"
#include "cpu/z80/Z80_interface.h"
#include "MSX_generic.h"
#include "fdc.h"
#include "lzav.h"
#include "printf.h"

#define MSX_SAVE_VER   0x0001  // Change this if the basic format of the .SAV file changes. Invalidates older .sav files.

// -----------------------------------------------------------------------------------------------------
// Since the main MemoryMap[] can point to differt things (RAM, ROM, BIOS, etc) and since we can't rely
// on the memory being in the same spot on subsequent versions of the emulator... we need to save off
// the type and the offset so that we can patch it back together when we load back a saved state.
// -----------------------------------------------------------------------------------------------------
struct RomOffset
{
    u8   type;
    u32  offset;
};

struct RomOffset Offsets[8];

#define TYPE_ROM   0
#define TYPE_RAM   1
#define TYPE_BIOS  2
#define TYPE_EXP   3
#define TYPE_FDC   4
#define TYPE_OTHER 5

/*********************************************************************************
 * Save the current state - save everything we need to a single .sav file.
 ********************************************************************************/

// --------------------------------------------------------------------------------------
// We use a 128K buffer on the back-end of the ROM_Memory to use for lzav compression.
// So we move that 128K block out to an unused VRAM area temporarily while we save/load.
// This saves us from having to allocate a large 128K buffer just for save/load use.
// --------------------------------------------------------------------------------------
void allocateCompressedMem(void)
{
    memcpy((u8*)0x6820000, COMPRESS_BUFFER, 128*1024);
}

void restoreCompressedMem(void)
{
    memcpy(COMPRESS_BUFFER, (u8*)0x6820000, 128*1024);
}

void msxSaveState(void)
{
    //TODO
}


/*********************************************************************************
 * Load the current state - read everything back from the .sav file.
 ********************************************************************************/
void msxLoadState(void)
{
    //TODO
}

// End of file
