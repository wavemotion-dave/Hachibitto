// =====================================================================================
// Copyright (c) 2021-2024 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave and
// Marat Fayzullin (fMSX core) are thanked profusely.
//
// The Hachibitto emulator is offered as-is, without any warranty. Please see readme.md
// =====================================================================================
#ifndef _MSX_GENERIC_H_
#define _MSX_GENERIC_H_

#include "Hachibitto.h"
#include "FMPAC.h"
#include "cpu/z80/Z80_interface.h"
#include "cpu/vdp9938/vdp9938.h"
#include "cpu/ay38910/AY38910.h"
#include "cpu/scc/SCC.h"

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define MAX_ROMS                    1024
#define MAX_ROM_NAME                160

#define MAX_CONFIGS                 2048
#define CONFIG_VER                  0x000C

#define MSXROM                      0x01
#define DIRECTORY                   0x02

#define ID_SHM_CANCEL               0x00
#define ID_SHM_YES                  0x01
#define ID_SHM_NO                   0x02

#define DPAD_NORMAL                 0
#define DPAD_DIAGONALS              1
#define DPAD_SLIDE_N_GLIDE          2

#define OVL_FULLKBD                 0
#define OVL_ALPHAKBD                1

typedef struct {
  char szName[MAX_ROM_NAME+1];
  u8 uType;
  u32 uCrc;
} FI_MSX;

struct __attribute__((__packed__)) GlobalConfig_t
{
    u16 config_ver;
    u32 bios_checksums;
    char szLastRom[MAX_ROM_NAME+1];
    char szLastPath[MAX_ROM_NAME+1];
    char reserved1[MAX_ROM_NAME+1];
    char reserved2[MAX_ROM_NAME+1];
    u8  showFPS;
    u8  global_0;
    u8  global_1;
    u8  global_2;
    u8  global_3;
    u8  global_4;
    u8  global_5;
    u8  global_6;
    u8  global_7;
    u8  debugger;
    u32 config_checksum;
};

struct __attribute__((__packed__)) Config_t
{
    u32 game_crc;
    u8  keymap[12];
    u8  msxMapper;
    u8  machineType;
    u8  autoFire;
    u8  keyboard;
    u8  maxSprites;
    u8  dpad;
    u8  memWipe;
    u8  musicExpand;
    u8  yOffset;
    u8  cpuBoost;
    u8  splitRefresh;
    u8  blendScr7;
    u8  scaleScreen;
    u8  maskBorders;
    u8  reserved5;
    u8  reserved6;
    u8  reserved7;
    u8  reserved8;
};

#define COMPRESS_BUFFER ((u8 *)(ROM_Memory + (1024*1024)))   // We use the back-end 256K of the ROM buffer for compression

#define NORAM                       0xFF    // When reading IO that is unmapped... we just return 0xFF
            
#define MACHINE_MSX2_A              0x00    // Standard MSX2 slot layout (slot 3 expanded)
#define MACHINE_MSX2_B              0x01    // Alternate MSX2 slot layout (slot 0 expanded)
#define MACHINE_MSX1                0x02    // Standard MSX1 slot layout (nothing expanded)

extern struct Config_t       myConfig;
extern struct GlobalConfig_t myGlobalConfig;

extern u8 special_memory_access;                // 0x00 if no special access, otherwise one or more of the SPEC_MEM_xxx below

#define SPEC_MEM_SUBSLOT_ACTIVE     0x01        // Allows read/write to special SubSlot register at 0xFFFF
#define SPEC_MEM_SCC_ENABLED        0x02        // SCC is enabled and requires memory traps
#define SPEC_MEM_SCC_PLUS_ENABLED   0x04        // SCC+ is enabled and requires memory traps
#define SPEC_MEM_SUPERLR_ACTIVE     0x08        // Super Lode Runner traps on writes to 0x0000 even if the cart isn't mapped in!
#define SPEC_MEM_DISK_CONTROLLER    0x10        // Standard Memory-Mapped Disk Controller active in Page 1

extern u8 last_special_key;
extern u8 last_special_key_dampen;
extern u16 msx_init;
extern u16 msx_basic;
extern u8 skip_render;
extern u16 timingFrames;
extern s8 temp_offset;

extern FI_MSX gpFic[MAX_ROMS];
extern int ucGameAct;
extern int ucGameChoice;

#define MSX_MODE_CART   1
#define MSX_MODE_DISK   2

extern u8 mapperType;
extern u8 mapperMask;
extern u8 msx_caps_lock;
extern u8 msx_kana_lock;
extern u8 mirror_ram_bank[4];

#define GUESS           0
#define MIRRORED        1
#define KON8            2
#define ASC8            3
#define SCC8            4
#define ASC16           5
#define ZEN8            6
#define ZEN16           7
#define XBLAM           8
#define SUPERLR         9
#define XEVIOUS         10
#define ASC8SRAM8       11
#define ASC8SRAM2       12
#define ASC16SRAM2      13
#define ASC16SRAM8      14
#define AT0K            15
#define AT4K            16
#define AT8K            17
#define LIN64           18

#define MAJUT           70  // Not one of the general mappers a user can pick

#define SCCPLUS_RAM     88  // For our special SCC+ "Cart"

#define MAX_GUESS_MAPPER 8   // The highest guess we can guess when examining ROM data

extern u32 MAX_CART_SIZE_KB;

extern u8 *ROM_Memory;
extern u8 RAM_Memory[0x40000];
extern u8 BIOS_Memory[0x8000];
extern u8 SRAM_Memory[0x10000];

extern const unsigned char MSXBios_DISK[0x4000];
extern const unsigned char MSXBios_MSX2[0x8000];
extern const unsigned char MSXBios_MSX2EXT[0x4000];
extern const unsigned char MSXBios_MSX1[0x8000];
extern const unsigned char MSXBios_FMPAC[0x4000];

extern u8 bCartInPage[4];
extern u8 bRAMInPage[4];
extern u8 *MSXCartPtr[8];
extern u8 *MSXRamPtr[8];
extern u8 *MemoryMap[8];
extern u8  sccplus_page[4];
extern u8  sccplus_mode;
extern AY38910 myAY;
extern AY38910 myAY2;
extern SCC     mySCC;
extern FMPAC   myYM;
extern u8 msx_scc_enable;
extern u8 JoyMode;
extern u32 JoyState;
extern u16 msx_block_size;
extern u32 file_crc;
extern u8 Port_PPI_A;
extern u8 Port_PPI_B;
extern u8 Port_PPI_C;
extern void ProcessBufferedKeys(void);
extern u8 BufferedKeys[32];
extern u8 BufferedKeysWriteIdx;
extern u8 BufferedKeysReadIdx;
extern u8 key_shift;
extern u8 key_ctrl;
extern u8 key_kana;
extern u8 key_graph;
extern u32 msx_last_file_size;
extern u8 msx_scc_capable_game;
extern u8 msx_music_capable_game;
extern u8 msx_subslot;
extern u8 XPalReal0;
extern u8 ALatch;
extern u32 frame_number;
extern u8 CurrentEpochSaved;
extern u8 msx_irq_pending;
extern u8 palette_latch;
extern uint8_t OccBuf[320];
extern u16 beeperFreq;

// --------------------------------------------------
// Some CPU and VDP and SGM stuff that we need
// --------------------------------------------------
extern void Loop9938(void);
extern void msxUpdateScreen(void);
extern void getfile_crc(const char *path);
extern void msxLoadState();
extern void msxSaveState();
extern void msxWipeRAM(void);
extern void msx_reset(void);
extern void msx_restore_bios(void);
extern void BufferKey(u8 key);
extern void BufferKeys(char *str);
extern void MSX_InitialMemoryLayout(u32 romSize);
extern void msxSaveEEPROM(void);
extern void msxLoadEEPROM(void);
extern void Z80_Interface_Reset(void);
extern void LoadFavorites(void);
extern void preserveCompressedMem(void);
extern void restoreCompressedMem(void);
extern void HandleSCCPlusModeRegister(u8 value);
extern void SCC_LegacyWrite(u8 value, u16 address);
extern void BuildScreen8ColorMap(void);
extern void msxRun(void);
extern void allocateCompressedMem(void);
extern void restoreCompressedMem(void);
extern void LoadConfig(void);
extern void HachibittoInitScreenUp(void);
extern void HachibittoFindFiles(void);
extern void HachibittoChangeOptions(void);
extern void DSPrint(int iX,int iY,int iScr,char *szMessage);
extern void DSPrint_fps(u16 fps);
extern void HachibittoChangeKeymap(void);
extern void HachibittoGameOptions(bool);
extern void FadeToColor(unsigned char ucSens, unsigned short ucBG, unsigned char ucScr, unsigned char valEnd, unsigned char uWait);
extern void DisplayFileName(void);
extern u32  ReadFileCarefully(char *filename, u8 *buf, u32 buf_size, u32 buf_offset);
extern u8   HachibittoChooseFile(void);
extern u8   showMessage(char *szCh1, char *szCh2);
extern u8   msxInit(char *szGame);
extern u8   LoadGameRom(const char *path);
extern u32  LoopZ80();
extern u8   RomDB_Lookup(u32 size);extern void HachibittoModeNormal(void);
extern void SaveConfig(bool bShow);
extern void ShowRandomPreviewSnaps(void);
extern void IndirectRegWrite9938(u8 Value);
extern void EnsureSaveDirectory(void);

#endif
