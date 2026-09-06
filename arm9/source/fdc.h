// =====================================================================================
// Copyright (c) 2021-2024 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave (Phoenix-Edition),
// Alekmaul (original port) and Marat Fayzullin (ColEM core) are thanked profusely.
//
// The Hachibitto emulator is offered as-is, without any warranty. Please see readme.md
// =====================================================================================

#ifndef _FDC_H
#define _FDC_H

#include <nds.h>
#include "Hachibitto.h"
#include "cpu/z80/Z80_interface.h"

#define WD1770  0
#define WD2793  1

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
    u8  spare;
    u8  track_dirty[2];
    u8  track_buffer[10240];  // Enough for 16+ sectors of 512 bytes or 10 sectors of 1024 bytes
    u16 track_buffer_idx;
    u16 track_buffer_end;
    u16 indexPulseCounter;     // now driven by LoopFDC() at scanline granularity
    u16 sector_byte_counter;
    u16 write_track_byte_counter;
    u32 cycle_deadline;     // NEW: CPU.TotalCycles value at which the current busy wait completes    
};

struct FDC_GEOMETRY_t
{
    u8  fdc_type;        // Either WD1770 or WD2793
    u8  drives;
    u8  sides;
    u8  tracks;
    u8  sectors;
    u16 sectorSize;
    u8  startSector;
    u8 *disk0;
    u8 *disk1;
};

// WD2793 Status Register Bit Definitions
#define ST_BUSY             0x01
#define ST_INDEX_DRQ        0x02  // Index Pulse (Type I) / Data Request (Type II/III)
#define ST_TRACK0_LOST      0x04  // Track 0 (Type I) / Lost Data (Type II/III)
#define ST_CRC_ERROR        0x08
#define ST_SEEK_RNF         0x10  // Seek Error (Type I) / Record Not Found (Type II/III)
#define ST_HEAD_LOADED_TYPE 0x20  // Head Loaded (Type I) / Record Type (Type II/III)
#define ST_WRITE_PROT       0x40
#define ST_NOT_READY        0x80  // WD2793: 1 = Not Ready, 0 = Ready (INVERTED from WD1770!)


extern struct FDC_t             FDC;
extern struct FDC_GEOMETRY_t    Geom;

extern u8   fdc_read(u8 addr);
extern void fdc_write(u8 addr, u8 data);
extern void fdc_setSide(u8 side);
extern void fdc_setDrive(u8 drive);
extern void fdc_reset(u8 full_reset);
extern void fdc_init(u8 fdc_type, u8 drives, u8 sides, u8 tracks, u8 sectors, u16 sectorSize, u8 startSector, u8 *diskBuffer0, u8 *diskBuffer1);
extern void LoopFDC(void);     // NEW: call once per scanline, like Loop9938()

#endif //_FDC_H
