// =====================================================================================
// Copyright (c) 2021 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, it's source code and associated 
// readme files, with or without modification, are permitted in any medium without 
// royalty provided this copyright notice is used and wavemotion-dave (Phoenix-Edition),
// Alekmaul (original port) and Marat Fayzullin (fMSX core) are thanked profusely.
//
// The Hachibitto emulator is offered as-is, without any warranty.
// =====================================================================================


#ifndef CRC32_H
#define CRC32_H
#include <nds.h>

extern u32 getFileCrc(const char* filename);
extern u32 getCRC32(u8 *buf, u32 size);
extern u32 crc32(unsigned int crc, const unsigned char *buf, unsigned int len);

#endif

