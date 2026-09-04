/******************************************************************************
* VDP 9938 (video) file
*
* File: vdp9938.c -- software implementation of the VDP 9938 Video Display Processor.
*
******************************************************************************/
#include <nds.h>
#include <stdio.h>
#include <string.h>

#include "../../Hachibitto.h"
#include "../../MSX_generic.h"
#include "../../V9938.h"
#include "../z80/Z80_interface.h"

#include "vdp9938.h"

u8 MaxSprites[2] __attribute__((section(".dtcm"))) = {32, 4};     // Normally the CV only shows 4 sprites on a line... for emulation we bump this up if configured

u16 *pVidFlipBuf __attribute__((section(".dtcm"))) = (u16*) (0x06000000);    // Video flipping buffer

u8 XBuf[256*212] ALIGN(32) = {0}; // VDP9938 screen is 256x212

// Look up table for colors - pre-generated and in VRAM for maximum speed!
u32 (*lutTablehh)[16][16] __attribute__((section(".dtcm"))) = (void*)0x068A0000;    // this is actually 16x16x16x4 = 16K

u16 my_config_clear_int __attribute__((section(".dtcm"))) = 0;
u16 ALatch              __attribute__((section(".dtcm"))) = 0;

u8 OH                   __attribute__((section(".dtcm"))) = 0;
u8 IH                   __attribute__((section(".dtcm"))) = 0;
u32 frame_number        __attribute__((section(".dtcm"))) = 0;
u8 CurrentEpoch         __attribute__((section(".dtcm"))) = 0;


  /* Per-scanline "has a sprite already written here" mask, aligned 1:1
     with ZBuf's addressing (P = ZBuf + AT[1] + 0/32, plus up to +31 for
     widened sprites -> max index 255+32+31 = 318, so 320 bytes covers it). */
uint8_t OccBuf[320]     __attribute__((section(".dtcm")));

static u16 nibbleLUT16[256] __attribute__((section(".dtcm")));

static u8 screen7LUT[256] __attribute__((section(".dtcm")));

void BuildScreen7LUT(void)
{
    for (int i = 0; i < 256; i++)
        screen7LUT[i] = (i >> 4) | (i & 0x0F);   // OR both nibbles - keeps thin strokes visible
}

void BuildNibbleLUT(void)
{
    for (int i = 0; i < 256; i++)
    {
        nibbleLUT16[i] = ((i >> 4) & 0x0F) | ((i & 0x0F) << 8);
    }
}

u8 msx_irq_pending = 0;   // new: bitmask, one bit per VDP interrupt source

void SetVDPIRQ(u8 bit, u8 set) //TODO: move this to... Z80_Interface?
{
    if (set) msx_irq_pending |= bit;
    else     msx_irq_pending &= ~bit;
    CPU.IRequest = msx_irq_pending ? INT_RST38 : INT_NONE;
}

// ---------------------------------------------------------------------------------------
// Screen handlers and masks for VDP table address registers.
// Screen modes are confusing as different documentation (MSX, Coleco, VDP manuals, etc)
// all seem to refer to 'Modes' vs 'Screens' vs more colorful names for the modes
// plus there are the undocumented modes. So I've done my best to comment using
// all of the names you will find out there in the wild world of VDP documentation!
// ---------------------------------------------------------------------------------------
tScrMode SCR[MAXSCREEN+1] __attribute__((section(".dtcm")))  = {
                // R2,  R3,  R4,  R5,  R6,  M2,  M3,  M4,  M5
  { RefreshLine0, 0x7F,0x00,0x3F,0x00,0xFF,0x00,0x00,0x00,0x00 }, /* SCR 0:  TEXT 40x24  */
  { RefreshLine1, 0x7F,0xFF,0x3F,0xFF,0xFF,0x00,0x00,0x00,0x00 }, /* SCR 1:  TEXT 32x24  */
  { RefreshLine2, 0x7F,0x80,0x3C,0xFF,0xFF,0x00,0x7F,0x03,0x00 }, /* SCR 2:  BLK 256x192 */
  { RefreshLine3, 0x7F,0x00,0x3F,0xFF,0xFF,0x00,0x00,0x00,0x00 }, /* SCR 3:  64x48x16    */
  { RefreshLine4, 0x7F,0x80,0x3C,0xFC,0xFF,0x00,0x7F,0x03,0x03 }, /* SCR 4:  BLK 256x192 */
  { RefreshLine5, 0x60,0x00,0x00,0xFC,0xFF,0x1F,0x00,0x00,0x03 }, /* SCR 5:  256x192x16  */
  { RefreshLine6, 0x60,0x00,0x00,0xFC,0xFF,0x1F,0x00,0x00,0x03 }, /* SCR 6:  512x192x4   */
  { RefreshLine7, 0x20,0x00,0x00,0xFC,0xFF,0x1F,0x00,0x00,0x03 }, /* SCR 7:  512x192x16  */
  { RefreshLine8, 0x20,0x00,0x00,0xFC,0xFF,0x1F,0x00,0x00,0x03 }, /* SCR 8:  256x192x256 */
};

void (*RefreshLine)(u8 uY) __attribute__((section(".dtcm"))) = RefreshLine0;

/** Palette9918[] ********************************************/
/** 16 standard colors used by VDP9938/TMS9928 VDP chips.   **/
/*************************************************************/
u8 VDP9938A_palette[16*3] = {
  0x00,0x00,0x00,   0x00,0x00,0x00,   0x20,0xC0,0x20,   0x60,0xE0,0x60,
  0x20,0x20,0xE0,   0x40,0x60,0xE0,   0xA0,0x20,0x20,   0x40,0xC0,0xE0,
  0xE0,0x20,0x20,   0xE0,0x60,0x60,   0xC0,0xC0,0x20,   0xC0,0xC0,0x80,
  0x20,0x80,0x20,   0xC0,0x40,0xA0,   0xA0,0xA0,0xA0,   0xE0,0xE0,0xE0,
};

u8 pVDPVidMem[0x20000] ALIGN(32) ={0};                      // VDP video memory... 128K for VDP9938 support

u16 CurLine         __attribute__((section(".dtcm")));      // Current scanline
u8 VDP[64]          __attribute__((section(".dtcm")));      // VDP Registers
u8 VDPStatus[10]    __attribute__((section(".dtcm")));      // VDP Status
u8 VDPDlatch        __attribute__((section(".dtcm")));      // VDP register D Latch
u16 VAddr           __attribute__((section(".dtcm")));      // VDP Video Address (Will be a 17-bit VDP9938 addresses via VPAGE[])
u8 *VPAGE           __attribute__((section(".dtcm")));      // VDP Video Page (to allow up to 128K support)
u8 VDPCtrlLatch     __attribute__((section(".dtcm")));      // VDP control latch key
u8 *ChrGen          __attribute__((section(".dtcm")));      // VDP tables (screens)
u8 *ChrTab          __attribute__((section(".dtcm")));      // VDP tables (screens)
u8 *ColTab          __attribute__((section(".dtcm")));      // VDP tables (screens)
u8 *SprGen          __attribute__((section(".dtcm")));      // VDP tables (sprites)
u8 *SprTab          __attribute__((section(".dtcm")));      // VDP tables (sprites)
u8 ScrMode          __attribute__((section(".dtcm")));      // Current screen mode
u8 FGColor          __attribute__((section(".dtcm")));      // Foreground Color
u8 BGColor          __attribute__((section(".dtcm")));      // Background Color

// Sprite and Character Masks for the VDP
u32 ChrTabM     __attribute__((section(".dtcm"))) = 0x3FFF;
u32 ColTabM     __attribute__((section(".dtcm"))) = 0x3FFF;
u32 ChrGenM     __attribute__((section(".dtcm"))) = 0x3FFF;
u32 SprTabM     __attribute__((section(".dtcm"))) = 0x3FFF;

/** CheckSprites() ***********************************************/
/** This function is periodically called to check for sprite    **/
/** collisions. The caller of this will set the flag as needed. **/
/** Returning zero (0) means no collision. Otherwise collision. **/
/*****************************************************************/
ITCM_CODE byte CheckSprites(void)
{
  unsigned int I,J,LS,LD;
  byte *S,*D,*PS,*PD,*T;
  int DH,DV;

  /* Find valid, displayed sprites */
  DV = VDP9938_Sprites16 ? -16:-8;
  for(I=J=0,S=SprTab;(I<32)&&(S[0]!=208);++I,S+=4)
  {
    if(((S[0]<191)||(S[0]>255+DV))&&((int)S[1]-(S[3]&0x80? 32:0)>DV))  J|=1<<I;
  }

  // ------------------------------------------------------------------
  // Run through all displayed sprites and see if there is any overlap.
  // This is a bit CPU intensive - we check vertical overlap first as
  // it's a bit faster to see if these have any chance of collision.
  // ------------------------------------------------------------------
  if(VDP9938_Sprites16)
  {
    for(S=SprTab;J;J>>=1,S+=4)
      if(J&1)
        for(I=J>>1,D=S+4;I;I>>=1,D+=4)
          if(I&1)
          {
            DV=(int)S[0]-(int)D[0]; // Check if these sprites might coincide vertically
            if((DV<16)&&(DV>-16))
            {
              DH=(int)S[1]-(int)D[1]-(S[3]&0x80? 32:0)+(D[3]&0x80? 32:0);
              if((DH<16)&&(DH>-16)) // Check if these sprites might coincide horizontally
              {
                PS=SprGen+((int)(S[2]&0xFC)<<3);
                PD=SprGen+((int)(D[2]&0xFC)<<3);
                if(DV>0) PD+=DV; else { DV=-DV;PS+=DV; }
                if(DH<0) { DH=-DH;T=PS;PS=PD;PD=T; }
                while(DV<16)
                {
                  LS=((unsigned int)PS[0]<<8)+PS[16];
                  LD=((unsigned int)PD[0]<<8)+PD[16];
                  if(LD&(LS>>DH)) break;
                  else { ++DV;++PS;++PD; }
                }
                if(DV<16) return(1);
              }
            }
          }
  }
  else
  {
    for(S=SprTab;J;J>>=1,S+=4)
      if(J&1)
        for(I=J>>1,D=S+4;I;I>>=1,D+=4)
          if(I&1)
          {
            DV=(int)S[0]-(int)D[0];
            if((DV<8)&&(DV>-8))
            {
              DH=(int)S[1]-(int)D[1]-(S[3]&0x80? 32:0)+(D[3]&0x80? 32:0);
              if((DH<8)&&(DH>-8))
              {
                PS=SprGen+((int)S[2]<<3);
                PD=SprGen+((int)D[2]<<3);
                if(DV>0) PD+=DV; else { DV=-DV;PS+=DV; }
                if(DH<0) { DH=-DH;T=PS;PS=PD;PD=T; }
                while((DV<8)&&!(*PD&(*PS>>DH))) { ++DV;++PS;++PD; }
                if(DV<8) return(1);
              }
            }
          }
  }

  /* No collision */
  return(0);
}


/** ScanSprites() ********************************************/
/** Compute bitmask of sprites shown in a given scanline.   **/
/** Returns the last sprite to be scanned or -1 if none.    **/
/** Also updates 5th sprite fields in the status register.  **/
/*************************************************************/
ITCM_CODE int ScanSprites(byte Y, unsigned int *Mask)
{
    byte *AT;
    u8 sprite,MS,S5;
    s16 K;

    // Assume no sprites shown - we OR in a '1' for each visible sprite
    *Mask = 0x00000000;

    // Must have MODE1+ and screen enabled - otherwise no sprites rendered
    if(!ScrMode || !VDP9938_ScreenON)
    {
        return(-1);
    }

    s16 fifth_sprite_num =-1;                   // Used to detect the 5th sprite on a line
    AT = SprTab;                                // Pointer to the sprite table in VDP memory
    MS = MaxSprites[myConfig.maxSprites]+1;     // We either render 4 sprites (normal - this is how an 9918 would work) or 32 sprites (enhanded mode for emulation only)
    S5 = 5;                                     // We always want to trap on the 5th sprite
    u8 last = 31;                               // The last sprite number is 31 but we may break early if Y==208

    // ------------------------------------------------------------------
    // Scan through all possible 32 sprites to see what's being shown...
    // ------------------------------------------------------------------
    for(sprite=0;sprite<32;++sprite,AT+=4)
    {
        K=AT[0];                            // K = sprite Y coordinate
        if(K==208) {last=sprite; break;}    // Iteration terminates if Y=208 and we save the last scanned sprite
        if(K>256-IH) K-=256;                // Y coordinate may be negative

        // -------------------------------------------------------------------------------------------
        // Mark all valid sprites with 1s, break at MaxSprites. Track last scanned sprite for 5S num
        // At first this looked wrong as if it was off by 1 for comparing the Y (scanline) number
        // with the sprite Y coordinate but the Y position is tricky. A coordinate of 0 means draw
        // at the first pixel line (one below the top-most pixel line of the screen). A 255 means
        // draw at the 0th top-most pixel line of the screen. Y positions below 255 but above 208 are
        // negative indexes which allow for the sprite to be positioned partially cropped at the top.
        // Finally, the reason 208 was chosen by TI as the sentinal value is that it's 16 pixels below
        // the lowest pixel row of 192 and the sprite would be completely off-screen. Tricky...
        // -------------------------------------------------------------------------------------------
        if((Y>K)&&(Y<=K+OH))
        {
            // If we exceed four sprites per line, set 5th sprite number
            if(!--S5) fifth_sprite_num = sprite;

            // If we exceed maximum number of sprites per line, stop here
            if(!--MS) break;

            // Mark sprite as ready to draw
            *Mask |= (1<<sprite);
        }
    }

    // ------------------------------------------------------------------------
    // The if a 5th  sprite was found on this line, we check to see if we've
    // already got a 5th sprite latched and if not, we will set this sprite as
    // the fifth sprite. The 5th sprite flag will be cleared on status read.
    // ------------------------------------------------------------------------
    if ((VDPStatus[0] & VDP9938_STAT_5THSPR) == 0) // If the 5S flag is not already latched
    {
        if (fifth_sprite_num != -1) // If we have a 5th sprite number detected
        {
            VDPStatus[0] &= ~VDP9938_STAT_5THNUM;                      // Clear out any previous sprite number
            VDPStatus[0] |= (VDP9938_STAT_5THSPR | fifth_sprite_num);  // Set the 5th sprite flag and number
        }
        else // This is undocumented behavior but a real VDP will behave like this and Miner 2049er will rely on it
        {
            VDPStatus[0] &= ~VDP9938_STAT_5THNUM;      // Clear out any previous sprite number
            VDPStatus[0] |= last;                      // Set the 5th sprite number to the last scanned sprite on the line (the one with Y==208 or else sprite 31)
        }
    }

  // Return last scanned sprite - the caller's Mask is also filled in with a list of all shown sprites
  return(sprite-1);
}


/** RefreshSprites() *****************************************/
/** This function is called from RefreshLine#() to refresh  **/
/** and draw sprites to a given pixel line.                 **/
/*************************************************************/
ITCM_CODE void RefreshSprites(register byte Y)
{
  register byte *PT,*AT;
  register byte *P,*T,C;
  register int L,K,N;
  unsigned int M;
  
  /* Find sprites to show, update 5th sprite status */
  N = ScanSprites(Y,&M);
  if((N<0) || !M) return;

  T  = XBuf+256*Y;
  AT = SprTab+(N<<2);

  /* For each possibly shown sprite... */
  for( ; N>=0 ; --N, AT-=4)
  {
    /* If showing this sprite... */
    if(M&(1<<N))
    {
      C=AT[3];                  /* C = sprite attributes */
      L=C&0x80? AT[1]-32:AT[1]; /* Sprite may be shifted left by 32 */
      C&=0x0F;                  /* C = sprite color */

      if((L<256) && (L>-OH) && C)
      {
        K=AT[0];                /* K = sprite Y coordinate */
        if(K>256-IH) K-=256;    /* Y coordinate may be negative */

        P  = T+L;
        K  = Y-K-1;
        PT = SprGen
           + ((int)(IH>8? (AT[2]&0xFC):AT[2])<<3)
           + (OH>IH? (K>>1):K);

        /* Mask 1: clip left sprite boundary */
        K=L>=0? 0xFFFF:(0x10000>>(OH>IH? (-L>>1):-L))-1;

        /* Mask 2: clip right sprite boundary */
        L+=(int)OH-257;
        if(L>=0)
        {
          L=(IH>8? 0x0002:0x0200)<<(OH>IH? (L>>1):L);
          K&=~(L-1);
        }

        /* Get and clip the sprite data */
        K&=((int)PT[0]<<8)|(IH>8? PT[16]:0x00);

        if(OH>IH)
        {
          /* Big (zoomed) sprite */

          /* Draw left 16 pixels of the sprite */
          if(K&0xFF00)
          {
            if(K&0x8000) P[1]=P[0]=C;
            if(K&0x4000) P[3]=P[2]=C;
            if(K&0x2000) P[5]=P[4]=C;
            if(K&0x1000) P[7]=P[6]=C;
            if(K&0x0800) P[9]=P[8]=C;
            if(K&0x0400) P[11]=P[10]=C;
            if(K&0x0200) P[13]=P[12]=C;
            if(K&0x0100) P[15]=P[14]=C;
          }

          /* Draw right 16 pixels of the sprite */
          if(K&0x00FF)
          {
            if(K&0x0080) P[17]=P[16]=C;
            if(K&0x0040) P[19]=P[18]=C;
            if(K&0x0020) P[21]=P[20]=C;
            if(K&0x0010) P[23]=P[22]=C;
            if(K&0x0008) P[25]=P[24]=C;
            if(K&0x0004) P[27]=P[26]=C;
            if(K&0x0002) P[29]=P[28]=C;
            if(K&0x0001) P[31]=P[30]=C;
          }
        }
        else
        {
          /* Normal (unzoomed) sprite */

          /* Draw left 8 pixels of the sprite */
          if(K&0xFF00)
          {
            if(K&0x8000) P[0]=C;
            if(K&0x4000) P[1]=C;
            if(K&0x2000) P[2]=C;
            if(K&0x1000) P[3]=C;
            if(K&0x0800) P[4]=C;
            if(K&0x0400) P[5]=C;
            if(K&0x0200) P[6]=C;
            if(K&0x0100) P[7]=C;
          }

          /* Draw right 8 pixels of the sprite */
          if(K&0x00FF)
          {
            if(K&0x0080) P[8]=C;
            if(K&0x0040) P[9]=C;
            if(K&0x0020) P[10]=C;
            if(K&0x0010) P[11]=C;
            if(K&0x0008) P[12]=C;
            if(K&0x0004) P[13]=C;
            if(K&0x0002) P[14]=C;
            if(K&0x0001) P[15]=C;
          }
        }
      }
    }
  }
}

/** ColorSprites() *******************************************/
/** This function is called from RefreshLine#() to refresh  **/
/** color sprites in SCREENs 4-8. The result is returned in **/
/** ZBuf, whose size must be 320 bytes (32+256+32).         **/
/*************************************************************/
ITCM_CODE void ColorSprites(uint8_t Y)
{
  static const uint8_t SprHeights[4] = { 8,16,16,32 };
  uint8_t C,IH,OH,J,OrThem;
  uint8_t *P,*PT,*AT,*O;
  int L,K;
  unsigned int M;

  uint8_t *ZBuf = (XBuf+256*Y) - 32;
  CurrentEpoch++;

  /* SPR_SET: unconditional overwrite (background OR earlier-sprite pixel),
     and record that this pixel now holds sprite data.
     SPR_OR:   only really OR if a sprite already touched this pixel this
               line; otherwise it's the first sprite content here, so
               overwrite (never OR against raw background). */
#define SPR_SET(n) do{ P[n]=C; O[n]=CurrentEpoch; }while(0)
#define SPR_OR(n)  do{ if(O[n]==CurrentEpoch) P[n]|=C; else { P[n]=C; O[n]=CurrentEpoch; } }while(0)

  /* No extra sprites yet */
  VDPStatus[0]&=~0x5F;

  if(SpritesOFF) return;

  /* Assign initial values before counting */
  OrThem = 0x00;
  OH = SprHeights[VDP[1]&0x03];
  IH = SprHeights[VDP[1]&0x02];
  AT = SprTab-4;
  C  = MAXSPRITE2+1;
  M  = 0;

  /* Count displayed sprites */
  for(L=0;L<32;++L)
  {
    M<<=1;AT+=4;              /* Iterating through SprTab      */
    K=AT[0];                  /* Read Y from SprTab            */
    if(K==216) break;         /* Iteration terminates if Y=216 */
    K=(uint8_t)(K-VScroll);   /* Sprite's actual Y coordinate  */
    if(K>256-IH) K-=256;      /* Y coordinate may be negative  */

    /* Mark all valid sprites with 1s, break at MAXSPRITE2 sprites */
    if((Y>K)&&(Y<=K+OH))
    {
      /* If we exceed the maximum number of sprites per line... */
      if(!--C)
      {
        /* Set 9thSprite flag in the VDP status register */
        VDPStatus[0]|=0x40;
        /* Stop drawing sprites, unless all-sprites option enabled */
        break;
      }

      /* Mark sprite as ready to draw */
      M|=1;
    }
  }

  /* Mark last checked sprite (9th in line, Y=216, or sprite #31) */
  VDPStatus[0]|=L<32? L:31;

  u8 zeroNotTransparent = (VDP[8]&0x20); // Zero index is a real color.. we can use BG_PALETTE[16]

  /* Draw all marked sprites */
  for(;M;M>>=1,AT-=4)
    if(M&1)
    {
      K=(uint8_t)(AT[0]-VScroll); /* K = sprite Y coordinate */
      if(K>256-IH) K-=256;        /* Y coordinate may be negative */

      J=Y-K-1;
      J = OH>IH? (J>>1):J;
      C=SprTab[-0x0200+((AT-SprTab)<<2)+J];
      OrThem|=C&0x40;

      if((C&0x0F) || zeroNotTransparent)
      {
        PT = SprGen+((int)(IH>8? AT[2]&0xFC:AT[2])<<3)+J;
        P=ZBuf+AT[1]+(C&0x80? 0:32);
        O=OccBuf+AT[1]+(C&0x80? 0:32);
        C&=0x0F;
        J=PT[0];

        if(OrThem&0x20)
        {
          if(OH>IH)
          {
            if(J&0x80) { SPR_OR(0);SPR_OR(1); }
            if(J&0x40) { SPR_OR(2);SPR_OR(3); }
            if(J&0x20) { SPR_OR(4);SPR_OR(5); }
            if(J&0x10) { SPR_OR(6);SPR_OR(7); }
            if(J&0x08) { SPR_OR(8);SPR_OR(9); }
            if(J&0x04) { SPR_OR(10);SPR_OR(11); }
            if(J&0x02) { SPR_OR(12);SPR_OR(13); }
            if(J&0x01) { SPR_OR(14);SPR_OR(15); }
            if(IH>8)
            {
              J=PT[16];
              if(J&0x80) { SPR_OR(16);SPR_OR(17); }
              if(J&0x40) { SPR_OR(18);SPR_OR(19); }
              if(J&0x20) { SPR_OR(20);SPR_OR(21); }
              if(J&0x10) { SPR_OR(22);SPR_OR(23); }
              if(J&0x08) { SPR_OR(24);SPR_OR(25); }
              if(J&0x04) { SPR_OR(26);SPR_OR(27); }
              if(J&0x02) { SPR_OR(28);SPR_OR(29); }
              if(J&0x01) { SPR_OR(30);SPR_OR(31); }
            }
          }
          else
          {
            if(J&0x80) SPR_OR(0);
            if(J&0x40) SPR_OR(1);
            if(J&0x20) SPR_OR(2);
            if(J&0x10) SPR_OR(3);
            if(J&0x08) SPR_OR(4);
            if(J&0x04) SPR_OR(5);
            if(J&0x02) SPR_OR(6);
            if(J&0x01) SPR_OR(7);
            if(IH>8)
            {
              J=PT[16];
              if(J&0x80) SPR_OR(8);
              if(J&0x40) SPR_OR(9);
              if(J&0x20) SPR_OR(10);
              if(J&0x10) SPR_OR(11);
              if(J&0x08) SPR_OR(12);
              if(J&0x04) SPR_OR(13);
              if(J&0x02) SPR_OR(14);
              if(J&0x01) SPR_OR(15);
            }
          }
        }
        else
        {
          if(OH>IH)
          {
            if(J&0x80) { SPR_SET(0);SPR_SET(1); }
            if(J&0x40) { SPR_SET(2);SPR_SET(3); }
            if(J&0x20) { SPR_SET(4);SPR_SET(5); }
            if(J&0x10) { SPR_SET(6);SPR_SET(7); }
            if(J&0x08) { SPR_SET(8);SPR_SET(9); }
            if(J&0x04) { SPR_SET(10);SPR_SET(11); }
            if(J&0x02) { SPR_SET(12);SPR_SET(13); }
            if(J&0x01) { SPR_SET(14);SPR_SET(15); }
            if(IH>8)
            {
              J=PT[16];
              if(J&0x80) { SPR_SET(16);SPR_SET(17); }
              if(J&0x40) { SPR_SET(18);SPR_SET(19); }
              if(J&0x20) { SPR_SET(20);SPR_SET(21); }
              if(J&0x10) { SPR_SET(22);SPR_SET(23); }
              if(J&0x08) { SPR_SET(24);SPR_SET(25); }
              if(J&0x04) { SPR_SET(26);SPR_SET(27); }
              if(J&0x02) { SPR_SET(28);SPR_SET(29); }
              if(J&0x01) { SPR_SET(30);SPR_SET(31); }
            }
          }
          else
          {
            if(J&0x80) SPR_SET(0);
            if(J&0x40) SPR_SET(1);
            if(J&0x20) SPR_SET(2);
            if(J&0x10) SPR_SET(3);
            if(J&0x08) SPR_SET(4);
            if(J&0x04) SPR_SET(5);
            if(J&0x02) SPR_SET(6);
            if(J&0x01) SPR_SET(7);
            if(IH>8)
            {
              J=PT[16];
              if(J&0x80) SPR_SET(8);
              if(J&0x40) SPR_SET(9);
              if(J&0x20) SPR_SET(10);
              if(J&0x10) SPR_SET(11);
              if(J&0x08) SPR_SET(12);
              if(J&0x04) SPR_SET(13);
              if(J&0x02) SPR_SET(14);
              if(J&0x01) SPR_SET(15);
            }
          }
        }
      }

      /* Update overlapping flag */
      OrThem>>=1;
    }

#undef SPR_SET
#undef SPR_OR
}
/** RefreshLine0() *********************************************/
/** Refresh line Y (0..191) of SCREEN0, including sprites in  **/
/** this line.  This is the only mode that shows fewer than   **/
/** 256 horizontal pixels and so we must deal with the border **/
/** (backdrop) here which is always the background color.     **/
/***************************************************************/
ITCM_CODE void RefreshLine0(u8 Y)
{
  register byte *T,K,Offset;
  register byte *P,FC,BC;
  u16 word1=0, word2=0, word3=0;

  BG_PALETTE[0] = BG_PALETTE[17]; // Legacy Handling
  
  P=XBuf+(Y<<8);
  BC = BGColor;
  FC = FGColor;

  if(!ScreenON)
    memset(P,BGColor,256);
  else
  {
    T=ChrTab+(Y>>3)*40;
    Offset=Y&0x07;

    u8 lastT = ~(*T);

    memset(P, BGColor, 8);  // Fill the first 8 pixels with background color since the screen in TEXT mode is 240 pixels and needs the border filled
    P += 8;                 // For this TEXT mode, we shift in 8 pixels to center the screen. We memset the background color to the first and last 8 pixels of a line to blank them.

    for(int X=0;X<40;X++)
    {
      if (lastT != *T) // Is this set of pixels different than the last one?
      {
          lastT=*T;
          K=ChrGen[((int)*T<<3)+Offset];
          P[0]=K&0x80? FC:BC;
          P[1]=K&0x40? FC:BC;
          P[2]=K&0x20? FC:BC;
          P[3]=K&0x10? FC:BC;
          P[4]=K&0x08? FC:BC;
          P[5]=K&0x04? FC:BC;
          // Save the data as we are likely to just repeat it...
          word1 = *((u16*)(P+0));
          word2 = *((u16*)(P+2));
          word3 = *((u16*)(P+4));
      }
      else // No change so we can just blast repeat the 6 bytes (16-bits at at time). This occurs frequently and saves us time.
      {
          u16 *destPtr = (u16*)P;
          *destPtr++ = word1;
          *destPtr++ = word2;
          *destPtr   = word3;
      }
      P+=6;T++;
    }

    memset(P, BGColor, 8);  // Fill the last 8 pixels with background color since the screen in TEXT mode is 240 pixels and needs the border filled
  }
}

/** RefreshLine1() *******************************************/
/** Refresh line Y (0..191) of SCREEN1, including sprites   **/
/** in this line.                                           **/
/*************************************************************/
ITCM_CODE void RefreshLine1(u8 uY)
{
  register byte K=0,Offset,FC,BC;
  register u8 *T;
  register u32 *P;
  u8 lastT;
  
  BG_PALETTE[0] = BG_PALETTE[17]; // Legacy Handling

  P=(u32*) (XBuf+(uY<<8));
  u32 ptLow = 0; u32 ptHigh = 0;

  if(!ScreenON)
    memset(P,BGColor,256);
  else
  {
    T=ChrTab+((int)(uY&0xF8)<<2);
    Offset=uY&0x07;

    lastT = ~(*T);

    for(int X=0;X<32;X++)
    {
      if (lastT != *T)
      {
          lastT=*T;
          BC=ColTab[lastT>>3];
          K=ChrGen[((int)lastT<<3)+Offset];
          FC=BC>>4;
          BC=BC&0x0F;
          u32* ptLut = (u32*) (lutTablehh[FC][BC]);
          ptLow = *(ptLut + ((K>>4)));
          ptHigh= *(ptLut + ((K & 0xF)));
      }
      *P++ = ptLow;
      *P++ = ptHigh;
      T++;
    }
    RefreshSprites(uY);
  }
}

/** RefreshLine2() *******************************************/
/** Refresh line Y (0..191) of SCREEN2, including sprites   **/
/** in this line.                                           **/
/*************************************************************/
ITCM_CODE void RefreshLine2(u8 uY) {
  u32 *P;
  register byte FC,BC;
  register byte K,*T;
  u16 J,I;

  BG_PALETTE[0] = BG_PALETTE[17]; // Legacy Handling
  
  P=(u32*)(XBuf+(uY<<8));

  if (!ScreenON)
    memset(P,BGColor,256);
  else
  {
    u32 ptLow = 0; u32 ptHigh = 0;

    J   = ((u16)((u16)uY&0xC0)<<5)+(uY&0x07);
    T   = ChrTab+((u16)((u16)uY&0xF8)<<2);
    u8 lastT = ~(*T);

    for(int X=0;X<32;X++)
    {
      if (lastT != *T)
      {
          lastT = *T;
          I    = (u16)lastT<<3;
          K    = ColTab[(J+I)&ColTabM];
          FC   = (K>>4);
          BC   = K & 0x0F;
          K    = ChrGen[(J+I)&ChrGenM];
          u32* ptLut = (u32*)(lutTablehh[FC][BC]);
          ptLow = *(ptLut + ((K>>4)));
          ptHigh = *(ptLut + ((K & 0xF)));
      }
      *P++ = ptLow;
      *P++ = ptHigh;
      T++;
    }

    RefreshSprites(uY);
  }
}

/** RefreshLine3() *******************************************/
/** Refresh line Y (0..191) of SCREEN3, including sprites   **/
/** in this line.                                           **/
/*************************************************************/
ITCM_CODE void RefreshLine3(u8 uY)
{
  byte X,K,Offset;
  byte *P,*T;
  u8 lastT;
  P=XBuf+(uY<<8);

  BG_PALETTE[0] = BG_PALETTE[17]; // Legacy Handling
  
  if(!VDP9938_ScreenON)
  {
    memset(P,BGColor,256);
  }
  else {
    u8 ptLow = 0; u8 ptHigh = 0;
    T=ChrTab+((int)(uY&0xF8)<<2);
    lastT = ~(*T);
    Offset=(uY&0x1C)>>2;
    u32 dword1=0, dword2=0;
    for(X=0;X<32;X++)
    {
      if (lastT != *T)
      {
          lastT = *T;
          K=ChrGen[((int)lastT<<3)+Offset];
          ptLow = K>>4;
          ptHigh = K&0x0F;
          P[0]=P[1]=P[2]=P[3]=ptLow;
          P[4]=P[5]=P[6]=P[7]=ptHigh;
          dword1 = *((u32*)(P+0));
          dword2 = *((u32*)(P+4));
      }
      else
      {
          u32 *destPtr = (u32*)P;
          *destPtr++ = dword1;
          *destPtr   = dword2;
      }
      P+=8;T++;
    }
    RefreshSprites(uY);
  }
}

/** RefreshLine4() *******************************************/
/** Refresh line Y (0..191) of SCREEN4, including sprites   **/
/** in this line.                                           **/
/*************************************************************/
ITCM_CODE void RefreshLine4(u8 uY)
{
  u32 *P;
  register byte FC,BC;
  register byte K,*T;
  u16 J,I;

  BG_PALETTE[0] = BG_PALETTE[16]; // MSX2 Handling
  
  P=(u32*)(XBuf+((uY+VScroll)<<8));

  if (!ScreenON)
    memset(P,BGColor,256);
  else
  {
    u32 ptLow = 0; u32 ptHigh = 0;

    J   = ((u16)((u16)uY&0xC0)<<5)+(uY&0x07);
    T   = ChrTab+((u16)((u16)uY&0xF8)<<2);
    u8 lastT = ~(*T);

    for(int X=0;X<32;X++)
    {
      if (lastT != *T)
      {
          lastT = *T;
          I    = (u16)lastT<<3;
          K    = ColTab[(J+I)&ColTabM];
          FC   = (K>>4);
          BC   = K & 0x0F;
          K    = ChrGen[(J+I)&ChrGenM];
          u32* ptLut = (u32*)(lutTablehh[FC][BC]);
          ptLow = *(ptLut + ((K>>4)));
          ptHigh = *(ptLut + ((K & 0xF)));
      }
      *P++ = ptLow;
      *P++ = ptHigh;
      T++;
    }

    RefreshSprites(uY);
  }
}


/*********************************************************************************
 * Emulator calls this function to write byte 'value' into a VDP register 'iReg'
 ********************************************************************************/
u8 VDP_RegisterMasks[] __attribute__((section(".dtcm"))) = { 0x1f,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                                                             0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                                                             0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                                                             0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                                                             0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                                                             0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                                                             0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                                                             0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff };
byte SprHeights[4] __attribute__((section(".dtcm"))) = { 8,16,16,32 };


//The VDP Video Modes (BASIC Screen 0-8) is selected by the Bits M1-M5 of VDP Register 0 and 1. The relationship between the bits and the screen is:
//  M1 M2 M5 M4 M3  Screen format
//  1  0  0  0  0   Text       40x24             (BASIC SCREEN 0)
//  0  0  0  0  0   Half text  32x24             (BASIC SCREEN 1)
//  0  0  0  0  1   Hi resolution 256x192        (BASIC SCREEN 2)
//  0  1  0  0  0   Multicolour  4x4pix blocks   (BASIC SCREEN 3)
//  ----Below MSX2 only----
//  0  0  0  1  0   Screen2 with 8 Sprites/Line  (BASIC SCREEN 4)
//  0  0  0  1  1   256*212, 16  colours/pixel   (BASIC SCREEN 5)
//  0  0  1  0  0   512*212, 4   colours/pixel   (BASIC SCREEN 6)
//  0  0  1  0  1   512*212, 16  colours/pixel   (BASIC SCREEN 7)
//  0  0  1  1  1   256*212, 256 colours/pixel   (BASIC SCREEN 8)
//  1  0  0  1  0   Text 80x24                   (BASIC SCREEN 0, WIDTH 80)
void CheckNewMode(void)
{
  u16 newMode;

  // Figure out new screen mode number:
  // This yields:  M1 M2 M5 M4 M3 - this matches the table above
  switch(((VDP[0]&0x0E)>>1)|(VDP[1]&0x18))
  {
    case 0x10: newMode=0;break;
    case 0x00: newMode=1;break;
    case 0x01: newMode=2;break;
    case 0x08: newMode=3;break;
    case 0x02: newMode=4;break;
    case 0x03: newMode=5;break;
    case 0x04: newMode=6;break;
    case 0x05: newMode=7;break;
    case 0x07: newMode=8;break;
    case 0x12: newMode=0;break; // Really 80 columns but ...
    default:   newMode=ScrMode;break;
  }
  
  ScrMode=newMode;

  RefreshLine = SCR[ScrMode].Refresh;
  OH = SprHeights[VDP[1]&0x03];
  IH = SprHeights[VDP[1]&0x02];

  u32 I=(ScrMode>6) ? 11:10;
  ChrTab=pVDPVidMem+(((int)(VDP[2]&SCR[ScrMode].R2)<<I));
  ChrGen=pVDPVidMem+(((int)(VDP[4]&SCR[ScrMode].R4)<<11));
  ColTab=pVDPVidMem+(((int)(VDP[3]&SCR[ScrMode].R3)<<6)) + ((int)VDP[10]<<14);
  SprTab=pVDPVidMem+(((int)(VDP[5]&SCR[ScrMode].R5)<<7)) + ((int)VDP[11]<<15);
  SprGen=pVDPVidMem+((int)(VDP[6]<<11));

  ChrTabM = ((int)(VDP[2]|(u8)~SCR[ScrMode].M2)<<I)|((1<<I)-1);
  ChrGenM = ((int)(VDP[4]|(u8)~SCR[ScrMode].M4)<<11)|0x07FF;
  ColTabM = ((int)(VDP[3]|(u8)~SCR[ScrMode].M3)<<6) |0x1C03F;
  SprTabM = ((int)(VDP[5]|(u8)~SCR[ScrMode].M5)<<7) |0x1807F;
}


ITCM_CODE void Write9938(u8 iReg, u8 value)
{
  /* There are VDP registers - map down to these and mask off irrelevant bits */
  iReg &= 0x3f;
  value &= VDP_RegisterMasks[iReg];

  // Clearing the VDP Line interrupt can drop the IRQ
  if ((iReg == 0) && ((VDPStatus[1]&0x01)&&!(value&0x10)))
  {
      VDPStatus[1]&=0xFE;
      SetVDPIRQ(VDP_IRQ_LINE, 0);
  }    
  
  /* Enabling IRQs may cause an IRQ here */
  if ((iReg==1) && ((VDP[1]^value)&value&VDP9938_REG1_IRQ) && (VDPStatus[0]&VDP9938_STAT_VBLANK))
  {
      SetVDPIRQ(VDP_IRQ_VBLANK, 1);
  }
  
  /* There are VDP registers - map down to these and mask off irrelevant bits */
  value &= VDP_RegisterMasks[iReg]; 

  /* Store value into the register */
  VDP[iReg]=value;
  
   /* Depending on the register, do... */
  switch (iReg)
  {
    case  7:
      FGColor=value>>4;
      BGColor=value&0x0F;

      // Handle "transparency"
      u8 r = (u8) ((float) VDP9938A_palette[BGColor*3+0]*0.121568f);
      u8 g = (u8) ((float) VDP9938A_palette[BGColor*3+1]*0.121568f);
      u8 b = (u8) ((float) VDP9938A_palette[BGColor*3+2]*0.121568f);
      
      BG_PALETTE[17] = RGB15(r,g,b); // We swap it into BG_PALETTE[0] when we render lines...
      break;

    case 10:
    case 11:
      ColTab=pVDPVidMem+(((int)(VDP[3]&SCR[ScrMode].R3)<<6)) + ((int)VDP[10]<<14);
      SprTab=pVDPVidMem+(((int)(VDP[5]&SCR[ScrMode].R5)<<7)) + ((int)VDP[11]<<15);
      break;

    case 14: VPAGE=pVDPVidMem+((int)VDP[14]<<14); break;
    
    case 44: VDPWrite(value); break;
    case 46: VDPDraw(value);break;
  }

  if (iReg < 7) CheckNewMode();
}


/** RdData9938() *********************************************/
/** Read a value from the VDP Data Port.                    **/
/*************************************************************/
ITCM_CODE byte RdData9938(void)
{
  byte data = VDPDlatch;
  VDPDlatch = VPAGE[VAddr];
  VAddr = (VAddr+1)&0x3FFF;
  
  if(!VAddr&&(ScrMode>3))
  {
    VDP[14]=(VDP[14]+1)&7;
    VPAGE=pVDPVidMem+((int)VDP[14]<<14);
  }

  VDPCtrlLatch = 0;

  return(data);
}


/** DirectRegWrite9938() *************************************/
/** VDP9938 direct register write via port 0x9B             **/
/*************************************************************/
ITCM_CODE void DirectRegWrite9938(u8 Value)
{
    u8 reg = VDP[17] & 0x3F;

    // Port 0x9B is the indirect register-write port: the value is written
    // into the register pointed to by R#17. R#17 bit 7 (0x80) is the auto
    // increment inhibit flag; when it is CLEAR the pointer auto-increments.
    // Bit 6 is always 0.  Register R#17 is never overwritten via this.
    if (reg != 17)
    {
        Write9938(reg, Value);
    }

    // Allow auto-increment?
    if ((VDP[17] & 0x80) == 0)
    {
        reg = (reg+1) & 0x3F;
        VDP[17] = (VDP[17] & 0x80) | reg;
    }
}

/** WrCtrl9938() *********************************************/
/** Write a value V to the VDP Control Port. Enabling IRQs  **/
/** in this function may cause an IRQ to be generated.      **/
/*************************************************************/
ITCM_CODE void WrCtrl9938(byte value)
{
  if(VDPCtrlLatch)  // Write the high byte of the video address
  {
    VDPCtrlLatch=0; // Set the VDP flip-flop so we do the low byte next

    switch(value&0xC0)
    {
      case 0x80:
      case 0xC0:
        Write9938(value&0x3F,ALatch); // Write VDP9938: registers 0-63
        break;

      case 0x00:
      case 0x40:
        VAddr=(((uint16_t)value<<8)+ALatch)&0x3FFF;
        /* When set for reading, perform first read */
        if(!(value&0x40))
        {
            VDPDlatch = VPAGE[VAddr];
            VAddr = (VAddr+1)&0x3FFF;
            if(!VAddr&&(ScrMode>3))
            {
                VDP[14]=(VDP[14]+1)&7;
                VPAGE=pVDPVidMem+((int)VDP[14]<<14);
            }
        }
        break;
    }
  }
  else  // Write the low byte of the video address / control register
  {
      VDPCtrlLatch=1;   // Set the VDP flip-flow so we do the high byte next
      ALatch=value;
  }
}


/** RdCtrl9938() ************************************************/
/** Read a value from the VDP Control Port (Read Status Bytes) **/
/****************************************************************/
ITCM_CODE byte RdCtrl9938(void)
{
  byte data = VDPStatus[VDP[15]];
  VDPCtrlLatch = 0;

  if (VDP[15] == 0) // Status register 0
  {
      VDPStatus[0] &= 0x1F; // Top bits are cleared on a read... This clears the VBLANK interrupt.
      SetVDPIRQ(VDP_IRQ_VBLANK, 0);
  }
  else if (VDP[15] == 1) // Status register 1
  {
      VDPStatus[1] &= 0xFE;  // Clear the LINE interrupt.
      SetVDPIRQ(VDP_IRQ_LINE, 0);
  }
  else if (VDP[15] == 2) // Status register 2
  {
      // Check if we are in the visible screen area
      if (CurLine < VDP9938_START_LINE || CurLine > VDP9938_END_LINE) data |= 0x40;

      // A standard scanline has 228 Z80 cycles. H-Blank typically kicks in 
      // roughly around cycle 170-174 depending on display widths.
      if (CPU.ICount < 60)
      {
          data |= 0x20; // Set HR Flag (Bit 5) -> We are inside H-Blank
      }
  }
  else if (VDP[15] == 7)
  {
      data=VDPStatus[7]=VDP[44]=VDPRead();
  }

  return(data);
}


/** Loop9938() ***********************************************/
/** Call this routine on every scanline to update the       **/
/** screen buffer. Loop9938() returns 1 if an interrupt is  **/
/** to be generated, 0 otherwise.                           **/
/*************************************************************/
u8  Internal_LineCounter = 0;

void Loop9938(void)
{
  // 1. Get the 0-indexed display scanline relative to the active display area
  int scanline = CurLine - VDP9938_START_LINE;

  // 2. Perform the absolute V9938 match check (accounts for vertical scroll R#23)
  // VDP[23] is the Vertical Scroll Offset, VDP[19] is the Line Interrupt Register
  if (((scanline + VDP[23]) & 255) == VDP[19])
  {
      VDPStatus[1] |= 0x01; // Set FH flag in S#1
      if (VDP9938_LineSync)
      {
         SetVDPIRQ(VDP_IRQ_LINE, 1);
      }
  }
  else
  {
      /* Reset flag immediately if IE1 interrupt disabled */
      if(!(VDP[0]&0x10)) VDPStatus[1]&=0xFE;
  }
  
  if (++CurLine >= VDP9938_LINES)
  {
      CurLine=0;
      /* When reaching end of screen, reset line coincidence */
      VDPStatus[1]&=0xFE;
      SetVDPIRQ(VDP_IRQ_LINE, 0);
  }
  else
  /* If refreshing display area, call scanline handler */
  if ((CurLine >= VDP9938_START_LINE) && (CurLine < VDP9938_END_LINE))
  {
      RefreshLine(CurLine - VDP9938_START_LINE);

      // ---------------------------------------------------------------------
      // Some programs require that we handle collisions more frequently
      // than just end of frame. So we check every 64 scanlines (or 255 if
      // we are the older DS-Lite/Phat). This is somewhat CPU intensive so
      // we are careful how often we run it - especially on older hardware.
      // ---------------------------------------------------------------------
      if ((CurLine % (isDSiMode() ? 64:255)) == 0)
      {
          if(!(VDPStatus[0]&VDP9938_STAT_OVRLAP)) // If not already in collision...
          {
            if(CheckSprites()) VDPStatus[0]|=VDP9938_STAT_OVRLAP; // Set the collision bit
          }
      }
  }
  /* If time for emulated VBlank... */
  else if (CurLine == VDP9938_END_LINE)
  {
      // -------------------------------------
      // !!!Into the Vertical Blank!!!
      // -------------------------------------
      frame_number++;

      /* Generate IRQ when enabled and when VBlank flag goes up */
      if (VDP9938_VBlankON && !(VDPStatus[0]&VDP9938_STAT_VBLANK))
      {
          SetVDPIRQ(VDP_IRQ_VBLANK, 1);
      }

      /* Set VBlank status flag */
      VDPStatus[0] |= VDP9938_STAT_VBLANK;

      /* Set Sprite Collision status flag */
      if(!(VDPStatus[0]&VDP9938_STAT_OVRLAP))
      {
          if(CheckSprites()) VDPStatus[0] |= VDP9938_STAT_OVRLAP;
      }
  }
}

ITCM_CODE void RefreshLine5(register u8 uY) 
{
    BG_PALETTE[0] = BG_PALETTE[16]; // MSX2 Solid
    
    if (!VDP9938_ScreenON) {
        memset(XBuf + (uY << 8), BGColor, 256);
        return;
    }
    
    // Sadly, the DS-Lite/Phat need some help...
    if (!isDSiMode())
    {
        extern u8 skip_render;
        extern u16 timingFrames;
        if (timingFrames & 1) {skip_render=1; return;}
    }
    
    u32 *dst32 = (u32*)((u8*)XBuf + ((u32)uY << 8));

    const u8 *src = ChrTab + (((u32)(uY+VScroll) << 7) & ChrTabM & 0x7FFF);
    if (FlipEvenOdd && OddPage && pVDPVidMem <= src - 0x8000) src -= 0x8000;

    for (int i = 0; i < 128; i += 8) 
    {
        u32 s0 = *(u32*)(src + i);
        u32 s1 = *(u32*)(src + i + 4);

        u32 r0 = nibbleLUT16[s0 & 0xFF]         | (nibbleLUT16[(s0 >> 8)  & 0xFF] << 16);
        u32 r1 = nibbleLUT16[(s0 >> 16) & 0xFF] | (nibbleLUT16[(s0 >> 24) & 0xFF] << 16);
        u32 r2 = nibbleLUT16[s1 & 0xFF]         | (nibbleLUT16[(s1 >> 8)  & 0xFF] << 16);
        u32 r3 = nibbleLUT16[(s1 >> 16) & 0xFF] | (nibbleLUT16[(s1 >> 24) & 0xFF] << 16);

        dst32[0] = r0; dst32[1] = r1; dst32[2] = r2; dst32[3] = r3;
        dst32 += 4;
    }
    ColorSprites(uY);
}

/** RefreshLine6() ********************************************/
/** Refresh VDP9938 Screen 6: 512x192, 4 colors bitmap     **/
/*************************************************************/
ITCM_CODE void RefreshLine6(register u8 uY) 
{
    BG_PALETTE[0] = BG_PALETTE[16]; // MSX2 Solid

    if (!VDP9938_ScreenON) {
        memset(XBuf + (uY << 8), BGColor, 256); 
        return;
    }

    register u32 *destPtr32 = (u32*)(XBuf + (uY << 8));
    u32 addr = ((u32)((uY + VScroll) & 1023) << 7);
    register u8 *srcPtr = &ChrTab[addr];

    // Loops 64 times. Processes exactly 128 source bytes.
    // Each iteration reads 2 source bytes and generates 4 destination pixels (1 word).
    for (int i = 0; i < 128; i += 2) {
        register u32 b0 = srcPtr[i];
        register u32 b1 = srcPtr[i+1];

        // This extracts two distinct 2-bit pixels per source byte.
        // It produces 4 continuous horizontal pixels, mapping perfectly 
        // to a 256-pixel wide screen without skipping half the line or overrunning.
        register u32 word = ((b0 >> 6) & 0x03) |
                            (((b0 >> 2) & 0x03) << 8) |
                            (((b1 >> 6) & 0x03) << 16) |
                            (((b1 >> 2) & 0x03) << 24);

        *destPtr32++ = word;
    }

    ColorSprites(uY);
}




/** RefreshLine7() ********************************************/
/** Refresh VDP9938 Screen 7: 512x212, 16 colors bitmap    **/
/*************************************************************/
ITCM_CODE void RefreshLine7(register u8 uY)
{
    BG_PALETTE[0] = BG_PALETTE[16]; // MSX2 Solid

    if (!VDP9938_ScreenON) { memset(XBuf + (uY << 8), BGColor, 256); return; }

    u32 *dst32 = (u32*)((u8*)XBuf + ((u32)uY << 8));
    const u8 *src = ChrTab + (((u32)((uY + VScroll) & 511)) << 8);

    for (int i = 0; i < 256; i += 8) {
        u32 b0 = screen7LUT[src[i+0]];
        u32 b1 = screen7LUT[src[i+1]];
        u32 b2 = screen7LUT[src[i+2]];
        u32 b3 = screen7LUT[src[i+3]];
        u32 b4 = screen7LUT[src[i+4]];
        u32 b5 = screen7LUT[src[i+5]];
        u32 b6 = screen7LUT[src[i+6]];
        u32 b7 = screen7LUT[src[i+7]];

        dst32[0] = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
        dst32[1] = b4 | (b5 << 8) | (b6 << 16) | (b7 << 24);
        dst32 += 2;
    }

    ColorSprites(uY);
}

/** RefreshLine8() ********************************************/
/** Refresh VDP9938 Screen 8: 256x192, 256 colors bitmap   **/
/*************************************************************/
ITCM_CODE void RefreshLine8(register u8 uY)
{
    BG_PALETTE[0] = BG_PALETTE[16]; // MSX2 Solid

    if (!VDP9938_ScreenON) {memset(XBuf+(uY<<8),BGColor,256); return;}

    memcpy(XBuf + (uY << 8), ChrTab + ((uY+VScroll) << 8), 256);
    
    ColorSprites(uY);
}

void Reset9938(void)
{
    memset(pVDPVidMem, 0x00, 0x20000);   // Reset Video memory (128K for VDP9938)
    memset(VDP, 0x00, sizeof(VDP));
    memset(VDPStatus, 0x00, sizeof(VDPStatus));
    
    BuildNibbleLUT();
    BuildScreen7LUT();
    
    memset(OccBuf,0,sizeof(OccBuf));

    VDP[0] = 0x02;                      // Graphic mode enabled
    VDP[1] = 0xE0;                      // 16K VRAM, IRQ enable, high-res mode
    VDP[2] = 0x00;                      // Name table for text modes
    VDP[3] = 0x00;                      // Color table
    VDP[4] = 0x00;                      // Pattern generator
    VDP[5] = 0x00;                      // Sprite attribute table
    VDP[6] = 0x00;                      // Sprite generator table
    VDP[7] = 0x00;                      // FG/BG colors

    VDPCtrlLatch=0;
    VAddr = 0x0000;
    FGColor=BGColor=0;
    ScrMode=0;                          // Default to Screen 0 for VDP9938
    CurLine=0;
    ChrTab=ColTab=ChrGen=pVDPVidMem;
    SprTab=SprGen=pVDPVidMem;
    VDPDlatch = 0;
    VPAGE=pVDPVidMem;                           /* VRAM page        */
    frame_number = 0;
    msx_irq_pending = 0;

    ChrTabM = 0x3FFF;
    ColTabM = 0x3FFF;
    ChrGenM = 0x3FFF;
    SprTabM = 0x3FFF;

    BG_PALETTE[0] = RGB15(0x00,0x00,0x00);

    pVidFlipBuf = (u16*) (0x06000000);
    
    RefreshLine = RefreshLine0;

    OH = IH = 0;

    my_config_clear_int = myConfig.clearInt;

    // ---------------------------------------------------------------
    // Our background/foreground color table makes computations FAST!
    // ---------------------------------------------------------------
    int colfg,colbg;
    for (colfg=0;colfg<16;colfg++) {
        for (colbg=0;colbg<16;colbg++) {
          lutTablehh[colfg][colbg][ 0] = (colbg<<0) | (colbg<<8) | (colbg<<16) | (colbg<<24); // 0 0 0 0
          lutTablehh[colfg][colbg][ 1] = (colbg<<0) | (colbg<<8) | (colbg<<16) | (colfg<<24); // 0 0 0 1
          lutTablehh[colfg][colbg][ 2] = (colbg<<0) | (colbg<<8) | (colfg<<16) | (colbg<<24); // 0 0 1 0
          lutTablehh[colfg][colbg][ 3] = (colbg<<0) | (colbg<<8) | (colfg<<16) | (colfg<<24); // 0 0 1 1
          lutTablehh[colfg][colbg][ 4] = (colbg<<0) | (colfg<<8) | (colbg<<16) | (colbg<<24); // 0 1 0 0
          lutTablehh[colfg][colbg][ 5] = (colbg<<0) | (colfg<<8) | (colbg<<16) | (colfg<<24); // 0 1 0 1
          lutTablehh[colfg][colbg][ 6] = (colbg<<0) | (colfg<<8) | (colfg<<16) | (colbg<<24); // 0 1 1 0
          lutTablehh[colfg][colbg][ 7] = (colbg<<0) | (colfg<<8) | (colfg<<16) | (colfg<<24); // 0 1 1 1

          lutTablehh[colfg][colbg][ 8] = (colfg<<0) | (colbg<<8) | (colbg<<16) | (colbg<<24); // 1 0 0 0
          lutTablehh[colfg][colbg][ 9] = (colfg<<0) | (colbg<<8) | (colbg<<16) | (colfg<<24); // 1 0 0 1
          lutTablehh[colfg][colbg][10] = (colfg<<0) | (colbg<<8) | (colfg<<16) | (colbg<<24); // 1 0 1 0
          lutTablehh[colfg][colbg][11] = (colfg<<0) | (colbg<<8) | (colfg<<16) | (colfg<<24); // 1 0 1 1
          lutTablehh[colfg][colbg][12] = (colfg<<0) | (colfg<<8) | (colbg<<16) | (colbg<<24); // 1 1 0 0
          lutTablehh[colfg][colbg][13] = (colfg<<0) | (colfg<<8) | (colbg<<16) | (colfg<<24); // 1 1 0 1
          lutTablehh[colfg][colbg][14] = (colfg<<0) | (colfg<<8) | (colfg<<16) | (colbg<<24); // 1 1 1 0
          lutTablehh[colfg][colbg][15] = (colfg<<0) | (colfg<<8) | (colfg<<16) | (colfg<<24); // 1 1 1 1
        }
    }
}

// End of file
