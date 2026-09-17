// =====================================================================================
// Copyright (c) 2021-2024 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave (Phoenix-Edition),
// Alekmaul (original port) and Marat Fayzullin (fMSX core) are thanked profusely.
//
// The Hachibitto emulator is offered as-is, without any warranty. Please see readme.md
// =====================================================================================

#ifndef _FDC_H
#define _FDC_H

#include <nds.h>
#include "Hachibitto.h"
#include "cpu/z80/Z80_interface.h"

// The MSX FDC controller
struct FDC_t
{
    u8  status;
    u8  command;
    u8  track;
    u8  sector;
    u8  data;
    u8  drive;
    u8  side;
    u8  motor;
    u8  wait_for_read;
    u8  wait_for_write;
    u8  commandType;
    u8  write_track_allowed;
    u8  stepDirection;
    u8  int_req;
    u8  read_timeout;
    u8  track_dirty[2];         // True if at least 1 track is dirty on this disk
    u8  track_buffer[10240];    // Enough for 16+ sectors of 512 bytes or 10 sectors of 1024 bytes
    u16 track_buffer_idx;
    u16 track_buffer_end;
    u16 indexPulseCounter;      // Driven by LoopFDC() at scanline granularity
    u16 sector_byte_counter;
    u16 write_track_byte_counter;
    u32 cycle_deadline;         // CPU.TotalCycles value at which the current busy wait completes    
};

struct FDC_GEOMETRY_t
{
    u8  drives;
    u8  sides;
    u8  tracks;
    u8  sectors;
    u16 sectorSize;
    u8  startSector;
    u8 *disk0;
    u8 *disk1;
};

extern struct FDC_t             FDC;
extern struct FDC_GEOMETRY_t    Geom;

extern u8   fdc_read(u8 addr);
extern void fdc_write(u8 addr, u8 data);
extern void fdc_reset(u8 full_reset);
extern void fdc_init(u8 drives, u8 sides, u8 tracks, u8 sectors, u16 sectorSize, u8 startSector, u8 *diskBuffer0, u8 *diskBuffer1);
extern void LoopFDC(void);     // Call once per scanline, like Loop9938() to process background disk "activity"

#endif //_FDC_H
