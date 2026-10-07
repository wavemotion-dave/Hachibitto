// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave and
// Marat Fayzullin (fMSX core) are thanked profusely.
//
// The Hachibitto emulator is offered as-is, without any warranty. Please see readme.md
//
// This file is our bridge between the Z80 CPU core and the rest of the system.
// Hachibitto currently supports the CZ80 CPU core.
// =====================================================================================
#include <nds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Z80_interface.h"
#include "../../Hachibitto.h"
#include "../../MSX_generic.h"
#include "../../fdc.h"
#include "../../printf.h"
#include "../scc/SCC.h"

u8 sram_write_enabled_a = 0; // Is SRAM writable in segment A (many SRAM games allow writes in 2 segments)
u8 sram_write_enabled_b = 0; // Is SRAM writable in segment B (many SRAM games allow writes in 2 segments)
u8 FMPAC_SRAM_in_view   = 0; // Is the FMPAC SRAM in view?
u8 FMPAC_Key1           = 0; // FM-PAC SRAM requires a special key write sequence to open up SRAM access

// -----------------------------------------------------------------------------------------------
// All memory fetches run through this except OP codes which are read directly from memory
// for maximum speed (an Op Code fetch should never result in accessing special memory registers).
// -----------------------------------------------------------------------------------------------
ITCM_CODE u8 cpu_readmem16(u16 address)
{
    // ----------------------------------------------------
    // Are we reading from the SCC chip memory mapped area?
    // ----------------------------------------------------
    if ((special_memory_access & SPEC_MEM_SCC_ENABLED) && ((address & 0xF800) == 0x9800))
    {
        if (bCartInPage[2])
        {
            u16 off = address & 0xFF;
            if (off < 0x80) return SCCRead(off, &mySCC);
            else if (off < 0x90) return SCCRead(off + 0x20, &mySCC);
        }
    }
    else if ((special_memory_access & SPEC_MEM_DISK_CONTROLLER) && (address >= 0x7FF8) && (address <= 0x7FFF))
    {
        if (address == 0x7FFF) return fdc_read(4);
        else return fdc_read(address & 7);
    }
    else if ((special_memory_access & SPEC_MEM_SCC_PLUS_ENABLED) && (address >= 0xB800) && (address <= 0xBFFD))
    {
        if (bCartInPage[2]) return SCCRead(address, &mySCC);    // If the cart is mapped in, return value from it
    }
    else if ((special_memory_access & SPEC_MEM_SUBSLOT_ACTIVE) && (address == 0xFFFF)) // Subslot check... only when page 3 is mapped to an expanded slot
    {
        return (u8)~msx_subslot; // Compliment is returned. MSX1 map will never set SPEC_MEM_SUBSLOT_ACTIVE.
    }

    // Otherwise normal read - just index into the 8K memory block and fetch the byte...
    return (MemoryMap[address>>13][address]);
}


// -----------------------------------------------------------------------
// Zemina 8K mapper:
// Page (8kB)    Switching address   Initial segment
// 4000h~5FFFh (mirror: C000h~DFFFh) 4000h (mirrors: 4001h~5FFFh)    0
// 6000h~7FFFh (mirror: E000h~FFFFh) 6000h (mirrors: 6001h~7FFFh)    1
// 8000h~9FFFh (mirror: 0000h~1FFFh) 8000h (mirrors: 8001h~9FFFh)    2
// A000h~BFFFh (mirror: 2000h~3FFFh) A000h (mirrors: A001h~BFFFh)    3
// -----------------------------------------------------------------------
void HandleZemina8K(u32* src, u8 block, u16 address)
{
    u8 media_id = bCartInPage[address>>14] - 1;
    
    if ((address >= 0x4000) && (address < 0x6000))
    {
        MSXCartPtr[media_id][2] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][6] = (u8*)src;  // Mirror
        MemoryMap[2] = (u8 *)(MSXCartPtr[media_id][2]) - 0x4000;
    }
    else if ((address >= 0x6000) && (address < 0x8000))
    {
        MSXCartPtr[media_id][3] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][7] = (u8*)src;  // Mirror
        MemoryMap[3] = (u8 *)(MSXCartPtr[media_id][3]) - 0x6000;
    }
    else if ((address >= 0x8000) && (address < 0xA000))
    {
        MSXCartPtr[media_id][4] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][0] = (u8*)src;  // Mirror
        MemoryMap[4] = (u8 *)(MSXCartPtr[media_id][4]) - 0x8000;
    }
    else if ((address >= 0xA000) && (address < 0xC000))
    {
        MSXCartPtr[media_id][5] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][1] = (u8*)src;  // Mirror
        MemoryMap[5] = (u8 *)(MSXCartPtr[media_id][5]) - 0xA000;
    }
}

// -------------------------------------------------------------------------
// The ZENMIA 16K Mapper:
// 4000h~7FFFh  via writes to 4000h-7FFF
// 8000h~BFFFh  via writes to 8000h-BFFF
// -------------------------------------------------------------------------
void HandleZemina16K(u32* src, u8 block, u16 address)
{
    u8 media_id = bCartInPage[address>>14] - 1;
    
    if (bCartInPage[1] && (address >= 0x4000) && (address < 0x8000))
    {
        MSXCartPtr[media_id][2] = (u8*)src;
        MSXCartPtr[media_id][3] = (u8*)src+0x2000;
        MemoryMap[2] = (u8 *)(MSXCartPtr[media_id][2]) - 0x4000;
        MemoryMap[3] = (u8 *)(MSXCartPtr[media_id][3]) - 0x6000;
        // Mirrors
        MSXCartPtr[media_id][6] = (u8*)src;
        MSXCartPtr[media_id][7] = (u8*)src+0x2000;
        if (bCartInPage[3] == (media_id+1))
        {
            MemoryMap[6] = (u8 *)(MSXCartPtr[media_id][6]) - 0xC000;
            MemoryMap[7] = (u8 *)(MSXCartPtr[media_id][7]) - 0xE000;
        }
    }
    else if (bCartInPage[1] && (address >= 0x8000) && (address < 0xC000))
    {
        MSXCartPtr[media_id][4] = (u8*)src;
        MSXCartPtr[media_id][5] = (u8*)src+0x2000;
        // Mirrors
        MSXCartPtr[media_id][0] = (u8*)src;
        MSXCartPtr[media_id][1] = (u8*)src+0x2000;
        if (bCartInPage[2] == (media_id+1))
        {
            MemoryMap[4] = (u8 *)(MSXCartPtr[media_id][4]) - 0x8000;
            MemoryMap[5] = (u8 *)(MSXCartPtr[media_id][5]) - 0xA000;
        }
        if (bCartInPage[0])
        {
            MemoryMap[0] = (u8 *)(MSXCartPtr[media_id][0]) - 0x0000;
            MemoryMap[1] = (u8 *)(MSXCartPtr[media_id][1]) - 0x2000;
        }
    }
}

#define SRAM_ENABLE_BIT     (MyMedia[media_id].mapperMask+1)      // SRAM Enable is the bit right after the rom selection bits...

void HandleAscii8_SRAM2(u32* src, u8 block, u16 address, u8 value)
{
    u8 media_id = bCartInPage[address>>14] - 1;
    
    if ((address & 0xF800) == 0x6000)
    {
        MSXCartPtr[media_id][2] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][6] = (u8*)src;  // Mirror
        MemoryMap[2] = MSXCartPtr[media_id][2] - 0x4000;
        if (bCartInPage[3])
        {
            MemoryMap[6] = MSXCartPtr[media_id][6] - 0xC000;
        }
    }
    else if ((address & 0xF800) == 0x6800)
    {
        MSXCartPtr[media_id][3] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][7] = (u8*)src;  // Mirror
        MemoryMap[3] = MSXCartPtr[media_id][3] - 0x6000;
        if (bCartInPage[3] == (media_id+1))
        {
            MemoryMap[7] = MSXCartPtr[media_id][7] - 0xE000;
        }
    }
    else if ((address & 0xF800) == 0x7000)
    {
        if (value & SRAM_ENABLE_BIT) // Is SRAM enabled?
        {
            sram_write_enabled_a = 1;
            MSXCartPtr[media_id][4] = (u8*)SRAM_Memory+0x0000;
        }
        else // SRAM disabled, normal ROM banking
        {
            sram_write_enabled_a = 0;
            MSXCartPtr[media_id][4] = (u8*)src;  // Main ROM
            MSXCartPtr[media_id][0] = (u8*)src;  // Mirror
        }

        if (bCartInPage[2] == (media_id+1))
        {
            MemoryMap[4] = MSXCartPtr[media_id][4] - 0x8000;
        }
        if (bCartInPage[0] == (media_id+1))
        {
            MemoryMap[0] = MSXCartPtr[media_id][0] - 0x0000;
        }
    }
    else if ((address & 0xF800) == 0x7800)
    {
        if (value & SRAM_ENABLE_BIT) // Is SRAM enabled?
        {
            sram_write_enabled_b = 1;
            MSXCartPtr[media_id][5] = (u8*)SRAM_Memory+0x0000;
        }
        else // SRAM disabled, normal ROM banking
        {
            sram_write_enabled_b = 0;
            MSXCartPtr[media_id][5] = (u8*)src;  // Main ROM
            MSXCartPtr[media_id][1] = (u8*)src;  // Mirror
        }

        if (bCartInPage[2] == (media_id+1))
        {
            MemoryMap[5] = MSXCartPtr[media_id][5] - 0xA000;
        }
        if (bCartInPage[0] == (media_id+1))
        {
            MemoryMap[1] = MSXCartPtr[media_id][1] - 0x2000;
        }
    }
    else if (((address & 0xF000) == 0x8000) && sram_write_enabled_a)
    {
        // We are writing to SRAM! Write all the mirrors...
        SRAM_Memory[(address & 0x7FF) + 0x0000] = value;
        SRAM_Memory[(address & 0x7FF) + 0x0800] = value;
        SRAM_Memory[(address & 0x7FF) + 0x1000] = value;
        SRAM_Memory[(address & 0x7FF) + 0x1800] = value;
        sram_show_status = 6;
    }
    else if (((address & 0xF000) == 0xA000) && sram_write_enabled_b)
    {
        // We are writing to SRAM! Write all the mirrors...
        SRAM_Memory[(address & 0x7FF) + 0x0000] = value;
        SRAM_Memory[(address & 0x7FF) + 0x0800] = value;
        SRAM_Memory[(address & 0x7FF) + 0x1000] = value;
        SRAM_Memory[(address & 0x7FF) + 0x1800] = value;
        sram_show_status = 6;
    }
}

void HandleAscii8_SRAM8(u32* src, u8 block, u16 address, u8 value)
{
    u8 media_id = bCartInPage[address>>14] - 1;
    
    if ((address & 0xF800) == 0x6000)
    {
        if (value & SRAM_ENABLE_BIT) // Is SRAM enabled?
        {
            // No write support here...
            MSXCartPtr[media_id][2] = (u8*)SRAM_Memory+0x0000; // Map 8K into 0x8000 view
        }
        else // SRAM disabled, normal ROM banking
        {
            MSXCartPtr[media_id][2] = (u8*)src;  // Main ROM
            MSXCartPtr[media_id][6] = (u8*)src;  // Mirror
        }

        MemoryMap[2] = MSXCartPtr[media_id][2] - 0x4000;
        if (bCartInPage[3] == (media_id+1))
        {
            MemoryMap[6] = MSXCartPtr[media_id][6] - 0xC000;
        }
    }
    else if ((address & 0xF800) == 0x6800)
    {
        if (value & SRAM_ENABLE_BIT) // Is SRAM enabled?
        {
            // No write support here...
            MSXCartPtr[media_id][3] = (u8*)SRAM_Memory+0x0000; // Map 8K into 0x8000 view
        }
        else
        {
            MSXCartPtr[media_id][3] = (u8*)src;  // Main ROM
            MSXCartPtr[media_id][7] = (u8*)src;  // Mirror
        }

        MemoryMap[3] = MSXCartPtr[media_id][3] - 0x6000;
        if (bCartInPage[3] == (media_id+1))
        {
            MemoryMap[7] = MSXCartPtr[media_id][7] - 0xE000;
        }
    }
    else if ((address & 0xF800) == 0x7000)
    {
        if (value & SRAM_ENABLE_BIT) // Is SRAM enabled?
        {
            sram_write_enabled_a = 1;
            MSXCartPtr[media_id][4] = (u8*)SRAM_Memory+0x0000; // Map 8K into 0x8000 view
        }
        else // SRAM disabled, normal ROM banking
        {
            sram_write_enabled_a = 0;
            MSXCartPtr[media_id][4] = (u8*)src;  // Main ROM
            MSXCartPtr[media_id][0] = (u8*)src;  // Mirror
        }

        if (bCartInPage[2] == (media_id+1))
        {
            MemoryMap[4] = MSXCartPtr[media_id][4] - 0x8000;
        }
        if (bCartInPage[0] == (media_id+1))
        {
            MemoryMap[0] = MSXCartPtr[media_id][0] - 0x0000;
        }
    }
    else if ((address & 0xF800) == 0x7800)
    {
        if (value & SRAM_ENABLE_BIT) // Is SRAM enabled?
        {
            sram_write_enabled_b = 1;
            MSXCartPtr[media_id][5] = (u8*)SRAM_Memory+0x0000; // Map 8K into 0x8000 view
        }
        else // SRAM disabled, normal ROM banking
        {
            sram_write_enabled_b = 0;
            MSXCartPtr[media_id][5] = (u8*)src;  // Main ROM
            MSXCartPtr[media_id][1] = (u8*)src;  // Mirror
        }

        if (bCartInPage[2] == (media_id+1))
        {
            MemoryMap[5] = MSXCartPtr[media_id][5] - 0xA000;
        }
        if (bCartInPage[0] == (media_id+1))
        {
            MemoryMap[1] = MSXCartPtr[media_id][1] - 0x2000;
        }
    }
    else if (((address & 0xE000) == 0x8000) && sram_write_enabled_a)
    {
        SRAM_Memory[(address & 0x1FFF) + 0x0000] = value;
        sram_show_status = 6;
    }
    else if (((address & 0xE000) == 0xA000) && sram_write_enabled_b)
    {
        SRAM_Memory[(address & 0x1FFF) + 0x0000] = value;
        sram_show_status = 6;
    }
}


// ----------------------------------------------
// ASCII 16K with 2K of SRAM (Hydlide II, etc).
// ----------------------------------------------
void HandleAscii16_SRAM2(u32* src, u8 block, u16 address, u8 value)
{
    u8 media_id = bCartInPage[address>>14] - 1;
    
    if ((address & 0xF800) == 0x6000)
    {
        if (value &  0x10) // Is SRAM enabled?
        {
            // Read-only access
            MSXCartPtr[media_id][4] = (u8*)SRAM_Memory+0x0000;
            MSXCartPtr[media_id][5] = (u8*)SRAM_Memory+0x0000; // Mirrored
        }
        else // SRAM disabled, normal ROM banking
        {
            MSXCartPtr[media_id][2] = (u8*)src;
            MSXCartPtr[media_id][3] = (u8*)src+0x2000;
        }

        MemoryMap[2] = MSXCartPtr[media_id][2] - 0x4000;
        MemoryMap[3] = MSXCartPtr[media_id][3] - 0x6000;
    }
    else if ((address & 0xF800) == 0x7000)
    {
        if (value &  0x10) // Is SRAM enabled?
        {
            sram_write_enabled_a = 1;
            MSXCartPtr[media_id][4] = (u8*)SRAM_Memory+0x0000;
            MSXCartPtr[media_id][5] = (u8*)SRAM_Memory+0x0000; // Mirrored
        }
        else // SRAM disabled, normal ROM banking
        {
            sram_write_enabled_a = 0;
            MSXCartPtr[media_id][4] = (u8*)src;
            MSXCartPtr[media_id][5] = (u8*)src+0x2000;
        }

        if (bCartInPage[2] == (media_id+1))
        {
            MemoryMap[4] = MSXCartPtr[media_id][4] - 0x8000;
            MemoryMap[5] = MSXCartPtr[media_id][5] - 0xA000;
        }
    }
    else if (((address & 0xF000) == 0x8000) && sram_write_enabled_a)
    {
        // We are writing to SRAM! Write all the mirrors...
        SRAM_Memory[(address & 0x7FF) + 0x0000] = value;
        SRAM_Memory[(address & 0x7FF) + 0x0800] = value;
        SRAM_Memory[(address & 0x7FF) + 0x1000] = value;
        SRAM_Memory[(address & 0x7FF) + 0x1800] = value;
        sram_show_status = 6;
    }
}

// ------------------------------------------------------------
// ASCII 16K with 8K of SRAM (A-Train is the only known game).
// ------------------------------------------------------------
void HandleAscii16_SRAM8(u32* src, u8 block, u16 address, u8 value)
{
    u8 media_id = bCartInPage[address>>14] - 1;
    
    if ((address & 0xF800) == 0x6000)
    {
        if (value &  0x10) // Is SRAM enabled?
        {
            // Read-only access
            MSXCartPtr[media_id][4] = (u8*)SRAM_Memory+0x0000;
            MSXCartPtr[media_id][5] = (u8*)SRAM_Memory+0x0000; // Mirrored
        }
        else // SRAM disabled, normal ROM banking
        {
            MSXCartPtr[media_id][2] = (u8*)src;
            MSXCartPtr[media_id][3] = (u8*)src+0x2000;
        }
        MemoryMap[2] = MSXCartPtr[media_id][2] - 0x4000;
        MemoryMap[3] = MSXCartPtr[media_id][3] - 0x6000;
    }
    else if ((address & 0xF800) == 0x7000)
    {
        if (value &  0x10) // Is SRAM enabled?
        {
            sram_write_enabled_a = 1;
            MSXCartPtr[media_id][4] = (u8*)SRAM_Memory+0x0000;
            MSXCartPtr[media_id][5] = (u8*)SRAM_Memory+0x0000; // Mirrored
        }
        else // SRAM disabled, normal ROM banking
        {
            sram_write_enabled_a = 0;
            MSXCartPtr[media_id][4] = (u8*)src;
            MSXCartPtr[media_id][5] = (u8*)src+0x2000;
        }

        if (bCartInPage[2] == (media_id+1))
        {
            MemoryMap[4] = MSXCartPtr[media_id][4] - 0x8000;
            MemoryMap[5] = MSXCartPtr[media_id][5] - 0xA000;
        }
    }
    else if (((address & 0xF000) == 0x8000) && sram_write_enabled_a)
    {
        // We are writing to SRAM!
        SRAM_Memory[(address & 0x1FFF) + 0x0000] = value;
        sram_show_status = 6;
    }
}

void HandleKonamiSCC8(u32* src, u8 block, u16 address, u8 value)
{
    u8 media_id = bCartInPage[address>>14] - 1;
    
    // --------------------------------------------------------
    // Konami 8K mapper with SCC
    //  Bank 1: 4000h - 5FFFh - mapped via writes to 5000h
    //  Bank 2: 6000h - 7FFFh - mapped via writes to 7000h
    //  Bank 3: 8000h - 9FFFh - mapped via writes to 9000h
    //  Bank 4: A000h - BFFFh - mapped via writes to B000h
    // --------------------------------------------------------
    if ((address & 0xF800) == 0x5000)
    {
        MSXCartPtr[media_id][2] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][6] = (u8*)src;  // Mirror
        MemoryMap[2] = (u8 *)(MSXCartPtr[media_id][2]) - 0x4000;

        if (bCartInPage[3] == (media_id+1))
        {
            MemoryMap[6] = MSXCartPtr[media_id][6] - 0xC000;
        }
    }
    else if ((address & 0xF800) == 0x7000)
    {
        MSXCartPtr[media_id][3] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][7] = (u8*)src;  // Mirror
        MemoryMap[3] = (u8 *)(MSXCartPtr[media_id][3]) - 0x6000;

        if (bCartInPage[3] == (media_id+1))
        {
            MemoryMap[7] = MSXCartPtr[media_id][7] - 0xE000;
        }
    }
    else if ((address & 0xF800) == 0x9000)
    {
        // --------------------------------------------------------------------------------------------------
        // For standard SCC carts we require the full 0x3F to be programmed to enable the SCC register view.
        // And yes, the banking logic below is still always called...
        // --------------------------------------------------------------------------------------------------
        if ((value & 0x3F) == 0x3F)
        {
            special_memory_access |= SPEC_MEM_SCC_ENABLED;  // SCC Registers are now "in view"
            msx_scc_capable_game = true;                 // SCC sound - set a flag so we process this special sound chip for this game
        }
        else
        {
            special_memory_access &= ~SPEC_MEM_SCC_ENABLED; // SCC Registers are no longer "in view"
        }

        MSXCartPtr[media_id][4] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][0] = (u8*)src;  // Mirror
        MemoryMap[4] = (u8 *)(MSXCartPtr[media_id][4]) - 0x8000;

        if (bCartInPage[0] == (media_id+1))
        {
            MemoryMap[0] = MSXCartPtr[media_id][0] - 0x0000;
        }
    }
    else if ((address & 0xF800) == 0xB000)
    {
        MSXCartPtr[media_id][5] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][1] = (u8*)src;  // Mirror
        MemoryMap[5] = (u8 *)(MSXCartPtr[media_id][5]) - 0xA000;

        if (bCartInPage[0] == (media_id+1))
        {
            MemoryMap[1] = MSXCartPtr[media_id][1] - 0x2000;
        }
    }
}

// ---------------------------------------------------------------------
// Xevious and a few other related games have special cart mapping...
// ---------------------------------------------------------------------
void HandleXevious(u32* src, u8 block, u16 address)
{
    u8 media_id = bCartInPage[address>>14] - 1;
    
    if ((address >= 0x6000) && (address <= 0x67FF))
    {
        MSXCartPtr[media_id][2] = (u8*)src;
        MSXCartPtr[media_id][3] = (u8*)src+0x2000;
        MemoryMap[2] = MSXCartPtr[media_id][2] - 0x4000;
        MemoryMap[3] = MSXCartPtr[media_id][3] - 0x6000;
    }
    else if ((address >= 0x7000) && (address <= 0x77FF))
    {
        MSXCartPtr[media_id][4] = (u8*)src;
        MSXCartPtr[media_id][5] = (u8*)src+0x2000;
        if (bCartInPage[2] == (media_id+1))
        {
            MemoryMap[4] = MSXCartPtr[media_id][4] - 0x8000;
            MemoryMap[5] = MSXCartPtr[media_id][5] - 0xA000;
        }
    }
}


// ---------------------------------------------------------------------------------------------
// Same as Konami8 but with DAC at 0x4000 range. We don't handle that yet, but game is playable.
// ---------------------------------------------------------------------------------------------
void HandleMajut(u32* src, u8 block, u16 address)
{
    u8 media_id = bCartInPage[address>>14] - 1;

    if (bCartInPage[1] && ((address & 0xE000) == 0x6000))
    {
        MSXCartPtr[media_id][3] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][7] = (u8*)src;  // Mirror
        MemoryMap[3] = (u8 *)(MSXCartPtr[media_id][3]) - 0x6000;
    }
    else if (bCartInPage[2] && ((address & 0xE000) == 0x8000))
    {
        MSXCartPtr[media_id][4] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][0] = (u8*)src;  // Mirror
        MemoryMap[4] = (u8 *)(MSXCartPtr[media_id][4]) - 0x8000;
    }
    else if (bCartInPage[2] && ((address & 0xE000) == 0xA000))
    {
        MSXCartPtr[media_id][5] = (u8*)src;  // Main ROM
        MSXCartPtr[media_id][1] = (u8*)src;  // Mirror
        MemoryMap[5] = (u8 *)(MSXCartPtr[media_id][5]) - 0xA000;
    }
}

// ---------------------------------------------------------------------
// XBlam switches on hits to addres 0x4045 and swaps out 16K at 0x8000
// ---------------------------------------------------------------------
void HandleXBlam(u32* src, u8 block, u16 address)
{
    u8 media_id = bCartInPage[address>>14] - 1;

    if (address == 0x4045)
    {
        MSXCartPtr[media_id][4] = (u8*)src;          // Main ROM at 8000
        MSXCartPtr[media_id][5] = (u8*)src+0x2000;   // Main ROM at A000
        if (bCartInPage[2])
        {
            MemoryMap[4] = MSXCartPtr[media_id][4] - 0x8000;
            MemoryMap[5] = MSXCartPtr[media_id][5] - 0xA000;
        }
    }
}

// ---------------------------------------------------------------------
// Lode Runner maps with writes to 0x0000 but it's even more unusual
// in that it doesn't care if the cart is mapped into view as it will
// respond to any write to 0x0000 no matter what.
// ---------------------------------------------------------------------
void HandleSuperLodeRunner(u8 media_id, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset);

    MSXCartPtr[media_id][4] = (u8*)src;
    MSXCartPtr[media_id][5] = (u8*)src+0x2000;

    if (bCartInPage[2])
    {
        MemoryMap[4] = MSXCartPtr[media_id][4] - 0x8000;
        MemoryMap[5] = MSXCartPtr[media_id][5] - 0xA000;
    }
}


// -------------------------------------------------------------------
// The FM-PAC presents 8K of SRAM for a game to use to save progress.
// This handler is called when any byte in the 0x4000-0x5FFF range is 
// written and will determine if/when the SRAM is available.
// -------------------------------------------------------------------
#define FMPAC_KEY1 0x4D
#define FMPAC_KEY2 0x69
void HandleFMPAC(u16 address, u8 value)
{
    if (address == 0x5FFE)
    {
        // Key register
        SRAM_Memory[0x1FFE] = value;

        if ((SRAM_Memory[0x1FFF] == 0x69) && (value == 0x4D))
        {
            FMPAC_SRAM_in_view = 1;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)SRAM_Memory;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        else
        {
            FMPAC_SRAM_in_view = 0;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)MSXBios_MSXMUSIC;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        return;
    }
    else if (address == 0x5FFF)
    {
        // Key register
        SRAM_Memory[0x1FFF] = value;

        if ((SRAM_Memory[0x1FFE] == 0x4D) && (value == 0x69))
        {
            FMPAC_SRAM_in_view = 1;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)SRAM_Memory;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        else
        {
            FMPAC_SRAM_in_view = 0;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)MSXBios_MSXMUSIC;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        return;
    }

    // Otherwise see if we are able to write into the SRAM
    if (FMPAC_SRAM_in_view && (address >= 0x4000) && (address < 0x5FFE))
    {
        SRAM_Memory[address & 0x1FFF] = value;
        sram_show_status = 5;
    }
}

// ---------------------------------------------------------------------
// Is this window currently forced into plain writable RAM? Bit4 (Global
// Memory Mode) forces every bank; otherwise Bits 0-2 force their own
// bank individually. Bank1 (4000-7FFF) covers BOTH 8K windows as one unit.
// winIdx: 2 or 3 -> Bank1, 4 -> Bank2 (8000-9FFF), 5 -> Bank3 (A000-BFFF)
// ---------------------------------------------------------------------
static inline u8 SCCPlus_WindowIsRAM(u8 winIdx)
{
    if (sccplus_mode & 0x10) return 1;                                    // Bit4: Global override
    if (winIdx == 2 || winIdx == 3) return (sccplus_mode & 0x01) ? 1 : 0; // Bit0: Bank1
    if (winIdx == 4)                return (sccplus_mode & 0x02) ? 1 : 0; // Bit1: Bank2
    /* winIdx == 5 */               return (sccplus_mode & 0x04) ? 1 : 0; // Bit2: Bank3
}

// -------------------------------------------------------------------
// Map the SCC+ 64K of special cart-based RAM into place. The SCC+
// cart is always inserted into CART2 so that's all we deal with here.
// -------------------------------------------------------------------
static inline void SCCPlus_MapWindow(u8 idx, u8 page)
{
    MSXCartPtr[MEDIA_CART2][idx] = SCC_Memory + ((page & 0x07) * 0x2000);
    MemoryMap[idx]  = MSXCartPtr[MEDIA_CART2][idx] - (0x2000 * idx);
}

// ------------------------------------------------------------------------
// When SCC+ switches banks, records the page and remap memory accordingly.
// Only reached when the relevant window is NOT in forced-RAM mode.
// ------------------------------------------------------------------------
void HandleSCCPlusBankSelect(u16 address, u8 value)
{
    switch (address & 0xF000)
    {
        case 0x5000: sccplus_page[0] = value; SCCPlus_MapWindow(2, value); break;
        case 0x7000: sccplus_page[1] = value; SCCPlus_MapWindow(3, value); break;
        case 0x9000: sccplus_page[2] = value; SCCPlus_MapWindow(4, value); break;
        case 0xB000: sccplus_page[3] = value; SCCPlus_MapWindow(5, value); break;
    }
}

void HandleSCCPlusModeRegister(u8 value)
{
    sccplus_mode = value;

    // Bit5 alone decides which window shows the audio registers
    if ((value & 0x20))
    {
        special_memory_access &= ~SPEC_MEM_SCC_ENABLED;
        special_memory_access |= SPEC_MEM_SCC_PLUS_ENABLED;
    }
    else // Normal SCC
    {
        special_memory_access &= ~SPEC_MEM_SCC_PLUS_ENABLED;
        special_memory_access |= SPEC_MEM_SCC_ENABLED;
    }

    if (special_memory_access & (SPEC_MEM_SCC_ENABLED | SPEC_MEM_SCC_PLUS_ENABLED))
    {
        msx_scc_capable_game = true;
    }

    // Nothing else to do here - MSXCartPtr[MEDIA_CART2][]/MemoryMap[] already hold the last
    // selected page for every window; Bit4/Bits0-2 only change how WRITES to
    // that window get interpreted, which cpu_writemem16 checks live below.
}

void HandleSCCPlus(u16 address, u8 value)
{
    // Mode Register - always intercepted whenever the cart occupies 8000-BFFF
    if ((bCartInPage[2] == 2) && (address == 0xBFFE || address == 0xBFFF))
    {
        HandleSCCPlusModeRegister(value);
        return;
    }

    // SCC+ registers shadow A000-BFFF whenever Sound Mode = SCC+
    if ((bCartInPage[2] == 2) && (special_memory_access & SPEC_MEM_SCC_PLUS_ENABLED) && (address >= 0xB800) && (address <= 0xBFFD))
    {
        SCCWrite(value, address, &mySCC);
        return;
    }

    // Classic SCC registers shadow 8000-9FFF whenever Sound Mode = compat
    if ((bCartInPage[2] == 2) && (special_memory_access & SPEC_MEM_SCC_ENABLED) && ((address & 0xF800) == 0x9800))
    {
        SCC_LegacyWrite(value, address&0xFF);
        return;
    }

    // Bank1: 4000-7FFF
    if ((bCartInPage[1] == 2) && (address >= 0x4000) && (address <= 0x7FFF))
    {
        if (SCCPlus_WindowIsRAM(2))
            MemoryMap[address>>13][address] = value;
        else if (((address & 0xF000) == 0x5000) || ((address & 0xF000) == 0x7000))
            HandleSCCPlusBankSelect(address, value);
        return;
    }

    // Bank2/Bank3: 8000-BFFF
    if ((bCartInPage[2] == 2) && (address >= 0x8000) && (address <= 0xBFFF))
    {
        u8 winIdx = (address < 0xA000) ? 4 : 5;
        if (SCCPlus_WindowIsRAM(winIdx))
            MemoryMap[address>>13][address] = value;
        else if (((address & 0xF000) == 0x9000) || ((address & 0xF000) == 0xB000))
            HandleSCCPlusBankSelect(address, value);
        return;
    }
}


// ------------------------------------------------------------------
// Write memory handles both normal writes and bankswitched since
// write is much less common than reads... We handle the popular MSX
// Konami 8K, SCC and ASCII 8K mappers directly here for max speed.
// ------------------------------------------------------------------
ITCM_CODE void cpu_writemem16(u16 address, u8 value)
{
    if ((special_memory_access & SPEC_MEM_SUBSLOT_ACTIVE) && (address == 0xFFFF)) // Subslot check... only when page 3 is mapped to an expanded slot
    {
        msx_subslot = value;
        cpu_writeport_msx(0xA8, Port_PPI_A); // Enable the new map...
    }
    else if (bRAMInPage[address >> 14]) // RAM Exists in this slot... write it.
    {
        MemoryMap[address>>13][address]=value;
    }
    else if ((special_memory_access & SPEC_MEM_DISK_CONTROLLER) && (address >= 0x7FF8) && (address <= 0x7FFF)) // Disk controller memory map...
    {
        if (address <= 0x7FFB) fdc_write(address & 3, value);
        if (address == 0x7FFC) fdc_setSide((value & 1) ? 1:0);  // Side: [xxxxxxxS]
        if (address == 0x7FFD) fdc_setDrive((value & 1) ? 1:0); // Drive: [xxxxxxxD]
    }
    else if ((special_memory_access & SPEC_MEM_SUPERLR_ACTIVE) && (address == 0x0000))
    {
        // ------------------------------------------------------------------------------
        // In theory, the write to 0x0000 can come with any slot mapped in anywhere
        // but this implementation requires that RAM not be mapped into page 0 or
        // else we would never get to this check. We could move this check much further
        // up but I don't want one game impacting other games with more common mappers.
        // So far with testing, this implementation works fine for SuperLodeRunner.
        // ------------------------------------------------------------------------------
        HandleSuperLodeRunner(bCartInPage[address>>14] - 1, value);
    }
    else if (bCartInPage[address>>14]) // Is there a Cartridge mapped into the write address area?
    {
        u8 media_id = bCartInPage[address>>14] - 1; // Indicates MEDIA_CART1 or MEDIA_CART2

        // -------------------------------------------------------------
        // Compute the block and offset of the new memory and we
        // can map it into place... this is fast since we are just
        // moving pointers around and not trying to copy memory blocks.
        // -------------------------------------------------------------
        u32 block = (value & MyMedia[media_id].mapperMask);
        u32 msx_offset = block * MyMedia[media_id].blockSize;
        u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

        // ---------------------------------------------------------------------------------
        // The Konami 8K Mapper without SCC:
        // 4000h-5FFFh - fixed ROM area (not swappable)
        // 6000h~7FFFh (mirror: E000h~FFFFh)    6000h (mirrors: 6001h~7FFFh)    1
        // 8000h~9FFFh (mirror: 0000h~1FFFh)    8000h (mirrors: 8001h~9FFFh)    Random
        // A000h~BFFFh (mirror: 2000h~3FFFh)    A000h (mirrors: A001h~BFFFh)    Random
        // ---------------------------------------------------------------------------------
        if (MyMedia[media_id].mapperType == KON8)
        {
            if ((address & 0xE000) == 0x6000)
            {
                MSXCartPtr[media_id][3] = (u8*)src;  // Main ROM
                MSXCartPtr[media_id][7] = (u8*)src;  // Mirror
                MemoryMap[3] = (u8 *)(MSXCartPtr[media_id][3]) - 0x6000;
            }
            else if ((address & 0xE000) == 0x8000)
            {
                MSXCartPtr[media_id][4] = (u8*)src;  // Main ROM
                MSXCartPtr[media_id][0] = (u8*)src;  // Mirror
                MemoryMap[4] = (u8 *)(MSXCartPtr[media_id][4]) - 0x8000;
            }
            else if ((address & 0xE000) == 0xA000)
            {
                MSXCartPtr[media_id][5] = (u8*)src;  // Main ROM
                MSXCartPtr[media_id][1] = (u8*)src;  // Mirror
                MemoryMap[5] = (u8 *)(MSXCartPtr[media_id][5]) - 0xA000;
            }
        }
        else if (MyMedia[media_id].mapperType == ASC8)
        {
            // -------------------------------------------------------------------------
            // The ASCII 8K Mapper:
            // 4000h~5FFFh (mirror: C000h~DFFFh)    6000h (mirrors: 6001h~67FFh)    0
            // 6000h~7FFFh (mirror: E000h~FFFFh)    6800h (mirrors: 6801h~68FFh)    0
            // 8000h~9FFFh (mirror: 0000h~1FFFh)    7000h (mirrors: 7001h~77FFh)    0
            // A000h~BFFFh (mirror: 2000h~3FFFh)    7800h (mirrors: 7801h~7FFFh)    0
            // -------------------------------------------------------------------------
            if ((address & 0xF800) == 0x6000)
            {
                MSXCartPtr[media_id][2] = (u8*)src;  // Main ROM
                MSXCartPtr[media_id][6] = (u8*)src;  // Mirror
                MemoryMap[2] = MSXCartPtr[media_id][2] - 0x4000;
                if (bCartInPage[3] == (media_id+1))
                {
                    MemoryMap[6] = MSXCartPtr[media_id][6] - 0xC000;
                }
            }
            else if ((address & 0xF800) == 0x6800)
            {
                MSXCartPtr[media_id][3] = (u8*)src;  // Main ROM
                MSXCartPtr[media_id][7] = (u8*)src;  // Mirror
                MemoryMap[3] = MSXCartPtr[media_id][3] - 0x6000;
                if (bCartInPage[3] == (media_id+1))
                {
                    MemoryMap[7] = MSXCartPtr[media_id][7] - 0xE000;
                }
            }
            else if ((address & 0xF800) == 0x7000)
            {
                MSXCartPtr[media_id][4] = (u8*)src;  // Main ROM
                MSXCartPtr[media_id][0] = (u8*)src;  // Mirror
                if (bCartInPage[2] == (media_id+1))
                {
                    MemoryMap[4] = MSXCartPtr[media_id][4] - 0x8000;
                }
                if (bCartInPage[0] == (media_id+1))
                {
                    MemoryMap[0] = MSXCartPtr[media_id][0] - 0x0000;
                }
            }
            else if ((address & 0xF800) == 0x7800)
            {
                MSXCartPtr[media_id][5] = (u8*)src;  // Main ROM
                MSXCartPtr[media_id][1] = (u8*)src;  // Mirror
                if (bCartInPage[2] == (media_id+1))
                {
                    MemoryMap[5] = MSXCartPtr[media_id][5] - 0xA000;
                }
                if (bCartInPage[0] == (media_id+1))
                {
                    MemoryMap[1] = MSXCartPtr[media_id][1] - 0x2000;
                }
            }
        }
        else if (MyMedia[media_id].mapperType == SCC8)
        {
            // -----------------------------------------------------------------------------------
            // Are we writing to the SCC chip memory mapped area and are the registers "in view"?
            // -----------------------------------------------------------------------------------
            if ((special_memory_access & SPEC_MEM_SCC_ENABLED) && ((address & 0xF800) == 0x9800))
            {
                 SCC_LegacyWrite(value, address&0xFF);
            }
            else // We only handle bank switching if the SCC registers were not accessed above...
            {
                HandleKonamiSCC8(src, block, address, value);
            }
        }
        else if (MyMedia[media_id].mapperType == ASC16)
        {
            // -------------------------------------------------------------------------
            // The ASCII 16K Mapper:
            // 4000h~7FFFh  via writes to 6000h to 67FFh
            // 8000h~BFFFh  via writes to 7000h to 77FFh
            // -------------------------------------------------------------------------
            if ((address & 0xF800) == 0x6000)
            {
                MSXCartPtr[media_id][2] = (u8*)src;
                MSXCartPtr[media_id][3] = (u8*)src+0x2000;
                MemoryMap[2] = MSXCartPtr[media_id][2] - 0x4000;
                MemoryMap[3] = MSXCartPtr[media_id][3] - 0x6000;
            }
            else if ((address & 0xF800) == 0x7000)
            {
                MSXCartPtr[media_id][4] = (u8*)src;
                MSXCartPtr[media_id][5] = (u8*)src+0x2000;
                if (bCartInPage[2] == (media_id+1))
                {
                    MemoryMap[4] = MSXCartPtr[media_id][4] - 0x8000;
                    MemoryMap[5] = MSXCartPtr[media_id][5] - 0xA000;
                }
            }
        }
        else if (MyMedia[media_id].mapperType == ZEN8)
        {
            HandleZemina8K(src, block, address);
        }
        else if (MyMedia[media_id].mapperType == ZEN16)
        {
            HandleZemina16K(src, block, address);
        }
        else if (MyMedia[media_id].mapperType == ASC8SRAM2)
        {
            HandleAscii8_SRAM2(src, block, address, value);
        }
        else if (MyMedia[media_id].mapperType == ASC8SRAM8)
        {
            HandleAscii8_SRAM8(src, block, address, value);
        }
        else if (MyMedia[media_id].mapperType == ASC16SRAM2)
        {
            HandleAscii16_SRAM2(src, block, address, value);
        }
        else if (MyMedia[media_id].mapperType == ASC16SRAM8)
        {
            HandleAscii16_SRAM8(src, block, address, value);
        }
        else if (MyMedia[media_id].mapperType == XEVIOUS)
        {
            HandleXevious(src, block, address);
        }
        else if (MyMedia[media_id].mapperType == MAJUT)
        {
            HandleMajut(src, block, address);
        }
        else if (MyMedia[media_id].mapperType == XBLAM)
        {
            HandleXBlam(src, block, address);
        }
        else if (MyMedia[media_id].mapperType == SCCPLUS_RAM)
        {
            HandleSCCPlus(address, value);
        }
        else if (MyMedia[media_id].mapperType == FMPAC_SRAM)
        {
            HandleFMPAC(address, value);
        }        
    }
}

// -----------------------------------------------------------------
// Reset a few key variables needed for proper Z80 interface use...
// -----------------------------------------------------------------
void Z80_Interface_Reset(void)
{
    CPU.CycleDeficit      = 0;
    sram_write_enabled_a  = 0;
    sram_write_enabled_b  = 0;
    FMPAC_SRAM_in_view    = 0;
    FMPAC_Key1            = 0;
}

// -----------------------------------------------------------------
// Trap and report illegal opcodes to the Hachibitto debugger...
// -----------------------------------------------------------------
void Trap_Bad_Ops(char *prefix, byte I, word W)
{
    if (myGlobalConfig.debugger)
    {
        char tmp[32];
        sprintf(tmp, "ILLOP: %s %02X %04X", prefix, I, W);
        DSPrint(0,0,6, tmp);
    }
}

// End of file
