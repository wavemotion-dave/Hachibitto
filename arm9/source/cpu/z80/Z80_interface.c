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
#include <stdint.h>
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


// ---------------------------------------------------------------------------------
// The Konami 8K Mapper without SCC:
// 4000h-5FFFh - fixed ROM area (not swappable)
// 6000h~7FFFh (mirror: E000h~FFFFh)    6000h (mirrors: 6001h~7FFFh)    1
// 8000h~9FFFh (mirror: 0000h~1FFFh)    8000h (mirrors: 8001h~9FFFh)    Random
// A000h~BFFFh (mirror: 2000h~3FFFh)    A000h (mirrors: A001h~BFFFh)    Random
// ---------------------------------------------------------------------------------
void __attribute__((noinline)) HandleKonami8(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

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

// -------------------------------------------------------------------------
// The ASCII 8K Mapper:
// 4000h~5FFFh (mirror: C000h~DFFFh)    6000h (mirrors: 6001h~67FFh)    0
// 6000h~7FFFh (mirror: E000h~FFFFh)    6800h (mirrors: 6801h~68FFh)    0
// 8000h~9FFFh (mirror: 0000h~1FFFh)    7000h (mirrors: 7001h~77FFh)    0
// A000h~BFFFh (mirror: 2000h~3FFFh)    7800h (mirrors: 7801h~7FFFh)    0
// -------------------------------------------------------------------------
void __attribute__((noinline)) HandleAscii8(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

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


// -------------------------------------------------------------------------
// The ASCII 16K Mapper:
// 4000h~7FFFh  via writes to 6000h to 67FFh
// 8000h~BFFFh  via writes to 7000h to 77FFh
// -------------------------------------------------------------------------
void __attribute__((noinline)) HandleAscii16(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

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

// MSX DOS 2 by ASCII only maps 0x4000-0x7FFF and the hotpsot is at 0x7FFE
void __attribute__((noinline)) HandleMSXDOS2(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

    if (address == 0x7FFE)
    {
        MSXCartPtr[media_id][2] = (u8*)src;
        MSXCartPtr[media_id][3] = (u8*)src+0x2000;
        MemoryMap[2] = MSXCartPtr[media_id][2] - 0x4000;
        MemoryMap[3] = MSXCartPtr[media_id][3] - 0x6000;
    }
}

// Segment 16 is fixed at 0x4000
void __attribute__((noinline)) HandleRType(u8 media_id, u16 address, u8 value)
{
    u32 block = value & ((value & 0x10) ? 0x17 : 0x1F);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

    if ((address & 0xF800) == 0x7000)
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

#define SRAM_ENABLE_BIT     (MyMedia[media_id].mapperMask+1)      // SRAM Enable is the bit right after the rom selection bits...

void __attribute__((noinline)) HandleAscii8_SRAM2(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

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


void __attribute__((noinline)) HandleAscii8_SRAM8(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

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
void __attribute__((noinline)) HandleAscii16_SRAM2(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

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
void __attribute__((noinline)) HandleAscii16_SRAM8(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

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

void __attribute__((noinline)) HandleKoei_SRAM32K(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

    if ((address & 0xF800) == 0x6000)
    {
        if (value & SRAM_ENABLE_BIT) // Is SRAM enabled?
        {
            // No write support here...
            int sram_bank = value & 3;
            MSXCartPtr[media_id][2] = (u8*)SRAM_Memory+(sram_bank * 0x2000); // Map 8K bank into 0x8000 view
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
            int sram_bank = value & 3;
            MSXCartPtr[media_id][3] = (u8*)SRAM_Memory+(sram_bank * 0x2000); // Map 8K bank into 0x8000 view
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
            int sram_bank = value & 3;
            MSXCartPtr[media_id][4] = (u8*)SRAM_Memory+(sram_bank * 0x2000); // Map 8K bank into 0x8000 view
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
            // No write support here...
            int sram_bank = value & 3;
            MSXCartPtr[media_id][5] = (u8*)SRAM_Memory+(sram_bank * 0x2000); // Map 8K bank into 0x8000 view
        }
        else // SRAM disabled, normal ROM banking
        {
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
    else if (((address & 0xE000) == 0x8000) && sram_write_enabled_a) // KOEI 32K SRAM only writable on Page 3
    {
        MSXCartPtr[media_id][4][address & 0x1FFF] = value;
        sram_show_status = 6;
    }
}


void __attribute__((noinline)) HandleKonamiSCC8(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

    // -----------------------------------------------------------------------------------
    // Are we writing to the SCC chip memory mapped area and are the registers "in view"?
    // -----------------------------------------------------------------------------------
    if ((special_memory_access & SPEC_MEM_SCC_ENABLED) && ((address & 0xF800) == 0x9800))
    {
         SCC_LegacyWrite(value, address&0xFF);
    }
    else // We only handle bank switching if the SCC registers were not accessed above...
    {
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
                if (!msx_scc_capable_game)
                {
                    msx_scc_capable_game = true;                 // SCC sound - set a flag so we process this special sound chip for this game
                    bClickRemoveNeeded = 1;
                }
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
}

// -----------------------------------------------------------------------------------
// This is a memory-mapper expansion (handled in msx.c) to bring total system memory
// up to 512K (DS-Lite) or 1MB (DSi/XL/LL) but also contains a standard SCC cart.
// -----------------------------------------------------------------------------------
void __attribute__((noinline)) HandleMegaSCC(u8 media_id, u16 address, u8 value)
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
        if ((address & 0xF800) == 0x9000)
        {
            // ------------------------------------------------------------------------
            // For the Mega SCC, we enable the SCC the same way a Konami cart would...
            // ------------------------------------------------------------------------
            if ((value & 0x3F) == 0x3F)
            {
                special_memory_access |= SPEC_MEM_SCC_ENABLED;  // SCC Registers are now "in view"
                if (!msx_scc_capable_game)
                {
                    msx_scc_capable_game = true;                // SCC sound - set a flag so we process this special sound chip for this game
                    bClickRemoveNeeded = 1;
                }
            }
            else
            {
                special_memory_access &= ~SPEC_MEM_SCC_ENABLED; // SCC Registers are no longer "in view"
            }
        }
    }
}

// --------------------------------------------------------------------------------------
// Very much like Konami SCC but without mirrors and with flash write possibilities...
// Games that use this: Manbow 2, Hamaraja Night, Angelic Warrior, etc.
// --------------------------------------------------------------------------------------

// AMD flash command state
// 0 = waiting for AA
// 1 = waiting for 55
// 2 = waiting for A0 (program) or 80 (erase setup)
// 3 = waiting for program data
// 4 = erase setup: waiting for AA
// 5 = erase setup: waiting for 55
// 6 = waiting for sector erase command (30)
static u8 sccFlashCommandState = 0;

void __attribute__((noinline)) HandleSCCFlash(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

   u16 flash_cmd_addr = address & 0x0FFF;

    // Reset command.
    if (value == 0xF0 && flash_cmd_addr == 0x0555)
    {
        sccFlashCommandState = 0;
    }
    else
    {
        switch (sccFlashCommandState)
        {
            case 0:
                if (flash_cmd_addr == 0x0555 && value == 0xAA)
                    sccFlashCommandState = 1;
                break;

            case 1:
                if ((flash_cmd_addr == 0x0AAA ||
                     flash_cmd_addr == 0x02AA) && value == 0x55)
                {
                    sccFlashCommandState = 2;
                }
                else
                {
                    sccFlashCommandState =
                        (flash_cmd_addr == 0x0555 && value == 0xAA) ? 1 : 0;
                }
                break;

            case 2:
                if (flash_cmd_addr == 0x0555 && value == 0xA0)
                {
                    sccFlashCommandState = 3;
                }
                else if (flash_cmd_addr == 0x0555 && value == 0x80)
                {
                    sccFlashCommandState = 4;
                }
                else
                {
                    sccFlashCommandState = 0;
                }
                break;

            case 3: // Program one byte
            {
                u8 bank = address >> 13;

                if (bank < 8 && MSXCartPtr[media_id][bank] != NULL)
                {
                    u8 *dest = MSXCartPtr[media_id][bank] + (address & 0x1FFF);
                    *dest &= value;
                }

                sccFlashCommandState = 0;
                flash_show_status = 4;
                break;
            }

            case 4: // Erase setup: second AA
                if (flash_cmd_addr == 0x0555 && value == 0xAA)
                    sccFlashCommandState = 5;
                else
                    sccFlashCommandState = 0;
                break;

            case 5: // Erase setup: second 55
                if ((flash_cmd_addr == 0x0AAA ||
                     flash_cmd_addr == 0x02AA) && value == 0x55)
                    sccFlashCommandState = 6;
                else
                    sccFlashCommandState = 0;
                break;

            case 6: // Sector erase, or ignore chip erase
                if (value == 0x30)
                {
                    u8 bank = address >> 13;

                    if (bank < 8 && MSXCartPtr[media_id][bank] != NULL)
                    {
                        u32 sector_size;
                        u32 flash_size;

                        if (MyMedia[media_id].mapperType == RBSC_FLASH)
                        {
                            sector_size = 0x10000; // AM29F016D: 64 KB
                            flash_size = 0x200000; // 2 MB
                        }
                        else
                        {
                            sector_size = 0x10000; // AM29F040B: 64 KB
                            flash_size = 0x80000;  // 512 KB
                        }

                        u32 cart_capacity = (MAX_CART_SIZE_KB / 2) * 1024;
                        if (flash_size > cart_capacity)
                            flash_size = cart_capacity;

                        uintptr_t cart_base = (uintptr_t)ROM_Memory + (uintptr_t)media_id * cart_capacity;
                        uintptr_t dest = (uintptr_t)MSXCartPtr[media_id][bank] + (address & 0x1FFF);

                        if ((dest >= cart_base) && (dest - cart_base < flash_size))
                        {
                            u32 offset = (u32)(dest - cart_base);
                            u32 sector_offset = offset & ~(sector_size - 1);

                            if (sector_offset <= flash_size - sector_size)
                            {
                                memset((void *)(cart_base + sector_offset), 0xFF, sector_size);
                                flash_show_status = 4;
                            }
                        }
                    }
                }
                // value == 0x10 is chip erase: intentionally ignored.

                sccFlashCommandState = 0;
                break;

            default:
                sccFlashCommandState = 0;
                break;
        }
    }

    // -------------------------------------------------------------------------
    // Existing SCC handling and mapper bank switching.
    // -------------------------------------------------------------------------
    if ((special_memory_access & SPEC_MEM_SCC_ENABLED) && ((address & 0xF800) == 0x9800))
    {
        SCC_LegacyWrite(value, address & 0xFF);
    }
    else
    {
        if ((address & 0xF800) == 0x5000)
        {
            MSXCartPtr[media_id][2] = (u8*)src;
            MemoryMap[2] = (u8*)MSXCartPtr[media_id][2] - 0x4000;
        }
        else if ((address & 0xF800) == 0x7000)
        {
            MSXCartPtr[media_id][3] = (u8*)src;
            MemoryMap[3] = (u8*)MSXCartPtr[media_id][3] - 0x6000;
        }
        else if ((address & 0xF800) == 0x9000)
        {
            if ((value & 0x3F) == 0x3F)
            {
                special_memory_access |= SPEC_MEM_SCC_ENABLED;

                if (!msx_scc_capable_game)
                {
                    msx_scc_capable_game = true;
                    bClickRemoveNeeded = 1;
                }
            }
            else
            {
                special_memory_access &= ~SPEC_MEM_SCC_ENABLED;
            }

            MSXCartPtr[media_id][4] = (u8*)src;
            MemoryMap[4] = (u8*)MSXCartPtr[media_id][4] - 0x8000;
        }
        else if ((address & 0xF800) == 0xB000)
        {
            MSXCartPtr[media_id][5] = (u8*)src;
            MemoryMap[5] = (u8*)MSXCartPtr[media_id][5] - 0xA000;
        }
    }
}


// ---------------------------------------------------------------------
// Xevious and a few other related games have special cart mapping...
// ---------------------------------------------------------------------
void __attribute__((noinline))  HandleXevious(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

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
void __attribute__((noinline))  HandleMajut(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

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
void __attribute__((noinline))  HandleXBlam(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

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

void __attribute__((noinline)) HandleGeneric8k(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

    switch (address >> 13)
    {
        case 0:
            MSXCartPtr[media_id][0] = (u8*)src;
            if (bCartInPage[address>>14])
            {
                MemoryMap[0] = (u8 *)(MSXCartPtr[media_id][0]) - 0x0000;
            }
            break;
        case 1:
            MSXCartPtr[media_id][1] = (u8*)src;
            if (bCartInPage[address>>14])
            {
                MemoryMap[1] = (u8 *)(MSXCartPtr[media_id][1]) - 0x2000;
            }
            break;

        case 2:
            MSXCartPtr[media_id][2] = (u8*)src;
            if (bCartInPage[address>>14])
            {
                MemoryMap[2] = (u8 *)(MSXCartPtr[media_id][2]) - 0x4000;
            }
            break;
        case 3:
            MSXCartPtr[media_id][3] = (u8*)src;
            if (bCartInPage[address>>14])
            {
                MemoryMap[3] = (u8 *)(MSXCartPtr[media_id][3]) - 0x6000;
            }
            break;

        case 4:
            MSXCartPtr[media_id][4] = (u8*)src;
            if (bCartInPage[address>>14])
            {
                MemoryMap[4] = (u8 *)(MSXCartPtr[media_id][4]) - 0x8000;
            }
            break;
        case 5:
            MSXCartPtr[media_id][5] = (u8*)src;
            if (bCartInPage[address>>14])
            {
                MemoryMap[5] = (u8 *)(MSXCartPtr[media_id][5]) - 0xA000;
            }
            break;

        case 6:
            MSXCartPtr[media_id][6] = (u8*)src;
            if (bCartInPage[address>>14])
            {
                MemoryMap[6] = (u8 *)(MSXCartPtr[media_id][6]) - 0xC000;
            }
            break;
        case 7:
            MSXCartPtr[media_id][7] = (u8*)src;
            if (bCartInPage[address>>14])
            {
                MemoryMap[7] = (u8 *)(MSXCartPtr[media_id][7]) - 0xE000;
            }
            break;
    }
}

u8 matra_previoius_blocks[4] = {0,0,0,0};
u8 matra_compilation_offset = 2;

void __attribute__((noinline)) HandleMatraCompilation(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u8 offset_block = block;

    if (matra_compilation_offset >= 2) offset_block += (matra_compilation_offset-2);
    u32 msx_offset = offset_block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

    if (address == 0x5000)
    {
        if (matra_compilation_offset >= 2)
        {
            MSXCartPtr[media_id][2] = (u8*)src;
            MemoryMap[2] = (u8 *)(MSXCartPtr[media_id][2]) - 0x4000;
            matra_previoius_blocks[0] = block;
        }
    }
    else if (address == 0x6000)
    {
        if (matra_compilation_offset >= 2)
        {
            MSXCartPtr[media_id][3] = (u8*)src;
            MemoryMap[3] = (u8 *)(MSXCartPtr[media_id][3]) - 0x6000;
            matra_previoius_blocks[1] = block;
        }
    }
    else if (address == 0x8000)
    {
        if (matra_compilation_offset >= 2)
        {
            MSXCartPtr[media_id][4] = (u8*)src;
            MemoryMap[4] = (u8 *)(MSXCartPtr[media_id][4]) - 0x8000;
            matra_previoius_blocks[2] = block;
        }
    }
    else if (address == 0xA000)
    {
        if (matra_compilation_offset >= 2)
        {
            MSXCartPtr[media_id][5] = (u8*)src;
            MemoryMap[5] = (u8 *)(MSXCartPtr[media_id][5]) - 0xA000;
            matra_previoius_blocks[3] = block;
        }
    }
    else if (address == 0xBA00) // Offset register
    {
        matra_compilation_offset = value;
        if (matra_compilation_offset >= 2)
        {
            for (int i=0; i<4; i++)
            {
                offset_block = matra_previoius_blocks[i] + (matra_compilation_offset-2);
                msx_offset = offset_block * MyMedia[media_id].blockSize;
                src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));
                MSXCartPtr[media_id][2+i] = (u8*)src;
                MemoryMap[2+i] = (u8 *)(MSXCartPtr[media_id][2+i]) - (0x2000 * (2+i));
            }
        }
    }
}

void __attribute__((noinline)) HandleGeneric16k(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

    switch (address >> 14)
    {
        case 0:
            MSXCartPtr[media_id][0] = (u8*)src;
            MSXCartPtr[media_id][1] = (u8*)src + 0x2000;
            if (bCartInPage[address>>14])
            {
                MemoryMap[0] = (u8 *)(MSXCartPtr[media_id][0]) - 0x0000;
                MemoryMap[1] = (u8 *)(MSXCartPtr[media_id][1]) - 0x2000;
            }
            break;
        case 1:
            MSXCartPtr[media_id][2] = (u8*)src;
            MSXCartPtr[media_id][3] = (u8*)src + 0x2000;
            if (bCartInPage[address>>14])
            {
                MemoryMap[2] = (u8 *)(MSXCartPtr[media_id][2]) - 0x4000;
                MemoryMap[3] = (u8 *)(MSXCartPtr[media_id][3]) - 0x6000;
            }
            break;
        case 2:
            MSXCartPtr[media_id][4] = (u8*)src;
            MSXCartPtr[media_id][5] = (u8*)src + 0x2000;
            if (bCartInPage[address>>14])
            {
                MemoryMap[4] = (u8 *)(MSXCartPtr[media_id][4]) - 0x8000;
                MemoryMap[5] = (u8 *)(MSXCartPtr[media_id][5]) - 0xA000;
            }
            break;
        case 3:
            MSXCartPtr[media_id][6] = (u8*)src;
            MSXCartPtr[media_id][7] = (u8*)src + 0x2000;
            if (bCartInPage[address>>14])
            {
                MemoryMap[6] = (u8 *)(MSXCartPtr[media_id][6]) - 0xC000;
                MemoryMap[7] = (u8 *)(MSXCartPtr[media_id][7]) - 0xE000;
            }
            break;
    }
}

void __attribute__((noinline)) HandleGameMaster2(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

    if ((address & 0xE000) == 0x6000)  // Bank 2 (0x6000-0x7FFF)
    {
        if (value & 0x10) // SRAM Enabled?
        {
            int sram_offset = (value & 0x20) ? 0x2000 : 0x0000; // SRAM Page 1 or 2
            MSXCartPtr[media_id][3] = (u8 *)SRAM_Memory + sram_offset;
        }
        else
        {
            MSXCartPtr[media_id][3] = (u8*)src;  // Main ROM
        }
        MemoryMap[3] = (u8 *)(MSXCartPtr[media_id][3]) - 0x6000;
    }
    else if ((address & 0xE000) == 0x8000)  // Bank 3 (0x8000-0x9FFF)
    {
        if (value & 0x10) // SRAM Enabled?
        {
            int sram_offset = (value & 0x20) ? 0x2000 : 0x0000; // SRAM Page 1 or 2
            MSXCartPtr[media_id][4] = (u8 *)SRAM_Memory + sram_offset;
        }
        else
        {
            MSXCartPtr[media_id][4] = (u8*)src;  // Main ROM
        }
        MemoryMap[4] = (u8 *)(MSXCartPtr[media_id][4]) - 0x8000;
    }
    else if ((address & 0xE000) == 0xA000)  // Bank 4 (0xA000-0xBFFF)
    {
        // ----------------------------------------------------------------------------
        // If the SRAM was enabled on this bank and we are writing to the Bxxx area...
        // ----------------------------------------------------------------------------
        if (sram_write_enabled_a && (address >= 0xB000))
        {
            int sram_offset = (sram_write_enabled_a & 0x20) ? 0x2000 : 0x0000; // SRAM Page 1 or 2
            SRAM_Memory[(address & 0x0FFF) + 0x0000 + sram_offset] = value;    // Main SRAM
            SRAM_Memory[(address & 0x0FFF) + 0x1000 + sram_offset] = value;    // Mirror (only 4K presents itself)
            sram_show_status = 6; // Show SRAM icon and persist to SD card...
        }
        else
        {
            if (value & 0x10) // SRAM Enabled?
            {
                int sram_offset = (value & 0x20) ? 0x2000 : 0x0000; // SRAM Page 1 or 2
                MSXCartPtr[media_id][5] = (u8 *)SRAM_Memory + sram_offset;
                sram_write_enabled_a = value;   // This bank allows writing...
            }
            else
            {
                MSXCartPtr[media_id][5] = (u8*)src;  // Main ROM
                sram_write_enabled_a = 0;   // No SRAM write when ROM paged in
            }
            MemoryMap[5] = (u8 *)(MSXCartPtr[media_id][5]) - 0xA000;
        }
    }
}

// ---------------------------------------------------------------------
// Lode Runner maps with writes to 0x0000 but it's even more unusual
// in that it doesn't care if the cart is mapped into view as it will
// respond to any write to 0x0000 no matter what.
// ---------------------------------------------------------------------
void __attribute__((noinline)) HandleSuperLodeRunner(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

    if (address == 0x0000)
    {
        MSXCartPtr[media_id][4] = (u8*)src;
        MSXCartPtr[media_id][5] = (u8*)src+0x2000;

        if (bCartInPage[2]) // If the cart is mapped in, make the change effective now
        {
            MemoryMap[4] = MSXCartPtr[media_id][4] - 0x8000;
            MemoryMap[5] = MSXCartPtr[media_id][5] - 0xA000;
        }
    }
}

// -------------------------------------------------------------------------------------
// This is really the same as ASCII-8 with 8K of SRAM but it uses bit 7 for SRAM access
// -------------------------------------------------------------------------------------
#define SRAM_ENABLE_BIT_WIZARDRY    0x80
void __attribute__((noinline)) HandleWizardry(u8 media_id, u16 address, u8 value)
{
    u32 block = (value & MyMedia[media_id].mapperMask);
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u32 *src = (u32*)((u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB/2) * 1024) : 0));

    if ((address & 0xF800) == 0x6000)
    {
        if (value & SRAM_ENABLE_BIT_WIZARDRY) // Is SRAM enabled?
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
        if (value & SRAM_ENABLE_BIT_WIZARDRY) // Is SRAM enabled?
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
        if (value & SRAM_ENABLE_BIT_WIZARDRY) // Is SRAM enabled?
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
        if (value & SRAM_ENABLE_BIT_WIZARDRY) // Is SRAM enabled?
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

// -------------------------------------------------------------------
// The FM-PAC presents 8K of SRAM for a game to use to save progress.
// This handler is called when any byte in the 0x4000-0x5FFF range is
// written and will determine if/when the SRAM is available.
// -------------------------------------------------------------------
#define FMPAC_KEY1 0x4D
#define FMPAC_KEY2 0x69
void __attribute__((noinline)) HandleFMPAC(u8 media_id, u16 address, u8 value)
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
            MSXCartPtr[MEDIA_CART2][2] = (u8*)MSXBios_FMPAC;
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
            MSXCartPtr[MEDIA_CART2][2] = (u8*)MSXBios_FMPAC;
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

void __attribute__((noinline)) HandleSCCPlusModeRegister(u8 value)
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
        if (!msx_scc_capable_game)
        {
            msx_scc_capable_game = true;                 // SCC sound - set a flag so we process this special sound chip for this game
            bClickRemoveNeeded = 1;
        }
    }

    // Nothing else to do here - MSXCartPtr[MEDIA_CART2][]/MemoryMap[] already hold the last
    // selected page for every window; Bit4/Bits0-2 only change how WRITES to
    // that window get interpreted, which cpu_writemem16 checks live below.
}

void __attribute__((noinline)) HandleSCCPlus(u8 media_id, u16 address, u8 value)
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

// --------------------------------------------------------------------
// Handle writes to the Disk Controller (FDC) memory mapped registers.
// --------------------------------------------------------------------
void __attribute__((noinline)) HandleDiskControl(u16 address, u8 value)
{
    if (address <= 0x7FFB) fdc_write(address & 3, value);
    if (address == 0x7FFC) fdc_setSide((value & 1) ? 1:0);  // Side: [xxxxxxxS]
    if (address == 0x7FFD) fdc_setDrive((value & 1) ? 1:0); // Drive: [xxxxxxxD]
}

typedef void (*MapperWriteHandler)(u8 media_id, u16 address, u8 value);

static MapperWriteHandler const mapperWriteHandlers[END_MAPPERS] __attribute__((section(".dtcm"))) =
{
    [KON8]              = HandleKonami8,
    [ASC8]              = HandleAscii8,
    [SCC8]              = HandleKonamiSCC8,
    [ASC16]             = HandleAscii16,
    [GENERIC_8K]        = HandleGeneric8k,
    [GENERIC_16K]       = HandleGeneric16k,
    [XBLAM]             = HandleXBlam,
    [SUPERLR]           = HandleSuperLodeRunner,
    [XEVIOUS]           = HandleXevious,
    [ASC8SRAM8]         = HandleAscii8_SRAM8,
    [ASC8SRAM2]         = HandleAscii8_SRAM2,
    [ASC16SRAM2]        = HandleAscii16_SRAM2,
    [ASC16SRAM8]        = HandleAscii16_SRAM8,
    [RTYPE]             = HandleRType,
    [GAME_MASTER2]      = HandleGameMaster2,
    [MAJUT]             = HandleMajut,
    [WIZARDRY]          = HandleWizardry,
    [KOEISRAM32]        = HandleKoei_SRAM32K,
    [MANBOW2]           = HandleSCCFlash,
    [HAMARAJANIGHT]     = HandleSCCFlash,
    [RBSC_FLASH]        = HandleSCCFlash,
    [MATRACOMP]         = HandleMatraCompilation,
    [MSXDOS2]           = HandleMSXDOS2,
    [MEGA_SCC]          = HandleMegaSCC,
    [FMPAC_SRAM]        = HandleFMPAC,
    [SCCPLUS_RAM]       = HandleSCCPlus,
};

// ---------------------------------------------------------------------
// Write memory handles both normal writes and bankswitched since
// write is much less common than reads... All of the MSX cart mappers
// are vectored here so we can handle all the bank switching, etc.
// ---------------------------------------------------------------------
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
        HandleDiskControl(address, value);
    }
    else if (bCartInPage[address>>14]) // Is there a Cartridge mapped into the write address area?
    {
        u8 media_id = bCartInPage[address>>14] - 1;     // Is this MEDIA_CART1 or MEDIA_CART2?
        u8 mapper_type = MyMedia[media_id].mapperType;  // And what type of CART is it?

        // -----------------------------------------------------------------------
        // Each cartridge mapper is different - call into the appropriate handler.
        // A table-lookup here keeps this execution path relatively deterministic
        // no matter how many mappers we have.
        // -----------------------------------------------------------------------
        if (mapperWriteHandlers[mapper_type] != NULL)
        {
            mapperWriteHandlers[mapper_type](media_id, address, value);
        }
    }
    else if ((special_memory_access & SPEC_MEM_SUPERLR_ACTIVE) && (address == 0x0000))
    {
        u8 media_id = MEDIA_CART1; // We only handle Super Lode Runner in CART1
        
        // ------------------------------------------------------------------------------
        // In theory, the write to 0x0000 can come with any slot mapped in anywhere
        // but this implementation requires that RAM not be mapped into page 0 or
        // else we would never get to this check. We could move this check further
        // up but I don't want one game impacting 99% of games with more common mappers.
        // So far with testing, this implementation works fine for SuperLodeRunner.
        // ------------------------------------------------------------------------------
        HandleSuperLodeRunner(media_id, address, value);
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
