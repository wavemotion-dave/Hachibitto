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

static char szLoadFile[256];        // We build the filename out of the base filename and tack on .sav, .ee, etc.

/*********************************************************************************
 * Save the current state - save everything we need to a single .sav file.
 ********************************************************************************/

// --------------------------------------------------------------------------------------
// We use a 256K buffer on the back-end of the ROM_Memory to use for lzav compression.
// So we move that 256K block out to an unused VRAM area temporarily while we save/load.
// This saves us from having to allocate a large 256K buffer just for save/load use.
// --------------------------------------------------------------------------------------
void preserveCompressedMem(void)
{
    memcpy((u8*)0x06860000, COMPRESS_BUFFER, 256*1024);
}

void restoreCompressedMem(void)
{
    memcpy(COMPRESS_BUFFER, (u8*)0x06860000, 256*1024);
}

void msxSaveState(void)
{
    size_t retVal = 0;
    
    preserveCompressedMem();

    chdir(initial_path);

    // Init filename = romname and SAV in place of ROM
    DIR* dir = opendir("sav");
    if (dir) closedir(dir);    // Directory exists... close it out and move on.
    else mkdir("sav", 0777);   // Otherwise create the directory...
    sprintf(szLoadFile,"sav/%s", initial_file);

    int len = strlen(szLoadFile);
    if (szLoadFile[len-3] == '.') // In case of .sg or .sc
    {
      szLoadFile[len-2] = 's';
      szLoadFile[len-1] = 'a';
      szLoadFile[len-0] = 'v';
      szLoadFile[len+1] = 0;
    }
    else
    {
      szLoadFile[len-3] = 's';
      szLoadFile[len-2] = 'a';
      szLoadFile[len-1] = 'v';
    }
    
    DSPrint(20,0, 2, "-./");
    DSPrint(20,1, 2, "MNO");

    FILE *handle = fopen(szLoadFile, "wb+");
    if (handle != NULL)
    {
        // Write Version
        u16 save_ver = MSX_SAVE_VER;
        retVal = fwrite(&save_ver, sizeof(u16), 1, handle);

        // Write CZ80 CPU
        retVal = fwrite(&CPU, sizeof(CPU), 1, handle);

        // -----------------------------------------------------------------------
        // Compress the 64K RAM data using 'high' compression ratio... it's
        // still quite fast for such small memory buffers and gets us under 32K
        // -----------------------------------------------------------------------
        int max_len = lzav_compress_bound_hi( sizeof(RAM_Memory) );
        int comp_len = lzav_compress_hi( RAM_Memory, COMPRESS_BUFFER, sizeof(RAM_Memory), max_len );

        if (retVal) retVal = fwrite(&comp_len,          sizeof(comp_len),  1, handle);
        if (retVal) retVal = fwrite(COMPRESS_BUFFER,     comp_len,         1, handle);
        

        max_len = lzav_compress_bound_hi( sizeof(VDP_Memory) );
        comp_len = lzav_compress_hi( VDP_Memory, COMPRESS_BUFFER, sizeof(VDP_Memory), max_len );

        if (retVal) retVal = fwrite(&comp_len,          sizeof(comp_len),  1, handle);
        if (retVal) retVal = fwrite(COMPRESS_BUFFER,     comp_len,         1, handle);

        fclose(handle);
    }

    DSPrint(20,0, 0, "   ");
    DSPrint(20,1, 0, "   ");
    WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;
    DisplayStatusLine(true);

    restoreCompressedMem();
}


/*********************************************************************************
 * Load the current state - read everything back from the .sav file.
 ********************************************************************************/
void msxLoadState(void)
{
    preserveCompressedMem();
    //TODO
    restoreCompressedMem();
}

// End of file
