// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave and
// Marat Fayzullin (fMSX core) are thanked profusely.
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

#define MSX_SAVE_VER   0x0010  // Change this if the basic format of the .SAV file changes. Invalidates older .sav files.

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

#define TYPE_ROM        0
#define TYPE_RAM        1
#define TYPE_FDC        2
#define TYPE_BIOS       3
#define TYPE_EBIOS      4
#define TYPE_FMPAC      5
#define TYPE_SRAM       6
#define TYPE_OTHER      9

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
    u8 spare = 0;

    // Put up a cool little saving data icon...
    DSPrint(20,0, 2, "-./");
    DSPrint(20,1, 2, "MNO");

    // Re-allocate the 256K buffer needed for compression
    preserveCompressedMem();

    // Return to the original path
    chdir(GetMasterPath());

    // Ensure 'sav' directory exists
    EnsureSaveDirectory();

    // Init filename = romname and SAV in place of ROM
    sprintf(szLoadFile,"sav/%s", GetMasterFilename());

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
            u8 *shifted_offset = MemoryMap[i] + (0x2000 * i);   // Shift the MemoryMap[] back up to non-offset range for the save/restore

            if ((shifted_offset >= ROM_Memory) && (shifted_offset < ROM_Memory+MAX_CART_SIZE_KB))
            {
                Offsets[i].type = TYPE_ROM;
                Offsets[i].offset = shifted_offset - ROM_Memory;
            }
            else if ((shifted_offset >= MSXBios_DISK) && (shifted_offset < MSXBios_DISK+(sizeof(MSXBios_DISK))))
            {
                Offsets[i].type = TYPE_FDC;
                Offsets[i].offset = shifted_offset - MSXBios_DISK;
            }
            else if ((shifted_offset >= RAM_Memory) && (shifted_offset < RAM_Memory+(sizeof(RAM_Memory))))
            {
                Offsets[i].type = TYPE_RAM;
                Offsets[i].offset = shifted_offset - RAM_Memory;
            }
            else if ((shifted_offset >= SRAM_Memory) && (shifted_offset < SRAM_Memory+(sizeof(SRAM_Memory))))
            {
                Offsets[i].type = TYPE_SRAM;
                Offsets[i].offset = shifted_offset - SRAM_Memory;
            }
            else if ((shifted_offset >= BIOS_Memory) && (shifted_offset < BIOS_Memory+(sizeof(BIOS_Memory))))
            {
                Offsets[i].type = TYPE_BIOS;
                Offsets[i].offset = shifted_offset - BIOS_Memory;
            }
            else if ((shifted_offset >= MSXBios_MSX2EXT) && (shifted_offset < MSXBios_MSX2EXT+(sizeof(MSXBios_MSX2EXT))))
            {
                Offsets[i].type = TYPE_EBIOS;
                Offsets[i].offset = shifted_offset - MSXBios_MSX2EXT;
            }
            else if ((shifted_offset >= MSXBios_FMPAC) && (shifted_offset < MSXBios_FMPAC+(sizeof(MSXBios_FMPAC))))
            {
                Offsets[i].type = TYPE_FMPAC;
                Offsets[i].offset = shifted_offset - MSXBios_FMPAC;
            }
            else
            {
                Offsets[i].type = TYPE_OTHER;
                Offsets[i].offset = (u32)shifted_offset;
            }
        }
        if (retVal) retVal = fwrite(Offsets, sizeof(Offsets),1, handle);

        // We need to save off the MSX Cart offsets so we can restore them properly...
        for (u8 i=0; i<8; i++)
        {
            if ((MSXCartPtr[MEDIA_CART1][i] >= ROM_Memory) && (MSXCartPtr[MEDIA_CART1][i] < ROM_Memory+(MAX_CART_SIZE_KB * 1024)))
            {
                Offsets[i].type = TYPE_ROM;
                Offsets[i].offset = MSXCartPtr[MEDIA_CART1][i] - ROM_Memory;
            }
            else if ((MSXCartPtr[MEDIA_CART1][i] >= SRAM_Memory) && (MSXCartPtr[MEDIA_CART1][i] < SRAM_Memory+(sizeof(SRAM_Memory))))
            {
                Offsets[i].type = TYPE_SRAM;
                Offsets[i].offset = MSXCartPtr[MEDIA_CART1][i] - SRAM_Memory;
            }
            else
            {
                Offsets[i].type = TYPE_OTHER;
                Offsets[i].offset = (u32)MSXCartPtr[MEDIA_CART1][i];
            }
        }
        if (retVal) retVal = fwrite(Offsets, sizeof(Offsets),1, handle);


        // We need to save off the MSX Cart2 offsets so we can restore them properly...
        for (u8 i=0; i<8; i++)
        {
            if ((MSXCartPtr[MEDIA_CART2][i] >= ROM_Memory) && (MSXCartPtr[MEDIA_CART2][i] < ROM_Memory+(MAX_CART_SIZE_KB * 1024)))
            {
                Offsets[i].type = TYPE_ROM;
                Offsets[i].offset = MSXCartPtr[MEDIA_CART2][i] - ROM_Memory;
            }
            else if ((MSXCartPtr[MEDIA_CART2][i] >= SRAM_Memory) && (MSXCartPtr[MEDIA_CART2][i] < SRAM_Memory+(sizeof(SRAM_Memory))))
            {
                Offsets[i].type = TYPE_SRAM;
                Offsets[i].offset = MSXCartPtr[MEDIA_CART2][i] - SRAM_Memory;
            }
            else
            {
                Offsets[i].type = TYPE_OTHER;
                Offsets[i].offset = (u32)MSXCartPtr[MEDIA_CART2][i];
            }
        }
        if (retVal) retVal = fwrite(Offsets, sizeof(Offsets),1, handle);

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
        if (retVal) retVal = fwrite(Offsets, sizeof(Offsets),1, handle);

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
        if (retVal) retVal = fwrite(&ChrTabM,               sizeof(ChrTabM),                1, handle);
        if (retVal) retVal = fwrite(&ColTabM,               sizeof(ColTabM),                1, handle);
        if (retVal) retVal = fwrite(&ChrGenM,               sizeof(ChrGenM),                1, handle);
        if (retVal) retVal = fwrite(&SprTabM,               sizeof(SprTabM),                1, handle);
        if (retVal) retVal = fwrite(XPal,                   sizeof(XPal),                   1, handle);
        if (retVal) retVal = fwrite(&MyMedia,               sizeof(MyMedia),                1, handle);

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
        if (retVal) retVal = fwrite(&myAY2,                 sizeof(myAY2),                  1, handle);
        if (retVal) retVal = fwrite(&mySCC,                 sizeof(mySCC),                  1, handle);
        if (retVal) retVal = fwrite(&myYM,                  sizeof(myYM),                   1, handle);

        // Write disk controller data
        if (retVal) retVal = fwrite(&FDC,                   sizeof(FDC),                    1, handle);

        // A few frame counters
        if (retVal) retVal = fwrite(&emuActFrames,          sizeof(emuActFrames),           1, handle);
        if (retVal) retVal = fwrite(&timingFrames,          sizeof(timingFrames),           1, handle);

        // Write stuff for MSX Port I/O and Mappers
        if (retVal) retVal = fwrite(&Port_PPI_A,            sizeof(Port_PPI_A),             1, handle);
        if (retVal) retVal = fwrite(&Port_PPI_B,            sizeof(Port_PPI_B),             1, handle);
        if (retVal) retVal = fwrite(&Port_PPI_C,            sizeof(Port_PPI_C),             1, handle);
        if (retVal) retVal = fwrite(&msx_subslot,           sizeof(msx_subslot),            1, handle);
        if (retVal) retVal = fwrite(bCartInPage,            sizeof(bCartInPage),            1, handle);
        if (retVal) retVal = fwrite(bRAMInPage,             sizeof(bRAMInPage),             1, handle);
        if (retVal) retVal = fwrite(&special_memory_access, sizeof(special_memory_access),  1, handle);
        if (retVal) retVal = fwrite(&msx_scc_capable_game,  sizeof(msx_scc_capable_game),   1, handle);
        if (retVal) retVal = fwrite(&sccplus_mode,          sizeof(sccplus_mode),           1, handle);
        if (retVal) retVal = fwrite(sccplus_page,           sizeof(sccplus_page),           1, handle);
        if (retVal) retVal = fwrite(&XPalReal0,             sizeof(XPalReal0),              1, handle);
        if (retVal) retVal = fwrite(&ALatch,                sizeof(ALatch),                 1, handle);
        if (retVal) retVal = fwrite(&frame_number,          sizeof(frame_number),           1, handle);
        if (retVal) retVal = fwrite(&drawn_frame_number,    sizeof(drawn_frame_number),     1, handle);
        if (retVal) retVal = fwrite(&CurrentEpochSaved,     sizeof(CurrentEpochSaved),      1, handle);
        if (retVal) retVal = fwrite(&msx_irq_pending,       sizeof(msx_irq_pending),        1, handle);
        if (retVal) retVal = fwrite(&palette_latch,         sizeof(palette_latch),          1, handle);
        if (retVal) retVal = fwrite(&zoom_screen,           sizeof(zoom_screen),            1, handle);
        if (retVal) retVal = fwrite(&screenPanX,            sizeof(screenPanX),             1, handle);
        if (retVal) retVal = fwrite(OccBuf,                 sizeof(OccBuf),                 1, handle);
        if (retVal) retVal = fwrite(&sram_write_enabled_a,  sizeof(sram_write_enabled_a),   1, handle);
        if (retVal) retVal = fwrite(&sram_write_enabled_b,  sizeof(sram_write_enabled_b),   1, handle);
        if (retVal) retVal = fwrite(&FMPAC_SRAM_in_view,    sizeof(FMPAC_SRAM_in_view),     1, handle);        
        if (retVal) retVal = fwrite(&msx_music_capable_game,sizeof(msx_music_capable_game), 1, handle);
        if (retVal) retVal = fwrite(mirror_ram_bank,        sizeof(mirror_ram_bank),        1, handle);
        if (retVal) retVal = fwrite(&msx_caps_lock,         sizeof(msx_caps_lock),          1, handle);
        if (retVal) retVal = fwrite(&msx_kana_lock,         sizeof(msx_kana_lock),          1, handle);
        if (retVal) retVal = fwrite(&myPaddle,              sizeof(myPaddle),               1, handle);
        if (retVal) retVal = fwrite(&render_blended,        sizeof(render_blended),         1, handle);
        if (retVal) retVal = fwrite(&spare,                 sizeof(spare),                  1, handle);
        if (retVal) retVal = fwrite(&spare,                 sizeof(spare),                  1, handle);
        if (retVal) retVal = fwrite(&spare,                 sizeof(spare),                  1, handle);
        if (retVal) retVal = fwrite(&spare,                 sizeof(spare),                  1, handle);

        // --------------------------------------------------------------------------------------------
        // Compress the 256K RAM data using 'high' compression ratio... We do this in two 128K chunks.
        // --------------------------------------------------------------------------------------------
        int max_len = lzav_compress_bound( 0x20000 );
        int comp_len = lzav_compress_default( RAM_Memory, COMPRESS_BUFFER, 0x20000, max_len );
        if (retVal) retVal = fwrite(&comp_len,           sizeof(comp_len),  1, handle);
        if (retVal) retVal = fwrite(COMPRESS_BUFFER,     comp_len,          1, handle);

        // --------------------------------------------------------------------------------------------
        // Compress the 256K RAM data using 'high' compression ratio... We do this in two 128K chunks.
        // --------------------------------------------------------------------------------------------
        comp_len = lzav_compress_default( RAM_Memory+0x20000, COMPRESS_BUFFER, 0x20000, max_len );
        if (retVal) retVal = fwrite(&comp_len,           sizeof(comp_len),  1, handle);
        if (retVal) retVal = fwrite(COMPRESS_BUFFER,     comp_len,          1, handle);

        // ------------------------------------------------------------------------------------
        // If we are MEGARAM, we have a pile of RAM sitting in ROM_Memory[] that needs backing.
        // ------------------------------------------------------------------------------------
        if (myConfig.expansion == EXP_MEGARAM)
        {
            // Do this in 128K chunks... DSi has 1MB, DS-Lite has 512K
            for (int i=0; i < (isDSiMode() ? 8:4); i++)
            {
                comp_len = lzav_compress_default( ROM_Memory+((MAX_CART_SIZE_KB/2)*1024)+(i*0x20000), COMPRESS_BUFFER, 0x20000, max_len );
                if (retVal) retVal = fwrite(&comp_len,           sizeof(comp_len),  1, handle);
                if (retVal) retVal = fwrite(COMPRESS_BUFFER,     comp_len,          1, handle);
            }
        }

        // -----------------------------------------------------------------------
        // Compress the 128K VRAM data using 'high' compression ratio...
        // -----------------------------------------------------------------------
        max_len = lzav_compress_bound( sizeof(VDP_Memory) );
        comp_len = lzav_compress_default( VDP_Memory, COMPRESS_BUFFER, sizeof(VDP_Memory), max_len );
        if (retVal) retVal = fwrite(&comp_len,           sizeof(comp_len),  1, handle);
        if (retVal) retVal = fwrite(COMPRESS_BUFFER,     comp_len,          1, handle);

        fclose(handle);
    }

    restoreCompressedMem();

    DSPrint(20,0, 0, "   ");
    DSPrint(20,1, 0, "   ");
    WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;
    DisplayStatusLine();
}


/*********************************************************************************
 * Load the current state - read everything back from the .sav file.
 ********************************************************************************/
void msxLoadState(void)
{
    u32 pSvg;
    size_t retVal = 0;
    u8 spare = 0;

    // Put up a cool little restore data icon...
    DSPrint(20,0, 2, "*+,");
    DSPrint(20,1, 2, "JKL");

    // Re-allocate the 256K buffer needed for compression
    preserveCompressedMem();

    // Return to the original path
    chdir(GetMasterPath());

    // Ensure 'sav' directory exists
    EnsureSaveDirectory();

    // Init filename = romname and SAV in place of ROM
    sprintf(szLoadFile,"sav/%s", GetMasterFilename());

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

            if (retVal) retVal = fread(Offsets, sizeof(Offsets),1, handle);
            for (u8 i=0; i<8; i++)
            {
                if (Offsets[i].type == TYPE_ROM)
                {
                    MemoryMap[i] = (u8 *) (ROM_Memory + Offsets[i].offset) - (0x2000 * i);
                }
                else if (Offsets[i].type == TYPE_FDC)
                {
                    MemoryMap[i] = (u8 *) (MSXBios_DISK + Offsets[i].offset) - (0x2000 * i);
                }
                else if (Offsets[i].type == TYPE_RAM)
                {
                    MemoryMap[i] = (u8 *) (RAM_Memory + Offsets[i].offset) - (0x2000 * i);
                }
                else if (Offsets[i].type == TYPE_SRAM)
                {
                    MemoryMap[i] = (u8 *) (SRAM_Memory + Offsets[i].offset) - (0x2000 * i);
                }
                else if (Offsets[i].type == TYPE_BIOS)
                {
                    MemoryMap[i] = (u8 *) (BIOS_Memory + Offsets[i].offset) - (0x2000 * i);
                }
                else if (Offsets[i].type == TYPE_EBIOS)
                {
                    MemoryMap[i] = (u8 *) (MSXBios_MSX2EXT + Offsets[i].offset) - (0x2000 * i);
                }
                else if (Offsets[i].type == TYPE_FMPAC)
                {
                    MemoryMap[i] = (u8 *) (MSXBios_FMPAC + Offsets[i].offset) - (0x2000 * i);
                }
                else // TYPE_OTHER - this is just a pointer to memory
                {
                    MemoryMap[i] = (u8 *) (Offsets[i].offset) - (0x2000 * i);
                }
            }

            if (retVal) retVal = fread(Offsets, sizeof(Offsets),1, handle);
            for (u8 i=0; i<8; i++)
            {
                if (Offsets[i].type == TYPE_ROM)
                {
                    MSXCartPtr[MEDIA_CART1][i] = (u8 *) (ROM_Memory + Offsets[i].offset);
                }
                else if (Offsets[i].type == TYPE_SRAM)
                {
                    MSXCartPtr[MEDIA_CART1][i] = (u8 *) (SRAM_Memory + Offsets[i].offset);
                }
                else // TYPE_OTHER - this is just a pointer to memory
                {
                    MSXCartPtr[MEDIA_CART1][i] = (u8 *) (Offsets[i].offset);
                }
            }

            if (retVal) retVal = fread(Offsets, sizeof(Offsets),1, handle);
            for (u8 i=0; i<8; i++)
            {
                if (Offsets[i].type == TYPE_ROM)
                {
                    MSXCartPtr[MEDIA_CART2][i] = (u8 *) (ROM_Memory + Offsets[i].offset);
                }
                else if (Offsets[i].type == TYPE_SRAM)
                {
                    MSXCartPtr[MEDIA_CART2][i] = (u8 *) (SRAM_Memory + Offsets[i].offset);
                }
                else // TYPE_OTHER - this is just a pointer to memory
                {
                    MSXCartPtr[MEDIA_CART2][i] = (u8 *) (Offsets[i].offset);
                }
            }

            if (retVal) retVal = fread(Offsets, sizeof(Offsets),1, handle);
            for (u8 i=0; i<8; i++)
            {
                if (Offsets[i].type == TYPE_RAM)
                {
                    MSXRamPtr[i] = (u8 *) (RAM_Memory + Offsets[i].offset);
                }
                else // TYPE_OTHER - this is just a pointer to memory
                {
                    MSXRamPtr[i] = (u8 *) (Offsets[i].offset);
                }
            }

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
            if (retVal) retVal = fread(&ChrTabM,               sizeof(ChrTabM),                1, handle);
            if (retVal) retVal = fread(&ColTabM,               sizeof(ColTabM),                1, handle);
            if (retVal) retVal = fread(&ChrGenM,               sizeof(ChrGenM),                1, handle);
            if (retVal) retVal = fread(&SprTabM,               sizeof(SprTabM),                1, handle);
            if (retVal) retVal = fread(XPal,                   sizeof(XPal),                   1, handle);
            if (retVal) retVal = fread(&MyMedia,               sizeof(MyMedia),                1, handle);

            // These are pointers into VDP Memory... save them as offsets...
            if (retVal) retVal = fread(&pSvg, sizeof(pSvg),1, handle);
            ChrGen = VDP_Memory + pSvg;
            if (retVal) retVal = fread(&pSvg, sizeof(pSvg),1, handle);
            ChrTab = VDP_Memory + pSvg;
            if (retVal) retVal = fread(&pSvg, sizeof(pSvg),1, handle);
            ColTab = VDP_Memory + pSvg;
            if (retVal) retVal = fread(&pSvg, sizeof(pSvg),1, handle);
            SprGen = VDP_Memory + pSvg;
            if (retVal) retVal = fread(&pSvg, sizeof(pSvg),1, handle);
            SprTab = VDP_Memory + pSvg;

            // Write sound chip data
            if (retVal) retVal = fread(&myAY,                  sizeof(myAY),                   1, handle);
            if (retVal) retVal = fread(&myAY2,                 sizeof(myAY2),                  1, handle);
            if (retVal) retVal = fread(&mySCC,                 sizeof(mySCC),                  1, handle);
            if (retVal) retVal = fread(&myYM,                  sizeof(myYM),                   1, handle);

            // Write disk controller data
            if (retVal) retVal = fread(&FDC,                   sizeof(FDC),                    1, handle);

            // A few frame counters
            if (retVal) retVal = fread(&emuActFrames,          sizeof(emuActFrames),           1, handle);
            if (retVal) retVal = fread(&timingFrames,          sizeof(timingFrames),           1, handle);

            // Write stuff for MSX Port I/O and Mappers
            if (retVal) retVal = fread(&Port_PPI_A,            sizeof(Port_PPI_A),             1, handle);
            if (retVal) retVal = fread(&Port_PPI_B,            sizeof(Port_PPI_B),             1, handle);
            if (retVal) retVal = fread(&Port_PPI_C,            sizeof(Port_PPI_C),             1, handle);
            if (retVal) retVal = fread(&msx_subslot,           sizeof(msx_subslot),            1, handle);
            if (retVal) retVal = fread(bCartInPage,            sizeof(bCartInPage),            1, handle);
            if (retVal) retVal = fread(bRAMInPage,             sizeof(bRAMInPage),             1, handle);
            if (retVal) retVal = fread(&special_memory_access, sizeof(special_memory_access),  1, handle);
            if (retVal) retVal = fread(&msx_scc_capable_game,  sizeof(msx_scc_capable_game),   1, handle);
            if (retVal) retVal = fread(&sccplus_mode,          sizeof(sccplus_mode),           1, handle);
            if (retVal) retVal = fread(sccplus_page,           sizeof(sccplus_page),           1, handle);
            if (retVal) retVal = fread(&XPalReal0,             sizeof(XPalReal0),              1, handle);
            if (retVal) retVal = fread(&ALatch,                sizeof(ALatch),                 1, handle);
            if (retVal) retVal = fread(&frame_number,          sizeof(frame_number),           1, handle);
            if (retVal) retVal = fread(&drawn_frame_number,    sizeof(drawn_frame_number),     1, handle);
            if (retVal) retVal = fread(&CurrentEpochSaved,     sizeof(CurrentEpochSaved),      1, handle);
            if (retVal) retVal = fread(&msx_irq_pending,       sizeof(msx_irq_pending),        1, handle);
            if (retVal) retVal = fread(&palette_latch,         sizeof(palette_latch),          1, handle);
            if (retVal) retVal = fread(&zoom_screen,           sizeof(zoom_screen),            1, handle);
            if (retVal) retVal = fread(&screenPanX,            sizeof(screenPanX),             1, handle);
            if (retVal) retVal = fread(OccBuf,                 sizeof(OccBuf),                 1, handle);
            if (retVal) retVal = fread(&sram_write_enabled_a,  sizeof(sram_write_enabled_a),   1, handle);
            if (retVal) retVal = fread(&sram_write_enabled_b,  sizeof(sram_write_enabled_b),   1, handle);
            if (retVal) retVal = fread(&FMPAC_SRAM_in_view,    sizeof(FMPAC_SRAM_in_view),     1, handle);
            if (retVal) retVal = fread(&msx_music_capable_game,sizeof(msx_music_capable_game), 1, handle);
            if (retVal) retVal = fread(mirror_ram_bank,        sizeof(mirror_ram_bank),        1, handle);
            if (retVal) retVal = fread(&msx_caps_lock,         sizeof(msx_caps_lock),          1, handle);
            if (retVal) retVal = fread(&msx_kana_lock,         sizeof(msx_kana_lock),          1, handle);
            if (retVal) retVal = fread(&myPaddle,              sizeof(myPaddle),               1, handle);
            if (retVal) retVal = fread(&render_blended,        sizeof(render_blended),         1, handle);
            if (retVal) retVal = fread(&spare,                 sizeof(spare),                  1, handle);
            if (retVal) retVal = fread(&spare,                 sizeof(spare),                  1, handle);
            if (retVal) retVal = fread(&spare,                 sizeof(spare),                  1, handle);
            if (retVal) retVal = fread(&spare,                 sizeof(spare),                  1, handle);

            // -----------------------------------------------------------------------------------
            // Restore Main RAM memory which was saved in a compressed format. Two chunks of 128K.
            // -----------------------------------------------------------------------------------
            int comp_len = 0;
            if (retVal) retVal = fread(&comp_len,               sizeof(comp_len), 1, handle);
            if (retVal) retVal = fread(COMPRESS_BUFFER,         comp_len,         1, handle);
            (void)lzav_decompress( COMPRESS_BUFFER, RAM_Memory, comp_len, 0x20000);

            if (retVal) retVal = fread(&comp_len,               sizeof(comp_len), 1, handle);
            if (retVal) retVal = fread(COMPRESS_BUFFER,         comp_len,         1, handle);
            (void)lzav_decompress( COMPRESS_BUFFER, RAM_Memory+0x20000, comp_len, 0x20000);

            // --------------------------------------------------------------------------------------
            // If we are MEGARAM, we have a pile of RAM sitting in ROM_Memory[] that needs restoring.
            // --------------------------------------------------------------------------------------
            if (myConfig.expansion == EXP_MEGARAM)
            {
                // Do this in 128K chunks... DSi has 1MB, DS-Lite has 512K
                for (int i=0; i < (isDSiMode() ? 8:4); i++)
                {
                    if (retVal) retVal = fread(&comp_len,               sizeof(comp_len), 1, handle);
                    if (retVal) retVal = fread(COMPRESS_BUFFER,         comp_len,         1, handle);
                    (void)lzav_decompress( COMPRESS_BUFFER, ROM_Memory+((MAX_CART_SIZE_KB*2)*1024)+(i*0x20000), comp_len, 0x20000);
                }
            }

            // -----------------------------------------------------------------------
            // Restore VDP RAM memory which was saved in a compressed format
            // -----------------------------------------------------------------------
            if (retVal) retVal = fread(&comp_len,               sizeof(comp_len), 1, handle);
            if (retVal) retVal = fread(COMPRESS_BUFFER,         comp_len,         1, handle);
            (void)lzav_decompress( COMPRESS_BUFFER, VDP_Memory, comp_len, sizeof(VDP_Memory));
        }

        fclose(handle);
    }

    // Recalculate a few things on the VDP side...
    VPAGE=VDP_Memory+((int)VDP[14]<<14);
    CheckNewMode(); // This will rebuild any lookup tables as needed
    
    // If there was a disk inserted, re-insert the same one
    if (MyMedia[MEDIA_DISK].filecrc)
    {
        chdir(MyMedia[MEDIA_DISK].filepath);  // Get into the right directory
        MyMedia[MEDIA_DISK].filesize = ReadFileCarefully(MyMedia[MEDIA_DISK].currdisk, DISK_Memory, (MAX_DISK_SIZE_KB * 1024), 0, NULL);
        fdc_init(1, (MyMedia[MEDIA_DISK].filesize/1024 == 360) ? 1:2, 80, 9, 512, 1, DISK_Memory, NULL);
        fdc_reset(false);
    }

    restoreCompressedMem();

    DSPrint(20,0, 0, "   ");
    DSPrint(20,1, 0, "   ");
    WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;
    DisplayStatusLine();
}

// End of file
