// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave (Phoenix-Edition),
// Alekmaul (original port) and Marat Fayzullin (ColEM core) are thanked profusely.
//
// The Hachibitto emulator is offered as-is, without any warranty. Please see readme.md
// =====================================================================================
#include <nds.h>

#include <stdlib.h>
#include <stdio.h>
#include <fat.h>
#include <dirent.h>
#include <unistd.h>

#include "Hachibitto.h"
#include "MSX_generic.h"
#include "options.h"
#include "topscreen.h"
#include "lzav.h"
#include "CRC32.h"
#include "printf.h"

typedef enum {FT_NONE,FT_FILE,FT_DIR} FILE_TYPE;

int countCV=0;
int ucGameAct=0;
int ucGameChoice = -1;
FI_MSX gpFic[MAX_ROMS];
char szName[256];
char szFile[256];
u8 bForceMSXLoad = false;
u32 file_size = 0;
char strBuf[40];

struct Config_t AllConfigs[MAX_CONFIGS];
struct Config_t myConfig __attribute((aligned(4))) __attribute__((section(".dtcm")));
struct GlobalConfig_t myGlobalConfig;
extern u32 file_crc;

u8 option_table=0;

const char szKeyName[MAX_KEY_OPTIONS][18] = {
  "P1 JOY UP",
  "P1 JOY DOWN",
  "P1 JOY LEFT",
  "P1 JOY RIGHT",
  "P1 BTN 1 (L/YLW)",
  "P1 BTN 2 (R/RED)",
  "P1 BTN 3 (PURP)",
  "P1 BTN 4 (BLUE)",
  "P1 KEYPAD #1",
  "P1 KEYPAD #2",
  "P1 KEYPAD #3",
  "P1 KEYPAD #4",
  "P1 KEYPAD #5",
  "P1 KEYPAD #6",
  "P1 KEYPAD #7",
  "P1 KEYPAD #8",
  "P1 KEYPAD #9",
  "P1 KEYPAD ##",
  "P1 KEYPAD #0",
  "P1 KEYPAD #*",

  "P2 JOY UP",
  "P2 JOY DOWN",
  "P2 JOY LEFT",
  "P2 JOY RIGHT",
  "P2 BTN 1 (L/YLW)",
  "P2 BTN 2 (R/RED)",
  "P2 BTN 3 (PURP)",
  "P2 BTN 4 (BLUE)",
  "P2 KEYPAD #1",
  "P2 KEYPAD #2",
  "P2 KEYPAD #3",
  "P2 KEYPAD #4",
  "P2 KEYPAD #5",
  "P2 KEYPAD #6",
  "P2 KEYPAD #7",
  "P2 KEYPAD #8",
  "P2 KEYPAD #9",
  "P2 KEYPAD ##",
  "P2 KEYPAD #0",
  "P2 KEYPAD #*",

  "KEYBOARD A", //45
  "KEYBOARD B",
  "KEYBOARD C",
  "KEYBOARD D",
  "KEYBOARD E",
  "KEYBOARD F",
  "KEYBOARD G",
  "KEYBOARD H",
  "KEYBOARD I",
  "KEYBOARD J",
  "KEYBOARD K",
  "KEYBOARD L",
  "KEYBOARD M",
  "KEYBOARD N",
  "KEYBOARD O",
  "KEYBOARD P",
  "KEYBOARD Q", // 60
  "KEYBOARD R",
  "KEYBOARD S",
  "KEYBOARD T",
  "KEYBOARD U",
  "KEYBOARD V",
  "KEYBOARD W",
  "KEYBOARD X",
  "KEYBOARD Y",
  "KEYBOARD Z",

  "KEYBOARD 0", // 70
  "KEYBOARD 1",
  "KEYBOARD 2",
  "KEYBOARD 3",
  "KEYBOARD 4",
  "KEYBOARD 5",
  "KEYBOARD 6",
  "KEYBOARD 7",
  "KEYBOARD 8",
  "KEYBOARD 9",

  "KEYBOARD SHIFT",
  "KEYBOARD CTRL",
  "KEYBOARD CODE",
  "KEYBOARD GRAPH",

  "KEYBOARD SPACE",
  "KEYBOARD RETURN",
  "KEYBOARD ESC",

  "KEYBOARD HOME",
  "KEYBOARD UP",
  "KEYBOARD DOWN",
  "KEYBOARD LEFT",
  "KEYBOARD RIGHT",

  "KEYBOARD PERIOD",
  "KEYBOARD COMMA",
  "KEYBOARD COLON",
  "KEYBOARD SEMI",
  "KEYBOARD QUOTE",
  "KEYBOARD SLASH",
  "KEYBOARD BSLASH",
  "KEYBOARD PLUS",
  "KEYBOARD MINUS",
  "KEYBOARD LBRACKET",
  "KEYBOARD RBRACKET",
  "KEYBOARD CARET",
  "KEYBOARD ASTERISK",
  "KEYBOARD ATSIGN",
  "KEYBOARD BS",
  "KEYBOARD TAB",
  "KEYBOARD INS",
  "KEYBOARD DEL",
  "KEYBOARD CLEAR",
  "KEYBOARD UNDO",
  "KEYBOARD MOVE",
  "KEYBOARD WILDCARD",
  "KEYBOARD STORE",
  "KEYBOARD PRINT",
  "KEYBOARD STOP/BRK",
  "KEYBOARD F1 (I)",
  "KEYBOARD F2 (II)",
  "KEYBOARD F3 (III)",
  "KEYBOARD F4 (IV)",
  "KEYBOARD F5 (V)",
  "KEYBOARD F6 (VI)",
  "KEYBOARD F7",
  "KEYBOARD F8",
};


/*********************************************************************************
 * Show A message with YES / NO
 ********************************************************************************/
u8 showMessage(char *szCh1, char *szCh2) {
  u16 iTx, iTy;
  u8 uRet=ID_SHM_CANCEL;
  u8 ucGau=0x00, ucDro=0x00,ucGauS=0x00, ucDroS=0x00, ucCho = ID_SHM_YES;

  BottomScreenOptions();

  DSPrint(16-strlen(szCh1)/2,10,6,szCh1);
  DSPrint(16-strlen(szCh2)/2,12,6,szCh2);
  DSPrint(8,14,6,("> YES <"));
  DSPrint(20,14,6,("  NO   "));
  while ((keysCurrent() & (KEY_TOUCH | KEY_LEFT | KEY_RIGHT | KEY_A ))!=0);

  while (uRet == ID_SHM_CANCEL)
  {
    WAITVBL;
    if (keysCurrent() & KEY_TOUCH) {
      touchPosition touch;
      touchRead(&touch);
      iTx = touch.px;
      iTy = touch.py;
      if ( (iTx>8*8) && (iTx<8*8+7*8) && (iTy>14*8-4) && (iTy<15*8+4) ) {
        if (!ucGauS) {
          DSPrint(8,14,6,("> YES <"));
          DSPrint(20,14,6,("  NO   "));
          ucGauS = 1;
          if (ucCho == ID_SHM_YES) {
            uRet = ucCho;
          }
          else {
            ucCho  = ID_SHM_YES;
          }
        }
      }
      else
        ucGauS = 0;
      if ( (iTx>20*8) && (iTx<20*8+7*8) && (iTy>14*8-4) && (iTy<15*8+4) ) {
        if (!ucDroS) {
          DSPrint(8,14,6,("  YES  "));
          DSPrint(20,14,6,("> NO  <"));
          ucDroS = 1;
          if (ucCho == ID_SHM_NO) {
            uRet = ucCho;
          }
          else {
            ucCho = ID_SHM_NO;
          }
        }
      }
      else
        ucDroS = 0;
    }
    else {
      ucDroS = 0;
      ucGauS = 0;
    }

    if (keysCurrent() & KEY_LEFT){
      if (!ucGau) {
        ucGau = 1;
        if (ucCho == ID_SHM_YES) {
          ucCho = ID_SHM_NO;
          DSPrint(8,14,6,("  YES  "));
          DSPrint(20,14,6,("> NO  <"));
        }
        else {
          ucCho  = ID_SHM_YES;
          DSPrint(8,14,6,("> YES <"));
          DSPrint(20,14,6,("  NO   "));
        }
        WAITVBL;
      }
    }
    else {
      ucGau = 0;
    }
    if (keysCurrent() & KEY_RIGHT) {
      if (!ucDro) {
        ucDro = 1;
        if (ucCho == ID_SHM_YES) {
          ucCho  = ID_SHM_NO;
          DSPrint(8,14,6,("  YES  "));
          DSPrint(20,14,6,("> NO  <"));
        }
        else {
          ucCho  = ID_SHM_YES;
          DSPrint(8,14,6,("> YES <"));
          DSPrint(20,14,6,("  NO   "));
        }
        WAITVBL;
      }
    }
    else {
      ucDro = 0;
    }
    if (keysCurrent() & KEY_A) {
      uRet = ucCho;
    }
  }
  while ((keysCurrent() & (KEY_TOUCH | KEY_LEFT | KEY_RIGHT | KEY_A ))!=0);

  BottomScreenKeypad();  // Could be generic or overlay...

  return uRet;
}

void HachibittoModeNormal(void) {
  REG_BG3CNT = BG_BMP8_256x256;
  REG_BG3PA = (1<<8);
  REG_BG3PB = 0;
  REG_BG3PC = 0;
  REG_BG3PD = (1<<8);
  REG_BG3X = 0;
  REG_BG3Y = 0;
}

//*****************************************************************************
// Put the top screen in refocused bitmap mode
//*****************************************************************************
void HachibittoInitScreenUp(void) {
  videoSetMode(MODE_5_2D | DISPLAY_BG3_ACTIVE);
  vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
  vramSetBankB(VRAM_B_MAIN_SPRITE);
  HachibittoModeNormal();
}

// ----------------------------------------------------------------------------
// This stuff handles the 'random' screen snapshot at the top screen...
// ----------------------------------------------------------------------------
void showRandomPreviewSnaps(void) {
  u16 *pusEcran=(u16*) bgGetMapPtr(bg1);
  u32 uX,uY;

  if (vusCptVBL>=5*60) {
    u8 uEcran = rand() % 6;
    vusCptVBL = 0;
    if (uEcran>2) {
      uEcran-=3;
      for (uY=24;uY<33;uY++) {
        for (uX=0;uX<12;uX++) {
          *(pusEcran + (15+uX) + ((10+uY-24)<<5)) = *(bgGetMapPtr(bg0) + (uY+uEcran*9)*32 + uX+12);
        }
      }
    }
    else
    {
      for (uY=24;uY<33;uY++) {
        for (uX=0;uX<12;uX++) {
          *(pusEcran + (15+uX) + ((10+uY-24)<<5)) = *(bgGetMapPtr(bg0) + (uY+uEcran*9)*32 + uX);
        }
      }
    }
  }
}

/*********************************************************************************
 * Show The 14 games on the list to allow the user to choose a new game.
 ********************************************************************************/
static char szName2[40];
void dsDisplayFiles(u16 NoDebGame, u8 ucSel)
{
  u16 ucBcl,ucGame;
  u8 maxLen;

  DSPrint(30,8,0,(NoDebGame>0 ? "<" : " "));
  DSPrint(30,21,0,(NoDebGame+14<countCV ? ">" : " "));
  sprintf(szName,"%03d/%03d FILES AVAILABLE     ",ucSel+1+NoDebGame,countCV);
  DSPrint(2,6,0, szName);
  for (ucBcl=0;ucBcl<14; ucBcl++)
  {
    ucGame= ucBcl+NoDebGame;
    if (ucGame < countCV)
    {
      maxLen=strlen(gpFic[ucGame].szName);
      strcpy(szName,gpFic[ucGame].szName);
      if (maxLen>28) szName[28]='\0';
      if (gpFic[ucGame].uType == DIRECTORY)
      {
        szName[26] = 0; // Needs to be 2 chars shorter with brackets
        sprintf(szName2, "[%s]",szName);
        sprintf(szName,"%-28s",szName2);
        DSPrint(1,8+ucBcl,(ucSel == ucBcl ? 2 :  0),szName);
      }
      else
      {
        sprintf(szName,"%-28s",strupr(szName));
        DSPrint(1,8+ucBcl,(ucSel == ucBcl ? 2 : 0 ),szName);
      }
    }
    else
    {
        DSPrint(1,8+ucBcl,(ucSel == ucBcl ? 2 : 0 ),"                            ");
    }
  }
}


// -------------------------------------------------------------------------
// Standard qsort routine for the coleco games - we sort all directory
// listings first and then a case-insenstive sort of all games.
// -------------------------------------------------------------------------
int colecoFilescmp (const void *c1, const void *c2)
{
  FI_MSX *p1 = (FI_MSX *) c1;
  FI_MSX *p2 = (FI_MSX *) c2;

  if (p1->szName[0] == '.' && p2->szName[0] != '.')
      return -1;
  if (p2->szName[0] == '.' && p1->szName[0] != '.')
      return 1;
  if ((p1->uType == DIRECTORY) && !(p2->uType == DIRECTORY))
      return -1;
  if ((p2->uType == DIRECTORY) && !(p1->uType == DIRECTORY))
      return 1;
  return strcasecmp (p1->szName, p2->szName);
}

/*********************************************************************************
 * Find files (COL / ROM) available - sort them for display.
 ********************************************************************************/
void HachibittoFindFiles(void)
{
  u32 uNbFile;
  DIR *dir;
  struct dirent *pent;

  uNbFile=0;
  countCV=0;

  dir = opendir(".");
  while (((pent=readdir(dir))!=NULL) && (uNbFile<MAX_ROMS))
  {
    strcpy(szFile,pent->d_name);

    if(pent->d_type == DT_DIR)
    {
      if (!((szFile[0] == '.') && (strlen(szFile) == 1)))
      {
        // Do not include the [sav] directory
        if (strcasecmp(szFile, "sav") != 0)
        {
            strcpy(gpFic[uNbFile].szName,szFile);
            gpFic[uNbFile].uType = DIRECTORY;
            uNbFile++;
            countCV++;
        }
      }
    }
    else {
      if ((strlen(szFile)>4) && (strlen(szFile)<(MAX_ROM_NAME-4)) && (szFile[0] != '.') && (szFile[0] != '_'))  // For MAC don't allow underscore files
      {
        if ( (strcasecmp(strrchr(szFile, '.'), ".rom") == 0) )  {
          strcpy(gpFic[uNbFile].szName,szFile);
          gpFic[uNbFile].uType = MSXROM;
          uNbFile++;
          countCV++;
        }
        if ( (strcasecmp(strrchr(szFile, '.'), ".bin") == 0) )  {
          strcpy(gpFic[uNbFile].szName,szFile);
          gpFic[uNbFile].uType = MSXROM;
          uNbFile++;
          countCV++;
        }
        if ( (strcasecmp(strrchr(szFile, '.'), ".dsk") == 0) )  {
          strcpy(gpFic[uNbFile].szName,szFile);
          gpFic[uNbFile].uType = MSXROM;
          uNbFile++;
          countCV++;
        }
      }
    }
  }
  closedir(dir);

  // ----------------------------------------------
  // If we found any files, go sort the list...
  // ----------------------------------------------
  if (countCV)
  {
    qsort (gpFic, countCV, sizeof(FI_MSX), colecoFilescmp);
  }
}


// ----------------------------------------------------------------
// Let the user select a new game (rom) file and load it up!
// ----------------------------------------------------------------
u8 HachibittoLoadFile(void)
{
  bool bDone=false;
  u16 ucHaut=0x00, ucBas=0x00,ucSHaut=0x00, ucSBas=0x00, romSelected= 0, firstRomDisplay=0,nbRomPerPage, uNbRSPage;
  s16 uLenFic=0, ucFlip=0, ucFlop=0;

  // Show the menu...
  while ((keysCurrent() & (KEY_TOUCH | KEY_START | KEY_SELECT | KEY_A | KEY_B))!=0);
  unsigned short dmaVal =  *(bgGetMapPtr(bg0b) + 24*32);
  dmaFillWords(dmaVal | (dmaVal<<16),(void*) bgGetMapPtr(bg1b)+5*32*2,32*19*2);
  DSPrint(7,5,0,"A=SELECT,  B=EXIT");

  HachibittoFindFiles();

  ucGameChoice = -1;

  nbRomPerPage = (countCV>=14 ? 14 : countCV);
  uNbRSPage = (countCV>=5 ? 5 : countCV);

  if (ucGameAct>countCV-nbRomPerPage)
  {
    firstRomDisplay=countCV-nbRomPerPage;
    romSelected=ucGameAct-countCV+nbRomPerPage;
  }
  else
  {
    firstRomDisplay=ucGameAct;
    romSelected=0;
  }
  dsDisplayFiles(firstRomDisplay,romSelected);

  // -----------------------------------------------------
  // Until the user selects a file or exits the menu...
  // -----------------------------------------------------
  while (!bDone)
  {
    if (keysCurrent() & KEY_UP)
    {
      if (!ucHaut)
      {
        ucGameAct = (ucGameAct>0 ? ucGameAct-1 : countCV-1);
        if (romSelected>uNbRSPage) { romSelected -= 1; }
        else {
          if (firstRomDisplay>0) { firstRomDisplay -= 1; }
          else {
            if (romSelected>0) { romSelected -= 1; }
            else {
              firstRomDisplay=countCV-nbRomPerPage;
              romSelected=nbRomPerPage-1;
            }
          }
        }
        ucHaut=0x01;
        dsDisplayFiles(firstRomDisplay,romSelected);
      }
      else {

        ucHaut++;
        if (ucHaut>10) ucHaut=0;
      }
      uLenFic=0; ucFlip=-50; ucFlop=0;
    }
    else
    {
      ucHaut = 0;
    }
    if (keysCurrent() & KEY_DOWN)
    {
      if (!ucBas) {
        ucGameAct = (ucGameAct< countCV-1 ? ucGameAct+1 : 0);
        if (romSelected<uNbRSPage-1) { romSelected += 1; }
        else {
          if (firstRomDisplay<countCV-nbRomPerPage) { firstRomDisplay += 1; }
          else {
            if (romSelected<nbRomPerPage-1) { romSelected += 1; }
            else {
              firstRomDisplay=0;
              romSelected=0;
            }
          }
        }
        ucBas=0x01;
        dsDisplayFiles(firstRomDisplay,romSelected);
      }
      else
      {
        ucBas++;
        if (ucBas>10) ucBas=0;
      }
      uLenFic=0; ucFlip=-50; ucFlop=0;
    }
    else {
      ucBas = 0;
    }

    // -------------------------------------------------------------
    // Left and Right on the D-Pad will scroll 1 page at a time...
    // -------------------------------------------------------------
    if (keysCurrent() & KEY_RIGHT)
    {
      if (!ucSBas)
      {
        ucGameAct = (ucGameAct< countCV-nbRomPerPage ? ucGameAct+nbRomPerPage : countCV-nbRomPerPage);
        if (firstRomDisplay<countCV-nbRomPerPage) { firstRomDisplay += nbRomPerPage; }
        else { firstRomDisplay = countCV-nbRomPerPage; }
        if (ucGameAct == countCV-nbRomPerPage) romSelected = 0;
        ucSBas=0x01;
        dsDisplayFiles(firstRomDisplay,romSelected);
      }
      else
      {
        ucSBas++;
        if (ucSBas>10) ucSBas=0;
      }
      uLenFic=0; ucFlip=-50; ucFlop=0;
    }
    else {
      ucSBas = 0;
    }

    // -------------------------------------------------------------
    // Left and Right on the D-Pad will scroll 1 page at a time...
    // -------------------------------------------------------------
    if (keysCurrent() & KEY_LEFT)
    {
      if (!ucSHaut)
      {
        ucGameAct = (ucGameAct> nbRomPerPage ? ucGameAct-nbRomPerPage : 0);
        if (firstRomDisplay>nbRomPerPage) { firstRomDisplay -= nbRomPerPage; }
        else { firstRomDisplay = 0; }
        if (ucGameAct == 0) romSelected = 0;
        if (romSelected > ucGameAct) romSelected = ucGameAct;
        ucSHaut=0x01;
        dsDisplayFiles(firstRomDisplay,romSelected);
      }
      else
      {
        ucSHaut++;
        if (ucSHaut>10) ucSHaut=0;
      }
      uLenFic=0; ucFlip=-50; ucFlop=0;
    }
    else {
      ucSHaut = 0;
    }

    // -------------------------------------------------------------------------
    // They B key will exit out of the ROM selection without picking a new game
    // -------------------------------------------------------------------------
    if ( keysCurrent() & KEY_B )
    {
      bDone=true;
      while (keysCurrent() & KEY_B);
    }

    // -------------------------------------------------------------------
    // Any of these keys will pick the current ROM and try to load it...
    // -------------------------------------------------------------------
    if (keysCurrent() & KEY_A || keysCurrent() & KEY_Y || keysCurrent() & KEY_X)
    {
      if (gpFic[ucGameAct].uType != DIRECTORY)
      {
        bDone=true;
        ucGameChoice = ucGameAct;
        bForceMSXLoad = false;
        if (keysCurrent() & KEY_X) bForceMSXLoad=true;
        WAITVBL;
      }
      else
      {
        chdir(gpFic[ucGameAct].szName);
        HachibittoFindFiles();
        ucGameAct = 0;
        nbRomPerPage = (countCV>=14 ? 14 : countCV);
        uNbRSPage = (countCV>=5 ? 5 : countCV);
        if (ucGameAct>countCV-nbRomPerPage) {
          firstRomDisplay=countCV-nbRomPerPage;
          romSelected=ucGameAct-countCV+nbRomPerPage;
        }
        else {
          firstRomDisplay=ucGameAct;
          romSelected=0;
        }
        dsDisplayFiles(firstRomDisplay,romSelected);
        while (keysCurrent() & KEY_A);
      }
    }

    // --------------------------------------------
    // If the filename is too long... scroll it.
    // --------------------------------------------
    if (strlen(gpFic[ucGameAct].szName) > 29)
    {
      ucFlip++;
      if (ucFlip >= 25)
      {
        ucFlip = 0;
        uLenFic++;
        if ((uLenFic+28)>strlen(gpFic[ucGameAct].szName))
        {
          ucFlop++;
          if (ucFlop >= 15)
          {
            uLenFic=0;
            ucFlop = 0;
          }
          else
            uLenFic--;
        }
        strncpy(szName,gpFic[ucGameAct].szName+uLenFic,28);
        szName[28] = '\0';
        DSPrint(1,8+romSelected,2,szName);
      }
    }
    showRandomPreviewSnaps();
    swiWaitForVBlank();
  }

  // Wait for some key to be pressed before returning
  while ((keysCurrent() & (KEY_TOUCH | KEY_START | KEY_SELECT | KEY_A | KEY_B | KEY_R | KEY_L | KEY_UP | KEY_DOWN))!=0);

  return 0x01;
}

// ---------------------------------------------------------------------------
// Write out the Hachibitto.dat configuration file to capture the settings for
// each game.  This one file contains global settings + 400 game settings.
// ---------------------------------------------------------------------------
void SaveConfig(bool bShow)
{
    FILE *fp;
    int slot = 0;

    if (bShow) DSPrint(6,23,0, (char*)"SAVING CONFIGURATION");

    allocateCompressedMem();

    // Set the global configuration version number...
    myGlobalConfig.config_ver = CONFIG_VER;

    // If there is a game loaded, save that into a slot... re-use the same slot if it exists
    myConfig.game_crc = file_crc;

    // Find the slot we should save into...
    for (slot=0; slot<MAX_CONFIGS; slot++)
    {
        if (AllConfigs[slot].game_crc == myConfig.game_crc)  // Got a match?!
        {
            break;
        }
        if (AllConfigs[slot].game_crc == 0x00000000)  // Didn't find it... use a blank slot...
        {
            break;
        }
    }

    // --------------------------------------------------------------------------
    // Copy our current game configuration to the main configuration database...
    // --------------------------------------------------------------------------
    if (myConfig.game_crc != 0x00000000)
    {
        memcpy(&AllConfigs[slot], &myConfig, sizeof(struct Config_t));
    }

    // --------------------------------------------------
    // Now save the config file out o the SD card...
    // --------------------------------------------------
    DIR* dir = opendir("/data");
    if (dir)
    {
        closedir(dir);  // Directory exists.
    }
    else
    {
        mkdir("/data", 0777);   // Doesn't exist - make it...
    }
    fp = fopen("/data/Hachibitto.dat", "wb+");
    if (fp != NULL)
    {
        myGlobalConfig.compressed = 1; // Write the AllConfigs[] below compressed
        fwrite(&myGlobalConfig, sizeof(myGlobalConfig), 1, fp); // Write the global config

        // --------------------------------------------------------------------
        // Compress the configuration data - this shrinks down quite nicely...
        // --------------------------------------------------------------------
        int max_len = lzav_compress_bound_hi( sizeof(AllConfigs) );
        int comp_len = lzav_compress_hi( &AllConfigs, COMPRESS_BUFFER, sizeof(AllConfigs), max_len );

        fwrite(&comp_len,          sizeof(comp_len), 1, fp);
        fwrite(COMPRESS_BUFFER,    comp_len,         1, fp);

        fclose(fp);
    } else DSPrint(4,23,0, (char*)"ERROR SAVING CONFIG FILE");

    if (bShow)
    {
        WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;
        DSPrint(4,23,0, (char*)"                        ");
    }

    restoreCompressedMem();
}

void MapPlayer1(void)
{
    myConfig.keymap[0]   = 0;    // NDS D-Pad mapped to CV Joystick UP
    myConfig.keymap[1]   = 1;    // NDS D-Pad mapped to CV Joystick DOWN
    myConfig.keymap[2]   = 2;    // NDS D-Pad mapped to CV Joystick LEFT
    myConfig.keymap[3]   = 3;    // NDS D-Pad mapped to CV Joystick RIGHT
    myConfig.keymap[4]   = 4;    // NDS A Button mapped to CV Button 1 (Yellow / Left Button)
    myConfig.keymap[5]   = 5;    // NDS B Button mapped to CV Button 2 (Red / Right Button)
    myConfig.keymap[6]   = 6;    // NDS X Button mapped to CV Button 3 (Purple / Super Action)
    myConfig.keymap[7]   = 7;    // NDS Y Button mapped to CV Button 4 (Blue / Super Action)
    myConfig.keymap[8]   = 81;   // NDS R      mapped to CTRL
    myConfig.keymap[9]   = 80;   // NDS L      mapped to SHIFT
    myConfig.keymap[10]  = 8;    // NDS Start  mapped to Keypad #1
    myConfig.keymap[11]  = 9;    // NDS Select mapped to Keypad #2
}

void MapPlayer2(void)
{
    myConfig.keymap[0]   = 20;    // NDS D-Pad mapped to CV Joystick UP
    myConfig.keymap[1]   = 21;    // NDS D-Pad mapped to CV Joystick DOWN
    myConfig.keymap[2]   = 22;    // NDS D-Pad mapped to CV Joystick LEFT
    myConfig.keymap[3]   = 23;    // NDS D-Pad mapped to CV Joystick RIGHT
    myConfig.keymap[4]   = 24;    // NDS A Button mapped to CV Button 1 (Yellow / Left Button)
    myConfig.keymap[5]   = 25;    // NDS B Button mapped to CV Button 2 (Red / Right Button)
    myConfig.keymap[6]   = 26;    // NDS X Button mapped to CV Button 3 (Purple / Super Action)
    myConfig.keymap[7]   = 27;    // NDS Y Button mapped to CV Button 4 (Blue / Super Action)
    myConfig.keymap[8]   = 81;    // NDS R      mapped to CTRL
    myConfig.keymap[9]   = 80;    // NDS L      mapped to SHIFT
    myConfig.keymap[10]  = 28;    // NDS Start  mapped to Keypad #1
    myConfig.keymap[11]  = 29;    // NDS Select mapped to Keypad #2
}

void MapQAOP(void)
{
    myConfig.keymap[0]   = 60;    // Q
    myConfig.keymap[1]   = 44;    // A
    myConfig.keymap[2]   = 58;    // O
    myConfig.keymap[3]   = 59;    // P
    myConfig.keymap[4]   = 84;    // Space
    myConfig.keymap[5]   = 84;    // Space
    myConfig.keymap[6]   = 92;    // Period
    myConfig.keymap[7]   = 92;    // Period
    myConfig.keymap[8]   = 81;    // NDS R      mapped to CTRL
    myConfig.keymap[9]   = 80;    // NDS L      mapped to SHIFT
    myConfig.keymap[10]  = 71;    // 1
    myConfig.keymap[11]  = 72;    // 2
}

void MapWASD(void)
{
    myConfig.keymap[0]   = 66;    // W
    myConfig.keymap[1]   = 44;    // A
    myConfig.keymap[2]   = 62;    // S
    myConfig.keymap[3]   = 47;    // D
    myConfig.keymap[4]   = 84;    // Space
    myConfig.keymap[5]   = 84;    // Space
    myConfig.keymap[6]   = 84;    // Space
    myConfig.keymap[7]   = 84;    // Space
    myConfig.keymap[8]   = 81;    // NDS R      mapped to CTRL
    myConfig.keymap[9]   = 80;    // NDS L      mapped to SHIFT
    myConfig.keymap[10]  = 71;    // 1
    myConfig.keymap[11]  = 72;    // 2
}

void MapZCPeriod(void)
{
    myConfig.keymap[0]   = 60;    // Q
    myConfig.keymap[1]   = 44;    // A
    myConfig.keymap[2]   = 69;    // Z
    myConfig.keymap[3]   = 46;    // C
    myConfig.keymap[4]   = 92;    // Period
    myConfig.keymap[5]   = 92;    // Period
    myConfig.keymap[6]   = 84;    // Space
    myConfig.keymap[7]   = 84;    // Space
    myConfig.keymap[8]   = 81;    // NDS R      mapped to CTRL
    myConfig.keymap[9]   = 80;    // NDS L      mapped to SHIFT
    myConfig.keymap[10]  = 71;    // 1
    myConfig.keymap[11]  = 72;    // 2
}

void MapZXSpace(void)
{
    myConfig.keymap[0]   = 84;    // Space
    myConfig.keymap[1]   = 92;    // Period
    myConfig.keymap[2]   = 69;    // Z
    myConfig.keymap[3]   = 67;    // X
    myConfig.keymap[4]   = 84;    // Space
    myConfig.keymap[5]   = 84;    // Space
    myConfig.keymap[6]   = 85;    // Return
    myConfig.keymap[7]   = 85;    // Return
    myConfig.keymap[8]   = 81;    // NDS R      mapped to CTRL
    myConfig.keymap[9]   = 80;    // NDS L      mapped to SHIFT
    myConfig.keymap[10]  = 71;    // 1
    myConfig.keymap[11]  = 72;    // 2
}



void MapArrows(void)
{
    myConfig.keymap[0]   = 88;    // UP Arrow
    myConfig.keymap[1]   = 89;    // Down Arrow
    myConfig.keymap[2]   = 90;    // Left Arrow
    myConfig.keymap[3]   = 91;    // Right Arrow
    myConfig.keymap[4]   = 84;    // Space
    myConfig.keymap[5]   = 84;    // Space
    myConfig.keymap[6]   = 84;    // Space
    myConfig.keymap[7]   = 84;    // Space
    myConfig.keymap[8]   = 81;    // NDS R      mapped to CTRL
    myConfig.keymap[9]   = 80;    // NDS L      mapped to SHIFT
    myConfig.keymap[10]  = 71;    // 1
    myConfig.keymap[11]  = 72;    // 2
}


void SetDefaultGlobalConfig(void)
{
    // A few global defaults...
    memset(&myGlobalConfig, 0x00, sizeof(myGlobalConfig));
    myGlobalConfig.showFPS        = 0;    // Don't show FPS counter by default
    myGlobalConfig.emuText        = 1;    // Default is to show Emulator Text
    myGlobalConfig.diskSfxMute    = 0;    // By default Disk/DDP loading sounds are enabled... 1=Mute
}

void SetDefaultGameConfig(void)
{
    myConfig.game_crc    = 0;    // No game in this slot yet

    MapPlayer1();                // Default to Player 1 mapping

    myConfig.msxMapper   = GUESS;                       // MSX mapper takes its best guess
    myConfig.slotType    = 0;                           // Type A
    myConfig.autoFire    = 0;                           // Default to no auto-fire on either button
    myConfig.overlay     = 0;                           // Default to normal CV overlay
    myConfig.maxSprites  = 0;                           // 0 means allow 32 sprites... 1 means limit to the original 4/8 sprites of the VDP
    myConfig.dpad        = DPAD_NORMAL;                 // Normal DPAD use - mapped to joystick
    myConfig.memWipe     = 0;                           // Default to RANDOM memory
    myConfig.yOffset     = 0;
    myConfig.soundDriver = SND_DRV_NORMAL;              // Default is normal sound driver (not Wave Direct)
    myConfig.overlay      = 1;                          // MSX defaults to full keyboard
    myConfig.reserved1   = 0;
    myConfig.reserved2   = 0;
    myConfig.reserved3   = 0;
    myConfig.reserved4   = 0;
    myConfig.reserved5   = 0;
    myConfig.reserved6   = 0;
    myConfig.reserved7   = 0;
    myConfig.reserved8   = 0xA5;    // So it's easy to spot on an "upgrade" and we can re-default it

    // ----------------------------------------------------------------------------------
    // A few games don't want more than 4 max sprites (they pull tricks that rely on it)
    // ----------------------------------------------------------------------------------
    if (file_crc == 0xee530ad2) myConfig.maxSprites  = 1;  // QBiqs
    if (file_crc == 0x275c800e) myConfig.maxSprites  = 1;  // Antartic Adventure
    if (file_crc == 0xa66e5ed1) myConfig.maxSprites  = 1;  // Antartic Adventure Prototype
    if (file_crc == 0x6af19e75) myConfig.maxSprites  = 1;  // Adventures in the Park
    if (file_crc == 0xbc8320a0) myConfig.maxSprites  = 1;  // Uridium

}

// ----------------------------------------------------------
// Load configuration into memory where we can use it.
// The configuration is stored in Hachibitto.dat
// ----------------------------------------------------------
void LoadConfig(void)
{
    allocateCompressedMem();

    // -----------------------------------------------------------------
    // Start with defaults.. if we find a match in our config database
    // below, we will fill in the config with data read from the file.
    // -----------------------------------------------------------------
    SetDefaultGameConfig();

    if (ReadFileCarefully("/data/Hachibitto.dat", (u8*)&myGlobalConfig, sizeof(myGlobalConfig), 0))  // Read Global Config
    {
        if (myGlobalConfig.compressed)
        {
            int comp_len = 0;
            ReadFileCarefully("/data/Hachibitto.dat", (u8*)&comp_len, sizeof(comp_len), sizeof(myGlobalConfig)); // Read the full game array of configs
            ReadFileCarefully("/data/Hachibitto.dat", (u8*)COMPRESS_BUFFER, comp_len, sizeof(myGlobalConfig) + sizeof(comp_len)); // Read the full game array of configs
            (void)lzav_decompress( COMPRESS_BUFFER, AllConfigs, comp_len, sizeof(AllConfigs) );
        }
        else // Old-format... not compressed
        {
            ReadFileCarefully("/data/Hachibitto.dat", (u8*)&AllConfigs, sizeof(AllConfigs), sizeof(myGlobalConfig)); // Read the full game array of configs
        }

        if (myGlobalConfig.config_ver != CONFIG_VER)
        {
            memset(&AllConfigs, 0x00, sizeof(AllConfigs));
            SetDefaultGameConfig();
            SetDefaultGlobalConfig();
            SaveConfig(FALSE);
        }
    }
    else    // Not found... init the entire database...
    {
        memset(&AllConfigs, 0x00, sizeof(AllConfigs));

        SetDefaultGameConfig();
        SetDefaultGlobalConfig();
        SaveConfig(FALSE);
    }

    restoreCompressedMem();
}

// -------------------------------------------------------------------------
// Try to match our loaded game to a configuration my matching CRCs
// -------------------------------------------------------------------------
void FindConfig(void)
{
    // -----------------------------------------------------------------
    // Start with defaults.. if we find a match in our config database
    // below, we will fill in the config with data read from the file.
    // -----------------------------------------------------------------
    SetDefaultGameConfig();

    for (u16 slot=0; slot<MAX_CONFIGS; slot++)
    {
        if (AllConfigs[slot].game_crc == file_crc)  // Got a match?!
        {
            memcpy(&myConfig, &AllConfigs[slot], sizeof(struct Config_t));
            break;
        }
    }
}


// ------------------------------------------------------------------------------
// Options are handled here... we have a number of things the user can tweak
// and these options are applied immediately. The user can also save off
// their option choices for the currently running game into the NINTV-DS.DAT
// configuration database. When games are loaded back up, NINTV-DS.DAT is read
// to see if we have a match and the user settings can be restored for the game.
// ------------------------------------------------------------------------------
struct options_t
{
    const char  *label;
    const char  *option[37];
    u8          *option_val;
    u8           option_max;
};

const struct options_t Option_Table[3][20] =
{
    // Page 1
    {
        {"OVERLAY",        {"FULL KEYBOARD", "ALPHA KEYBOARD"},                                                                                                                 &myConfig.overlay,    2},
        {"MAX SPRITES",    {"32",  "4/8"},                                                                                                                                      &myConfig.maxSprites, 2},
        {"AUTO FIRE",      {"OFF", "B1 ONLY", "B2 ONLY", "BOTH"},                                                                                                               &myConfig.autoFire,   4},
        {"JOYSTICK",       {"NORMAL", "DIAGONALS", "SLIDE-N-GLILDE"},                                                                                                           &myConfig.dpad,       3},
        {"RAM WIPE",       {"RANDOM", "CLEAR"},                                                                                                                                 &myConfig.memWipe,    2},
        {"MSX MAPPER",     {"GUESS","KONAMI 8K","ASCII 8K","KONAMI SCC","ASCII 16K","ZEMINA 8K","ZEMINA 16K","CROSSBLAIM","LODERUNNER", "XEVIOUS", "RESERVED","RESERVED",
                            "AT 0000H","AT 4000H","AT 8000H","64K LINEAR"},                                                                                                     &myConfig.msxMapper,  16},
        {"SLOT TYPE",      {"TYPE A", "TYPE B"},                                                                                                                                &myConfig.slotType,    2},
        {"Y OFFSET",       {"None", "+1", "+2", "+3", "+4", "+5", "+6", "+7", "+8", "+9", "+10", "+11", "+12", "+13", "+14", "+15", "+16", "+17", "+18", "+19", "+20"},         &myConfig.yOffset,    21},
        {NULL,             {"",      ""},                                                                                                                                       NULL,                 1},
    },
    // Global Options
    {
        {"FPS",            {"OFF", "ON", "ON FULLSPEED"},                                                                                                                       &myGlobalConfig.showFPS,  3},
        {"DEBUGGER",       {"OFF", "BAD OPS", "DEBUG", "FULL DEBUG"},                                                                                                           &myGlobalConfig.debugger, 4},

        {NULL,             {"",      ""},                                                                                                                                       NULL,                     1},
    }
};


// ------------------------------------------------------------------
// Display the current list of options for the user.
// ------------------------------------------------------------------
u8 display_options_list(bool bFullDisplay)
{
    s16 len=0;

    DSPrint(1,21, 0, (char *)"                              ");
    if (bFullDisplay)
    {
        while (true)
        {
            sprintf(strBuf, " %-12s : %-14s", Option_Table[option_table][len].label, Option_Table[option_table][len].option[*(Option_Table[option_table][len].option_val)]);
            DSPrint(1,5+len, (len==0 ? 2:0), strBuf); len++;
            if (Option_Table[option_table][len].label == NULL) break;
        }

        // Blank out rest of the screen... option menus are of different lengths...
        for (int i=len; i<16; i++)
        {
            DSPrint(1,5+i, 0, (char *)"                               ");
        }
    }

    DSPrint(1,22, 0, (char *)"  B=EXIT, X=MORE, START=SAVE  ");
    return len;
}


//*****************************************************************************
// Change Game Options for the current game
//*****************************************************************************
void HachibittoGameOptions(bool bIsGlobal)
{
    u8 optionHighlighted;
    u8 idx;
    bool bDone=false;
    int keys_pressed;
    int last_keys_pressed = 999;

    option_table = (bIsGlobal ? 1:0);

    idx=display_options_list(true);
    optionHighlighted = 0;
    while (keysCurrent() != 0)
    {
        WAITVBL;
    }
    while (!bDone)
    {
        keys_pressed = keysCurrent();
        if (keys_pressed != last_keys_pressed)
        {
            last_keys_pressed = keys_pressed;
            if (keysCurrent() & KEY_UP) // Previous option
            {
                sprintf(strBuf, " %-12s : %-14s", Option_Table[option_table][optionHighlighted].label, Option_Table[option_table][optionHighlighted].option[*(Option_Table[option_table][optionHighlighted].option_val)]);
                DSPrint(1,5+optionHighlighted,0, strBuf);
                if (optionHighlighted > 0) optionHighlighted--; else optionHighlighted=(idx-1);
                sprintf(strBuf, " %-12s : %-14s", Option_Table[option_table][optionHighlighted].label, Option_Table[option_table][optionHighlighted].option[*(Option_Table[option_table][optionHighlighted].option_val)]);
                DSPrint(1,5+optionHighlighted,2, strBuf);
            }
            if (keysCurrent() & KEY_DOWN) // Next option
            {
                sprintf(strBuf, " %-12s : %-14s", Option_Table[option_table][optionHighlighted].label, Option_Table[option_table][optionHighlighted].option[*(Option_Table[option_table][optionHighlighted].option_val)]);
                DSPrint(1,5+optionHighlighted,0, strBuf);
                if (optionHighlighted < (idx-1)) optionHighlighted++;  else optionHighlighted=0;
                sprintf(strBuf, " %-12s : %-14s", Option_Table[option_table][optionHighlighted].label, Option_Table[option_table][optionHighlighted].option[*(Option_Table[option_table][optionHighlighted].option_val)]);
                DSPrint(1,5+optionHighlighted,2, strBuf);
            }

            if (keysCurrent() & KEY_RIGHT)  // Toggle option clockwise
            {
                *(Option_Table[option_table][optionHighlighted].option_val) = (*(Option_Table[option_table][optionHighlighted].option_val) + 1) % Option_Table[option_table][optionHighlighted].option_max;
                sprintf(strBuf, " %-12s : %-14s", Option_Table[option_table][optionHighlighted].label, Option_Table[option_table][optionHighlighted].option[*(Option_Table[option_table][optionHighlighted].option_val)]);
                DSPrint(1,5+optionHighlighted,2, strBuf);
            }
            if (keysCurrent() & KEY_LEFT)  // Toggle option counterclockwise
            {
                if ((*(Option_Table[option_table][optionHighlighted].option_val)) == 0)
                    *(Option_Table[option_table][optionHighlighted].option_val) = Option_Table[option_table][optionHighlighted].option_max -1;
                else
                    *(Option_Table[option_table][optionHighlighted].option_val) = (*(Option_Table[option_table][optionHighlighted].option_val) - 1) % Option_Table[option_table][optionHighlighted].option_max;
                sprintf(strBuf, " %-12s : %-14s", Option_Table[option_table][optionHighlighted].label, Option_Table[option_table][optionHighlighted].option[*(Option_Table[option_table][optionHighlighted].option_val)]);
                DSPrint(1,5+optionHighlighted,2, strBuf);
            }
            if (keysCurrent() & KEY_START)  // Save Options
            {
                SaveConfig(TRUE);
            }
            if (keysCurrent() & (KEY_X)) // Toggle Table
            {
                option_table ^= 1;
                idx=display_options_list(true);
                optionHighlighted = 0;
                while (keysCurrent() != 0)
                {
                    WAITVBL;
                }
            }
            if ((keysCurrent() & KEY_B) || (keysCurrent() & KEY_A))  // Exit options
            {
                option_table = 0;   // Reset for next time
                break;
            }
        }
        showRandomPreviewSnaps();
        swiWaitForVBlank();
    }

    // Give a third of a second time delay...
    for (int i=0; i<20; i++)
    {
        swiWaitForVBlank();
    }

    return;
}

//*****************************************************************************
// Change Keymap Options for the current game
//*****************************************************************************
char szCha[34];
void DisplayKeymapName(u32 uY)
{
  sprintf(szCha," PAD UP    : %-17s",szKeyName[myConfig.keymap[0]]);
  DSPrint(1, 6,(uY==  6 ? 2 : 0),szCha);
  sprintf(szCha," PAD DOWN  : %-17s",szKeyName[myConfig.keymap[1]]);
  DSPrint(1, 7,(uY==  7 ? 2 : 0),szCha);
  sprintf(szCha," PAD LEFT  : %-17s",szKeyName[myConfig.keymap[2]]);
  DSPrint(1, 8,(uY==  8 ? 2 : 0),szCha);
  sprintf(szCha," PAD RIGHT : %-17s",szKeyName[myConfig.keymap[3]]);
  DSPrint(1, 9,(uY== 9 ? 2 : 0),szCha);
  sprintf(szCha," KEY A     : %-17s",szKeyName[myConfig.keymap[4]]);
  DSPrint(1,10,(uY== 10 ? 2 : 0),szCha);
  sprintf(szCha," KEY B     : %-17s",szKeyName[myConfig.keymap[5]]);
  DSPrint(1,11,(uY== 11 ? 2 : 0),szCha);
  sprintf(szCha," KEY X     : %-17s",szKeyName[myConfig.keymap[6]]);
  DSPrint(1,12,(uY== 12 ? 2 : 0),szCha);
  sprintf(szCha," KEY Y     : %-17s",szKeyName[myConfig.keymap[7]]);
  DSPrint(1,13,(uY== 13 ? 2 : 0),szCha);
  sprintf(szCha," KEY R     : %-17s",szKeyName[myConfig.keymap[8]]);
  DSPrint(1,14,(uY== 14 ? 2 : 0),szCha);
  sprintf(szCha," KEY L     : %-17s",szKeyName[myConfig.keymap[9]]);
  DSPrint(1,15,(uY== 15 ? 2 : 0),szCha);
  sprintf(szCha," START     : %-17s",szKeyName[myConfig.keymap[10]]);
  DSPrint(1,16,(uY== 16 ? 2 : 0),szCha);
  sprintf(szCha," SELECT    : %-17s",szKeyName[myConfig.keymap[11]]);
  DSPrint(1,17,(uY== 17 ? 2 : 0),szCha);
}

u8 keyMapType = 0;
void SwapKeymap(void)
{
    keyMapType = (keyMapType+1) % 7;
    switch (keyMapType)
    {
        case 0: MapPlayer1();  break;
        case 1: MapPlayer2();  break;
        case 2: MapQAOP();     break;
        case 3: MapWASD();     break;
        case 4: MapZCPeriod(); break;
        case 5: MapZXSpace();  break;
        case 6: MapArrows();   break;
    }
}


// ------------------------------------------------------------------------------
// Allow the user to change the key map for the current game and give them
// the option of writing that keymap out to a configuration file for the game.
// ------------------------------------------------------------------------------
void HachibittoChangeKeymap(void)
{
  u32 ucHaut=0x00, ucBas=0x00,ucL=0x00,ucR=0x00,ucY= 6, bOK=0, bIndTch=0;

  // ------------------------------------------------------
  // Clear the screen so we can put up Key Map infomation
  // ------------------------------------------------------
  unsigned short dmaVal =  *(bgGetMapPtr(bg0b) + 24*32);
  dmaFillWords(dmaVal | (dmaVal<<16),(void*) bgGetMapPtr(bg1b)+5*32*2,32*19*2);

  // --------------------------------------------------
  // Give instructions to the user...
  // --------------------------------------------------
  DSPrint(1 ,19,0,("   D-PAD : CHANGE KEY MAP    "));
  DSPrint(1 ,20,0,("       B : RETURN MAIN MENU  "));
  DSPrint(1 ,21,0,("       X : SWAP KEYMAP TYPE  "));
  DSPrint(1 ,22,0,("   START : SAVE KEYMAP       "));
  DisplayKeymapName(ucY);

  bIndTch = myConfig.keymap[0];

  // -----------------------------------------------------------------------
  // Clear out any keys that might be pressed on the way in - make sure
  // NDS keys are not being pressed. This prevents the inadvertant A key
  // that enters this menu from also being acted on in the keymap...
  // -----------------------------------------------------------------------
  while ((keysCurrent() & (KEY_TOUCH | KEY_B | KEY_A | KEY_X | KEY_UP | KEY_DOWN))!=0)
      ;
  WAITVBL;

  while (!bOK) {
    if (keysCurrent() & KEY_UP) {
      if (!ucHaut) {
        DisplayKeymapName(32);
        ucY = (ucY == 6 ? 17 : ucY -1);
        bIndTch = myConfig.keymap[ucY-6];
        ucHaut=0x01;
        DisplayKeymapName(ucY);
      }
      else {
        ucHaut++;
        if (ucHaut>10) ucHaut=0;
      }
    }
    else {
      ucHaut = 0;
    }
    if (keysCurrent() & KEY_DOWN) {
      if (!ucBas) {
        DisplayKeymapName(32);
        ucY = (ucY == 17 ? 6 : ucY +1);
        bIndTch = myConfig.keymap[ucY-6];
        ucBas=0x01;
        DisplayKeymapName(ucY);
      }
      else {
        ucBas++;
        if (ucBas>10) ucBas=0;
      }
    }
    else {
      ucBas = 0;
    }

    if (keysCurrent() & KEY_START)
    {
        SaveConfig(true); // Save options
    }

    if (keysCurrent() & KEY_B)
    {
      bOK = 1;  // Exit menu
    }

    if (keysCurrent() & KEY_LEFT)
    {
        if (ucL == 0) {
          bIndTch = (bIndTch == 0 ? (MAX_KEY_OPTIONS-1) : bIndTch-1);
          ucL=1;
          myConfig.keymap[ucY-6] = bIndTch;
          DisplayKeymapName(ucY);
        }
        else {
          ucL++;
          if (ucL > 7) ucL = 0;
        }
    }
    else
    {
        ucL = 0;
    }

    if (keysCurrent() & KEY_RIGHT)
    {
        if (ucR == 0)
        {
          bIndTch = (bIndTch == (MAX_KEY_OPTIONS-1) ? 0 : bIndTch+1);
          ucR=1;
          myConfig.keymap[ucY-6] = bIndTch;
          DisplayKeymapName(ucY);
        }
        else
        {
          ucR++;
          if (ucR > 7) ucR = 0;
        }
    }
    else
    {
        ucR=0;
    }

    // Swap Player 1 and Player 2 keymap
    if (keysCurrent() & KEY_X)
    {
        SwapKeymap();
        bIndTch = myConfig.keymap[ucY-6];
        DisplayKeymapName(ucY);
        while (keysCurrent() & KEY_X)
            ;
        WAITVBL
    }
    showRandomPreviewSnaps();
    swiWaitForVBlank();
  }
  while (keysCurrent() & KEY_B);
}


// -----------------------------------------------------------------------------------------
// At the bottom of the main screen we show the currently selected filename, size and CRC32
// -----------------------------------------------------------------------------------------
void DisplayFileName(void)
{
    sprintf(szName, "[%d K] [CRC: %08X]", file_size/1024, file_crc);
    DSPrint((16 - (strlen(szName)/2)),19,0,szName);

    sprintf(szName,"%s",gpFic[ucGameChoice].szName);
    for (u8 i=strlen(szName)-1; i>0; i--) if (szName[i] == '.') {szName[i]=0;break;}
    if (strlen(szName)>30) szName[30]='\0';
    DSPrint((16 - (strlen(szName)/2)),21,0,szName);
    if (strlen(gpFic[ucGameChoice].szName) >= 35)   // If there is more than a few characters left, show it on the 2nd line
    {
        sprintf(szName,"%s",gpFic[ucGameChoice].szName+30);
        for (u8 i=strlen(szName)-1; i>0; i--) if (szName[i] == '.') {szName[i]=0;break;}
        if (strlen(szName)>30) szName[30]='\0';
        DSPrint((16 - (strlen(szName)/2)),22,0,szName);
    }
}

//*****************************************************************************
// Display Hachibitto info screen and change options "main menu"
//*****************************************************************************
void dispInfoOptions(u32 uY)
{
    DSPrint(2, 7,(uY== 7 ? 2 : 0),("         LOAD  GAME         "));
    DSPrint(2, 9,(uY== 9 ? 2 : 0),("         PLAY  GAME         "));
    DSPrint(2,11,(uY==11 ? 2 : 0),("     REDEFINE  KEYS         "));
    DSPrint(2,13,(uY==13 ? 2 : 0),("         GAME  OPTIONS      "));
    DSPrint(2,15,(uY==15 ? 2 : 0),("       GLOBAL  OPTIONS      "));
    DSPrint(2,17,(uY==17 ? 2 : 0),("         QUIT  EMULATOR     "));
}

// --------------------------------------------------------------------
// Some main menu selections don't make sense without a game loaded.
// --------------------------------------------------------------------
void NoGameSelected(u32 ucY)
{
    unsigned short dmaVal = *(bgGetMapPtr(bg1b)+24*32);
    while (keysCurrent()  & (KEY_START | KEY_A));
    dmaFillWords(dmaVal | (dmaVal<<16),(void*) bgGetMapPtr(bg1b)+5*32*2,32*18*2);
    DSPrint(5,10,0,("   NO GAME SELECTED   "));
    DSPrint(5,12,0,("  PLEASE, USE OPTION  "));
    DSPrint(5,14,0,("      LOAD  GAME      "));
    WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;
    while (!(keysCurrent()  & (KEY_START | KEY_A)));
    while (keysCurrent()  & (KEY_START | KEY_A));
    dmaFillWords(dmaVal | (dmaVal<<16),(void*) bgGetMapPtr(bg1b)+5*32*2,32*18*2);
    dispInfoOptions(ucY);
}

/*********************************************************************************
 * Look for MSX 'AB' header in the ROM file or possibly 0xF331 for SVI ROMs
 ********************************************************************************/
void CheckRomHeaders(char *szGame)
{
    // ------------------------------------------------------------------------------------------
    // MSX Header Bytes:
    //  0 DEFB "AB" ; expansion ROM header
    //  2 DEFW initcode ; start of the init code, 0 if no initcode
    //  4 DEFW callstat; pointer to CALL statement handler, 0 if no such handler
    //  6 DEFW device; pointer to expansion device handler, 0 if no such handler
    //  8 DEFW basic ; pointer to the start of a tokenized basicprogram, 0 if no basicprogram
    // ------------------------------------------------------------------------------------------

    // ---------------------------------------------------------------------
    // Do some auto-detection for game ROM. MSX games have 'AB' in their
    // header and we also want to track the INIT address for those ROMs
    // so we can take a better guess at mapping them into our Slot1 memory
    // ---------------------------------------------------------------------
    msx_init = 0x4000;
    msx_basic = 0x0000;
    if ((ROM_Memory[0] == 'A') && (ROM_Memory[1] == 'B'))
    {
        msx_mode = 1;      // MSX roms start with AB (might be in bank 0)
        msx_init = ROM_Memory[2] | (ROM_Memory[3]<<8);
        if (msx_init == 0x0000) msx_basic = ROM_Memory[8] | (ROM_Memory[8]<<8);
        if (msx_init == 0x0000)   // If 0, check for 2nd header... this might be a dummy
        {
            if ((ROM_Memory[0x4000] == 'A') && (ROM_Memory[0x4001] == 'B'))
            {
                msx_init = ROM_Memory[0x4002] | (ROM_Memory[0x4003]<<8);
                if (msx_init == 0x0000) msx_basic = ROM_Memory[0x4008] | (ROM_Memory[0x4009]<<8);
            }
        }
    }
    else if ((ROM_Memory[0x4000] == 'A') && (ROM_Memory[0x4001] == 'B'))
    {
        msx_mode = 1;      // MSX roms start with AB (might be in bank 1)
        msx_init = ROM_Memory[0x4002] | (ROM_Memory[0x4003]<<8);
        if (msx_init == 0x0000) msx_basic = ROM_Memory[0x4008] | (ROM_Memory[0x4009]<<8);
    }
}


void ReadFileCRCAndConfig(void)
{
    u8 checkROM = 0;

    // Reset the mode related vars...
    msx_mode = 0;
    keyMapType = 0;

    // ----------------------------------------------------------------------------------
    // Clear the entire ROM buffer[] - fill with 0xFF to emulate non-responsive memory
    // ----------------------------------------------------------------------------------
    memset(ROM_Memory, 0xFF, (MAX_CART_SIZE * 1024));

    // Grab the all-important file CRC - this also loads the file into ROM_Memory[]
    getfile_crc(gpFic[ucGameChoice].szName);

    if (strstr(gpFic[ucGameChoice].szName, ".msx") != 0) msx_mode = 1;
    if (strstr(gpFic[ucGameChoice].szName, ".MSX") != 0) msx_mode = 1;
    if (strstr(gpFic[ucGameChoice].szName, ".dsk") != 0) msx_mode = 3;
    if (strstr(gpFic[ucGameChoice].szName, ".DSK") != 0) msx_mode = 3;
    if (strstr(gpFic[ucGameChoice].szName, ".rom") != 0) checkROM = 1;
    if (strstr(gpFic[ucGameChoice].szName, ".ROM") != 0) checkROM = 1;

    if (checkROM) CheckRomHeaders(gpFic[ucGameChoice].szName);   // See if we've got an MSX or SVI cart - this may set msx_mode=1 or svi_mode=2

    FindConfig();    // Try to find keymap and config for this file...
}


// ----------------------------------------------------------------------
// Read file twice and ensure we get the same CRC... if not, do it again
// until we get a clean read. Return the filesize to the caller...
// ----------------------------------------------------------------------
u32 ReadFileCarefully(char *filename, u8 *buf, u32 buf_size, u32 buf_offset)
{
    u32 crc1 = 0;
    u32 crc2 = 1;
    u32 fileSize = 0;

    // --------------------------------------------------------------------------------------------
    // I've seen some rare issues with reading files from the SD card on a DSi so we're doing
    // this slow and careful - we will read twice and ensure that we get the same CRC both times.
    // --------------------------------------------------------------------------------------------
    do
    {
        // Read #1
        crc1 = 0xFFFFFFFF;
        FILE* file = fopen(filename, "rb");
        if (file)
        {
            if (buf_offset) fseek(file, buf_offset, SEEK_SET);
            fileSize = fread(buf, 1, buf_size, file);
            crc1 = getCRC32(buf, buf_size);
            fclose(file);
        }

        // Read #2
        crc2 = 0xFFFFFFFF;
        FILE* file2 = fopen(filename, "rb");
        if (file2)
        {
            if (buf_offset) fseek(file2, buf_offset, SEEK_SET);
            fread(buf, 1, buf_size, file2);
            crc2 = getCRC32(buf, buf_size);
            fclose(file2);
        }

        // TODO add a trap for infinite loop - exit with error.
   } while (crc1 != crc2); // If the file couldn't be read, file_size will be 0 and the CRCs will both be 0xFFFFFFFF

   return fileSize;
}

// --------------------------------------------------------------------
// Let the user select new options for the currently loaded game...
// --------------------------------------------------------------------
void HachibittoChangeOptions(void)
{
  u16 ucHaut=0x00, ucBas=0x00,ucA=0x00,ucY= 7, bOK=0;

  // Upper Screen Background
  videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_SPR_1D_LAYOUT | DISPLAY_SPR_ACTIVE);
  vramSetBankA(VRAM_A_MAIN_BG);
  vramSetBankB(VRAM_B_MAIN_SPRITE_0x06400000);
  bg0 = bgInit(0, BgType_Text8bpp, BgSize_T_256x512, 31,0);
  bg1 = bgInit(1, BgType_Text8bpp, BgSize_T_256x512, 29,0);
  bgSetPriority(bg0,1);bgSetPriority(bg1,0);
  decompress(topscreenTiles, bgGetGfxPtr(bg0), LZ77Vram);
  decompress(topscreenMap, (void*) bgGetMapPtr(bg0), LZ77Vram);
  dmaCopy((void*) topscreenPal,(void*) BG_PALETTE,256*2);
  unsigned short dmaVal =  *(bgGetMapPtr(bg0) + 51*32);
  dmaFillWords(dmaVal | (dmaVal<<16),(void*) bgGetMapPtr(bg1),32*24*2);

  // Lower Screen Background
  BottomScreenOptions();

  dispInfoOptions(ucY);

  if (ucGameChoice != -1)
  {
      DisplayFileName();
  }

  while (!bOK) {
    if (keysCurrent()  & KEY_UP) {
      if (!ucHaut) {
        dispInfoOptions(32);
        ucY = (ucY == 7 ? 17 : ucY -2);
        ucHaut=0x01;
        dispInfoOptions(ucY);
      }
      else {
        ucHaut++;
        if (ucHaut>10) ucHaut=0;
      }
    }
    else {
      ucHaut = 0;
    }
    if (keysCurrent()  & KEY_DOWN) {
      if (!ucBas) {
        dispInfoOptions(32);
        ucY = (ucY == 17 ? 7 : ucY +2);
        ucBas=0x01;
        dispInfoOptions(ucY);
      }
      else {
        ucBas++;
        if (ucBas>10) ucBas=0;
      }
    }
    else {
      ucBas = 0;
    }
    if (keysCurrent()  & KEY_A) {
      if (!ucA) {
        ucA = 0x01;
        switch (ucY) {
          case 7 :      // LOAD GAME
            HachibittoLoadFile();
            dmaFillWords(dmaVal | (dmaVal<<16),(void*) bgGetMapPtr(bg1b)+5*32*2,32*19*2);
            if (ucGameChoice != -1)
            {
                ReadFileCRCAndConfig(); // Get CRC32 of the file and read the config/keys
                DisplayFileName();      // And put up the filename on the bottom screen
            }
            ucY = 9;
            dispInfoOptions(ucY);
            break;
          case 9 :     // PLAY GAME
            if (ucGameChoice != -1)
            {
                bOK = 1;
            }
            else
            {
                NoGameSelected(ucY);
            }
            break;
          case 11 :     // REDEFINE KEYS
            if (ucGameChoice != -1)
            {
                HachibittoChangeKeymap();
                dmaFillWords(dmaVal | (dmaVal<<16),(void*) bgGetMapPtr(bg1b)+5*32*2,32*18*2);
                dispInfoOptions(ucY);
                DisplayFileName();
            }
            else
            {
                NoGameSelected(ucY);
            }
            break;
          case 13 :     // GAME OPTIONS
            if (ucGameChoice != -1)
            {
                HachibittoGameOptions(false);
                dmaFillWords(dmaVal | (dmaVal<<16),(void*) bgGetMapPtr(bg1b)+5*32*2,32*18*2);
                dispInfoOptions(ucY);
                DisplayFileName();
            }
            else
            {
               NoGameSelected(ucY);
            }
            break;

          case 15 :     // GLOBAL OPTIONS
            HachibittoGameOptions(true);
            dmaFillWords(dmaVal | (dmaVal<<16),(void*) bgGetMapPtr(bg1b)+5*32*2,32*18*2);
            dispInfoOptions(ucY);
            DisplayFileName();
            break;

          case 17 :     // QUIT EMULATOR
            exit(1);
            break;
        }
      }
    }
    else
      ucA = 0x00;
    if (keysCurrent()  & KEY_START) {
      if (ucGameChoice != -1)
      {
        bOK = 1;
      }
      else
      {
        NoGameSelected(ucY);
      }
    }
    showRandomPreviewSnaps();
    swiWaitForVBlank();
  }
  while (keysCurrent()  & (KEY_START | KEY_A));
}

//*****************************************************************************
// Displays a message on the screen
//*****************************************************************************
void DSPrint(int iX,int iY,int iScr,char *szMessage)
{
  u16 *pusScreen,*pusMap;
  u16 usCharac;
  char *pTrTxt=szMessage;

  pusScreen=(u16*) (iScr != 1 ? bgGetMapPtr(bg1b) : bgGetMapPtr(bg1))+iX+(iY<<5);
  pusMap=(u16*) (iScr != 1 ? (iScr == 6 ? bgGetMapPtr(bg0b)+24*32 : (iScr == 0 ? bgGetMapPtr(bg0b)+24*32 : bgGetMapPtr(bg0b)+26*32 )) : bgGetMapPtr(bg0)+51*32 );

  while((*pTrTxt)!='\0' )
  {
    char ch = *pTrTxt++;
    if (ch >= 'a' && ch <= 'z') ch -= 32;   // Faster than strcpy/strtoupper

    if (((ch)<' ') || ((ch)>'_'))
      usCharac=*(pusMap);                   // Will render as a vertical bar
    else if((ch)<'@')
      usCharac=*(pusMap+(ch)-' ');          // Number from 0-9 or punctuation
    else
      usCharac=*(pusMap+32+(ch)-'@');       // Character from A-Z
    *pusScreen++=usCharac;
  }
}

/******************************************************************************
* Routine FadeToColor :  Fade from background to black or white
******************************************************************************/
void FadeToColor(unsigned char ucSens, unsigned short ucBG, unsigned char ucScr, unsigned char valEnd, unsigned char uWait) {
  unsigned short ucFade;
  unsigned char ucBcl;

  // Fade-out to black
  if (ucScr & 0x01) REG_BLDCNT=ucBG;
  if (ucScr & 0x02) REG_BLDCNT_SUB=ucBG;
  if (ucSens == 1) {
    for(ucFade=0;ucFade<valEnd;ucFade++) {
      if (ucScr & 0x01) REG_BLDY=ucFade;
      if (ucScr & 0x02) REG_BLDY_SUB=ucFade;
      for (ucBcl=0;ucBcl<uWait;ucBcl++) {
        swiWaitForVBlank();
      }
    }
  }
  else {
    for(ucFade=16;ucFade>valEnd;ucFade--) {
      if (ucScr & 0x01) REG_BLDY=ucFade;
      if (ucScr & 0x02) REG_BLDY_SUB=ucFade;
      for (ucBcl=0;ucBcl<uWait;ucBcl++) {
        swiWaitForVBlank();
      }
    }
  }
}

void _putchar(char character) {}

// End of file
