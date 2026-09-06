// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave (Phoenix-Edition),
// Alekmaul (original port) and Marat Fayzullin (ColEM core) are thanked profusely.
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
#include "../../printf.h"
#include "../scc/SCC.h"

u8  msx_sram_at_8000        __attribute__((section(".dtcm"))) = 0;
u8  msx_scc_enable          __attribute__((section(".dtcm"))) = 0;
u8  msx_scc_capable_game    __attribute__((section(".dtcm"))) = 0;

extern u8 msx_subslot;

u8 SubslotRead(u16 address)
{
    if (myConfig.machineType == MACHINE_MSX2_B) // Type B... Expanded Slot 0
    {
        if (((Port_PPI_A>>6) & 0x03) == 0x00) // Is Slot 0 mapped in? That's the expanded slot.
        {
          return ~msx_subslot; // Compliment is returned
        }
    }
    else // Type A... Expanded Slot 3
    {
        if (((Port_PPI_A>>6) & 0x03) == 0x03) // Is Slot 3 mapped in? That's the expanded slot.
        {
          return ~msx_subslot; // Compliment is returned
        }
    }
    return *(MemoryMap[address>>13] + (address&0x1FFF));
}

// ----------------------------------------------------------------
// All memory fetches run through this except OP codes which are 
// read directly from memory. 
// ----------------------------------------------------------------
ITCM_CODE u8 cpu_readmem16(u16 address) 
{
    // Everything in this block is accessing high-memory...
    if (address & 0x8000)
    {
        if ((address == 0xFFFF)) // Subslot check... only for Slot 0 where the BIOS / Extended BIOS sits
        {
            if (myConfig.machineType != MACHINE_MSX1)
            {
                return SubslotRead(address);
            }
        }
        else if (msx_sram_at_8000) // Don't need to check msx_mode as this can only be true in that mode
        {
            if (address <= 0xBFFF) // Between 0x8000 and 0xBFFF
            {
              return SRAM_Memory[address&0x3FFF];
            }
        }
        // ----------------------------------------------------
        // Are we reading from the SCC chip memory mapped area?
        // ----------------------------------------------------
        else if (msx_scc_enable && ((address & 0xF800) == 0x9800))
        {
             if (bCartInSegment[2])
             {
                // 1. Only addresses 0x9800 to 0x987F actually read from the SCC Wave RAM
                if (address >= 0x9800 && address <= 0x987F)
                {
                    return SCCRead(address, &mySCC); 
                }                
            }
        }
    }
    
    // Otherwise normal read - just index into the 8K memory block and fetch the byte...
    return *(MemoryMap[address>>13] + (address&0x1FFF));
}


// -----------------------------------------------------------------------
// Zemina 8K mapper:
//Page (8kB)    Switching address   Initial segment
//4000h~5FFFh (mirror: C000h~DFFFh) 4000h (mirrors: 4001h~5FFFh)    0
//6000h~7FFFh (mirror: E000h~FFFFh) 6000h (mirrors: 6001h~7FFFh)    1
//8000h~9FFFh (mirror: 0000h~1FFFh) 8000h (mirrors: 8001h~9FFFh)    2
//A000h~BFFFh (mirror: 2000h~3FFFh) A000h (mirrors: A001h~BFFFh)    3
// -----------------------------------------------------------------------
void HandleZemina8K(u32* src, u8 block, u16 address)
{
    if (bCartInSegment[1] && (address >= 0x4000) && (address < 0x6000))
    {
        MSXCartPtr[2] = (u8*)src;  // Main ROM
        MSXCartPtr[6] = (u8*)src;  // Mirror
        MemoryMap[2] = (u8 *)(MSXCartPtr[2]);
    }
    else if (bCartInSegment[1] && (address >= 0x6000) && (address < 0x8000))
    {
        MSXCartPtr[3] = (u8*)src;  // Main ROM
        MSXCartPtr[7] = (u8*)src;  // Mirror
        MemoryMap[3] = (u8 *)(MSXCartPtr[3]);
    }
    else if (bCartInSegment[2] && (address >= 0x8000) && (address < 0xA000))
    {
        MSXCartPtr[4] = (u8*)src;  // Main ROM
        MSXCartPtr[0] = (u8*)src;  // Mirror                            
        MemoryMap[4] = (u8 *)(MSXCartPtr[4]);
    }
    else if (bCartInSegment[2] && (address >= 0xA000) && (address < 0xC000))
    {
        MSXCartPtr[5] = (u8*)src;  // Main ROM
        MSXCartPtr[1] = (u8*)src;  // Mirror                            
        MemoryMap[5] = (u8 *)(MSXCartPtr[5]);
    }
}    

// -------------------------------------------------------------------------
// The ZENMIA 16K Mapper:
// 4000h~7FFFh  via writes to 4000h-7FFF
// 8000h~BFFFh  via writes to 8000h-BFFF
// -------------------------------------------------------------------------
void HandleZemina16K(u32* src, u8 block, u16 address)
{
    if (bCartInSegment[1] && (address >= 0x4000) && (address < 0x8000))
    {
        MSXCartPtr[2] = (u8*)src;
        MSXCartPtr[3] = (u8*)src+0x2000;
        MemoryMap[2] = (u8 *)(MSXCartPtr[2]);
        MemoryMap[3] = (u8 *)(MSXCartPtr[3]);
        // Mirrors
        MSXCartPtr[6] = (u8*)src;
        MSXCartPtr[7] = (u8*)src+0x2000;
        if (bCartInSegment[3]) 
        {
            MemoryMap[6] = (u8 *)(MSXCartPtr[6]);
            MemoryMap[7] = (u8 *)(MSXCartPtr[7]);
        }
    }
    else if (bCartInSegment[1] && (address >= 0x8000) && (address < 0xC000))
    {
        MSXCartPtr[4] = (u8*)src;
        MSXCartPtr[5] = (u8*)src+0x2000;
        // Mirrors
        MSXCartPtr[0] = (u8*)src;
        MSXCartPtr[1] = (u8*)src+0x2000;
        if (bCartInSegment[2])
        {
            MemoryMap[4] = (u8 *)(MSXCartPtr[4]);
            MemoryMap[5] = (u8 *)(MSXCartPtr[5]);
        }
        if (bCartInSegment[0]) 
        {
            MemoryMap[0] = (u8 *)(MSXCartPtr[0]);
            MemoryMap[1] = (u8 *)(MSXCartPtr[1]);
        }            
    }
}    

void HandleKonamiSCC8(u32* src, u8 block, u16 address, u8 value)
{
    // Mask the address to cover the 2KB Konami mapper register windows
    u16 reg_address = address & 0xF800;

    if (reg_address == 0x5000)
    {
        MSXCartPtr[2] = (u8*)src; 
        MemoryMap[2] = (u8 *)(MSXCartPtr[2]);
    }
    else if (reg_address == 0x7000)
    {
        MSXCartPtr[3] = (u8*)src; 
        MemoryMap[3] = (u8 *)(MSXCartPtr[3]);
    }
    else if (reg_address == 0x9000)
    {
        if ((value & 0x3F) == 0x3F) 
        {
            msx_scc_enable = true;
            msx_scc_capable_game = true;
        }
        else
        {
            msx_scc_enable = false;
        }

        MSXCartPtr[4] = (u8*)src; 
        MemoryMap[4] = (u8 *)(MSXCartPtr[4]);
    }
    else if (reg_address == 0xB000)
    {
        MSXCartPtr[5] = (u8*)src; 
        MemoryMap[5] = (u8 *)(MSXCartPtr[5]);
    }
}

// -------------------------------------------------------------------------
// The ASCII 16K Mapper:
// 4000h~7FFFh  via writes to 6000h
// 8000h~BFFFh  via writes to 7000h or 77FFh
// -------------------------------------------------------------------------
void HandleAscii16K(u32* src, u8 block, u16 address)
{
    if (bCartInSegment[1] && (address & 0xF800) == 0x6000)
    {
        MSXCartPtr[2] = (u8*)src;
        MSXCartPtr[3] = (u8*)src+0x2000;
        MemoryMap[2] = MSXCartPtr[2];
        MemoryMap[3] = MSXCartPtr[3];
    }
    else if (bCartInSegment[1] && (address & 0xF800) == 0x7000)
    {
        // ---------------------------------------------------------------------------------------------------------
        // Check if we have an SRAM capable game - those games (e.g. Hydlide II) use the block at 0x8000 for SRAM.
        // In theory this 2K or 8K of SRAM is mirrored but we don't worry about it - just allow writes.
        // ---------------------------------------------------------------------------------------------------------
        if (msx_sram_enabled && (block == msx_sram_enabled))
        {
            msx_sram_at_8000 = true;
        }
        else
        {
            msx_sram_at_8000 = false;
            MSXCartPtr[4] = (u8*)src;
            MSXCartPtr[5] = (u8*)src+0x2000;
            if (bCartInSegment[2]) 
            {
                MemoryMap[4] = MSXCartPtr[4];
                MemoryMap[5] = MSXCartPtr[5];
            }
        }
    }
}

void HandleXevious(u32* src, u8 block, u16 address)
{
    if (bCartInSegment[1] && (address >= 0x6000) && (address <= 0x67FF))
    {
        MSXCartPtr[2] = (u8*)src;
        MSXCartPtr[3] = (u8*)src+0x2000;
        MemoryMap[2] = MSXCartPtr[2];
        MemoryMap[3] = MSXCartPtr[3];
    }
    else if (bCartInSegment[1] && (address >= 0x7000) && (address <= 0x77FF))
    {
        MSXCartPtr[4] = (u8*)src;
        MSXCartPtr[5] = (u8*)src+0x2000;
        if (bCartInSegment[2]) 
        {
            MemoryMap[4] = MSXCartPtr[4];
            MemoryMap[5] = MSXCartPtr[5];
        }
    }
}

void HandleSuperLodeRunner(u32* src, u8 block, u16 address)
{
    MSXCartPtr[4] = (u8*)src;
    MSXCartPtr[5] = (u8*)src+0x2000;

    if (bCartInSegment[2])
    {
        MemoryMap[4] = MSXCartPtr[4];
        MemoryMap[5] = MSXCartPtr[5];
    }
}

void SubslotWrite(u8 value)
{
    if (myConfig.machineType == MACHINE_MSX2_B) // Type B... Expanded Slot 0
    {
        if (((Port_PPI_A>>6) & 0x03) == 0x00) // Is Slot 0 mapped into upper memory?
        {
            msx_subslot = value;
            cpu_writeport_msx(0xA8, Port_PPI_A); // Enable the new map...
        }
    }
    else // Must be MACHINE_MSX2_A
    {
        if (((Port_PPI_A>>6) & 0x03) == 0x03) // Is Slot 3 mapped into upper memory?
        {
            msx_subslot = value;
            cpu_writeport_msx(0xA8, Port_PPI_A); // Enable the new map...
        }
    }
}

// ------------------------------------------------------------------
// Write memory handles both normal writes and bankswitched since
// write is much less common than reads...   We handle the MSX
// Konami 8K, SCC and ASCII 8K mappers directly here for max speed.
// ------------------------------------------------------------------
ITCM_CODE void cpu_writemem16(u8 value,u16 address) 
{
    if ((address == 0xFFFF)) // Subslot check... only for Slot 3 where Extended BIOS and Disk Controller sits
    {
        if (myConfig.machineType != MACHINE_MSX1)
        {
            SubslotWrite(value);
            return;
        }
    }

    // -------------------------------------------------------
    // First see if this is a write to a RAM enabled slot...
    // -------------------------------------------------------
    if (bRAMInSegment[0] && (address < 0x4000))
    {
        *(MemoryMap[address>>13] + (address&0x1FFF))=value;  // Allow write - this is a RAM mapped slot
    }
    else if (bRAMInSegment[1] && (address >= 0x4000) && (address <= 0x7FFF))
    {
        *(MemoryMap[address>>13] + (address&0x1FFF))=value;  // Allow write - this is a RAM mapped slot
    }
    else if ((bRAMInSegment[2] || msx_sram_at_8000) && (address >= 0x8000) && (address <= 0xBFFF))
    {
        if (msx_sram_at_8000) 
        {
            SRAM_Memory[address&0x3FFF] = value;   // Write SRAM area
            write_NV_counter = 4;                  // This will back the EE in 4 seconds of non-activity on the SRAM
        }
        else *(MemoryMap[address>>13] + (address&0x1FFF))=value;  // Allow write - this is a RAM mapped slot
    }
    else if ((bRAMInSegment[3] == 1) && (address >= 0xC000)) // A value of 1 here means we can write to the entire 16K page
    {
        *(MemoryMap[address>>13] + (address&0x1FFF))=value;  // Allow write - this is a RAM mapped slot
    }
    else    // Check for MSX Mappers Mappers
    {
        if (mapperMask)
        {
            // -------------------------------------------------------------
            // Compute the block and offset of the new memory and we 
            // can map it into place... this is fast since we are just
            // moving pointers around and not trying to copy memory blocks.
            // -------------------------------------------------------------
            u32 block = (value & mapperMask);
            u32 msx_offset = block * msx_block_size;
            u32 *src = (u32*)((u8*)ROM_Memory + msx_offset);

            // ---------------------------------------------------------------------------------
            // The Konami 8K Mapper without SCC:
            // 4000h-5FFFh - fixed ROM area (not swappable)
            // 6000h~7FFFh (mirror: E000h~FFFFh)    6000h (mirrors: 6001h~7FFFh)    1
            // 8000h~9FFFh (mirror: 0000h~1FFFh)    8000h (mirrors: 8001h~9FFFh)    Random
            // A000h~BFFFh (mirror: 2000h~3FFFh)    A000h (mirrors: A001h~BFFFh)    Random
            // ---------------------------------------------------------------------------------
            if (mapperType == KON8)
            {
                if (bCartInSegment[1] && (address == 0x4000))
                {
                    MSXCartPtr[2] = (u8*)src;  // Main ROM
                    MemoryMap[2] = (u8 *)(MSXCartPtr[2]);
                }
                else if (bCartInSegment[1] && (address == 0x6000))
                {
                    MSXCartPtr[3] = (u8*)src;  // Main ROM
                    MemoryMap[3] = (u8 *)(MSXCartPtr[3]);
                }
                else if (bCartInSegment[2] && (address == 0x8000))
                {
                    MSXCartPtr[4] = (u8*)src;  // Main ROM
                    MemoryMap[4] = (u8 *)(MSXCartPtr[4]);
                }
                else if (bCartInSegment[2] && (address == 0xA000))
                {
                    MSXCartPtr[5] = (u8*)src;  // Main ROM
                    MemoryMap[5] = (u8 *)(MSXCartPtr[5]);
                }
            }
            else if (mapperType == ASC8)
            {
                // -------------------------------------------------------------------------
                // The ASCII 8K Mapper:
                // 4000h~5FFFh (mirror: C000h~DFFFh)    6000h (mirrors: 6001h~67FFh)    0
                // 6000h~7FFFh (mirror: E000h~FFFFh)    6800h (mirrors: 6801h~68FFh)    0
                // 8000h~9FFFh (mirror: 0000h~1FFFh)    7000h (mirrors: 7001h~77FFh)    0
                // A000h~BFFFh (mirror: 2000h~3FFFh)    7800h (mirrors: 7801h~7FFFh)    0     
                // -------------------------------------------------------------------------
                if (bCartInSegment[1] && (address >= 0x6000) && (address < 0x6800))
                {
                    MSXCartPtr[2] = (u8*)src;  // Main ROM
                    MemoryMap[2] = MSXCartPtr[2];
               }
                else if (bCartInSegment[1] && (address >= 0x6800)  && (address < 0x7000))
                {
                    MSXCartPtr[3] = (u8*)src;  // Main ROM
                    MemoryMap[3] = MSXCartPtr[3];
                }
                else if (bCartInSegment[1] && (address >= 0x7000)  && (address < 0x7800))
                {
                    if (msx_sram_enabled && (block == msx_sram_enabled))
                    {
                        msx_sram_at_8000 = true;
                    }
                    else
                    {
                        msx_sram_at_8000 = false;
                        MSXCartPtr[4] = (u8*)src;  // Main ROM
                        if (bCartInSegment[2])
                        {
                            MemoryMap[4] = MSXCartPtr[4];
                        }
                    }
                }
                else if (bCartInSegment[1] && (address >= 0x7800) && (address < 0x8000))
                {
                    if (msx_sram_enabled && (block == msx_sram_enabled))
                    {
                        msx_sram_at_8000 = true;
                    }
                    else
                    {
                        msx_sram_at_8000 = false;
                        MSXCartPtr[5] = (u8*)src;  // Main ROM
                        if (bCartInSegment[2]) 
                        {
                            MemoryMap[5] = MSXCartPtr[5];
                        }
                    }
                }
            }
            else if (mapperType == SCC8)
            {
                // ----------------------------------------------------
                // Are we writing to the SCC chip memory mapped area?
                // ----------------------------------------------------
                if (msx_scc_enable && ((address & 0xF800) == 0x9800))
                {
                     SCCWrite(value, address, &mySCC);
                }
                
                HandleKonamiSCC8(src, block, address, value);
            }
            else if (mapperType == ASC16)
            {
                HandleAscii16K(src, block, address);
            }
            else if (mapperType == ZEN8)
            {
                HandleZemina8K(src, block, address);
            }
            else if (mapperType == ZEN16)
            {
                HandleZemina16K(src, block, address);
            }
            else if (mapperType == XEVIOUS)
            {
                HandleXevious(src, block, address);
            }
            else if (mapperType == SUPERLR)
            {
                if (address <= 0x3FFF) 
                {
                    HandleSuperLodeRunner(src, block, address);
                }
            }
            else if (mapperType == XBLAM)
            {
                if (address == 0x4045)
                {
                    MSXCartPtr[4] = (u8*)src;          // Main ROM at 8000
                    MSXCartPtr[5] = (u8*)src+0x2000;   // Main ROM at A000                  
                    if (bCartInSegment[2]) 
                    {
                        MemoryMap[4] = MSXCartPtr[4];
                        MemoryMap[5] = MSXCartPtr[5];
                    }
                }
            }                
        }
        else if (mapperType == FAKE_SCC8)
        {
            if (bCartInSegment[2])
            {
                // ----------------------------------------------------
                // Are we writing to the SCC chip memory mapped area?
                // ----------------------------------------------------
                if (msx_scc_enable && ((address & 0xF800) == 0x9800))
                {
                     SCCWrite(value, address, &mySCC);
                }
                else if (address == 0x9000)
                {
                    if ((value & 0x3F) == 0x3F) 
                    {
                        msx_scc_enable = true;
                    }
                    else
                    {
                        msx_scc_enable = false;
                    }
                }
            }
        }
    }
}

// -----------------------------------------------------------------
// Reset a few key variables needed for proper Z80 interface use...
// -----------------------------------------------------------------
void Z80_Interface_Reset(void) 
{
  CPU.CycleDeficit      = 0;
  msx_sram_at_8000      = 0;
  msx_scc_enable        = 0;
  msx_scc_capable_game  = 0;
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
