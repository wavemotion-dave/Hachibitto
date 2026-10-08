// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave and
// Marat Fayzullin (fMSX core) are thanked profusely.
//
// The Hachibitto emulator is offered as-is, without any warranty. Please see readme.md
// =====================================================================================
#include <nds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fat.h>
#include <dirent.h>

#include "Hachibitto.h"
#include "CRC32.h"
#include "cpu/z80/Z80_interface.h"
#include "MSX_generic.h"
#include "fdc.h"
#include "printf.h"

// -------------------------------------------------------------
// Some MSX Mapper / Slot Handling and sound generation stuff...
// -------------------------------------------------------------
u8 bCartInPage[4]           __attribute__((section(".dtcm"))) = {0,0,0,0};
u8 bRAMInPage[4]            __attribute__((section(".dtcm"))) = {0,0,0,0};

u8 *MSXCartPtr[2][8]        __attribute__((section(".dtcm"))) = { {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0} };
u8 *MSXRamPtr[8]            __attribute__((section(".dtcm"))) = {0,0,0,0,0,0,0,0};

u16 beeperFreq              __attribute__((section(".dtcm"))) = 0;      // Crude square wave beeper handling. Not much uses this.
u8 msx_scc_capable_game     __attribute__((section(".dtcm"))) = 0;      // True if game is making use of SCC / SCC+
u8 msx_music_capable_game   __attribute__((section(".dtcm"))) = 0;      // True if game is making use of MSX-MUSIC (YM Sound)
u8 special_memory_access    __attribute__((section(".dtcm"))) = 0x00;   // This lets us know we've got something special mapped into memory
u32 msx_music_writes        __attribute__((section(".dtcm"))) = 0;      // If we cross a threshold, we decare we are MSX-MUSIC capable (and mix in the proper sound)
u8 msx_subslot              __attribute__((section(".dtcm"))) = 0x00;   // Only one slot is expanded so we only need to track one register here

SCC     mySCC               __attribute__((section(".dtcm")));          // Declare new SCC module for Konami MSX games that use it
AY38910 myAY                __attribute__((section(".dtcm")));          // Declare new AY structure for basic MSX sounds
AY38910 myAY2               __attribute__((section(".dtcm")));          // Declare new AY structure for 2x PSG
YM      myYM                __attribute__((section(".dtcm")));          // Declare new YM module (MSX-MUSIC) for MSX games that use it

// ---------------------------------------------------------------------
// Konami SCC+ 64K RAM Cartridge (flash-cart style: 8x8K RAM pages)
// ---------------------------------------------------------------------
u8  sccplus_page[4]     __attribute__((section(".dtcm"))) = {0,1,2,3}; // Last byte written to each of the 4 select regs
u8  sccplus_mode        __attribute__((section(".dtcm"))) = 0x00;      // BFFE/BFFF: bit5=RAM mode, bit4=SCC+ compat

// --------------------------------------------------------------------------
// These aren't used very often so we don't need them in fast .dtcm memory
// --------------------------------------------------------------------------
u16 msx_init            = 0x4000;
u16 msx_basic           = 0x0000;
u8 mirror_ram_bank[4]   = {3,2,1,0}; // For port readback mostly... though not all MSX2 machines report it back.

static u8 Unmapped_Memory[0x2000]; // Full of 0xFF values. We point all memory segments here that are not mapped to some other device.

static uint8_t rtc_reg  = 0;      // Selected register index (0-15)
static uint8_t rtc_bank = 0;      // Active bank selected by Reg 13 (0-3)

// Complete 4-bank RAM array initialized with valid MSX2 checksums
static uint8_t rtc_ram[4][16] = {
    // Bank 0: Time & Date Tracking
    { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0, 0x01, 0, 0 },

    // Bank 1: Alarm Settings (Do not place System/Screen settings here)
    { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0, 0, 0, 0 },

    // Bank 2: MSX system initialization settings
    {
        0x0A,  // Reg 0: RTC settings valid — REQUIRED
        0x00,  // Reg 1: X adjust
        0x00,  // Reg 2: Y adjust
        0x00,  // Reg 3: SCREEN 0, normal display
        0x08,  // Reg 4: WIDTH low nibble
        0x02,  // Reg 5: WIDTH high bits (40 columns)
        0x0F,  // Reg 6: Initial text color
        0x04,  // Reg 7: Initial background color
        0x04,  // Reg 8: Initial border color
        0x00,  // Reg 9: Default transfer/keyboard options
        0x00,  // Reg 10: BEEP settings
        0x00,  // Reg 11: Startup logo colors
        0x01,  // Reg 12: Area code (US)
        0x00, 0x00, 0x00
    },

    // Bank 3: Color Palette Settings + General Validation Checksums
    { 0x04, 0x04, 0x07, 0x05, 0x02, 0x02, 0x07, 0x07,
      0x01, 0x01, 0x03, 0x03, 0x08, 0x05, 0x00, 0x00 }
};

// WRITE PORT 0xB4: Selects Register Index ONLY
void write_port_RTC_index(uint8_t data)
{
    rtc_reg = data & 0x0F; // Only lower 4 bits pick the register!
}

// WRITE PORT 0xB5: Writes Data into the current Bank & Register
void write_port_RTC_data(uint8_t data)
{
    uint8_t val = data & 0x0F;

    // Store data in current active bank
    rtc_ram[rtc_bank][rtc_reg] = val;

    // IF writing to Register 13 (Mode Register), update the ACTIVE BANK!
    if (rtc_reg == 13 || rtc_reg == 0x0D) {
        rtc_bank = val & 0x03; // Bits 0-1 select Banks 0-3
    }
}

// RTC Register Layout Constants for the date/time handling
#define R_1_SEC    0
#define R_10_SEC   1
#define R_1_MIN    2
#define R_10_MIN   3
#define R_1_HOUR   4
#define R_10_HOUR  5
#define R_WEEK     6
#define R_1_DAY    7
#define R_10_DAY   8
#define R_1_MON    9
#define R_10_MON   10
#define R_1_YEAR   11
#define R_10_YEAR  12

/**
 * Splits a decimal value into individual 4-bit BCD nibbles
 * and stores them sequentially into the RTC RAM array.
 */
void store_nibbles(uint8_t rtc_sub_ram[], int index_1, int index_10, int val)
{
    rtc_sub_ram[index_1]  = (uint8_t)(val % 10);        // Lower 4-bits (Units)
    rtc_sub_ram[index_10] = (uint8_t)((val / 10) % 10); // Upper 4-bits (Tens)
}

// ---------------------------------------------------------------------------------------
// Grab system time/date and populate RTC registers... unlikely any program really cares.
// ---------------------------------------------------------------------------------------
void populate_msx_rtc()
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    // 1. Time Components
    store_nibbles(rtc_ram[0], R_1_SEC, R_10_SEC, t->tm_sec);
    store_nibbles(rtc_ram[0], R_1_MIN, R_10_MIN, t->tm_min);
    store_nibbles(rtc_ram[0], R_1_HOUR, R_10_HOUR, t->tm_hour);

    // 2. Day of the Week (0 = Sunday, 6 = Saturday)
    // Matches the native C struct tm standard perfectly for MSX2.
    rtc_ram[0][R_WEEK] = (uint8_t)t->tm_wday;

    // 3. Calendar Components (struct tm uses 0-11 for months; RP5C01 expects 1-12)
    store_nibbles(rtc_ram[0], R_1_DAY, R_10_DAY, t->tm_mday);
    store_nibbles(rtc_ram[0], R_1_MON, R_10_MON, t->tm_mon + 1);

    // 4. Year Tracking (2-digit, 00-99)
    int year_2_digits = t->tm_year % 100;
    store_nibbles(rtc_ram[0], R_1_YEAR, R_10_YEAR, year_2_digits);
}

// READ PORT 0xB5: Reads Data from current Bank & Register
uint8_t read_port_RTC_data(void)
{
    populate_msx_rtc(); // Get current system time/date and populate RTC

    uint8_t val = rtc_ram[rtc_bank][rtc_reg] & 0x0F;

    // Reg 13 (Mode Register): Force BUSY flag (Bit 1) to 0
    if (rtc_reg == 13)
    {
        val &= ~0x02; // Bit 1 = 0 (NOT BUSY)
    }

    return val;
}

// ------------------------------------------------------------------------------------------
// Keyboard / Joystick reading is generally done every frame so at most 60 times per second.
// It's fine to keep this out of ITCM fast memory as it's not a hot spot for emulation.
// ------------------------------------------------------------------------------------------
u8 readport_keyboard(void)
{
      // ----------------------------------------------------------
      // Keyboard Port (Japanese Layout)
      //  Row   Bit_7 Bit_6 Bit_5 Bit_4 Bit_3 Bit_2 Bit_1 Bit_0
      //   0     "7"   "6"   "5"   "4"   "3"   "2"   "1"   "0"
      //   1     ":"   ";"   "["   YEN   "="   "-"   "9"   "8"
      //   2     "B"   "A"  DEAD   /     "."   ","   "]"  QUOTE
      //   3     "J"   "I"   "H"   "G"   "F"   "E"   "D"   "C"
      //   4     "R"   "Q"   "P"   "O"   "N"   "M"   "L"   "K"
      //   5     "Z"   "Y"   "X"   "W"   "V"   "U"   "T"   "S"
      //   6     F3    F2    F1   KANA   CAPS GRAPH CTRL  SHIFT
      //   7     RET   SEL   BS   STOP   TAB   ESC   F5    F4
      //   8    RIGHT DOWN   UP   LEFT   DEL   INS  HOME  SPACE
      // ----------------------------------------------------------

      // For the full keyboard overlay... this is a bit of a hack for SHIFT and CTRL
      if (last_special_key != 0)
      {
          if ((last_special_key_dampen > 0) && (last_special_key_dampen != 20))
          {
              if (--last_special_key_dampen == 0)
              {
                  last_special_key = 0;
              }
          }

          if (last_special_key == KBD_KEY_SHIFT)
          {
            key_shift = 1;
          }
          else if (last_special_key == KBD_KEY_CTRL)
          {
            key_ctrl = 1;
          }
          else if (last_special_key == KBD_KEY_KANA)
          {
            key_kana = 1;
          }
          else if (last_special_key == KBD_KEY_GRAPH)
          {
            key_graph = 1;
          }

          if ((kbd_key != 0) && (kbd_key != KBD_KEY_SHIFT) && (kbd_key != KBD_KEY_CTRL) && (kbd_key != KBD_KEY_KANA) && (kbd_key != KBD_KEY_GRAPH))
          {
              if (last_special_key_dampen == 20) last_special_key_dampen = 19;    // Start the SHIFT/CONTROL countdown... this should be enough time for it to register
          }
      }

      u8 key1 = 0x00;   // Accumulate keys here...

      // -------------------------------------------------
      // Check every key that might have been pressed...
      // -------------------------------------------------
      for (u8 i=0; i< (kbd_keys_pressed ? kbd_keys_pressed:1); i++) // Always one pass at least for joysticks...
      {
          kbd_key = kbd_keys[i];

          if ((Port_PPI_C & 0x0F) == 0)      // Row 0
          {
              if (kbd_key)
              {
                  if (kbd_key == '0')           key1  |= 0x01;
                  if (kbd_key == '1')           key1  |= 0x02;
                  if (kbd_key == '2')           key1  |= 0x04;
                  if (kbd_key == '3')           key1  |= 0x08;
                  if (kbd_key == '4')           key1  |= 0x10;
                  if (kbd_key == '5')           key1  |= 0x20;
                  if (kbd_key == '6')           key1  |= 0x40;
                  if (kbd_key == '7')           key1  |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 1)  // Row 1
          {
              if (kbd_key)
              {
                  if (kbd_key == '8')           key1 |= 0x01;
                  if (kbd_key == '9')           key1 |= 0x02;
                  if (kbd_key == '-')           key1 |= 0x04;
                  if (kbd_key == '=')           key1 |= 0x08;
                  if (kbd_key == '|')           key1 |= 0x10; // YEN
                  if (kbd_key == '@')           key1 |= 0x20;
                  if (kbd_key == '[')           key1 |= 0x40;
                  if (kbd_key == ';')           key1 |= 0x80;
                  if (kbd_key == ':')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 2)  // Row 2
          {
              if (kbd_key)
              {
                  if (kbd_key == KBD_KEY_QUOTE) key1 |= 0x01;
                  if (kbd_key == ']')           key1 |= 0x02;
                  if (kbd_key == ',')           key1 |= 0x04;
                  if (kbd_key == '.')           key1 |= 0x08;
                  if (kbd_key == '/')           key1 |= 0x10;
                  if (kbd_key == KBD_KEY_DEAD)  key1 |= 0x20;
                  if (kbd_key == 'A')           key1 |= 0x40;
                  if (kbd_key == 'B')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 3)  // Row 3
          {
              if (kbd_key)
              {
                  if (kbd_key == 'C')           key1 |= 0x01;
                  if (kbd_key == 'D')           key1 |= 0x02;
                  if (kbd_key == 'E')           key1 |= 0x04;
                  if (kbd_key == 'F')           key1 |= 0x08;
                  if (kbd_key == 'G')           key1 |= 0x10;
                  if (kbd_key == 'H')           key1 |= 0x20;
                  if (kbd_key == 'I')           key1 |= 0x40;
                  if (kbd_key == 'J')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 4)  // Row 4
          {
              if (kbd_key)
              {
                  if (kbd_key == 'K')           key1 |= 0x01;
                  if (kbd_key == 'L')           key1 |= 0x02;
                  if (kbd_key == 'M')           key1 |= 0x04;
                  if (kbd_key == 'N')           key1 |= 0x08;
                  if (kbd_key == 'O')           key1 |= 0x10;
                  if (kbd_key == 'P')           key1 |= 0x20;
                  if (kbd_key == 'Q')           key1 |= 0x40;
                  if (kbd_key == 'R')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 5)  // Row 5
          {
              if (kbd_key)
              {
                  if (kbd_key == 'S')           key1 |= 0x01;
                  if (kbd_key == 'T')           key1 |= 0x02;
                  if (kbd_key == 'U')           key1 |= 0x04;
                  if (kbd_key == 'V')           key1 |= 0x08;
                  if (kbd_key == 'W')           key1 |= 0x10;
                  if (kbd_key == 'X')           key1 |= 0x20;
                  if (kbd_key == 'Y')           key1 |= 0x40;
                  if (kbd_key == 'Z')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 6) // Row 6
          {
              if (kbd_key)
              {
                  if (kbd_key == KBD_KEY_SHIFT) key1 |= 0x01;
                  if (kbd_key == KBD_KEY_CTRL)  key1 |= 0x02;
                  if (kbd_key == KBD_KEY_GRAPH) key1 |= 0x04;
                  if (kbd_key == KBD_KEY_CAPS)  key1 |= 0x08;
                  if (kbd_key == KBD_KEY_KANA)  key1 |= 0x10;
                  if (kbd_key == KBD_KEY_F1)    key1 |= 0x20;
                  if (kbd_key == KBD_KEY_F2)    key1 |= 0x40;
                  if (kbd_key == KBD_KEY_F3)    key1 |= 0x80;
              }
              if (key_shift)  key1 |= 0x01;  // SHIFT
              if (key_ctrl)   key1 |= 0x02;  // CTRL
              if (key_graph)  key1 |= 0x04;  // GRAPH
              if (key_kana)   key1 |= 0x10;  // KANA
          }
          else if ((Port_PPI_C & 0x0F) == 7) // Row 7
          {
              if (kbd_key)
              {
                  if (kbd_key == KBD_KEY_F4)    key1 |= 0x01;
                  if (kbd_key == KBD_KEY_F5)    key1 |= 0x02;
                  if (kbd_key == KBD_KEY_ESC)   key1 |= 0x04;
                  if (kbd_key == KBD_KEY_TAB)   key1 |= 0x08;
                  if (kbd_key == KBD_KEY_STOP)  key1 |= 0x10;
                  if (kbd_key == KBD_KEY_BS)    key1 |= 0x20;
                  if (kbd_key == KBD_KEY_SEL)   key1 |= 0x40;
                  if (kbd_key == KBD_KEY_RET)   key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 8) // Row 8
          {
              if (kbd_key)
              {
                  if (kbd_key == ' ')           key1 |= 0x01;
                  if (kbd_key == KBD_KEY_HOME)  key1 |= 0x02;
                  if (kbd_key == KBD_KEY_INS)   key1 |= 0x04;
                  if (kbd_key == KBD_KEY_DEL)   key1 |= 0x08;
                  if (kbd_key == KBD_KEY_LEFT)  key1 |= 0x10;
                  if (kbd_key == KBD_KEY_UP)    key1 |= 0x20;
                  if (kbd_key == KBD_KEY_DOWN)  key1 |= 0x40;
                  if (kbd_key == KBD_KEY_RIGHT) key1 |= 0x80;
              }
          }
      }
      return ~key1;
}

// --------------------------------------------------------------------
// Arkanoid Vaus paddle
//
// Joystick port 1:
//
//   Pin 1 = serial DATA  -> PSG R#14 bit 0
//   Pin 2 = FIRE         -> PSG R#14 bit 1
//   Pin 6 = CLOCK        -> PSG R#15 bit 0
//   Pin 8 = RESET/START  -> PSG R#15 bit 4
//
// Vaus sends 9 bits, MSB first.
//
// Sequence:
//
//   pin8 LOW -> HIGH       start conversion
//   wait
//   read bit 8
//   pin6 LOW -> HIGH       shift
//   read bit 7
//   pin6 LOW -> HIGH       shift
//   ...
//
// Pin 8 is HIGH for the first clock, then LOW for the remaining clocks.
// --------------------------------------------------------------------
ArkanoidPaddle myPaddle;

// -------------------------------------------------------------------------------
// Experimentally determined that 0-290 is about the usable range in both
// Arkanoid games which are the only games known to use the paddles. Good enough..
// -------------------------------------------------------------------------------
void update_arkanoid_paddle_position(u8 clockwise, u8 speed)
{
    if (clockwise)
    {
        myPaddle.current_position += speed;
        if (myPaddle.current_position > 290) myPaddle.current_position = 290;
    }
    else
    {
        if (myPaddle.current_position > speed) myPaddle.current_position -= speed;
        else myPaddle.current_position = 0;
    }
}

// --------------------------------------------------------------------
// Arkanoid Vaus paddle
//
// Joystick port 1:
//
//   Pin 1 = DATA  -> PSG R#14 bit 0
//   Pin 2 = FIRE  -> PSG R#14 bit 1, active LOW
//   Pin 6 = CLOCK -> PSG R#15 bit 0
//   Pin 8 = START -> PSG R#15 bit 4, active LOW pulse
//
// Vaus protocol:
//
//   pin 8 LOW  = start a new conversion
//   pin 8 HIGH = keep conversion result available
//
//   read DATA
//   pin 6 LOW
//   pin 6 HIGH
//   read DATA
//   ... nine bits total
//
// The first 9-bit result after a pin-8 conversion is what we present
// to the game. We emulate the Vaus conversion instantaneously.
// --------------------------------------------------------------------

// --------------------------------------------------------------------
// Start a new Vaus conversion.
//
// IMPORTANT:
// The Vaus is triggered by the FALLING edge of pin 8.
// --------------------------------------------------------------------
static void arkanoid_start_conversion(ArkanoidPaddle *paddle)
{
    uint16_t pos = paddle->current_position;

    /*
     * Our experimentally calibrated range.
     *
     * 150 -> left edge
     * 305 -> right edge
     */
    paddle->shift_register = 150 + ((pos * 145) >> 8);

    paddle->shift_register &= 0x01FF;
}


// --------------------------------------------------------------------
// Update Vaus control lines
// --------------------------------------------------------------------
static void update_arkanoid_paddle(ArkanoidPaddle *paddle,
                                   uint8_t pin6,
                                   uint8_t pin8)
{
    /*
     * Pin 8 FALLING edge starts a new conversion.
     *
     * Do NOT do anything on the rising edge.
     *
     * This is important because pin 8 is supposed to remain HIGH
     * while the paddle result is being read.
     */
    if (!pin8 && paddle->last_pin8_state)
    {
        arkanoid_start_conversion(paddle);
    }

    /*
     * Pin 6 rising edge shifts the serial register.
     *
     * The game reads the current bit BEFORE generating this pulse.
     *
     * So:
     *
     *     R14 read -> current bit
     *     pin6 LOW
     *     pin6 HIGH -> advance to next bit
     */
    if (pin6 && !paddle->last_pin6_state)
    {
        paddle->shift_register <<= 1;
    }

    paddle->last_pin6_state = pin6;
    paddle->last_pin8_state = pin8;
}


// --------------------------------------------------------------------
// Read Vaus serial data.
//
// IMPORTANT: Reading does NOT shift the register.
// Pin 6 does that.
// --------------------------------------------------------------------
static bool read_paddle_pin1(ArkanoidPaddle *paddle)
{
    return (paddle->shift_register & 0x0100) != 0;
}


// --------------------------------------------------------------------
// PSG Port B output
// --------------------------------------------------------------------
void ayPortBOutHandler(u8 value)
{
    /*
     * Bit 6 selects joystick port 1.
     */
    if ((value & 0x40) == 0)
    {
        uint8_t pin6 = (value & 0x01) ? 1 : 0;
        uint8_t pin8 = (value & 0x10) ? 1 : 0;

        update_arkanoid_paddle(&myPaddle, pin6, pin8);

        /*
         * Your existing dpad/analog mapping goes here.
         *
         * Leave this as whatever code you are currently using
         * to produce 0..255.
         */
        myPaddle.current_position = myPaddle.current_position;

        if (JoyState & JST_FIRE1)
            myPaddle.button_pressed = 1;
        else
            myPaddle.button_pressed = 0;
    }
}

// --------------------------------------------------------------------
// MSX IO Port Read - The MSX has a lot of I/O mapped peripherals
// including Joystick, PSG, SCC, RTC, keyboard, etc.
// --------------------------------------------------------------------
ITCM_CODE unsigned char cpu_readport_msx(register u8 Port)
{
  //98h~9Bh   Access to the VDP I/O ports.
  if      (Port == 0x98) return RdData9938();               // VDP Data
  else if (Port == 0x99) return RdCtrl9938();               // VDP Control (Status)
  else if (Port == 0xB5) {return read_port_RTC_data();}     // RTC Data
  else if (Port == 0xA2)  // PSG Read... might be joypad data
  {
      // -------------------------------------------
      // Only port 1 is used for the first Joystick
      // -------------------------------------------
      if (myAY.ayRegIndex == 14)
      {
          u8 joy1 = 0x00;

          // -------------------------------------------------------------
          // Only port 1... not port 2. AY register 15 (PortB) bit 6 is
          // set to 0 for the port 1 joystick and that's the only one
          // this emulator will respond to...
          // -------------------------------------------------------------
          if ((myAY.ayPortBOut & 0x40) == 0)
          {
              if (myConfig.dpad == DPAD_DIAGONALS)
              {
                  if (JoyState & JST_UP)    joy1 |= (0x01 | 0x08);
                  if (JoyState & JST_DOWN)  joy1 |= (0x02 | 0x04);
                  if (JoyState & JST_LEFT)  joy1 |= (0x04 | 0x01);
                  if (JoyState & JST_RIGHT) joy1 |= (0x08 | 0x02);

                  if (JoyState & JST_FIRE1) joy1 |= 0x10;
                  if (JoyState & JST_FIRE2) joy1 |= 0x20;
              }
              else if (myConfig.dpad == DPAD_ARKANOID)
              {
                  /*
                   * joy1 is active-high here because it gets inverted below:
                   *
                   *     myAY.ayPortAIn = ~joy1;
                   *
                   * Vaus:
                   *   pin 1 = serial data
                   *   pin 2 = fire, active LOW
                   */

                  /*
                   * Pin 1 / serial data
                   *
                   * Actual PSG input is active-low, so:
                   *
                   *   serial bit = 1 -> joy1 bit 0 = 0
                   *   serial bit = 0 -> joy1 bit 0 = 1
                   */
                  if (!read_paddle_pin1(&myPaddle))
                      joy1 |= 0x01;

                  /*
                   * Pin 2 / fire button
                   *
                   * Actual Vaus button:
                   *   0 = pressed
                   *   1 = released
                   *
                   * Since joy1 gets inverted later:
                   *   joy1 bit 1 = 0 -> PSG bit 1 = 1
                   *   joy1 bit 1 = 1 -> PSG bit 1 = 0
                   *
                   * Therefore we want joy1 bit 1 SET when NOT pressed.
                   */
                  if ((JoyState & JST_FIRE1))
                      joy1 |= 0x02;
              }
              else // DPAD_NORMAL or SLIDE-N-GLIDE
              {
                  if (JoyState & JST_UP)    joy1 |= 0x01;
                  if (JoyState & JST_DOWN)  joy1 |= 0x02;
                  if (JoyState & JST_LEFT)  joy1 |= 0x04;
                  if (JoyState & JST_RIGHT) joy1 |= 0x08;

                  if (JoyState & JST_FIRE1) joy1 |= 0x10;
                  if (JoyState & JST_FIRE2) joy1 |= 0x20;
              }
          }

          myAY.ayPortAIn = ~joy1;
      }
      else if (myAY.ayRegIndex == 15)
      {
          // When reading PORTB of the PSG, just echo back the last value written (the MSX BIOS needs this as it will preserve the KANA LED bit)
          myAY.ayPortBIn = myAY.ayPortBOut;
      }
      return ay38910DataR(&myAY);
  }
  else if (Port == 0x12)  // 2xPSG Read...
  {
      return ay38910DataR(&myAY2);
  }
  else if (Port == 0xA8)  // Feedback on Slot mapping
  {
      return Port_PPI_A;
  }
  else if (Port == 0xA9)  // Keyboard read
  {
      return readport_keyboard();
  }
  else if (Port == 0xAA)  // Port C feedback
  {
      return Port_PPI_C;
  }
  else if (Port >= 0xFC) // Mirror of RAM select. Not all MSX2 machine return this but we do.
  {
      return mirror_ram_bank[Port - 0xFC];
  }
  else // Unknown port read
  {
      //debug[DX++ & 0xF] = Port;
  }

  // No such port
  return(NORAM);
}

//------------------------------------------------------------------------------
// Generic Japanese MSX1 with Cart in Slot 1 and 64K RAM in Slot 2
//------------------------------------------------------------------------------
// Memory          Slot 0       Slot 1      Slot 2      Slot 3
// C000h~FFFFh      ---       Cartridge     16K RAM      ---
// 8000h~BFFFh      ---       Cartridge     16K RAM      ---
// 4000h~7FFFh    Main-ROM    Cartridge     16K RAM      Disk-ROM
// 0000h~3FFFh    Main-ROM    Cartridge     16K RAM      ---
//---------------------------------------------------------------
void msx_slot_map_msx1(unsigned char Value)
{
    special_memory_access &= ~SPEC_MEM_SUBSLOT_ACTIVE; // MSX1 has no subslots
    special_memory_access &= ~SPEC_MEM_DISK_CONTROLLER;

    switch ((Value>>0) & 0x03)  // Page 0 [0x0000~0x3FFF]
    {
        case 0x00:  // Slot 0:  Maps to BIOS Rom
            bCartInPage[0] = 0;
            bRAMInPage[0] = 0;
            MemoryMap[0] = BIOS_Memory + 0x0000 - 0x0000;
            MemoryMap[1] = BIOS_Memory + 0x2000 - 0x2000;
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[0] = 1;
            bRAMInPage[0] = 0;
            MemoryMap[0] = (u8 *)(MSXCartPtr[MEDIA_CART1][0]) - 0x0000;
            MemoryMap[1] = (u8 *)(MSXCartPtr[MEDIA_CART1][1]) - 0x2000;
            break;
        case 0x02:  // Slot 2:  Maps to our 64K of RAM
            bCartInPage[0] = 0;
            bRAMInPage[0] = 1;
            MemoryMap[0] = (u8 *)(MSXRamPtr[0]) - 0x0000;
            MemoryMap[1] = (u8 *)(MSXRamPtr[1]) - 0x2000;
            break;
        case 0x03:  // Slot 3:  Maps to second Cart slot
            bCartInPage[0] = 2;
            bRAMInPage[0] = 0;
            MemoryMap[0] = (u8 *)(MSXCartPtr[MEDIA_CART2][0]) - 0x0000;
            MemoryMap[1] = (u8 *)(MSXCartPtr[MEDIA_CART2][1]) - 0x2000;
            break;
    }

    switch ((Value>>2) & 0x03)  // Page 1 [0x4000~0x7FFF]
    {
        case 0x00:  // Slot 0:  Maps to BIOS Rom
            bCartInPage[1] = 0;
            bRAMInPage[1] = 0;
            MemoryMap[2] = BIOS_Memory + 0x4000 - 0x4000;
            MemoryMap[3] = BIOS_Memory + 0x6000 - 0x6000;
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[1] = 1;
            bRAMInPage[1] = 0;
            MemoryMap[2] = (u8 *)(MSXCartPtr[MEDIA_CART1][2]) - 0x4000;
            MemoryMap[3] = (u8 *)(MSXCartPtr[MEDIA_CART1][3]) - 0x6000;
            break;
        case 0x02:  // Slot 2:  Maps to our 64K of RAM
            bCartInPage[1] = 0;
            bRAMInPage[1] = 1;
            MemoryMap[2] = (u8 *)(MSXRamPtr[2]) - 0x4000;
            MemoryMap[3] = (u8 *)(MSXRamPtr[3]) - 0x6000;
            break;
        case 0x03:  // Slot 3:  Cart 2 or Disk ROM mapped here
            bCartInPage[1] = 2;
            bRAMInPage[1] = 0;
            if (MyMedia[MEDIA_CART2].filecrc)
            {
                MemoryMap[2] = (u8 *)(MSXCartPtr[MEDIA_CART2][2]) - 0x4000;
                MemoryMap[3] = (u8 *)(MSXCartPtr[MEDIA_CART2][3]) - 0x6000;
            }
            else // If no cart loaded, we put in the Disk Controller by default
            {
                special_memory_access |= SPEC_MEM_DISK_CONTROLLER;
                MemoryMap[2] = (u8 *)MSXBios_DISK + 0x0000 - 0x4000;
                MemoryMap[3] = (u8 *)MSXBios_DISK + 0x2000 - 0x6000;
            }
            break;
    }

    switch ((Value>>4) & 0x03)  // Page 2 [0x8000~0xBFFF]
    {
        case 0x00:  // Slot 0:  Maps to nothing... 0xFF
            bCartInPage[2] = 0;
            bRAMInPage[2] = 0;
            MemoryMap[4] = Unmapped_Memory - 0x8000;
            MemoryMap[5] = Unmapped_Memory - 0xA000;
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[2] = 1;
            bRAMInPage[2] = 0;
            MemoryMap[4] = (u8 *)(MSXCartPtr[MEDIA_CART1][4]) - 0x8000;
            MemoryMap[5] = (u8 *)(MSXCartPtr[MEDIA_CART1][5]) - 0xA000;
            break;
        case 0x02:  // Slot 2:  Maps to our 64K of RAM
            bCartInPage[2] = 0;
            bRAMInPage[2] = 1;
            MemoryMap[4] = (u8 *)(MSXRamPtr[4]) - 0x8000;
            MemoryMap[5] = (u8 *)(MSXRamPtr[5]) - 0xA000;
            break;
        case 0x03:  // Slot 3:  Maps to Cart 2
            bCartInPage[2] = 2;
            bRAMInPage[2] = 0;
            MemoryMap[4] = (u8 *)(MSXCartPtr[MEDIA_CART2][4]) - 0x8000;
            MemoryMap[5] = (u8 *)(MSXCartPtr[MEDIA_CART2][5]) - 0xA000;
            break;
    }

    switch ((Value>>6) & 0x03)  // Page 3 [0xC000~0xFFFF]
    {
        case 0x00:  // Slot 0:  Maps to nothing... 0xFF
            bCartInPage[3] = 0;
            bRAMInPage[3] = 0;
            MemoryMap[6] = Unmapped_Memory - 0xC000;
            MemoryMap[7] = Unmapped_Memory - 0xE000;
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[3] = 1;
            bRAMInPage[3] = 0;
            MemoryMap[6] = (u8 *)(MSXCartPtr[MEDIA_CART1][6]) - 0xC000;
            MemoryMap[7] = (u8 *)(MSXCartPtr[MEDIA_CART1][7]) - 0xE000;
            break;
        case 0x02:  // Slot 2 is RAM so we allow RAM writes now
            bCartInPage[3] = 0;
            bRAMInPage[3] = 1;
            MemoryMap[6] = (u8 *)(MSXRamPtr[6]) - 0xC000;
            MemoryMap[7] = (u8 *)(MSXRamPtr[7]) - 0xE000;
            break;
        case 0x03:  // Slot 3:  Maps to Cart 2
            bCartInPage[3] = 2;
            bRAMInPage[3] = 0;
            MemoryMap[6] = (u8 *)(MSXCartPtr[MEDIA_CART2][6]) - 0xC000;
            MemoryMap[7] = (u8 *)(MSXCartPtr[MEDIA_CART2][7]) - 0xE000;
            break;
    }
}

//--------------------------------------------------------------------------------------------------
// MSX2 Machine Configuration: Slot 3 is expanded. Slot 2 contains RAM. Slot 1 is main Cartridge.
//--------------------------------------------------------------------------------------------------
// Memory          Slot 0       Slot 1      Slot 2      Slot 3-0   Slot 3-1    Slot 3-2    Slot 3-3
// C000h~FFFFh      ---         Cart1       Cart2       ---        ---         ---         16K RAM
// 8000h~BFFFh      ---         Cart1       Cart2       ---        ---         ---         16K RAM
// 4000h~7FFFh    Main-ROM      Cart1       Cart2       ---        Disk-ROM    MSX-Music   16K RAM
// 0000h~3FFFh    Main-ROM      Cart1       Cart2       Ext-ROM    ---         ---         16K RAM
//--------------------------------------------------------------------------------------------------
void msx_slot_map_msx2(unsigned char Value)
{
    special_memory_access &= ~SPEC_MEM_SUBSLOT_ACTIVE; // Until proven otherwise below...
    special_memory_access &= ~SPEC_MEM_DISK_CONTROLLER;
    
    switch ((Value>>0) & 0x03)  // Page 0 [0x0000~0x3FFF]
    {
        case 0x00:  // Slot 0:  Maps to Main BIOS ROM
            bCartInPage[0] = 0;
            bRAMInPage[0] = 0;
            MemoryMap[0] = BIOS_Memory + 0x0000 - 0x0000;
            MemoryMap[1] = BIOS_Memory + 0x2000 - 0x2000;
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[0] = 1;
            bRAMInPage[0] = 0;
            MemoryMap[0] = (u8 *)(MSXCartPtr[MEDIA_CART1][0]) - 0x0000;
            MemoryMap[1] = (u8 *)(MSXCartPtr[MEDIA_CART1][1]) - 0x2000;
            break;
        case 0x02:  // Slot 2:  Secondary Cart Slot
            bCartInPage[0] = 2;
            bRAMInPage[0] = 0;
            MemoryMap[0] = (u8 *)(MSXCartPtr[MEDIA_CART2][0]) - 0x0000;
            MemoryMap[1] = (u8 *)(MSXCartPtr[MEDIA_CART2][1]) - 0x2000;
            break;
        case 0x03:  // Slot 3:  This is an expanded slot... has Extended BIOS and Disk Controller ROMs
            bCartInPage[0] = 0;
            bRAMInPage[0] = 0;
            if (((msx_subslot & 0x03) >> 0) == 0) // Subslot 0 has Extended BIOS
            {
                MemoryMap[0] = (u8 *)MSXBios_MSX2EXT+0x0000 - 0x0000;
                MemoryMap[1] = (u8 *)MSXBios_MSX2EXT+0x2000 - 0x2000;
            }
            else if (((msx_subslot & 0x03) >> 0) == 3) // Subslot 3 has RAM Mapper
            {
                bCartInPage[0] = 0;
                bRAMInPage[0] = 1;
                MemoryMap[0] = (u8 *)(MSXRamPtr[0]) - 0x0000;
                MemoryMap[1] = (u8 *)(MSXRamPtr[1]) - 0x2000;
            }
            else // Other subslots have nothing in this page
            {
                MemoryMap[0] = Unmapped_Memory - 0x0000;
                MemoryMap[1] = Unmapped_Memory - 0x2000;
            }
            break;
    }

    switch ((Value>>2) & 0x03)  // Page 1 [0x4000~0x7FFF]
    {
        case 0x00:  // Slot 0:  Maps to Main BIOS ROM
            bCartInPage[1] = 0;
            bRAMInPage[1] = 0;
            MemoryMap[2] = BIOS_Memory + 0x4000 - 0x4000;
            MemoryMap[3] = BIOS_Memory + 0x6000 - 0x6000;
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[1] = 1;
            bRAMInPage[1] = 0;
            MemoryMap[2] = (u8 *)(MSXCartPtr[MEDIA_CART1][2]) - 0x4000;
            MemoryMap[3] = (u8 *)(MSXCartPtr[MEDIA_CART1][3]) - 0x6000;
            break;
        case 0x02:  // Slot 2:  Secondary Cart Slot
            bCartInPage[1] = 2;
            bRAMInPage[1] = 0;
            MemoryMap[2] = (u8 *)(MSXCartPtr[MEDIA_CART2][2]) - 0x4000;
            MemoryMap[3] = (u8 *)(MSXCartPtr[MEDIA_CART2][3]) - 0x6000;
            break;
        case 0x03:  // Slot 3:  Expanded slot has the Disk Controller in subslot 1
            bCartInPage[1] = 0;
            bRAMInPage[1] = 0;

            if (((msx_subslot & 0x0C) >> 2) == 1) // Subslot 1 has Disk Controller
            {
                special_memory_access |= SPEC_MEM_DISK_CONTROLLER;
                MemoryMap[2] = (u8 *)MSXBios_DISK + 0x0000 - 0x4000;
                MemoryMap[3] = (u8 *)MSXBios_DISK + 0x2000 - 0x6000;
            }
            else if (((msx_subslot & 0x0C) >> 2) == 3) // Subslot 3 has RAM Mapper
            {
                bCartInPage[1] = 0;
                bRAMInPage[1] = 1;
                MemoryMap[2] = (u8 *)(MSXRamPtr[2]) - 0x4000;
                MemoryMap[3] = (u8 *)(MSXRamPtr[3]) - 0x6000;
            }
            else // Other subslots have nothing in this page
            {
                MemoryMap[2] = Unmapped_Memory - 0x4000;
                MemoryMap[3] = Unmapped_Memory - 0x6000;
            }
            break;
    }

    switch ((Value>>4) & 0x03)  // Page 2 [0x8000~0xBFFF]
    {
        case 0x00:  // Slot 0:  Maps to nothing... 0xFF
            bCartInPage[2] = 0;
            bRAMInPage[2] = 0;
            MemoryMap[4] = Unmapped_Memory - 0x8000;
            MemoryMap[5] = Unmapped_Memory - 0xA000;
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[2] = 1;
            bRAMInPage[2] = 0;
            MemoryMap[4] = (u8 *)(MSXCartPtr[MEDIA_CART1][4]) - 0x8000;
            MemoryMap[5] = (u8 *)(MSXCartPtr[MEDIA_CART1][5]) - 0xA000;
            break;
        case 0x02:  // Slot 2:  Secondary Cart Slot
            bCartInPage[2] = 2;
            bRAMInPage[2] = 0;
            MemoryMap[4] = (u8 *)(MSXCartPtr[MEDIA_CART2][4]) - 0x8000;
            MemoryMap[5] = (u8 *)(MSXCartPtr[MEDIA_CART2][5]) - 0xA000;
            break;
        case 0x03:  // Slot 3:  Maps to nothing... 0xFF. Expanded slot but nothing lives here.
            if (((msx_subslot & 0x30) >> 4) == 3) // Subslot 3 has RAM Mapper
            {
                bCartInPage[2] = 0;
                bRAMInPage[2] = 1;
                MemoryMap[4] = (u8 *)(MSXRamPtr[4]) - 0x8000;
                MemoryMap[5] = (u8 *)(MSXRamPtr[5]) - 0xA000;
            }
            else
            {
                bCartInPage[2] = 0;
                bRAMInPage[2] = 0;
                MemoryMap[4] = Unmapped_Memory - 0x8000;
                MemoryMap[5] = Unmapped_Memory - 0xA000;
            }
            break;
    }

    switch ((Value>>6) & 0x03)  // Page 3 [0xC000~0xFFFF]
    {
        case 0x00:  // Slot 0:  Maps to nothing... 0xFF
            bCartInPage[3] = 0;
            bRAMInPage[3] = 0;
            MemoryMap[6] = Unmapped_Memory - 0xC000;
            MemoryMap[7] = Unmapped_Memory - 0xE000;
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[3] = 1;
            bRAMInPage[3] = 0;
            MemoryMap[6] = (u8 *)(MSXCartPtr[MEDIA_CART1][6]) - 0xC000;
            MemoryMap[7] = (u8 *)(MSXCartPtr[MEDIA_CART1][7]) - 0xE000;
            break;
        case 0x02:  // Slot 2:  Secondary Cart Slot
            bCartInPage[3] = 2;
            bRAMInPage[3] = 0;
            MemoryMap[6] = (u8 *)(MSXCartPtr[MEDIA_CART2][6]) - 0xC000;
            MemoryMap[7] = (u8 *)(MSXCartPtr[MEDIA_CART2][7]) - 0xE000;
            break;
        case 0x03:  // Slot 3:  Maps to nothing... 0xFF. Expanded slot but nothing lives here.
            special_memory_access |= SPEC_MEM_SUBSLOT_ACTIVE; // Opens up 0xFFFF
            if (((msx_subslot & 0xC0) >> 6) == 3) // Subslot 3 has RAM Mapper
            {
                bCartInPage[3] = 0;
                bRAMInPage[3] = 1;
                MemoryMap[6] = (u8 *)(MSXRamPtr[6]) - 0xC000;
                MemoryMap[7] = (u8 *)(MSXRamPtr[7]) - 0xE000;
            }
            else
            {
                bCartInPage[3] = 0;
                bRAMInPage[3] = 0;
                MemoryMap[6] = Unmapped_Memory - 0xC000;
                MemoryMap[7] = Unmapped_Memory - 0xE000;
            }
            break;
    }
}

// -----------------------------------------------------------------------------------------------
// MSX IO Port Write - VDP and AY Sound Chip, Disk I/O, MSX2 Expanded Memory plus Slot Mapper $A8
// -----------------------------------------------------------------------------------------------
ITCM_CODE void cpu_writeport_msx(register u8 Port,register unsigned char Value)
{
    static u8 msx_music_register = 0;

    if      (Port == 0x98) {WrData9938(Value);}                 // VDP Data
    else if (Port == 0x99) {WrCtrl9938(Value);}                 // VDP Control
    else if (Port == 0x9A) {write_port_palette(Value);}         // VDP Palette
    else if (Port == 0x9B) {IndirectRegWrite9938(Value);}       // Indirect Register Area
    else if (Port == 0xA0) {ay38910IndexW(Value&0xF, &myAY);}   // PSG Area
    else if (Port == 0xA1) {ay38910DataW(Value, &myAY);}        // PSG Area
    else if (Port == 0x10) {ay38910IndexW(Value&0xF, &myAY2);}  // PSG Area - 2nd PSG
    else if (Port == 0x11) {ay38910DataW(Value, &myAY2);}       // PSG Area - 2nd PSG
    else if (Port == 0xB4) {write_port_RTC_index(Value);}       // RTC Area (index register)
    else if (Port == 0xB5) {write_port_RTC_data(Value);}        // RTC Area (data register)
    else if (Port == 0xA8) // Slot system for MSX
    {
        if (myConfig.machineType)
            msx_slot_map_msx1(Value);
        else
            msx_slot_map_msx2(Value);

        Port_PPI_A = Value;
    }
    else if (Port == 0xA9)  // PPI - Register B
    {
        Port_PPI_B = Value;
    }
    else if (Port == 0xAA)  // PPI - Register C
    {
        if (Value & 0x80)  // Beeper ON
        {
            if (((Port_PPI_C ^ Value) & 0x80) == 0) beeperFreq++; // Beeper toggle
        }
        Port_PPI_C = Value;
        msx_caps_lock = ((Port_PPI_C & 0x40) ? 0:1);
    }
    else if (Port == 0xAB)  // PPI - Register C Fast Modify
    {
        if ((Value & 0x0E) == 0x0E) // Are we hitting the Beeper ON/OFF bit?
        {
            if ((Value & 1) && ((Port_PPI_C & 0x80) == 0)) beeperFreq++;   // Beeper toggle
            else if (!(Value & 1) && ((Port_PPI_C & 0x80))) beeperFreq++;  // Beeper toggle
        }

        // Set or clear the proper bit in PORTC
        u8 bit =  (Value & 0x0E) >> 1;
        if (Value & 1) Port_PPI_C |= (1 << bit);
        else Port_PPI_C &= ~(1 << bit);

        msx_caps_lock = ((Port_PPI_C & 0x40) ? 0:1);
    }
    else if (Port >= 0xFC && Port <= 0xFF) // Expanded Memory...
    {
        // The MSX1 machine has only 64K emulated...
        if (myConfig.machineType != MACHINE_MSX1)
        {
            u8 page = Port-0xFC;
            u8 bank = Value & 0x7; // 128K is 16K in 8 banks
            
            mirror_ram_bank[page] = bank; // For read-back

            MSXRamPtr[(page*2)+0] = RAM_Memory + (0x4000 * bank);
            MSXRamPtr[(page*2)+1] = RAM_Memory + (0x4000 * bank) + 0x2000;

            cpu_writeport_msx(0xA8, Port_PPI_A); // Enable the new map...
        }
    }
    else if (Port == 0x7C)
    {
        msx_music_register = Value;
    }
    else if (Port == 0x7D)
    {
        if (++msx_music_writes == 10) // Arbitrary... if we hit it at least 10 times, we turn on MSX MUSIC output
        {
            if (myConfig.expansion == MUSIC_MSX) 
            {
                msx_music_capable_game = 1;
                bFirstSoundOutput = 1;
            }
        }

        YMWrite(Value, msx_music_register, &myYM);    // address = resolved register 0x00-0x38, not a Z80 address
    }
    else // Unhandled port write...
    {
        //debug[DX++ & 0xF] = Port;
    }
}

// --------------------------------------------------------------------------
// Try to guess the ROM type from the loaded binary... basically we are
// counting the number of load addresses that would access a mapper hot-spot.
// --------------------------------------------------------------------------
u8 MSX_GuessROMType(u32 size)
{
    u8 type = KON8;  // Default to Konami 8K mapper
    u32 guess[MAX_GUESS_MAPPER];

    memset(guess, 0x00, sizeof(guess));
    for (int i=0; i<size - 3; i++)
    {
        if (ROM_Memory[i] == 0x32)   // LD,A instruction
        {
            u16 value = ROM_Memory[i+1] + (ROM_Memory[i+2] << 8);
            switch (value)
            {
                case 0x5000:
                case 0x9000:
                case 0xb000:
                    guess[SCC8]++;
                    break;
                case 0x4000:
                case 0x8000:
                case 0xa000:
                    guess[KON8]++;
                    break;
                case 0x6800:
                case 0x7800:
                    guess[ASC8]++;guess[ASC8]++;
                    break;
                case 0x6000:
                    guess[KON8]++;
                    guess[ASC8]++;
                    guess[ASC16]++;
                    break;
                case 0x7000:
                    guess[SCC8]++;
                    guess[ASC8]++;
                    guess[ASC16]++;
                    break;
                case 0x77FF:
                    guess[ASC16]++;guess[ASC16]++;
                    break;
            }
        }
    }

    // Now pick the mapper that had the most Load addresses above...
    if      ((guess[ASC16] > guess[KON8]) && (guess[ASC16] > guess[SCC8]) && (guess[ASC16] > guess[ASC8]))    type = ASC16;
    else if ((guess[ASC8]  > guess[KON8]) && (guess[ASC8]  > guess[SCC8]) && (guess[ASC8] >= guess[ASC16]))   type = ASC8;      // ASC8 wins "ties" over ASC16
    else if ((guess[SCC8]  > guess[KON8]) && (guess[SCC8]  > guess[ASC8]) && (guess[SCC8]  > guess[ASC16]))   type = SCC8;
    else type = KON8;

    if (size == (64 * 1024)) type = ASC16;      // Big percentage of 64K mapper ROMs are ASCII16 so default to that and override below

    return type;
}


/*********************************************************************************
 * We wipe main RAM with 0x00 values (helps with compression of save states even
 * if not truly accurate to real hardware) and SRAM is wiped to 0xFF values.
 ********************************************************************************/
void msxWipeRAM(void)
{
    memset(RAM_Memory, 0x00, sizeof(RAM_Memory));
}

// -------------------------------------------------------------------
// Return 1 if the MyMedia[MEDIA_CART1].mapperType indicates we are a 16K banking cart...
// Otherwise the system will assume a standard 8K mapper.
// -------------------------------------------------------------------
u8 Is16kBanking(void)
{
    if (MyMedia[MEDIA_CART1].mapperType == ASC16 || MyMedia[MEDIA_CART1].mapperType == ASC16SRAM2 || MyMedia[MEDIA_CART1].mapperType == ASC16SRAM8 || MyMedia[MEDIA_CART1].mapperType == ZEN16 || MyMedia[MEDIA_CART1].mapperType == XBLAM || MyMedia[MEDIA_CART1].mapperType == SUPERLR || MyMedia[MEDIA_CART1].mapperType == XEVIOUS)
        return 1;
    else
        return 0;
}

void MSX_CartSetup(u8 media_id)
{
    u8 *CART_Memory = ROM_Memory + (media_id == MEDIA_CART1 ? 0 : ((MAX_CART_SIZE_KB/2)*1024));

    // ------------------------------------------------------------
    // Setup the Z80 memory based on the MSX game ROM size loaded
    // ------------------------------------------------------------
    if (MyMedia[media_id].filesize == (8 * 1024))
    {
        if (msx_basic)  // Basic Game loads at 0x8000 ONLY (no mirrors)
        {
            MSXCartPtr[media_id][0] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][1] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][2] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][3] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 4 - Actual ROM is here
            MSXCartPtr[media_id][5] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][6] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][7] = (u8*)Unmapped_Memory;          // Segment NA
        }
        else            // Mirrors at every 8K
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x0000;        // Segment 0
        }
    }
    else if (MyMedia[media_id].filesize <= (16 * 1024) && (MyMedia[media_id].mapperType != SCC8))
    {
        if (MyMedia[media_id].mapperType == AT4K)  // Load the 16K rom at 0x4000 without Mirrors
        {
                MSXCartPtr[media_id][0] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][1] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0
                MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;        // Segment 1
                MSXCartPtr[media_id][4] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][5] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][6] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][7] = (u8*)Unmapped_Memory;          // Segment NA
        }
        else if (MyMedia[media_id].mapperType == AT8K) // Load the 16K rom at 0x8000 without Mirrors
        {
                MSXCartPtr[media_id][0] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][1] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][2] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][3] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0
                MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x2000;        // Segment 1
                MSXCartPtr[media_id][6] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][7] = (u8*)Unmapped_Memory;          // Segment NA
        }
        else // This game loads with MIRRORS active
        {
            if (msx_basic)  // Basic Game loads at 0x8000 without Mirrors
            {
                MSXCartPtr[media_id][0] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][1] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][2] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][3] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0
                MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x2000;        // Segment 1
                MSXCartPtr[media_id][6] = (u8*)Unmapped_Memory;          // Segment NA
                MSXCartPtr[media_id][7] = (u8*)Unmapped_Memory;          // Segment NA
            }
            else    // Mirrors every 16K
            {
                MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x0000;        // Segment 0
                MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x2000;        // Segment 1
                MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0
                MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;        // Segment 1
                MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0
                MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x2000;        // Segment 1
                MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x0000;        // Segment 0
                MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x2000;        // Segment 1
            }
        }
    }
    else if (MyMedia[media_id].filesize <= (32 * 1024) && (MyMedia[media_id].mapperType != SCC8))
    {
        // ------------------------------------------------------------------------------------------------------
        // For 32K roms, we need more information to determine exactly where to load it... however
        // this simple algorithm handles at least 90% of all real-world games... basically the header
        // of the .ROM file has a INIT load address that we can use as a clue as to what banks the actual
        // code should be loaded... if the INIT is address 0x4000 or higher (this is fairly common) then we
        // load the 32K rom into banks 1+2 and we mirror the first 16K on page 0 and the upper 16K on page 3.
        // ------------------------------------------------------------------------------------------------------
        if (MyMedia[media_id].mapperType == AT0K)  // Then the full 32K ROM is mapped here
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x2000;        // Segment 1
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x4000;        // Segment 2
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x6000;        // Segment 3
            MSXCartPtr[media_id][4] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][5] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][6] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][7] = (u8*)Unmapped_Memory;          // Segment NA
        }
        else  if (MyMedia[media_id].mapperType == AT4K)  // Then the full 32K ROM is mapped here
        {
            MSXCartPtr[media_id][0] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][1] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;        // Segment 1
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x4000;        // Segment 2
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x6000;        // Segment 3
            MSXCartPtr[media_id][6] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][7] = (u8*)Unmapped_Memory;          // Segment NA
        }
        else if (MyMedia[media_id].mapperType == AT8K)  // Then the full 32K ROM is mapped here
        {
            MSXCartPtr[media_id][0] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][1] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][2] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][3] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x2000;        // Segment 1
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x4000;        // Segment 2
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x6000;        // Segment 3
        }
        else // MIRRORED (in some way)
        {
            if (msx_init >= 0x4000 || msx_basic) // This comes from the .ROM header - if the init address is 0x4000 or higher, we load in bank 1+2
            {
                MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x4000;        // Segment 2 Mirror
                MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x6000;        // Segment 3 Mirror
                MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0
                MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;        // Segment 1
                MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x4000;        // Segment 2
                MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x6000;        // Segment 3
                MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x0000;        // Segment 0 Mirror
                MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x2000;        // Segment 1 Mirror
            }
            else  // Otherwise we load in bank 1+2 and mirrors on 0+3
            {
                MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x0000;        // Segment 0 Mirror
                MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x2000;        // Segment 1 Mirror
                MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x4000;        // Segment 2
                MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x6000;        // Segment 3
                MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0
                MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x2000;        // Segment 1
                MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x4000;        // Segment 2 Mirror
                MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x6000;        // Segment 3 Mirror
            }
        }
    }
    else if (MyMedia[media_id].filesize == (48 * 1024) && (MyMedia[media_id].mapperType != SCC8))
    {
        if ((MyMedia[media_id].mapperType == KON8) || (MyMedia[media_id].mapperType == ZEN8))
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x4000;        // Segment 2 Mirror
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x6000;        // Segment 3 Mirror
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;        // Segment 1
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x4000;        // Segment 2
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x6000;        // Segment 3
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x0000;        // Segment 0 Mirror
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x2000;        // Segment 1 Mirror
            MyMedia[media_id].mapperMask = 0x07;
        }
        else if (MyMedia[media_id].mapperType == ASC8)
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x0000;        // Segment 0
            MyMedia[media_id].mapperMask = 0x07;
        }
        else if (myConfig.msxMapper == ASC16)
        {
            MSXCartPtr[media_id][0] = (u8*)Unmapped_Memory;          // Unmapped
            MSXCartPtr[media_id][1] = (u8*)Unmapped_Memory;          // Unmapped
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;        // Segment 1
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x2000;        // Segment 1
            MSXCartPtr[media_id][6] = (u8*)Unmapped_Memory;          // Unmapped
            MSXCartPtr[media_id][7] = (u8*)Unmapped_Memory;          // Unmapped
            MyMedia[media_id].mapperMask = 0x03;
        }
        else if (myConfig.msxMapper == ZEN16)
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x2000;        // Segment 1
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;        // Segment 1
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x2000;        // Segment 1
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x2000;        // Segment 1
            MyMedia[media_id].mapperMask = 0x03;
        }
        else if (MyMedia[media_id].mapperType == AT4K) // Mirror Page 1 to Page 0
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x0000;        // Mirror of Segment 0
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x2000;        // Mirror of Segment 1
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;        // Segment 1
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x4000;        // Segment 2
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x6000;        // Segment 3
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x8000;        // Segment 4
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0xA000;        // Segment 5
        }
        else // Load the rom at AT0K
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x0000;        // Segment 0
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x2000;        // Segment 1
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x4000;        // Segment 2
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x6000;        // Segment 3
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x8000;        // Segment 4
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0xA000;        // Segment 5
            MSXCartPtr[media_id][6] = (u8*)Unmapped_Memory;          // Segment NA
            MSXCartPtr[media_id][7] = (u8*)Unmapped_Memory;          // Segment NA
        }
    }
    else if ((MyMedia[media_id].filesize == (64 * 1024)) && (MyMedia[media_id].mapperType == LIN64))   // 64K Linear ROM
    {
        MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x0000;        // Segment 0
        MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x2000;        // Segment 1
        MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x4000;        // Segment 2
        MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x6000;        // Segment 3
        MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x8000;        // Segment 4
        MSXCartPtr[media_id][5] = (u8*)CART_Memory+0xA000;        // Segment 5
        MSXCartPtr[media_id][6] = (u8*)CART_Memory+0xC000;        // Segment 6
        MSXCartPtr[media_id][7] = (u8*)CART_Memory+0xE000;        // Segment 7
    }
    else if ((MyMedia[media_id].filesize >= (16 * 1024)) && (MyMedia[media_id].filesize <= (MAX_CART_SIZE_KB * 1024)))   // We'll take anything between these two...
    {
        if ((MyMedia[media_id].mapperType == KON8) || (MyMedia[media_id].mapperType == SCC8) || (MyMedia[media_id].mapperType == ZEN8) || (MyMedia[media_id].mapperType == MAJUT))
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x4000;        // Segment 2 Mirror
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x6000;        // Segment 3 Mirror
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;        // Segment 1 default
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x4000;        // Segment 2 default
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x6000;        // Segment 3 default
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x0000;        // Segment 0 Mirror
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x2000;        // Segment 1 Mirror
        }
        else if ((MyMedia[media_id].mapperType == ASC8) || (MyMedia[media_id].mapperType == ASC8SRAM2) || (MyMedia[media_id].mapperType == ASC8SRAM8))
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x0000;        // Segment 0 default
        }
        else if (MyMedia[media_id].mapperType == ASC16 || MyMedia[media_id].mapperType == ZEN16 || MyMedia[media_id].mapperType == ASC16SRAM2 || MyMedia[media_id].mapperType == ASC16SRAM8)
        {
            MSXCartPtr[media_id][0] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][1] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;        // Segment 0 default
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x2000;        // Segment 0 default
            MSXCartPtr[media_id][6] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][7] = (u8*)Unmapped_Memory;          // Segment Unmapped
        }
        else if (MyMedia[media_id].mapperType == XEVIOUS)
        {
            MSXCartPtr[media_id][0] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][1] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;        // Segment 0 default
            MSXCartPtr[media_id][4] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][5] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][6] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][7] = (u8*)Unmapped_Memory;          // Segment Unmapped
        }
        else if (MyMedia[media_id].mapperType == XBLAM)        // Just for Cross Blaim
        {
            MSXCartPtr[media_id][0] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][1] = (u8*)CART_Memory+0x2000;        // Segment 0 default
            MSXCartPtr[media_id][2] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][3] = (u8*)CART_Memory+0x2000;        // Segment 0 default
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x2000;        // Segment 0 default
            MSXCartPtr[media_id][6] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][7] = (u8*)CART_Memory+0x2000;        // Segment 0 default
        }
        else if (MyMedia[media_id].mapperType == SUPERLR)        // Just for Super Lode Runner
        {
            special_memory_access = SPEC_MEM_SUPERLR_ACTIVE;
            MSXCartPtr[media_id][0] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][1] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][2] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][3] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][4] = (u8*)CART_Memory+0x0000;        // Segment 0 default
            MSXCartPtr[media_id][5] = (u8*)CART_Memory+0x2000;        // Segment 0 default
            MSXCartPtr[media_id][6] = (u8*)Unmapped_Memory;          // Segment Unmapped
            MSXCartPtr[media_id][7] = (u8*)Unmapped_Memory;          // Segment Unmapped
        }

        // ---------------------------------------------------------------------
        // We now set our memory masks such that we page memory in properly...
        // ---------------------------------------------------------------------
        if (MyMedia[media_id].filesize <= (128 * 1024))
        {
            if (Is16kBanking())
                MyMedia[media_id].mapperMask = (MyMedia[media_id].filesize <= (64 * 1024)) ? 0x03:0x07;
            else
                MyMedia[media_id].mapperMask = (MyMedia[media_id].filesize <= (64 * 1024)) ? 0x07:0x0F;
        }
        else if (MyMedia[media_id].filesize <= (512 * 1024))
        {
            if (Is16kBanking())
                MyMedia[media_id].mapperMask = (MyMedia[media_id].filesize <= (256 * 1024)) ? 0x0F:0x1F;
            else
                MyMedia[media_id].mapperMask = (MyMedia[media_id].filesize <= (256 * 1024)) ? 0x1F:0x3F;
        }
        else if (MyMedia[media_id].filesize <= (1024 * 1024))
        {
            if (Is16kBanking())
                MyMedia[media_id].mapperMask = 0x3F;
            else
                MyMedia[media_id].mapperMask = 0x7F;
        }
        else if (MyMedia[media_id].filesize <= (2048 * 1024))
        {
            if (Is16kBanking())
                MyMedia[media_id].mapperMask = 0x7F;
            else
                MyMedia[media_id].mapperMask = 0xFF;
        }
        else // Must be 4096K...  really only the 16K banking works here...
        {
            MyMedia[media_id].mapperMask = 0xFF;
        }
    }
    else
    {
        // Size not right for MSX support... we've already pre-filled 0xFF so nothing more to do here... System will not run.
    }

    // --------------------------------------------------------------------------
    // Some mappers have 8K blocks, some have 16K blocks... sort that out here.
    // --------------------------------------------------------------------------
    MyMedia[media_id].blockSize = (Is16kBanking() ? 0x4000:0x2000);
}


// -------------------------------------------------------------------------
// Setup the initial MSX memory layout based on the size of the ROM loaded.
// -------------------------------------------------------------------------
void MSX_InitialMemoryLayout(void)
{
    // -----------------------------------------------------
    // Fill the unused memory buffer. Anything unmapped in
    // our memory mapping will point here and read 0xFF.
    // -----------------------------------------------------
    for (int i=0; i<0x2000; i++)
    {
        Unmapped_Memory[i] = 0xFF;
    }

    // -------------------------------------
    // Make sure the MSX ports are clear
    // -------------------------------------
    Port_PPI_A = 0x00;
    Port_PPI_B = 0x00;
    Port_PPI_C = 0x00;

    // ----------------------------------
    // Some helper variables for the MSX
    // ----------------------------------
    msx_music_writes = 0;
    special_memory_access = 0x00;
    msx_subslot = 0x00;

    // -----------------------------------------
    // Setup RAM/ROM pointers back to defaults
    // -----------------------------------------
    memset(bRAMInPage,  0, sizeof(bRAMInPage));   // Default to no RAM paged in until told so
    memset(bCartInPage, 0, sizeof(bCartInPage));  // Default to no ROM paged in until told so

    for (u8 i=0; i<8; i++)
    {
        MSXCartPtr[MEDIA_CART1][i] = Unmapped_Memory;          // Cart1 has nothing in it by default - will fill it in below
        MSXCartPtr[MEDIA_CART2][i] = Unmapped_Memory;          // Cart2 has nothing in it by default - will fill it in below
    }

    // ---------------------------------------------
    // RAM maps backwards... for historical reasons.
    // ---------------------------------------------
    MSXRamPtr[0] = RAM_Memory + 0xC000;
    MSXRamPtr[1] = RAM_Memory + 0xE000;
    MSXRamPtr[2] = RAM_Memory + 0x8000;
    MSXRamPtr[3] = RAM_Memory + 0xA000;
    MSXRamPtr[4] = RAM_Memory + 0x4000;
    MSXRamPtr[5] = RAM_Memory + 0x6000;
    MSXRamPtr[6] = RAM_Memory + 0x0000;
    MSXRamPtr[7] = RAM_Memory + 0x2000;

    // ---------------------------------------------
    // Restore the MSX BIOS and point to it
    // ---------------------------------------------
    msx_restore_bios();

    // -----------------------------------------------------------------
    // SCC music and FM-PAC are "carts" mapped into Slot 2 (Cart 2)...
    // -----------------------------------------------------------------
    if (myConfig.expansion == MUSIC_SCC) // SCC PLUS enabled?
    {
        // ---------------------------------------------------------------
        // This cart supplies its own private 64K of RAM (not ROM_Memory).
        // Pre-load the game image into it once, then map pages 0-3 into
        // the four windows as the power-on default (mirrors how a real
        // flash cart boots before any bank-select writes happen). We use
        // the back end 64K of the RAM_Memory[] which is otherwise unused
        // with SCC+ games.
        // ---------------------------------------------------------------

        sccplus_mode = 0x00;
        HandleSCCPlusModeRegister(0x00);   // derives SPEC_MEM_SCC_ENABLED/SPEC_MEM_SCC_PLUS_ENABLED bits correctly
        sccplus_page[0] = 0; sccplus_page[1] = 1;
        sccplus_page[2] = 2; sccplus_page[3] = 3;

        MyMedia[MEDIA_CART2].mapperMask = 0;
        MyMedia[MEDIA_CART2].blockSize = 0x2000;
        MyMedia[MEDIA_CART2].mapperType = SCCPLUS_RAM;

        MSXCartPtr[MEDIA_CART2][0] = (u8*)Unmapped_Memory;          // Segment Unmapped
        MSXCartPtr[MEDIA_CART2][1] = (u8*)Unmapped_Memory;          // Segment Unmapped
        MSXCartPtr[MEDIA_CART2][2] = SCC_Memory + (0 * 0x2000);     // 4000-5FFF -> page 0
        MSXCartPtr[MEDIA_CART2][3] = SCC_Memory + (1 * 0x2000);     // 6000-7FFF -> page 1
        MSXCartPtr[MEDIA_CART2][4] = SCC_Memory + (2 * 0x2000);     // 8000-9FFF -> page 2
        MSXCartPtr[MEDIA_CART2][5] = SCC_Memory + (3 * 0x2000);     // A000-BFFF -> page 3
        MSXCartPtr[MEDIA_CART2][6] = (u8*)Unmapped_Memory;          // Segment Unmapped
        MSXCartPtr[MEDIA_CART2][7] = (u8*)Unmapped_Memory;          // Segment Unmapped
    }
    if (myConfig.expansion == MUSIC_MSX) // MSX-MUSIC enabled? Enabled FM-PAC SRAM handling...
    {
        MyMedia[MEDIA_CART2].mapperMask = 0;
        MyMedia[MEDIA_CART2].blockSize = 0x2000;
        MyMedia[MEDIA_CART2].mapperType = FMPAC_SRAM;

        MSXCartPtr[MEDIA_CART2][0] = (u8*)Unmapped_Memory;           // Segment Unmapped
        MSXCartPtr[MEDIA_CART2][1] = (u8*)Unmapped_Memory;           // Segment Unmapped
        MSXCartPtr[MEDIA_CART2][2] = (u8*)MSXBios_MSXMUSIC + 0x0000; // 4000-5FFF -> MSX-MUSIC / FM-PAC BIOS
        MSXCartPtr[MEDIA_CART2][3] = (u8*)MSXBios_MSXMUSIC + 0x2000; // 6000-7FFF -> MSX-MUSIC / FM-PAC BIOS
        MSXCartPtr[MEDIA_CART2][4] = (u8*)Unmapped_Memory;           // Segment Unmapped
        MSXCartPtr[MEDIA_CART2][5] = (u8*)Unmapped_Memory;           // Segment Unmapped
        MSXCartPtr[MEDIA_CART2][6] = (u8*)Unmapped_Memory;           // Segment Unmapped
        MSXCartPtr[MEDIA_CART2][7] = (u8*)Unmapped_Memory;           // Segment Unmapped
        
        msxLoadSRAM();  // FM-PAC with MSX-MUSIC also has 8K of SRAM
    }

    if (MyMedia[MEDIA_CART1].filecrc) // For ROMs we need a mapper type... User might have pre-selected one or we need to guess.
    {
        if (myConfig.msxMapper == GUESS)
        {
            // Look for the matching SHA1 in the ROM Database
            MyMedia[MEDIA_CART1].mapperType = RomDB_Lookup(MEDIA_CART1);
            if (MyMedia[MEDIA_CART1].mapperType == 0xFF && (MyMedia[MEDIA_CART1].filesize >= (64*1024))) // Not found... let's do our best to guess it...
            {
                MyMedia[MEDIA_CART1].mapperType = MSX_GuessROMType(MyMedia[MEDIA_CART1].filesize); // We only bother if the game is 64K or larger
            }
        }
        else // User has selected a specific mapper... who are we to argue?!
        {
            MyMedia[MEDIA_CART1].mapperType = myConfig.msxMapper;
        }

        MSX_CartSetup(MEDIA_CART1);
    }

    // For Slot 2 we only look-up by ROM DB... there is no override.
    // We don't allow any CART2 if we have the SCC-I cart or the FM-PAC (MSX-MUSIC) cart enabled...
    if (MyMedia[MEDIA_CART2].filecrc && (myConfig.expansion != MUSIC_SCC) && (myConfig.expansion != MUSIC_MSX))
    {
        MyMedia[MEDIA_CART2].mapperType = RomDB_Lookup(MEDIA_CART2);
        MSX_CartSetup(MEDIA_CART2);
    }

    // ---------------------------------------------------------------------------------------------------------
    // If we are dealing with one of the rare SRAM games, read the SRAM file from the SD card back into memory.
    // We only allow this for CART1 - we will ignore any SRAM-based CART that is in slot 2...
    // ---------------------------------------------------------------------------------------------------------
    if ((MyMedia[MEDIA_CART1].mapperType == ASC8SRAM2) || (MyMedia[MEDIA_CART1].mapperType == ASC8SRAM8) || (MyMedia[MEDIA_CART1].mapperType == ASC16SRAM2) || (MyMedia[MEDIA_CART1].mapperType == ASC16SRAM8))
    {
        msxLoadSRAM();
    }
}

// ---------------------------------------------------------------------------
// Classic Konami SCC register layout (relative to 9800h) puts the register
// block at 0x80 and shares one 32-byte waveform between Ch3 and Ch4 (the
// SCC+ driver instead treats 0x80-0x9F as Ch4's own independent wave RAM
// and moved the register block to 0xA0) - so classic-mode writes need to be
// relocated before reaching SCCWrite, and Ch3 wave writes need to be mirrored
// into Ch4's wave RAM to reproduce the real shared-waveform behavior.
// ---------------------------------------------------------------------------
void SCC_LegacyWrite(u8 value, u16 address)
{
    u8 off = address & 0xFF;

    if (off < 0x60)                        // Ch0-Ch2 wave RAM - untouched by the layout shift
    {
        SCCWrite(value, off, &mySCC);
    }
    else if (off < 0x80)                   // Ch3 wave RAM - also mirror into Ch4 (shared on real hardware)
    {
        SCCWrite(value, off, &mySCC);
        SCCWrite(value, off + 0x20, &mySCC);
    }
    else if (off < 0x90)                   // Old freq/vol/control block (0x80-0x8F) -> new 0xA0-0xAF block
    {
        SCCWrite(value, off + 0x20, &mySCC);
    }
    // Otherwise it's one of the mirrors or deformation registers which we aren't emulating yet...
}


// ---------------------------------------------------------------
// Restore the MSX BIOS into the memory buffer and point to it...
// ---------------------------------------------------------------
void msx_restore_bios(void)
{
    if (myConfig.machineType == MACHINE_MSX1)
    {
        memcpy(BIOS_Memory, MSXBios_MSX1, 0x8000);
    }
    else
    {
        memcpy(BIOS_Memory, MSXBios_MSX2, 0x8000);
    }

    // Slot 0 - BIOS is mapped in to start...
    MemoryMap[0] = BIOS_Memory + 0x0000 - 0x0000;
    MemoryMap[1] = BIOS_Memory + 0x2000 - 0x2000;
    MemoryMap[2] = BIOS_Memory + 0x4000 - 0x4000;
    MemoryMap[3] = BIOS_Memory + 0x6000 - 0x6000;

    MemoryMap[4] = Unmapped_Memory - 0x8000;
    MemoryMap[5] = Unmapped_Memory - 0xA000;
    MemoryMap[6] = Unmapped_Memory - 0xC000;
    MemoryMap[7] = Unmapped_Memory - 0xE000;
}


// ---------------------------------------------------------
// The MSX reset has some special memory mapping depending
// on if we have loaded a .DSK or a .ROM file.
// ---------------------------------------------------------
void msx_reset(void)
{
    msx_init = 0x4000;
    msx_basic = 0x0000;

    MSX_InitialMemoryLayout();

    // -------------------------------------------------
    // If a disk was part of the setup, load it now...
    // -------------------------------------------------
    if (MyMedia[MEDIA_DISK].filecrc)
    {
        //2 sides * 80 tracks * 9 sectors per track * 512 bytes per sector = 737280 Bytes (720kB)
        fdc_init(1, (MyMedia[MEDIA_DISK].filesize/1024 == 360) ? 1:2, 80, 9, 512, 1, DISK_Memory, NULL);
        fdc_reset(true);
    }

    // Setup the PortB write - Arkanoid paddles need this...
    myAY.ayPortBOutFptr = ayPortBOutHandler;
    myPaddle.current_position = 130;
}

// -------------------------------------------------------------------
// For the few games that have SRAM built in. We back the SRAM onto
// a file with the same base filename but the extension is .SRM
// -------------------------------------------------------------------
extern char szName[];
void msxSaveSRAM(void)
{
    // Return to the original path
    chdir(GetMasterPath());

    // Make sure the 'sav' directory exists
    EnsureSaveDirectory();

    // Init filename = romname and SRM (SRAM) in place of ROM
    sprintf(szName,"sav/%s", GetMasterFilename());

    int len = strlen(szName);
    szName[len-3] = 's';
    szName[len-2] = 'r';
    szName[len-1] = 'm';
    szName[len-0] = 0;

    FILE *handle = fopen(szName, "wb+");
    if (handle != NULL)
    {
        // SRAM is either 2K or 8K but we always just save 8K as the 2K has mirrors
        fwrite(SRAM_Memory, 0x2000, 1, handle);
        fclose(handle);
    }
}

void msxLoadSRAM(void)
{
    // Return to the original path
    chdir(GetMasterPath());

    // Make sure the 'sav' directory exists
    EnsureSaveDirectory();

    // Init filename = romname and SRM (SRAM) in place of ROM
    sprintf(szName,"sav/%s", GetMasterFilename());

    int len = strlen(szName);
    szName[len-3] = 's';
    szName[len-2] = 'r';
    szName[len-1] = 'm';
    szName[len-0] = 0;

    // Check if the file exists... if so read it...
    if (ReadFileCarefully(szName, SRAM_Memory, 0x2000, 0, NULL) == 0)
    {
        memset(SRAM_Memory, 0xFF, 0x2000); // Not found, so init the SRAM area
    }
}

// Ensure 'sav' directory exists. If not: create it.
void EnsureSaveDirectory(void)
{
    DIR* dir = opendir("sav");
    if (dir) closedir(dir);    // Directory exists... close it out and move on.
    else mkdir("sav", 0777);   // Otherwise create the directory...
}

// End of file
