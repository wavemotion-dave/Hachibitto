#ifndef _VDP9938A_H_
#define _VDP9938A_H_

#include <nds.h>

#define MAXSCREEN           8   // Highest screen mode supported

#define VDP9938_BASE        10738635    // Standard 3.58 Mhz

// ---------------------------------------------------
// The default NTSC machine time bases 
// ---------------------------------------------------
#define VDP9938_FRAMES      60
#define VDP9938_LINE        ((VDP9938_BASE/(3*60*262)))

#define VDP9938_LINES       262
#define VDP9938_START_LINE  (3+13+27)
#define VDP9938_END_LINE    (VDP9938_START_LINE+212)

#define VDP9938_REG1_RAM16K 0x80 /* 1: 16kB VRAM (0=4kB)     */
#define VDP9938_REG1_SCREEN 0x40 /* 1: Enable display        */
#define VDP9938_REG1_IRQ    0x20 /* 1: IRQs on VBlanks       */
#define VDP9938_REG0_IRQ    0x10 /* 1: IRQs on Scanline      */
#define VDP9938_REG1_SPR16  0x02 /* 1: 16x16 sprites (0=8x8) */
#define VDP9938_REG1_BIGSPR 0x01 /* 1: Magnify sprites x2    */

#define VDP9938_STAT_VBLANK 0x80 /* 1: VBlank has occured    */
#define VDP9938_STAT_5THSPR 0x40 /* 1: 5th Sprite Detected   */
#define VDP9938_STAT_OVRLAP 0x20 /* 1: Sprites overlap       */
#define VDP9938_STAT_5THNUM 0x1F /* Number of the 5th sprite */

#define VDP9938_Mode      (((VDP[0]&0x02)>>1)|(((VDP[1]&0x18)>>2)))
#define VDP9938_VRAMMask  (VDP[1]&VDP9938_REG1_RAM16K ? 0x1FFFF:0x0FFF)
#define VDP9938_VBlankON  (VDP[1]&VDP9938_REG1_IRQ)
#define VDP9938_LineSync  (VDP[0]&VDP9938_REG0_IRQ)
#define VDP9938_Sprites16 (VDP[1]&VDP9938_REG1_SPR16)
#define VDP9938_ScreenON  (VDP[1]&VDP9938_REG1_SCREEN)


#define MAXSPRITE2  8       /* Sprites/line in SCREEN 4-8    */

#define ScreenON      (VDP[1]&0x40)   // Show screen         
#define BigSprites    (VDP[1]&0x01)   // Zoomed sprites      
#define Sprites16x16  (VDP[1]&0x02)   // 16x16/8x8 sprites   
#define VScroll       VDP[23]
#define HScroll       ((VDP[27]&0x07)|((int)(VDP[26]&0x3F)<<3))
#define SpritesOFF    (VDP[8]&0x02)   /* Don't show sprites  */
#define FlipEvenOdd   (VDP[9]&0x04)   /* Flip even/odd pages */
#define OddPage       (frame_number&1)

extern byte pVDPVidMem[];
extern u8 VDP[64];
extern u16 VAddr;

typedef struct {
  void (*Refresh)(u8 uY);
  byte R2,R3,R4,R5,R6,M2,M3,M4,M5;
} tScrMode;

extern u8 XBuf[];
extern u8 OH;
extern u8 IH;
extern u8 *VPAGE;

extern u8 bResetVLatch;

extern u8 VDP9938A_palette[16*3];
extern tScrMode SCR[MAXSCREEN+1];

extern void RefreshLine0(u8 uY);
extern void RefreshLine1(u8 uY);
extern void RefreshLine2(u8 uY);
extern void RefreshLine3(u8 uY);
extern void RefreshLine4(u8 uY);
extern void RefreshLine5(u8 uY);
extern void RefreshLine6(u8 uY);
extern void RefreshLine7(u8 uY);
extern void RefreshLine8(u8 uY);

extern void WrCtrl9938(byte value);

extern byte RdData9938(void);
extern byte RdCtrl9938(void);
extern void Reset9938(void);

extern u16 CurLine;                            // Current Scanline
extern u8 VDP[64],VDPStatus[10],VDPDlatch;     // VDP registers
extern u16 VAddr;                              // Storage for VIDRAM addresses
extern u8 VDPCtrlLatch;                        // VDP control latch
extern u8 *ChrGen,*ChrTab,*ColTab;             // VDP tables (screens)
extern u8 *SprGen,*SprTab;                     // VDP tables (sprites)
extern u8 ScrMode;                             // Current screen mode
extern u8 FGColor,BGColor;                     // Colors
extern u32 ColTabM, ChrGenM;                   // Color and Character Masks

/** WrData9938() *********************************************/
/** Write a value V to the VDP Data Port.                   **/
/*************************************************************/
static inline __attribute__((always_inline)) void WrData9938(byte V)  // This one is used frequently so we always inline it
{
    VDPDlatch = VPAGE[VAddr] = V;
    VAddr     = (VAddr+1)&0x3FFF;
    if(!VAddr&&(ScrMode>3))
    {
      VDP[14]=(VDP[14]+1)&7;
      VPAGE=pVDPVidMem+((int)VDP[14]<<14);
    }
    VDPCtrlLatch = 0;
}

extern u32 frame_number;

#endif
