// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave (Phoenix-Edition),
// Alekmaul (original port) and Marat Fayzullin (fMSX core) are thanked profusely.
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
#define TYPE_SCC   2
#define TYPE_FDC   3
#define TYPE_BIOS  4
#define TYPE_EBIOS 5
#define TYPE_SRAM  6
#define TYPE_OTHER 9

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
    u32 pSvg;

    // Put up a cool little saving data icon...
    DSPrint(20,0, 2, "-./");
    DSPrint(20,1, 2, "MNO");

    // Re-allocate the 256K buffer needed for compression
    preserveCompressedMem();

    // Return to the original path
    chdir(initial_path);

    // Init filename = romname and SAV in place of ROM
    DIR* dir = opendir("sav");
    if (dir) closedir(dir);    // Directory exists... close it out and move on.
    else mkdir("sav", 0777);   // Otherwise create the directory...
    sprintf(szLoadFile,"sav/%s", initial_file);

    // Replace the original filename extension with .sav
    int len = strlen(szLoadFile);
    szLoadFile[len-3] = 's';
    szLoadFile[len-2] = 'a';
    szLoadFile[len-1] = 'v';

    // Open the file and let's get writing!
    FILE *handle = fopen(szLoadFile, "wb+");
    if (handle != NULL)
    {
        // Write Version
        u16 save_ver = MSX_SAVE_VER;
        retVal = fwrite(&save_ver, sizeof(u16), 1, handle);

        // Write CZ80 CPU
        retVal = fwrite(&CPU, sizeof(CPU), 1, handle);

        // Save the Memory Map - we must only save offsets so that this is generic when we change code and memory shifts...
        for (u8 i=0; i<8; i++)
        {
            if ((MemoryMap[i] >= ROM_Memory) && (MemoryMap[i] < ROM_Memory+MAX_CART_SIZE))
            {
                Offsets[i].type = TYPE_ROM;
                Offsets[i].offset = MemoryMap[i] - ROM_Memory;
            }
            else if ((MemoryMap[i] >= fastdrom_cdx2) && (MemoryMap[i] < fastdrom_cdx2+(sizeof(fastdrom_cdx2))))
            {
                Offsets[i].type = TYPE_FDC;
                Offsets[i].offset = MemoryMap[i] - fastdrom_cdx2;
            }
            else if ((MemoryMap[i] >= RAM_Memory) && (MemoryMap[i] < RAM_Memory+(sizeof(RAM_Memory))))
            {
                Offsets[i].type = TYPE_RAM;
                Offsets[i].offset = MemoryMap[i] - RAM_Memory;
            }
            else if ((MemoryMap[i] >= SRAM_Memory) && (MemoryMap[i] < SRAM_Memory+(sizeof(SRAM_Memory))))
            {
                Offsets[i].type = TYPE_SRAM;
                Offsets[i].offset = MemoryMap[i] - SRAM_Memory;
            }
            else if ((MemoryMap[i] >= BIOS_Memory) && (MemoryMap[i] < BIOS_Memory+(sizeof(BIOS_Memory))))
            {
                Offsets[i].type = TYPE_BIOS;
                Offsets[i].offset = MemoryMap[i] - BIOS_Memory;
            }
            else if ((MemoryMap[i] >= MSXBios_MSX2EXT) && (MemoryMap[i] < MSXBios_MSX2EXT+(sizeof(MSXBios_MSX2EXT))))
            {
                Offsets[i].type = TYPE_EBIOS;
                Offsets[i].offset = MemoryMap[i] - MSXBios_MSX2EXT;
            }
            else
            {
                Offsets[i].type = TYPE_OTHER;
                Offsets[i].offset = (u32)MemoryMap[i];
            }
        }
        //if (retVal) retVal = fwrite(Offsets, sizeof(Offsets),1, handle);
        if (retVal) retVal = fwrite(MemoryMap, sizeof(MemoryMap),1, handle);

        // We need to save off the MSX Cart offsets so we can restore them properly...
        for (u8 i=0; i<8; i++)
        {
            if ((MSXCartPtr[i] >= ROM_Memory) && (MSXCartPtr[i] < ROM_Memory+(sizeof(ROM_Memory))))
            {
                Offsets[i].type = TYPE_ROM;
                Offsets[i].offset = MSXCartPtr[i] - ROM_Memory;
            }
            else
            {
                Offsets[i].type = TYPE_OTHER;
                Offsets[i].offset = (u32)MSXCartPtr[i];
            }
        }
        //if (retVal) retVal = fwrite(Offsets, sizeof(Offsets),1, handle);
        if (retVal) retVal = fwrite(MSXCartPtr, sizeof(MSXCartPtr),1, handle);

        // We need to save off the MSX RAM offsets so we can restore them properly...
        for (u8 i=0; i<8; i++)
        {
            if ((MSXRamPtr[i] >= RAM_Memory) && (MSXRamPtr[i] < RAM_Memory+(sizeof(RAM_Memory))))
            {
                Offsets[i].type = TYPE_RAM;
                Offsets[i].offset = MSXRamPtr[i] - RAM_Memory;
            }
            else
            {
                Offsets[i].type = TYPE_OTHER;
                Offsets[i].offset = (u32)MSXRamPtr[i];
            }
        }
        //if (retVal) retVal = fwrite(Offsets, sizeof(Offsets),1, handle);
        if (retVal) retVal = fwrite(MSXRamPtr, sizeof(MSXRamPtr),1, handle);

        // Write VDP
        if (retVal) retVal = fwrite(VDP,                    sizeof(VDP),                    1, handle);
        if (retVal) retVal = fwrite(VDPStatus,              sizeof(VDPStatus),              1, handle);
        if (retVal) retVal = fwrite(&VDPCtrlLatch,          sizeof(VDPCtrlLatch),           1, handle);
        if (retVal) retVal = fwrite(&VDPStatus,             sizeof(VDPStatus),              1, handle);
        if (retVal) retVal = fwrite(&FGColor,               sizeof(FGColor),                1, handle);
        if (retVal) retVal = fwrite(&BGColor,               sizeof(BGColor),                1, handle);
        if (retVal) retVal = fwrite(&OH,                    sizeof(OH),                     1, handle);
        if (retVal) retVal = fwrite(&IH,                    sizeof(IH),                     1, handle);
        if (retVal) retVal = fwrite(&ScrMode,               sizeof(ScrMode),                1, handle);
        if (retVal) retVal = fwrite(&VDPDlatch,             sizeof(VDPDlatch),              1, handle);
        if (retVal) retVal = fwrite(&VAddr,                 sizeof(VAddr),                  1, handle);
        if (retVal) retVal = fwrite(&CurLine,               sizeof(CurLine),                1, handle);
        if (retVal) retVal = fwrite(&ColTabM,               sizeof(ColTabM),                1, handle);
        if (retVal) retVal = fwrite(&ChrGenM,               sizeof(ChrGenM),                1, handle);
        if (retVal) retVal = fwrite(XPal,                   sizeof(XPal),                   1, handle);

        // These are pointers into VDP Memory... save them as offsets...
        pSvg = ChrGen-VDP_Memory;
        if (retVal) retVal = fwrite(&pSvg, sizeof(pSvg),1, handle);
        pSvg = ChrTab-VDP_Memory;
        if (retVal) retVal = fwrite(&pSvg, sizeof(pSvg),1, handle);
        pSvg = ColTab-VDP_Memory;
        if (retVal) retVal = fwrite(&pSvg, sizeof(pSvg),1, handle);
        pSvg = SprGen-VDP_Memory;
        if (retVal) retVal = fwrite(&pSvg, sizeof(pSvg),1, handle);
        pSvg = SprTab-VDP_Memory;
        if (retVal) retVal = fwrite(&pSvg, sizeof(pSvg),1, handle);        

        // Write sound chip data
        if (retVal) retVal = fwrite(&myAY,                  sizeof(myAY),                   1, handle);
        if (retVal) retVal = fwrite(&mySCC,                 sizeof(mySCC),                  1, handle);

        // Write disk controller data
        if (retVal) retVal = fwrite(&FDC,                   sizeof(FDC),                    1, handle);

        // A few frame counters
        if (retVal) retVal = fwrite(&emuActFrames,          sizeof(emuActFrames),           1, handle);
        if (retVal) retVal = fwrite(&timingFrames,          sizeof(timingFrames),           1, handle);

        // Write stuff for MSX Port I/O and Mappers
        if (retVal) retVal = fwrite(&Port_PPI_A,            sizeof(Port_PPI_A),             1, handle);
        if (retVal) retVal = fwrite(&Port_PPI_B,            sizeof(Port_PPI_B),             1, handle);
        if (retVal) retVal = fwrite(&Port_PPI_C,            sizeof(Port_PPI_C),             1, handle);
        if (retVal) retVal = fwrite(&mapperType,            sizeof(mapperType),             1, handle);
        if (retVal) retVal = fwrite(&mapperMask,            sizeof(mapperMask),             1, handle);
        if (retVal) retVal = fwrite(&msx_subslot,           sizeof(msx_subslot),            1, handle);
        if (retVal) retVal = fwrite(bCartInPage,            sizeof(bCartInPage),            1, handle);
        if (retVal) retVal = fwrite(bRAMInPage,             sizeof(bRAMInPage),             1, handle);
        if (retVal) retVal = fwrite(&msx_last_file_size,    sizeof(msx_last_file_size),     1, handle);
        if (retVal) retVal = fwrite(&special_ram_access,    sizeof(special_ram_access),     1, handle);
        if (retVal) retVal = fwrite(&msx_sram_enabled,      sizeof(msx_sram_enabled),       1, handle);
        if (retVal) retVal = fwrite(&msx_scc_capable_game,  sizeof(msx_scc_capable_game),   1, handle);
        if (retVal) retVal = fwrite(&sccplus_mode,          sizeof(sccplus_mode),           1, handle);
        if (retVal) retVal = fwrite(sccplus_page,           sizeof(sccplus_page),           1, handle);
        if (retVal) retVal = fwrite(&XPalReal0,             sizeof(XPalReal0),              1, handle);
        if (retVal) retVal = fwrite(&ALatch,                sizeof(ALatch),                 1, handle);
        if (retVal) retVal = fwrite(&frame_number,          sizeof(frame_number),           1, handle);
        if (retVal) retVal = fwrite(&CurrentEpochSaved,     sizeof(CurrentEpochSaved),      1, handle);
        if (retVal) retVal = fwrite(&msx_irq_pending,       sizeof(msx_irq_pending),        1, handle);
        if (retVal) retVal = fwrite(&palette_latch,         sizeof(palette_latch),          1, handle);
        if (retVal) retVal = fwrite(OccBuf,                 sizeof(OccBuf),                 1, handle);
        if (retVal) retVal = fwrite(nibbleLUT16,            sizeof(nibbleLUT16),            1, handle);
        if (retVal) retVal = fwrite(screen7LUT,             sizeof(screen7LUT),             1, handle);

        // -----------------------------------------------------------------------
        // Compress the 128K RAM data using 'high' compression ratio...
        // -----------------------------------------------------------------------
        int max_len = lzav_compress_bound_hi( sizeof(RAM_Memory) );
        int comp_len = lzav_compress_hi( RAM_Memory, COMPRESS_BUFFER, sizeof(RAM_Memory), max_len );

        if (retVal) retVal = fwrite(&comp_len,           sizeof(comp_len),  1, handle);
        if (retVal) retVal = fwrite(COMPRESS_BUFFER,     comp_len,          1, handle);

        // -----------------------------------------------------------------------
        // Compress the 128K VRAM data using 'high' compression ratio...
        // -----------------------------------------------------------------------
        max_len = lzav_compress_bound_hi( sizeof(VDP_Memory) );
        comp_len = lzav_compress_hi( VDP_Memory, COMPRESS_BUFFER, sizeof(VDP_Memory), max_len );

        if (retVal) retVal = fwrite(&comp_len,           sizeof(comp_len),  1, handle);
        if (retVal) retVal = fwrite(COMPRESS_BUFFER,     comp_len,          1, handle);

        // -----------------------------------------------------------------------
        // Compress the 64K SRAM data using 'high' compression ratio...
        // -----------------------------------------------------------------------
        max_len = lzav_compress_bound_hi( sizeof(SRAM_Memory) );
        comp_len = lzav_compress_hi( SRAM_Memory, COMPRESS_BUFFER, sizeof(SRAM_Memory), max_len );

        if (retVal) retVal = fwrite(&comp_len,           sizeof(comp_len),  1, handle);
        if (retVal) retVal = fwrite(COMPRESS_BUFFER,     comp_len,          1, handle);

        fclose(handle);
    }
    
    restoreCompressedMem();
    
    DSPrint(20,0, 0, "   ");
    DSPrint(20,1, 0, "   ");
    WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;
    DisplayStatusLine(true);
}


/*********************************************************************************
 * Load the current state - read everything back from the .sav file.
 ********************************************************************************/
void msxLoadState(void)
{
    u32 pSvg;
    size_t retVal = 0;

    // Put up a cool little restore data icon...
    DSPrint(20,0, 2, "*+,");
    DSPrint(20,1, 2, "JKL");

    // Re-allocate the 256K buffer needed for compression
    preserveCompressedMem();

    // Return to the original path
    chdir(initial_path);

    // Init filename = romname and SAV in place of ROM
    DIR* dir = opendir("sav");
    if (dir) closedir(dir);    // Directory exists... close it out and move on.
    else mkdir("sav", 0777);   // Otherwise create the directory...
    sprintf(szLoadFile,"sav/%s", initial_file);

    // Replace the original filename extension with .sav
    int len = strlen(szLoadFile);
    szLoadFile[len-3] = 's';
    szLoadFile[len-2] = 'a';
    szLoadFile[len-1] = 'v';

    // Open the file and read out all the data...
    FILE *handle = fopen(szLoadFile, "rb");
    if (handle != NULL)
    {
        // Read Version
        u16 save_ver = 0xBEEF;
        retVal = fread(&save_ver, sizeof(u16), 1, handle);

        if (save_ver == MSX_SAVE_VER)
        {
            // Write CZ80 CPU
            retVal = fread(&CPU, sizeof(CPU), 1, handle);

            if (retVal) retVal = fread(MemoryMap, sizeof(MemoryMap),1, handle);

            if (retVal) retVal = fread(MSXCartPtr, sizeof(MSXCartPtr),1, handle);

            if (retVal) retVal = fread(MSXRamPtr, sizeof(MSXRamPtr),1, handle);

            // Write VDP
            if (retVal) retVal = fread(VDP,                    sizeof(VDP),                    1, handle);
            if (retVal) retVal = fread(VDPStatus,              sizeof(VDPStatus),              1, handle);
            if (retVal) retVal = fread(&VDPCtrlLatch,          sizeof(VDPCtrlLatch),           1, handle);
            if (retVal) retVal = fread(&VDPStatus,             sizeof(VDPStatus),              1, handle);
            if (retVal) retVal = fread(&FGColor,               sizeof(FGColor),                1, handle);
            if (retVal) retVal = fread(&BGColor,               sizeof(BGColor),                1, handle);
            if (retVal) retVal = fread(&OH,                    sizeof(OH),                     1, handle);
            if (retVal) retVal = fread(&IH,                    sizeof(IH),                     1, handle);
            if (retVal) retVal = fread(&ScrMode,               sizeof(ScrMode),                1, handle);
            if (retVal) retVal = fread(&VDPDlatch,             sizeof(VDPDlatch),              1, handle);
            if (retVal) retVal = fread(&VAddr,                 sizeof(VAddr),                  1, handle);
            if (retVal) retVal = fread(&CurLine,               sizeof(CurLine),                1, handle);
            if (retVal) retVal = fread(&ColTabM,               sizeof(ColTabM),                1, handle);
            if (retVal) retVal = fread(&ChrGenM,               sizeof(ChrGenM),                1, handle);
            if (retVal) retVal = fread(XPal,                   sizeof(XPal),                   1, handle);

            // These are pointers into VDP Memory... save them as offsets...
            if (retVal) retVal = fread(&pSvg, sizeof(pSvg),1, handle);
            ChrGen = pSvg + VDP_Memory;
            if (retVal) retVal = fread(&pSvg, sizeof(pSvg),1, handle);
            ChrTab = pSvg + VDP_Memory;
            if (retVal) retVal = fread(&pSvg, sizeof(pSvg),1, handle);
            ColTab = pSvg + VDP_Memory;
            if (retVal) retVal = fread(&pSvg, sizeof(pSvg),1, handle);
            SprGen = pSvg + VDP_Memory;
            if (retVal) retVal = fread(&pSvg, sizeof(pSvg),1, handle);
            SprTab = pSvg + VDP_Memory;

            // Write sound chip data
            if (retVal) retVal = fread(&myAY,                  sizeof(myAY),                   1, handle);
            if (retVal) retVal = fread(&mySCC,                 sizeof(mySCC),                  1, handle);

            // Write disk controller data
            if (retVal) retVal = fread(&FDC,                   sizeof(FDC),                    1, handle);

            // A few frame counters
            if (retVal) retVal = fread(&emuActFrames,          sizeof(emuActFrames),           1, handle);
            if (retVal) retVal = fread(&timingFrames,          sizeof(timingFrames),           1, handle);

            // Write stuff for MSX Port I/O and Mappers
            if (retVal) retVal = fread(&Port_PPI_A,            sizeof(Port_PPI_A),             1, handle);
            if (retVal) retVal = fread(&Port_PPI_B,            sizeof(Port_PPI_B),             1, handle);
            if (retVal) retVal = fread(&Port_PPI_C,            sizeof(Port_PPI_C),             1, handle);
            if (retVal) retVal = fread(&mapperType,            sizeof(mapperType),             1, handle);
            if (retVal) retVal = fread(&mapperMask,            sizeof(mapperMask),             1, handle);
            if (retVal) retVal = fread(&msx_subslot,           sizeof(msx_subslot),            1, handle);
            if (retVal) retVal = fread(bCartInPage,            sizeof(bCartInPage),            1, handle);
            if (retVal) retVal = fread(bRAMInPage,             sizeof(bRAMInPage),             1, handle);
            if (retVal) retVal = fread(&msx_last_file_size,    sizeof(msx_last_file_size),     1, handle);
            if (retVal) retVal = fread(&special_ram_access,    sizeof(special_ram_access),     1, handle);
            if (retVal) retVal = fread(&msx_sram_enabled,      sizeof(msx_sram_enabled),       1, handle);
            if (retVal) retVal = fread(&msx_scc_capable_game,  sizeof(msx_scc_capable_game),   1, handle);
            if (retVal) retVal = fread(&sccplus_mode,          sizeof(sccplus_mode),           1, handle);
            if (retVal) retVal = fread(sccplus_page,           sizeof(sccplus_page),           1, handle);
            if (retVal) retVal = fread(&XPalReal0,             sizeof(XPalReal0),              1, handle);
            if (retVal) retVal = fread(&ALatch,                sizeof(ALatch),                 1, handle);
            if (retVal) retVal = fread(&frame_number,          sizeof(frame_number),           1, handle);
            if (retVal) retVal = fread(&CurrentEpochSaved,     sizeof(CurrentEpochSaved),      1, handle);
            if (retVal) retVal = fread(&msx_irq_pending,       sizeof(msx_irq_pending),        1, handle);
            if (retVal) retVal = fread(&palette_latch,         sizeof(palette_latch),          1, handle);
            if (retVal) retVal = fread(OccBuf,                 sizeof(OccBuf),                 1, handle);
            if (retVal) retVal = fread(nibbleLUT16,            sizeof(nibbleLUT16),            1, handle);
            if (retVal) retVal = fread(screen7LUT,             sizeof(screen7LUT),             1, handle);

            // -----------------------------------------------------------------------
            // Restore Main RAM memory which was saved in a compressed format
            // -----------------------------------------------------------------------
            int comp_len = 0;
            if (retVal) retVal = fread(&comp_len,               sizeof(comp_len), 1, handle);
            if (retVal) retVal = fread(COMPRESS_BUFFER,         comp_len,         1, handle);
            (void)lzav_decompress( COMPRESS_BUFFER, RAM_Memory, comp_len, sizeof(RAM_Memory));

            // -----------------------------------------------------------------------
            // Restore VDP RAM memory which was saved in a compressed format
            // -----------------------------------------------------------------------
            if (retVal) retVal = fread(&comp_len,               sizeof(comp_len), 1, handle);
            if (retVal) retVal = fread(COMPRESS_BUFFER,         comp_len,         1, handle);
            (void)lzav_decompress( COMPRESS_BUFFER, VDP_Memory, comp_len, sizeof(VDP_Memory));

            // -----------------------------------------------------------------------
            // Restore SRAM memory which was saved in a compressed format
            // -----------------------------------------------------------------------
            if (retVal) retVal = fread(&comp_len,                sizeof(comp_len), 1, handle);
            if (retVal) retVal = fread(COMPRESS_BUFFER,          comp_len,         1, handle);
            (void)lzav_decompress( COMPRESS_BUFFER, SRAM_Memory, comp_len, sizeof(SRAM_Memory));
        }

        fclose(handle);
    }

    // Recalculate a few things...
    VPAGE=VDP_Memory+((int)VDP[14]<<14);
    RebuildLutTablehh();

    restoreCompressedMem();

    DSPrint(20,0, 0, "   ");
    DSPrint(20,1, 0, "   ");
    WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;
    DisplayStatusLine(true);
}

// End of file
