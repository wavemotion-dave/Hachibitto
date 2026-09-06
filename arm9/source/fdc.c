// =====================================================================================
// Copyright (c) 2021-2026 Dave Bernazzani (wavemotion-dave)
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
#include <fcntl.h>

#include "Hachibitto.h"
#include "fdc.h"
#include "CRC32.h"
#include "cpu/z80/Z80_interface.h"
#include "MSX_generic.h"
#include "printf.h"

// -----------------------------------------------------------------------------------------
// WD2793-only Floppy Drive Controller for the CDX2-FDD interface (MSX2-compatible).
// Handles basic seeks, sector reads, sector writes, and track formatting -- enough
// for the vast majority of MSX2 .dsk-based disk games to play properly.
// -----------------------------------------------------------------------------------------
struct FDC_t            FDC;
struct FDC_GEOMETRY_t   Geom;

extern u8 disk_unsaved_data[];

// ---------------------------------------------------------------------------
// WD2793 Status Register Bit Definitions
// ---------------------------------------------------------------------------
#define ST_BUSY          0x01
#define ST_INDEX_DRQ     0x02   // Index Pulse (Type I) / Data Request (Type II/III)
#define ST_TRACK0        0x04   // 1 = AT track 0 (Type I) / Lost Data (Type II/III)
#define ST_CRC_ERROR     0x08
#define ST_RNF           0x10   // Seek Error (Type I) / Record Not Found (Type II/III)
#define ST_HEAD_ENGAGED  0x20   // Head Engaged (Type I) / Record Type (Type II/III)
#define ST_WRITE_PROT    0x40
#define ST_NOT_READY     0x80   // 1 = Not Ready, 0 = Ready

// ---------------------------------------------------------------------------
// Cycle-accurate FDC timing. CPU.TotalCycles is a free-running Z80 T-state
// counter that never resets, so we can timestamp "when is the next byte/step
// allowed" and compare against it on every access -- pacing no longer depends
// on how often (or how unevenly) a given disk driver polls the FDC ports.
// ---------------------------------------------------------------------------
#define FDC_CPU_CLOCK        3579545                                  // Z80 clock, NTSC
#define FDC_DATA_RATE_BPS    250000                                   // MSX double-density (MFM)
#define FDC_CYCLES_PER_BYTE  (FDC_CPU_CLOCK / (FDC_DATA_RATE_BPS/8))  // ~114 T-states/byte
#define FDC_STEP_CYCLES      (FDC_CYCLES_PER_BYTE * 8)                // Seek/step settle time -- tune to taste

void fdc_debug(u8 bWrite, u8 addr, u8 data)
{
#if 0 // Set to 1 to enable debug
    char tmpBuf[33];
    static u8 line=0;
    static u8 idx=0;

    if (bWrite)
        sprintf(tmpBuf, "W%04d %d=%02X  %02X %02X %02X %d %02X %d", idx++, addr, data, FDC.status, FDC.track, FDC.sector, FDC.side, FDC.data, FDC.drive);
    else
        sprintf(tmpBuf, "R%04d %d     %02X %02X %02X %d %02X %d", idx++, addr, FDC.status, FDC.track, FDC.sector, FDC.side, FDC.data, FDC.drive);
    DSPrint(0,5+line++, 7, tmpBuf);
    line = line % 19;
#endif
}

// -------------------------------------------------------------------------------------------------------------------------
// Read one track worth of sectors in proper sector order (0..N) and buffer that in our track buffer for easy read/write.
// -------------------------------------------------------------------------------------------------------------------------
void fdc_buffer_track(void)
{
    u16 track_len = Geom.sectorSize*Geom.sectors;
    u8 *diskPtr = (FDC.drive == 0) ? Geom.disk0 : Geom.disk1;
    if (diskPtr)
        memcpy(FDC.track_buffer, diskPtr + (((Geom.sides * FDC.track) + FDC.side) * track_len), track_len);
    FDC.track_dirty[FDC.drive] = 0;
}

// ---------------------------------------------------------------------------------------------------
// If any sector in our track buffer has changed, write all sectors back out to the main disk memory.
// ---------------------------------------------------------------------------------------------------
void fdc_flush_track(void)
{
    if (FDC.track_dirty[FDC.drive])
    {
        u16 track_len = Geom.sectorSize*Geom.sectors;
        u8 *diskPtr = (FDC.drive == 0) ? Geom.disk0 : Geom.disk1;
        if (diskPtr)
            memcpy(diskPtr + (((Geom.sides * FDC.track) + FDC.side) * track_len), FDC.track_buffer, track_len);
        FDC.track_dirty[FDC.drive] = 0;
    }
}

// Status Register for WD2793
//   Bit |      Type I      |    Type II    |   Type III    |
//   ----+------------------+---------------+---------------|
//    7  |    Not Ready     | ---------- Not Ready -------- |
//    6  |    Not used.     | -- Disk is write protected. - |
//    5  |  Head Engaged    |    1=Engaged, 0=Not Engaged   |
//    4  | Record not found | ----- Record not found ------ |
//    3  |    CRC error.    | --------- CRC error --------- |
//    2  |   TRACK 0 (1=yes)| ------ Lost data / byte ----- |
//    1  |   Index Pulse    | -------- Data request ------- |
//    0  |       Busy       | ------------ Busy ----------- |

// ---------------------------------------------------------------------------
// LoopFDC() -- call once per scanline (same cadence as Loop9938() etc).
// Generates the Type-I index pulse at a real, scanline-driven rate instead
// of ticking it once per port access.
// ---------------------------------------------------------------------------
#define FDC_SCANLINES_PER_REV   3147   // ~200ms/rev at ~300rpm, NTSC scanline rate
#define FDC_INDEX_PULSE_WIDTH   16     // scanlines the index pulse stays asserted per revolution

void LoopFDC(void)
{
    if (FDC.commandType == 1)   // Index pulse only meaningful in Type-I status format
    {
        if (!(FDC.status & ST_NOT_READY))   // Drive ready
        {
            if (++FDC.indexPulseCounter >= FDC_SCANLINES_PER_REV) FDC.indexPulseCounter = 0;

            if (FDC.indexPulseCounter < FDC_INDEX_PULSE_WIDTH)
                FDC.status |= ST_INDEX_DRQ;
            else
                FDC.status &= ~ST_INDEX_DRQ;
        }
    }
}

void fdc_state_machine(void)
{
    // Cycle-based busy gate: bail out, touching nothing, until the deadline
    // for the current step/byte has actually elapsed.
    if (!(FDC.status & ST_BUSY)) return;
    if ((s32)(CPU.TotalCycles - FDC.cycle_deadline) < 0) return;

    switch(FDC.command & 0xF0)
    {
        case 0x00: // Restore - same as Seek Track except track=0
            FDC.data = 0x00;                            // Data also zeroed here
            // No break
        case 0x10: // Seek Track
            FDC.track = FDC.data;                       // Settle on requested track
            FDC.wait_for_read = 2;                      // No data to transfer
            FDC.status = ST_HEAD_ENGAGED | (FDC.track ? 0x00 : ST_TRACK0);
            break;

        case 0x20: // Step
        case 0x30: // Step
            if (FDC.stepDirection) // Outwards... towards track 0
            {
                if (FDC.track > 0) FDC.track--;
            }
            else // Inwards
            {
                FDC.track++;
            }
            FDC.status = ST_HEAD_ENGAGED | (FDC.track ? 0x00 : ST_TRACK0);
            break;

        case 0x40: // Step in
        case 0x50: // Step in
            FDC.stepDirection = 0; // Step inwards
            FDC.track++;
            FDC.status = ST_HEAD_ENGAGED | (FDC.track ? 0x00 : ST_TRACK0);
            break;

        case 0x60: // Step out
        case 0x70: // Step out
            FDC.stepDirection = 1;  // Step Outwards... towards track 0
            if (FDC.track > 0) FDC.track--;
            FDC.status = ST_HEAD_ENGAGED | (FDC.track ? 0x00 : ST_TRACK0);
            break;

        case 0x80: // Read Sector (single)
        case 0x90: // Read Sector (multiple)
            if (FDC.wait_for_read == 0)
            {
                if (FDC.track_buffer_idx >= FDC.track_buffer_end) // Is there any more data to put out?
                {
                    FDC.status &= ~ST_BUSY;               // Done. No longer busy.
                    FDC.wait_for_read = 2;                // Don't fetch more FDC data
                    FDC.sector_byte_counter = 0;          // And reset our counter
                }
                else
                {
                    FDC.status |= (ST_BUSY | ST_INDEX_DRQ);              // Data Ready and no errors... still busy
                    FDC.data = FDC.track_buffer[FDC.track_buffer_idx++]; // Read data from our track buffer
                    FDC.wait_for_read = 1;                               // Wait for the CPU to fetch the data
                    FDC.cycle_deadline = CPU.TotalCycles + FDC_CYCLES_PER_BYTE;  // Pace the next byte
                    if (++FDC.sector_byte_counter >= Geom.sectorSize)    // Did we cross a sector boundary?
                    {
                        if (FDC.command & 0x10) FDC.sector++;       // Bump the sector number only if multiple sector command
                        FDC.sector_byte_counter = 0;                // And reset our counter
                    }
                }
            }
            break;

        case 0xA0: // Write Sector (single)
        case 0xB0: // Write Sector (multiple)
            if (FDC.wait_for_write == 3)
            {
                FDC.status |= (ST_BUSY | ST_INDEX_DRQ);   // We're good to accept data now
                FDC.wait_for_write = 1;                   // And start looking for data
                FDC.cycle_deadline = CPU.TotalCycles + FDC_CYCLES_PER_BYTE;  // Pace first byte
            }
            else if (FDC.wait_for_write == 0)
            {
                FDC.track_dirty[FDC.drive] = 1;
                disk_unsaved_data[FDC.drive] = 1;
                FDC.track_buffer[FDC.track_buffer_idx++] = FDC.data; // Store CPU byte into our FDC buffer
                if (FDC.track_buffer_idx >= FDC.track_buffer_end)
                {
                    FDC.status &= ~ST_BUSY;              // Done. No longer busy.
                    FDC.wait_for_write = 2;               // Don't write more FDC data
                    FDC.sector_byte_counter = 0;          // And reset our counter
                    fdc_flush_track();                    // Write the buffer back out
                }
                else
                {
                    FDC.status |= (ST_BUSY | ST_INDEX_DRQ);  // Data Ready and no errors... still busy
                    FDC.wait_for_write = 1;                  // Wait for the CPU to give us more data
                    FDC.cycle_deadline = CPU.TotalCycles + FDC_CYCLES_PER_BYTE;  // Pace next byte
                    if (++FDC.sector_byte_counter >= Geom.sectorSize)   // Did we cross a sector boundary?
                    {
                        if (FDC.command & 0x10) FDC.sector++;   // Bump the sector number only if multiple sector command
                        FDC.sector_byte_counter = 0;            // And reset our counter
                    }
                }
            }
            break;

        case 0xF0: // Write Track (format - this is very Einstein specific)
            if (FDC.wait_for_write == 0)
            {
                FDC.wait_for_write = 1;                 // Wait for the CPU to give us more data
                FDC.cycle_deadline = CPU.TotalCycles + FDC_CYCLES_PER_BYTE;  // Pace next byte
                if (FDC.write_track_allowed < 4)
                {
                    FDC.status |= (ST_BUSY | ST_INDEX_DRQ); // More data please!

                    if (FDC.write_track_allowed == 2)
                    {
                        if (++FDC.write_track_byte_counter >= 78) // Allow runout gap of 78x of E5 which is enough....
                        {
                            FDC.status &= ~(ST_BUSY | ST_INDEX_DRQ); // Done, not busy, no more data needed.
                            FDC.write_track_allowed = 4;             // And stop looking for more data
                        }
                    }
                    else if (FDC.write_track_allowed == 1)
                    {
                        disk_unsaved_data[FDC.drive] = 1;
                        FDC.track_dirty[FDC.drive] = 1;
                        FDC.track_buffer[FDC.track_buffer_idx++] = FDC.data; // Store CPU byte into our FDC buffer

                        if (++FDC.sector_byte_counter >= Geom.sectorSize)   // Did we cross a sector boundary?
                        {
                            FDC.sector_byte_counter = 0;        // And reset our counter
                            if (++FDC.sector == Geom.sectors)   // Bump the sector count
                            {
                                fdc_flush_track();              // Write the buffer back out
                                if (FDC.track < Geom.tracks) FDC.track++; else FDC.track=0;
                                FDC.sector = Geom.startSector;
                                FDC.write_track_allowed = 2;
                            }
                            else FDC.write_track_allowed = 0; // Look for 3x F5 followed by FB
                            FDC.write_track_byte_counter=0;
                        }
                    }
                    else
                    {
                        // We're looking for the magic bytes... three F5 bytes followed by an FB to signify start of actual data
                        if (FDC.data == 0xF5) FDC.write_track_byte_counter++;
                        else
                        {
                            if ((FDC.write_track_byte_counter==3) && (FDC.data == 0xFB))
                            {
                                FDC.write_track_allowed=1;
                            }
                            FDC.write_track_byte_counter=0;
                        }
                    }
                }
            }
            break;

        case 0xC0: // Read Address
            FDC.status &= ~ST_BUSY;                        // Not handled yet... just clear busy
            break;
        case 0xD0: // Force Interrupt
            FDC.status = (FDC.track ? 0x00 : ST_TRACK0) | ST_HEAD_ENGAGED;
            break;
        case 0xE0: // Read Track
            FDC.status &= ~ST_BUSY;                        // Not handled yet... just clear busy
            break;
        default: break;
    }
}

//  Address offset      Contains on read    on write
//  ------------------------------------------------------
//         0                 Status         Command
//         1                 ------- Track --------
//         2                 ------- Sector -------
//         3                 ------- Data ---------
u8 fdc_read(u8 addr)
{
    if (FDC.drive >= Geom.drives) return ST_NOT_READY; // Make sure this is a valid drive

    fdc_state_machine();    // Clock the floppy drive controller state machine

    fdc_debug(0, addr, 0);  // Debug the read routine

    switch (addr)
    {
        case 0: return FDC.status;
        case 1: return FDC.track;
        case 2: return FDC.sector;
        case 3:
            FDC.status &= ~ST_INDEX_DRQ;     // Clear Data Available flag
            FDC.wait_for_read = 0;           // Clock in next byte (or end sequence if we're read all there is)
            return FDC.data;                 // Return data to caller
        case 4:
        {
            u8 ret = 0x7f;
            if (FDC.status & ST_BUSY)      ret |= 0x80;
            if (FDC.status & ST_INDEX_DRQ) ret &= ~0x40;
            return ret;
        }
    }

    return ST_NOT_READY;
}


// FDC Commands:
//   I    Restore            0   0   0   0   h   v   r1  r0
//   I    Seek               0   0   0   1   h   v   r1  r0
//   I    Step               0   0   1   u   h   v   r1  r0
//   I    Step in            0   1   0   u   h   v   r1  r0
//   I    Step out           0   1   1   u   h   v   r1  r0
//   II   Read sector        1   0   0   m  h/s  e  0/c  0
//   II   Write sector       1   0   1   m  h/s  e  p/c  a
//   III  Read address       1   1   0   0  h/0  e   0   0
//   III  Read track         1   1   1   0  h/0  e   0   0
//   III  Write track        1   1   1   1  h/0  e  p/0  0
//   IV   Force interrupt    1   1   0   1   i3  i2  i1  i0
void fdc_write(u8 addr, u8 data)
{
    // -------------------------------------------------------
    // Handle the write - most of the time it's a command...
    // -------------------------------------------------------
    switch (addr)
    {
        case 0: if (!(FDC.status & ST_BUSY)) FDC.command = data;  break;
        case 1: if (!(FDC.status & ST_BUSY)) FDC.track   = data;  break;
        case 2: if (!(FDC.status & ST_BUSY)) FDC.sector  = data;  break;
        case 3:
            FDC.data = data;
            FDC.status &= ~ST_INDEX_DRQ;
            FDC.wait_for_write = 0;
            break;
        case 4: //  D4h      W    Drive (bit 1), Side (bit 4), Motor (bit 5)
            FDC.drive = (data & 0x01 ? 0:1);
            FDC.side  = (data & 0x10 ? 1:0);
            FDC.motor = (data & 0x20 ? 1:0);
            break;
        default: break;
    }

    fdc_debug(1, addr, data);   // Debug the write routine

    if (FDC.drive >= Geom.drives) return; // Make sure this is a valid drive before we process anything below...

    // ---------------------------------------------------------
    // If command.... we must set the right bits in the status
    // register.
    // ---------------------------------------------------------
    if (addr == 0x00)
    {
        // First check if we are busy... if so, only a Force Interrupt can override us
        if (FDC.status & ST_BUSY)
        {
            if ((data & 0xF0) != 0xD0)     // Only a Force Interrupt can override busy
            {
                return;                    // We were given a command while busy - ignore it.
            }
            else FDC.command = data;       // Otherwise the last command was a Force Interrupt
        }

        if ((data & 0x80) == 0) // Is this a Type-I command?
        {
            FDC.commandType = 1;                            // Type-I command
            FDC.status = (data & 0x08) ? (ST_BUSY | ST_HEAD_ENGAGED) : ST_BUSY; // Busy, check if we engage the head
            FDC.cycle_deadline = CPU.TotalCycles + FDC_STEP_CYCLES;  // Paces Restore/Seek/Step/StepIn/StepOut alike

            if ((data&0xF0) == 0x00)                        // Restore (Seek Track 0)
            {
                FDC.status |= (FDC.track ? 0x00 : ST_TRACK0);   // Check if we are track 0
                FDC.wait_for_read = 2;                      // Not fetching any data
                FDC.wait_for_write = 2;                     // Not writing any data
                FDC.stepDirection = 1;                      // Step towards track 0
            }
            else if ((data&0xF0) == 0x10)                   // Seek Track
            {
                FDC.status |= (FDC.track ? 0x00 : ST_TRACK0);   // Check if we are track 0
                FDC.wait_for_read = 2;                      // Not fetching any data
                FDC.wait_for_write = 2;                     // Not storing any data
            }
            // Step / Step-In / Step-Out (0x20-0x70) need no extra setup here
            // beyond the cycle_deadline and status bits already set above.
        }
        else    // Type II or III command (essentially same handling for status) - we also handle Type IV 'Force Interrupt' here
        {
            FDC.commandType = (data & 0x40) ? 3:2;          // Type-II or Type-III
            FDC.status = ST_BUSY;                           // All Type-II or III set busy and we assume drive is ready

            if ((data & 0xF0) == 0xD0)     // Force Interrupt... ensure we are back to Type-I status...
            {
                FDC.status = (FDC.track ? 0x00 : ST_TRACK0) | ST_HEAD_ENGAGED;
                fdc_flush_track();                            // In case any data changed, write it back to main memory
                FDC.wait_for_read = 2;                        // Not fetching any data
                FDC.wait_for_write = 2;                       // Not writing any data
                FDC.commandType = 1;                          // Back to Type-I status
            }
            else if (((data&0xF0) == 0x80) || ((data&0xF0) == 0x90)) // Read Sector... either single or multiple
            {
                fdc_buffer_track();                                                         // Get track into our buffer
                FDC.track_buffer_idx = (FDC.sector-Geom.startSector)*Geom.sectorSize;       // Start reading here
                FDC.track_buffer_end = (data & 0x10) ? (Geom.sectorSize*Geom.sectors) : (FDC.track_buffer_idx+Geom.sectorSize);
                FDC.wait_for_read = 0;                                                      // Start fetching data
                FDC.sector_byte_counter = 0;                                                // Reset our fetch counter
                FDC.cycle_deadline = CPU.TotalCycles + FDC_CYCLES_PER_BYTE;                 // Pace first byte
                if (io_show_status == 0) io_show_status = 4;                                // And let the world know we are reading...
            }
            else if (((data&0xF0) == 0xA0) || ((data&0xF0) == 0xB0)) // Write Sector... either single or multiple
            {
                fdc_buffer_track();                                                         // Get track into our buffer
                FDC.track_buffer_idx = (FDC.sector-Geom.startSector)*Geom.sectorSize;       // Start writing here
                FDC.track_buffer_end = (data & 0x10) ? (Geom.sectorSize*Geom.sectors) : (FDC.track_buffer_idx+Geom.sectorSize);
                FDC.sector_byte_counter = 0;                                                // Reset our sector byte counter
                FDC.wait_for_write = 3;                                                     // Start the Write Process... we will allow data shortly
                FDC.cycle_deadline = CPU.TotalCycles + FDC_CYCLES_PER_BYTE;                 // Pace first byte
                io_show_status = 5;                                                         // And let the world know we are writing...
            }
            else if ((data&0xF0) == 0xE0) // Read Track
            {
                // Not implemented yet... only for diagnostics use
            }
            else if ((data&0xF0) == 0xF0) // Write Track (format)
            {
                fdc_buffer_track();                                         // Get track into our buffer
                FDC.sector = Geom.startSector;                              // We always start a track write at the first sector
                FDC.track_buffer_idx = 0;                                   // From the top
                FDC.track_buffer_end = (Geom.sectorSize*Geom.sectors);      // All bytes in the track
                FDC.sector_byte_counter = 0;                                // Reset the sector write counter
                FDC.wait_for_write = 1;                                     // Start the Write Process...
                FDC.cycle_deadline = CPU.TotalCycles + FDC_CYCLES_PER_BYTE; // Pace first byte
                io_show_status = 5;                                         // Let the world know we are writing...
                FDC.status |= ST_INDEX_DRQ;                                 // Accept data immediately
                FDC.write_track_allowed = 0;                                // But wait for data from the CPU
            }
        }
    }
}

void fdc_setDrive(u8 drive)
{
    FDC.drive = drive;                      // Record the drive in use
}

void fdc_setSide(u8 side)
{
    FDC.side = side;                        // Record the side in use
}

void fdc_reset(u8 full_reset)
{
    if (full_reset)
    {
        memset(&FDC, 0x00, sizeof(FDC));    // Clear all registers and the buffers
    }

    FDC.status = 0x00;                                   // Drive ready, not busy
    FDC.commandType = 1;                                 // We are back to Type I
    FDC.wait_for_read = 2;                               // Not fetching any data
    FDC.wait_for_write = 2;                              // Not storing any data
}

void fdc_init(u8 fdc_type, u8 drives, u8 sides, u8 tracks, u8 sectors, u16 sectorSize, u8 startSector, u8 *diskBuffer0, u8 *diskBuffer1)
{
    Geom.fdc_type   = fdc_type;                         // Kept for header compatibility; unused now (WD2793-only)
    Geom.drives     = drives;                           // Number of drives (must be 1 or 2)
    Geom.sides      = sides;                            // Number of sides on each drive
    Geom.tracks     = tracks;                           // Number of tracks on each drive
    Geom.sectors    = sectors;                          // Number of sectors on each drive
    Geom.sectorSize = sectorSize;                       // The sector size (256, 512, 1024, etc)
    Geom.disk0      = diskBuffer0;                      // Pointer to the first raw sector dump drive
    Geom.disk1      = diskBuffer1;                      // Pointer to the second raw sector dump drive
    Geom.startSector= startSector;                      // Starting sector (some systems like MSX will start sector numbering at 1)
}

// End of file
