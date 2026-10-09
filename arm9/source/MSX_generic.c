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

#include <stdlib.h>
#include <stdio.h>
#include <fat.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>

#include "Hachibitto.h"
#include "MSX_generic.h"
#include "options.h"
#include "topscreen.h"
#include "lzav.h"
#include "CRC32.h"
#include "printf.h"

int countMSX     =  0;
int ucGameAct    =  0;
int ucGameChoice = -1;

FI_MSX gpFic[MAX_FILES];
char szName[256];
char szFile[256];
char strBuf[40];

struct Config_t         AllConfigs[MAX_CONFIGS];
struct Config_t         myConfig __attribute((aligned(4))) __attribute__((section(".dtcm")));
struct GlobalConfig_t   myGlobalConfig;

typedef struct
{
  u32   name_hash;  // Repurpose the lower bit for love vs like
} Favorites_t;

#define MAX_FAVS  1024

Favorites_t myFavs[MAX_FAVS]; // Total of 4K of space with 32 bit hash

u8 option_table_idx = 0;
u8 last_media_id    = 99;

const char szKeyName[MAX_KEY_OPTIONS][18] = {
  "P1 JOY UP",
  "P1 JOY DOWN",
  "P1 JOY LEFT",
  "P1 JOY RIGHT",
  "P1 BUTTON 1",
  "P1 BUTTON 2", // 5

  "P2 JOY UP",
  "P2 JOY DOWN",
  "P2 JOY LEFT",
  "P2 JOY RIGHT",
  "P2 BUTTON 1", //10
  "P2 BUTTON 2",

  "KEYBOARD A", //12
  "KEYBOARD B",
  "KEYBOARD C",
  "KEYBOARD D", //15
  "KEYBOARD E",
  "KEYBOARD F",
  "KEYBOARD G",
  "KEYBOARD H",
  "KEYBOARD I", //20
  "KEYBOARD J",
  "KEYBOARD K",
  "KEYBOARD L",
  "KEYBOARD M",
  "KEYBOARD N", //25
  "KEYBOARD O",
  "KEYBOARD P",
  "KEYBOARD Q",
  "KEYBOARD R",
  "KEYBOARD S", //30
  "KEYBOARD T",
  "KEYBOARD U",
  "KEYBOARD V",
  "KEYBOARD W",
  "KEYBOARD X", //35
  "KEYBOARD Y",
  "KEYBOARD Z",

  "KEYBOARD 0", //38
  "KEYBOARD 1",
  "KEYBOARD 2", //40
  "KEYBOARD 3",
  "KEYBOARD 4",
  "KEYBOARD 5",
  "KEYBOARD 6",
  "KEYBOARD 7", //45
  "KEYBOARD 8",
  "KEYBOARD 9", //47

  "KEYBOARD SHIFT", //48
  "KEYBOARD CTRL",
  "KEYBOARD CODE",  //50
  "KEYBOARD GRAPH",

  "KEYBOARD SPACE", //52
  "KEYBOARD RETURN",
  "KEYBOARD ESC",

  "KEYBOARD HOME", //55
  "KEYBOARD UP",
  "KEYBOARD DOWN",
  "KEYBOARD LEFT",
  "KEYBOARD RIGHT",

  "KEYBOARD PERIOD", //60
  "KEYBOARD COMMA",
  "KEYBOARD COLON",
  "KEYBOARD SEMI",
  "KEYBOARD QUOTE",
  "KEYBOARD SLASH", //65
  "KEYBOARD CARET",
  "KEYBOARD MINUS",
  "KEYBOARD LBRACKET",
  "KEYBOARD RBRACKET",
  "KEYBOARD ATSIGN", //70
  "KEYBOARD YEN",
  "KEYBOARD BS",
  "KEYBOARD TAB",
  "KEYBOARD INS",
  "KEYBOARD DEL", // 75
  "KEYBOARD STOP",
  "KEYBOARD F1",
  "KEYBOARD F2",
  "KEYBOARD F3",
  "KEYBOARD F4", //80
  "KEYBOARD F5", //81

  "PAN UP",      //82
  "PAN DOWN",    //83
  "SHOW TOP",    //84
  "SHOW BOTTOM", //85
};


/*********************************************************************************
 * Show A message with YES / NO
 ********************************************************************************/
u8 showMessage(char *szCh1, char *szCh2)
{
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

// ----------------------------------------------------------------------------
// This stuff handles the 'random' screen snapshot at the top screen...
// ----------------------------------------------------------------------------
void ShowRandomPreviewSnaps(void)
{
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

// --------------------------------------------------------------
// Provide an array of filename hashes to store game "Favorites"
// --------------------------------------------------------------
void LoadFavorites(void)
{
    memset(myFavs, 0x00, sizeof(myFavs));
    FILE *fp = fopen("/data/Hachibitto.fav", "rb");
    if (fp)
    {
        fread(&myFavs, sizeof(myFavs), 1, fp);
        fclose(fp);
    }
}

void SaveFavorites(void)
{
    // --------------------------------------------------
    // Now save the config file out o the SD card...
    // --------------------------------------------------
    DIR* dir = opendir("/data");
    if (dir)
    {
        closedir(dir);  // directory exists.
    }
    else
    {
        mkdir("/data", 0777);   // Doesn't exist - make it...
    }

    FILE *fp = fopen("/data/Hachibitto.fav", "wb");
    if (fp)
    {
        fwrite(&myFavs, sizeof(myFavs), 1, fp);
        fclose(fp);
    }
}

u8 IsFavorite(char *name)
{
    u32 filename_crc32 = getCRC32((u8 *)name, strlen(name));

    for (int i=0; i<MAX_FAVS; i++)
    {
        if ((myFavs[i].name_hash & 0xFFFFFFFE) == (filename_crc32 & 0xFFFFFFFE)) return (1 + (myFavs[i].name_hash&1));
    }
    return 0;
}

void ToggleFavorite(char *name)
{
    int firstZero = 0;
    u32 filename_crc32 = getCRC32((u8 *)name, strlen(name));

    for (int i=0; i<MAX_FAVS; i++)
    {
        // We use the lower bit of the filename hash (CRC32) as the flag for 'like' vs 'love'
        // Basically there are 3 states:
        //    - No hash found... not a favorite
        //    - Hash found with lower bit 0... Love
        //    - Hash found with lower bit 1... Like
        if ((myFavs[i].name_hash & 0xFFFFFFFE) == (filename_crc32 & 0xFFFFFFFE))
        {
            if ((myFavs[i].name_hash & 1) == 0)
            {
                myFavs[i].name_hash |= 1;
                return;
            }
            else
            {
                myFavs[i].name_hash = 0x00000000;
                return;
            }
        }

        if (myFavs[i].name_hash == 0x00000000)
        {
            if (!firstZero) firstZero = i;
        }
    }

    myFavs[firstZero].name_hash = (filename_crc32 & 0xFFFFFFFE);
}

/*********************************************************************************
 * Show The 16 games on the list to allow the user to choose a new game.
 ********************************************************************************/
static char szName2[40];
void dsDisplayFiles(u16 NoDebGame, u8 ucSel)
{
    u16 ucBcl,ucGame;
    u8 maxLen;

    DSPrint(30,5,0,(NoDebGame>0 ? "<" : " "));
    DSPrint(30,22,0,(NoDebGame+16<countMSX ? ">" : " "));
    sprintf(szName,"%03d/%03d FILES AVAILABLE     ",ucSel+1+NoDebGame,countMSX);
    DSPrint(4,4,0, szName);

    for (ucBcl=0;ucBcl<16; ucBcl++)
    {
      ucGame= ucBcl+NoDebGame;
      if (ucGame < countMSX)
      {
        maxLen=strlen(gpFic[ucGame].szName);
        strcpy(szName,gpFic[ucGame].szName);
        if (maxLen>28) szName[30]='\0';
        if (gpFic[ucGame].uType == DIRECTORY)
        {
          szName[26] = 0; // Needs to be 2 chars shorter with brackets
          sprintf(szName2, "[%s]",szName);
          sprintf(szName,"%-30s",szName2);
          DSPrint(1,6+ucBcl,(ucSel == ucBcl ? 2 :  0),szName);
          DSPrint(0,6+ucBcl,0,(char*)" ");
        }
        else
        {
          sprintf(szName,"%-30s",strupr(szName));
          DSPrint(1,6+ucBcl,(ucSel == ucBcl ? 2 : 0 ),szName);

          if (IsFavorite(gpFic[ucGame].szName))
          {
              DSPrint(0,6+ucBcl,(IsFavorite(gpFic[ucGame].szName) == 1) ? 0:2,(char*)"@");
          }
          else
          {
              DSPrint(0,6+ucBcl,0,(char*)" ");
          }
        }
      }
      else
      {
          DSPrint(0,6+ucBcl,(ucSel == ucBcl ? 2 : 0 ),"                                ");
      }
    }
}


// -------------------------------------------------------------------------
// Standard qsort routine for the MSX game list - we sort all directory
// listings first and then a case-insensitive sort of all games.
// -------------------------------------------------------------------------
int msxFilescmp (const void *c1, const void *c2)
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
void HachibittoFindFiles(u8 media_id)
{
    u32 uNbFile;
    DIR *dir;
    struct dirent *pent;

    uNbFile=0;
    countMSX=0;

    dir = opendir(".");
    while (((pent=readdir(dir))!=NULL) && (uNbFile<MAX_FILES))
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
              countMSX++;
          }
        }
      }
      else {
        if ((strlen(szFile)>4) && (strlen(szFile)<(MAX_FILE_NAME_LEN-4)) && (szFile[0] != '.') && (szFile[0] != '_'))  // For MAC don't allow underscore files
        {
          if ( (strcasecmp(strrchr(szFile, '.'), ".rom") == 0) && (media_id != MEDIA_DISK) )  {
            strcpy(gpFic[uNbFile].szName,szFile);
            gpFic[uNbFile].uType = MSXROM;
            uNbFile++;
            countMSX++;
          }
          if ( (strcasecmp(strrchr(szFile, '.'), ".bin") == 0) && (media_id != MEDIA_DISK) )  {
            strcpy(gpFic[uNbFile].szName,szFile);
            gpFic[uNbFile].uType = MSXROM;
            uNbFile++;
            countMSX++;
          }
          if ( (strcasecmp(strrchr(szFile, '.'), ".dsk") == 0) && (media_id == MEDIA_DISK) )  {
            strcpy(gpFic[uNbFile].szName,szFile);
            gpFic[uNbFile].uType = MSXROM;
            uNbFile++;
            countMSX++;
          }
        }
      }
    }
    closedir(dir);

    // ----------------------------------------------
    // If we found any files, go sort the list...
    // ----------------------------------------------
    if (countMSX)
    {
        qsort (gpFic, countMSX, sizeof(FI_MSX), msxFilescmp);
    }
}


// ----------------------------------------------------------------
// Let the user select a new game (rom) file and load it up!
// ----------------------------------------------------------------
u8 HachibittoChooseFile(u8 media_id, u8 allow_dir_change)
{
    bool bDone=false;
    u16 ucHaut=0x00, ucBas=0x00,ucSHaut=0x00, ucSBas=0x00, romSelected= 0, firstRomDisplay=0,nbRomPerPage, uNbRSPage;
    s16 uLenFic=0, ucFlip=0, ucFlop=0;

    // Show the menu...
    while ((keysCurrent() & (KEY_TOUCH | KEY_START | KEY_SELECT | KEY_A | KEY_B))!=0);
    unsigned short dmaVal =  *(bgGetMapPtr(bg0b) + 24*32);
    dmaFillWords(dmaVal | (dmaVal<<16),(void*) bgGetMapPtr(bg1b)+5*32*2,32*19*2);

    DSPrint(3,23,0,"A=LOAD, SELECT=FAV, B=EXIT");

    chdir(MyMedia[media_id].filepath);  // Get into the right directory
    HachibittoFindFiles(media_id);      // And get all files of the appropriate type (Cart vs Disk)

    // If we are selecting a different media type (CART vs DISK), start at the top
    if (media_id != last_media_id)
    {
        ucGameAct = 0;
        last_media_id = media_id;
    }

    ucGameChoice = -1;

    nbRomPerPage = (countMSX>=16 ? 16 : countMSX);
    uNbRSPage = (countMSX>=5 ? 5 : countMSX);

    if (ucGameAct>countMSX-nbRomPerPage)
    {
      firstRomDisplay=countMSX-nbRomPerPage;
      romSelected=ucGameAct-countMSX+nbRomPerPage;
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
          ucGameAct = (ucGameAct>0 ? ucGameAct-1 : countMSX-1);
          if (romSelected>uNbRSPage) { romSelected -= 1; }
          else {
            if (firstRomDisplay>0) { firstRomDisplay -= 1; }
            else {
              if (romSelected>0) { romSelected -= 1; }
              else {
                firstRomDisplay=countMSX-nbRomPerPage;
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
          ucGameAct = (ucGameAct< countMSX-1 ? ucGameAct+1 : 0);
          if (romSelected<uNbRSPage-1) { romSelected += 1; }
          else {
            if (firstRomDisplay<countMSX-nbRomPerPage) { firstRomDisplay += 1; }
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
          ucGameAct = (ucGameAct< countMSX-nbRomPerPage ? ucGameAct+nbRomPerPage : countMSX-nbRomPerPage);
          if (firstRomDisplay<countMSX-nbRomPerPage) { firstRomDisplay += nbRomPerPage; }
          else { firstRomDisplay = countMSX-nbRomPerPage; }
          if (ucGameAct == countMSX-nbRomPerPage) romSelected = 0;
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

      // The SELECT key will toggle favorites
      if (keysCurrent() & KEY_SELECT)
      {
          if (gpFic[ucGameAct].uType != DIRECTORY)
          {
              ToggleFavorite(gpFic[ucGameAct].szName);
              dsDisplayFiles(firstRomDisplay,romSelected);
              SaveFavorites();
              while (keysCurrent() & KEY_SELECT)
              {
                  WAITVBL;
              }
          }
      }

      // -------------------------------------------------------------------------
      // The B key will exit out of the ROM selection without picking a new game
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
          WAITVBL;
        }
        else
        {
            if (allow_dir_change)
            {
              chdir(gpFic[ucGameAct].szName);
              HachibittoFindFiles(media_id);
              ucGameAct = 0;
              nbRomPerPage = (countMSX>=16 ? 16 : countMSX);
              uNbRSPage = (countMSX>=5 ? 5 : countMSX);
              if (ucGameAct>countMSX-nbRomPerPage) {
                firstRomDisplay=countMSX-nbRomPerPage;
                romSelected=ucGameAct-countMSX+nbRomPerPage;
              }
              else {
                firstRomDisplay=ucGameAct;
                romSelected=0;
              }
              dsDisplayFiles(firstRomDisplay,romSelected);
          }
          else
          {
              DSPrint(5,22,0,"NO DIR CHANGE FOR SWAP");
              WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;
              DSPrint(5,22,0,"                      ");
          }
          while (keysCurrent() & KEY_A);
        }
      }

      // --------------------------------------------
      // If the filename is too long... scroll it.
      // --------------------------------------------
      if (strlen(gpFic[ucGameAct].szName) > 31)
      {
        ucFlip++;
        if (ucFlip >= 10)
        {
          ucFlip = 0;
          uLenFic++;
          if ((uLenFic+30)>strlen(gpFic[ucGameAct].szName))
          {
            ucFlop++;
            if (ucFlop >= 15)
            {
              uLenFic = 0;
              ucFlop  = 0;
              ucFlip  = -35;
            }
            else
            {
              uLenFic--;
            }
          }
          strncpy(szName,gpFic[ucGameAct].szName+uLenFic,30);
          szName[30] = '\0';
          DSPrint(1,6+romSelected,2,szName);
        }
      }
      ShowRandomPreviewSnaps();
      swiWaitForVBlank();
    }

    // Wait for key to be released before returning
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

    preserveCompressedMem();

    // Set the global configuration version number...
    myGlobalConfig.config_ver = CONFIG_VER;

    // If there is a game loaded, save that into a slot... re-use the same slot if it exists
    myConfig.game_crc = GetMasterCRC();

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
    // Now save the config file out to the SD card...
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


#define TWEAK_NONE 0xFF

typedef struct
{
    const char *match1;
    const char *match2;

    u8 expansion;
    u8 frameSkip;
    u8 maxSprites;
    u8 machineType;
    u8 dpad;

    u8 dsiOnly;
} GameTweak;


// -------------------------------------------------------------------------------------------------------------
// Game tweaks for some games based on filenames loaded.
// e.g. Snatcher gets SCC+, Q-Bert gets diagonals, etc.
// -------------------------------------------------------------------------------------------------------------
static const GameTweak gameTweaks[] =
{
    // match1,           match2,              expansion,       frameSkip,   maxSprites,  machineType,   dpad,             dsiOnly

    // Sprite limit
    { "QBIQS",           NULL,                TWEAK_NONE,      TWEAK_NONE,  0,           TWEAK_NONE,    TWEAK_NONE,         0 },
    { "URIDIUM",         NULL,                TWEAK_NONE,      TWEAK_NONE,  0,           TWEAK_NONE,    TWEAK_NONE,         0 },
    { "ANTARCTIC",       "ADVENTURE",         TWEAK_NONE,      TWEAK_NONE,  0,           TWEAK_NONE,    TWEAK_NONE,         0 },
    { "ADVENTURES",      "PARK",              TWEAK_NONE,      TWEAK_NONE,  0,           TWEAK_NONE,    TWEAK_NONE,         0 },
                                                                                                                            
    // SCC+                                                                                                                 
    { "SNATCHER",        NULL,                MUSIC_SCC,    FS_AGGRESSIVE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
                                                                                                                            
    // Aggressive frame skip on DS-Lite                                                                                     
    { "MANBOW",          NULL,                TWEAK_NONE,   FS_AGGRESSIVE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
                                                                                                                            
    // MSX-MUSIC - works on DS-Lite                                                                                         
    { "LUBECK",          NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
    { "XAK",             NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
    { "FAMICLE",         NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
    { "FRAY",            NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
    { "SINGULAR",        "STONE",             MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
                                                                                                                            
    // MSX-MUSIC + aggressive frame skip on DS-Lite                                                                         
    { "LILLY",           "SAGA",              MUSIC_MSX,    FS_AGGRESSIVE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
                                                                                                                            
    // MSX-MUSIC - DSi only                                                                                                 
    { "ALESTE",          NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "MONOGATARI",      NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "SLAYER",          "VI",                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "GOLVELLIUS 2",    NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "GOLVELLIUS II",   NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "ILLUSION",        "CITY",              MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "SUPER",           "COOKS",             MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "PSYCH",           "WORLD",             MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "MAD",             "HOUSE",             MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "DAIKOUKAI",       "JIDAI",             MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "DEAD",            "BRAIN",             MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "EUROPE",          "WAR",               MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "FAMILY",          "STADIUM",           MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "FLEET",           "COMMANDER 2",       MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "GAMBLER",         "JIKICHUSHINPA",     MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "MAISON",          "IKKOKU",            MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "EMERALD",         "DRAGON",            MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "PENGUIN",         "WARS",              MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "BLASTER",         "BURN",              MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "LAYDOCK",         "LAST",              MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "BUSHOUHUUNROKU",  NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "SENKOKUGUNYUDEN", NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "FARDRAUT",        NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "SUIKODEN",        NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "PAC-MANIA",       NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "GENCHOHISI",      NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "QUINPL",          NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "UNDEADLINE",      NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "CRIMSON",         NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "FEEDBACK",        NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "MASTER 3",        NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
    { "MASTER III",      NULL,                MUSIC_MSX,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         1 },
                                                                                                                            
    // PSG                                                                                                                  
    { "BLADE",           "LORDS",             MUSIC_PSG,       TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
                                                                                                                            
    // D-Pad                                                                                                                
    { "ARKANOID",        NULL,                TWEAK_NONE,      TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    DPAD_ARKANOID,      0 },
    { "CHUCKIE",         NULL,                TWEAK_NONE,      TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    DPAD_SLIDE_N_GLIDE, 0 },
    { "QBERT",           NULL,                TWEAK_NONE,      TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    DPAD_DIAGONALS,     0 },
    { "Q-BERT",          NULL,                TWEAK_NONE,      TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    DPAD_DIAGONALS,     0 },
                                                                                                                            
    // Machine type                                                                                                         
    { "KING",            "BALLOON",           TWEAK_NONE,      TWEAK_NONE,  TWEAK_NONE,  MACHINE_MSX1,  TWEAK_NONE,         0 },
                                                                                                                            
    // Beeper                                                                                                               
    { "WAY",             "TIGER",             MUSIC_BEEPER,    TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
    { "JACK",            "NIPPER",            MUSIC_BEEPER,    TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
    { "MASTER",          "LAMPS",             MUSIC_BEEPER,    TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
    { "FUTURE",          "KNIGHT",            MUSIC_BEEPER,    TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
    { "AVENGER",         NULL,                MUSIC_BEEPER,    TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
    { "BOUNDER",         NULL,                MUSIC_BEEPER,    TWEAK_NONE,  TWEAK_NONE,  TWEAK_NONE,    TWEAK_NONE,         0 },
};


void ApplyDatabaseTweaks(void)
{
    const char *filename = GetMasterFilename();

    for (unsigned int i = 0; i < sizeof(gameTweaks) / sizeof(gameTweaks[0]); i++)
    {
        const GameTweak *t = &gameTweaks[i];

        if (!strstr(filename, t->match1))
            continue;

        if (t->match2 && !strstr(filename, t->match2))
            continue;

        // DSi-only expansion
        if (t->dsiOnly && !isDSiMode())
            continue;

        if (t->expansion != TWEAK_NONE)
            myConfig.expansion = t->expansion;

        // Frame skip is ALWAYS DS-Lite only.
        if (t->frameSkip != TWEAK_NONE && !isDSiMode())
            myConfig.frameSkip = t->frameSkip;

        if (t->maxSprites != TWEAK_NONE)
            myConfig.maxSprites = t->maxSprites;

        if (t->machineType != TWEAK_NONE)
            myConfig.machineType = t->machineType;

        if (t->dpad != TWEAK_NONE)
            myConfig.dpad = t->dpad;
    }
}

void MapPlayer1(void)
{
    myConfig.keymap[0]   = 0;    // NDS D-Pad mapped to MSX Joystick UP
    myConfig.keymap[1]   = 1;    // NDS D-Pad mapped to MSX Joystick DOWN
    myConfig.keymap[2]   = 2;    // NDS D-Pad mapped to MSX Joystick LEFT
    myConfig.keymap[3]   = 3;    // NDS D-Pad mapped to MSX Joystick RIGHT
    myConfig.keymap[4]   = 4;    // NDS A Button mapped to MSX Button 1
    myConfig.keymap[5]   = 5;    // NDS B Button mapped to MSX Button 2
    myConfig.keymap[6]   = 82;   // NDS X Button mapped to PAN UP
    myConfig.keymap[7]   = 83;   // NDS Y Button mapped to PAN DN
    myConfig.keymap[8]   = 49;   // NDS R      mapped to CTRL
    myConfig.keymap[9]   = 48;   // NDS L      mapped to SHIFT
    myConfig.keymap[10]  = 53;   // NDS Start  mapped to RETURN
    myConfig.keymap[11]  = 52;   // NDS Select mapped to SPACE
}

void MapPlayer2(void)
{
    myConfig.keymap[0]   = 6;    // NDS D-Pad mapped to MSX Joystick UP
    myConfig.keymap[1]   = 7;    // NDS D-Pad mapped to MSX Joystick DOWN
    myConfig.keymap[2]   = 8;    // NDS D-Pad mapped to MSX Joystick LEFT
    myConfig.keymap[3]   = 9;    // NDS D-Pad mapped to MSX Joystick RIGHT
    myConfig.keymap[4]   = 10;   // NDS A Button mapped to MSX Button 1
    myConfig.keymap[5]   = 11;   // NDS B Button mapped to MSX Button 2
    myConfig.keymap[6]   = 82;   // NDS X Button mapped to PAN UP
    myConfig.keymap[7]   = 83;   // NDS Y Button mapped to PAN DN
    myConfig.keymap[8]   = 49;   // NDS R      mapped to CTRL
    myConfig.keymap[9]   = 48;   // NDS L      mapped to SHIFT
    myConfig.keymap[10]  = 53;   // NDS Start  mapped to RETURN
    myConfig.keymap[11]  = 52;   // NDS Select mapped to SPACE
}

void MapCursors(void)
{
    myConfig.keymap[0]   = 56;   // UP Arrow
    myConfig.keymap[1]   = 57;   // Down Arrow
    myConfig.keymap[2]   = 58;   // Left Arrow
    myConfig.keymap[3]   = 59;   // Right Arrow
    myConfig.keymap[4]   = 52;   // Space
    myConfig.keymap[5]   = 56;   // Up Arrow
    myConfig.keymap[6]   = 82;   // NDS X Button mapped to PAN UP
    myConfig.keymap[7]   = 83;   // NDS Y Button mapped to PAN DN
    myConfig.keymap[8]   = 49;   // NDS R      mapped to CTRL
    myConfig.keymap[9]   = 48;   // NDS L      mapped to SHIFT
    myConfig.keymap[10]  = 53;   // NDS Start  mapped to RETURN
    myConfig.keymap[10]  = 53;   // NDS Select mapped to RETURN
}


void SetDefaultGlobalConfig(void)
{
    // A few global defaults...
    memset(&myGlobalConfig, 0x00, sizeof(myGlobalConfig));
    myGlobalConfig.showFPS = 0;             // Don't show FPS counter by default
    myGlobalConfig.debugger = 0;            // No debugger by default.
    myGlobalConfig.bShowInstructions = 1;   // Always show instructions until config saved
    myGlobalConfig.bDiskSounds = 1;         // Default is to have disk sounds
}

void SetDefaultGameConfig(void)
{
    myConfig.game_crc    = 0;    // No game in this slot yet

    MapPlayer1();                // Default to Player 1 mapping

    myConfig.msxMapper    = GUESS;                       // MSX mapper takes its best guess
    myConfig.machineType  = MACHINE_MSX2;                // Default machine is MSX2 with Slot 3 Expanded
    myConfig.autoFire     = 0;                           // Default to no auto-fire on either button
    myConfig.keyboard     = OVL_FULLKBD;                 // Default to normal full MSX keyboard
    myConfig.maxSprites   = 1;                           // 0 means limit to the original 4/8 sprites of the VDP, 1 means 32 sprites for emulation
    myConfig.dpad         = DPAD_NORMAL;                 // Normal DPAD use - mapped to joystick
    myConfig.yOffset      = 0;                           // Default is no Y offset
    myConfig.expansion  = MUSIC_PSG;                   // Default is no expansion (normal PSG sound)
    myConfig.cpuBoost     = 0;                           // Run CPU at true speed (1=boost 10%)
    myConfig.splitRefresh = 2;                           // 0=Strict, 1=Refresh a line, 2= Refresh two lines
    myConfig.scaleScreen  = 0;                           // 0=No Screen Scale. 1=Vertical Compression (yuck!)
    myConfig.maskBorders  = 0;                           // No border masking by default
    myConfig.frameSkip    = (isDSiMode() ? 0:1);         // Frame Skip is disabled on DSi and above
    myConfig.reserved1    = 0;
    myConfig.reserved2    = 0;
    myConfig.reserved3    = 0;
    myConfig.reserved4    = 0;
    myConfig.reserved5    = 0;
    myConfig.reserved6    = 0;
    myConfig.reserved7    = 0xA5;    // So it's easy to spot on an "upgrade" and we can re-default it

    // For smaller games, even on the DS-Lite we can generally get away with no frameskip
    if (!isDSiMode())
    {
        // If there is no disk... and the Cart is small, we can avoid frameskip even on the older DS-Lite/Phat
        if (MyMedia[MEDIA_DISK].filecrc == 0)
        {
            if (MyMedia[MEDIA_CART1].filesize <= (48*1024)) myConfig.frameSkip = 0;
        }
    }

    ApplyDatabaseTweaks(); // Some games need tweaks to default settings... do this now
}

// ----------------------------------------------------------
// Load configuration into memory where we can use it.
// The configuration is stored in Hachibitto.dat
// ----------------------------------------------------------
void LoadConfigDatabase(void)
{
    u8 bInitDatabase = 0;

    preserveCompressedMem();

    // -----------------------------------------------------------------
    // Start with defaults.. if we find a match in our config database
    // below, we will fill in the config with data read from the file.
    // -----------------------------------------------------------------
    SetDefaultGameConfig();

    if (ReadFileCarefully("/data/Hachibitto.dat", (u8*)&myGlobalConfig, sizeof(myGlobalConfig), 0, NULL))  // Read Global Config
    {
        int comp_len = 0;
        ReadFileCarefully("/data/Hachibitto.dat", (u8*)&comp_len, sizeof(comp_len), sizeof(myGlobalConfig), NULL); // Read the full game array of configs (compressed length)
        ReadFileCarefully("/data/Hachibitto.dat", (u8*)COMPRESS_BUFFER, comp_len, sizeof(myGlobalConfig) + sizeof(comp_len), NULL); // Read the full game array of configs (actual data)
        (void)lzav_decompress( COMPRESS_BUFFER, AllConfigs, comp_len, sizeof(AllConfigs) );

        // If our config version changed... we init the entire database
        if (myGlobalConfig.config_ver != CONFIG_VER)
        {
            bInitDatabase = 1;
        }
    }
    else    // Not found... init the entire database...
    {
        bInitDatabase = 1;
    }

    if (bInitDatabase)
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
        if (AllConfigs[slot].game_crc == GetMasterCRC())  // Got a match?!
        {
            memcpy(&myConfig, &AllConfigs[slot], sizeof(struct Config_t));
            break;
        }
    }
}


// ------------------------------------------------------------------------------
// Options are handled here... we have a number of things the user can tweak
// and these options are applied immediately. The user can also save off
// their option choices for the currently running game into the Hachibitto.DAT
// configuration database. When games are loaded back up, Hachibitto.DAT is read
// to see if we have a match and the user settings can be restored for the game.
// ------------------------------------------------------------------------------
struct options_t
{
    const char  *label;
    const char  *option[32];
    u8          *option_val;
    u8           option_max;
};

const struct options_t Option_Table[1][20] =
{
    // Page 1
    {
        {"MSX MAPPER",     {"GUESS","MIRRORED", "KONAMI 8K","ASCII 8K","KONAMI SCC","ASCII 16K","ZEMINA 8K","ZEMINA 16K","ASC8 SRAM 2K", "ASC8 SRAM 8K", "ASC16 SRAM 2K",
                            "ASC16 SRAM 8K", "CROSSBLAIM","LODERUNNER", "XEVIOUS", "AT 0000H","AT 4000H","AT 8000H","64K LINEAR"},                                              &myConfig.msxMapper,        19},
        {"MACHINE TYPE",   {"MSX2 - NORMAL", "MSX1 - LEGACY"},                                                                                                                  &myConfig.machineType,      2},
        {"KEYBOARD",       {"FULL KEYBOARD", "ALPHA KEYBOARD"},                                                                                                                 &myConfig.keyboard,         2},
        {"MAX SPRITES",    {"4/8 PER LINE", "32 PER LINE"},                                                                                                                     &myConfig.maxSprites,       2},
        {"AUTO FIRE",      {"OFF", "B1 ONLY", "B2 ONLY", "BOTH"},                                                                                                               &myConfig.autoFire,         4},
        {"JOYSTICK",       {"NORMAL", "DIAGONALS", "ARKANOID", "SLIDE-N-GLILDE"},                                                                                               &myConfig.dpad,             4},
        {"SPLIT TIMING",   {"0 LINES", "1 LINE", "2 LINES"},                                                                                                                    &myConfig.splitRefresh,     3},
        {"CPU SPEED",      {"NORMAL", "BOOSTED 10%", "BOOSTED 20%"},                                                                                                            &myConfig.cpuBoost,         3},
        {"EXPANSION",      {"NONE", "FM-PAC", "SCC+ (SCC-I)", "2x PSG", "BEEPER"},                                                                                              &myConfig.expansion,      5},
        {"Y OFFSET",       {"None", "+1", "+2", "+3", "+4", "+5", "+6", "+7", "+8", "+9", "+10", "+11", "+12", "+13", "+14", "+15", "+16", "+17", "+18", "+19", "+20"},         &myConfig.yOffset,          21},
        {"SCREEN SCALE",   {"NONE", "COMPRESSED"},                                                                                                                              &myConfig.scaleScreen,      2},
        {"BORDER MASK",    {"NONE", "LEFT", "RIGHT", "LEFT + RIGHT"},                                                                                                           &myConfig.maskBorders,      4},
        {"FRAMESKIP",      {"NONE", "LIGHT", "AGGRESSIVE"},                                                                                                                     &myConfig.frameSkip,        3},
        {"DISK SOUND",     {"OFF", "ON"},                                                                                                                                       &myGlobalConfig.bDiskSounds,2},
        {"FPS",            {"OFF", "ON", "ON FULLSPEED"},                                                                                                                       &myGlobalConfig.showFPS,    3},
        {"DEBUGGER",       {"OFF", "FULL DEBUG"},                                                                                                                               &myGlobalConfig.debugger,   2},
        {NULL,             {"",      ""},                                                                                                                                       NULL,                       1},
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
            sprintf(strBuf, " %-12s : %-14s", Option_Table[option_table_idx][len].label, Option_Table[option_table_idx][len].option[*(Option_Table[option_table_idx][len].option_val)]);
            DSPrint(1,5+len, (len==0 ? 2:0), strBuf); len++;
            if (Option_Table[option_table_idx][len].label == NULL) break;
        }

        // Blank out rest of the screen... option menus are of different lengths...
        for (int i=len; i<16; i++)
        {
            DSPrint(1,5+i, 0, (char *)"                               ");
        }
    }

    DSPrint(0,22, 0, (char *)"      B=EXIT,  START=SAVE       ");
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

    option_table_idx = 0;

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
                sprintf(strBuf, " %-12s : %-14s", Option_Table[option_table_idx][optionHighlighted].label, Option_Table[option_table_idx][optionHighlighted].option[*(Option_Table[option_table_idx][optionHighlighted].option_val)]);
                DSPrint(1,5+optionHighlighted,0, strBuf);
                if (optionHighlighted > 0) optionHighlighted--; else optionHighlighted=(idx-1);
                sprintf(strBuf, " %-12s : %-14s", Option_Table[option_table_idx][optionHighlighted].label, Option_Table[option_table_idx][optionHighlighted].option[*(Option_Table[option_table_idx][optionHighlighted].option_val)]);
                DSPrint(1,5+optionHighlighted,2, strBuf);
            }
            if (keysCurrent() & KEY_DOWN) // Next option
            {
                sprintf(strBuf, " %-12s : %-14s", Option_Table[option_table_idx][optionHighlighted].label, Option_Table[option_table_idx][optionHighlighted].option[*(Option_Table[option_table_idx][optionHighlighted].option_val)]);
                DSPrint(1,5+optionHighlighted,0, strBuf);
                if (optionHighlighted < (idx-1)) optionHighlighted++;  else optionHighlighted=0;
                sprintf(strBuf, " %-12s : %-14s", Option_Table[option_table_idx][optionHighlighted].label, Option_Table[option_table_idx][optionHighlighted].option[*(Option_Table[option_table_idx][optionHighlighted].option_val)]);
                DSPrint(1,5+optionHighlighted,2, strBuf);
            }

            if (keysCurrent() & KEY_RIGHT)  // Toggle option clockwise
            {
                *(Option_Table[option_table_idx][optionHighlighted].option_val) = (*(Option_Table[option_table_idx][optionHighlighted].option_val) + 1) % Option_Table[option_table_idx][optionHighlighted].option_max;
                sprintf(strBuf, " %-12s : %-14s", Option_Table[option_table_idx][optionHighlighted].label, Option_Table[option_table_idx][optionHighlighted].option[*(Option_Table[option_table_idx][optionHighlighted].option_val)]);
                DSPrint(1,5+optionHighlighted,2, strBuf);
            }
            if (keysCurrent() & KEY_LEFT)  // Toggle option counterclockwise
            {
                if ((*(Option_Table[option_table_idx][optionHighlighted].option_val)) == 0)
                    *(Option_Table[option_table_idx][optionHighlighted].option_val) = Option_Table[option_table_idx][optionHighlighted].option_max -1;
                else
                    *(Option_Table[option_table_idx][optionHighlighted].option_val) = (*(Option_Table[option_table_idx][optionHighlighted].option_val) - 1) % Option_Table[option_table_idx][optionHighlighted].option_max;
                sprintf(strBuf, " %-12s : %-14s", Option_Table[option_table_idx][optionHighlighted].label, Option_Table[option_table_idx][optionHighlighted].option[*(Option_Table[option_table_idx][optionHighlighted].option_val)]);
                DSPrint(1,5+optionHighlighted,2, strBuf);
            }
            if (keysCurrent() & KEY_START)  // Save Options
            {
                SaveConfig(TRUE);
            }
            if ((keysCurrent() & KEY_B) || (keysCurrent() & KEY_A))  // Exit options
            {
                option_table_idx = 0;   // Reset for next time
                break;
            }
        }
        ShowRandomPreviewSnaps();
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
    keyMapType = (keyMapType+1) % 3;
    switch (keyMapType)
    {
        case 0: MapPlayer1();  break;
        case 1: MapPlayer2();  break;
        case 2: MapCursors();  break;
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
  // NDS keys are not being pressed. This prevents the inadvertent A key
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
    ShowRandomPreviewSnaps();
    swiWaitForVBlank();
  }
  while (keysCurrent() & KEY_B);
}


// ----------------------------------------------------------------
// We show the CART1, CART2 and DISK filenames in the main menu...
// ----------------------------------------------------------------
void DisplayFileNames(void)
{
    for (int id=0; id < (MEDIA_DISK+1); id++)
    {
        sprintf(szName, "[%d K] [CRC %08X]", MyMedia[id].filesize/1024, MyMedia[id].filecrc);
        DSPrint(5, 4+(5*id)+0,0,szName);

        sprintf(szName,"%s",MyMedia[id].filename);
        for (u8 i=strlen(szName)-1; i>0; i--) if (szName[i] == '.') {szName[i]=0;break;}
        if (strlen(szName)>24) szName[24]='\0';
        DSPrint(5, 4+(5*id)+1,0,szName);
        if (strlen(MyMedia[id].filename) >= 29)   // If there is more than a few characters left, show it on the 2nd line
        {
            sprintf(szName,"%s",MyMedia[id].filename+24);
            for (u8 i=strlen(szName)-1; i>0; i--) if (szName[i] == '.') {szName[i]=0;break;}
            if (strlen(szName)>24) szName[24]='\0';
            DSPrint(5, 4+(5*id)+2,0,szName);
        }

        // If a game is loaded into a slot, we show a red X icon so the user can remove it...
        if ((MyMedia[id].filecrc != 0) && (MyMedia[id].filecrc != 0xFFFFFFFF))  // 0xFFFFFFFF is for SCC+ cart
        {
            DSPrint(30,4+(5*id)+0,2, "!\"");
            DSPrint(30,4+(5*id)+1,2, "AB");
        }
        else
        {
            DSPrint(30,4+(5*id)+0,0, "  ");
            DSPrint(30,4+(5*id)+1,0, "  ");
        }
    }
}

// ----------------------------------------------------------------------
// Read file twice and ensure we get the same CRC... if not, do it again
// until we get a clean read. Return the filesize to the caller...
// ----------------------------------------------------------------------
u32 ReadFileCarefully(char *filename, u8 *buf, u32 buf_size, u32 buf_offset, u32 *crc)
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
            crc1 = getCRC32(buf, fileSize);
            fclose(file);
        }

        // Read #2
        crc2 = 0xFFFFFFFF;
        FILE* file2 = fopen(filename, "rb");
        if (file2)
        {
            if (buf_offset) fseek(file2, buf_offset, SEEK_SET);
            fileSize = fread(buf, 1, buf_size, file2);
            crc2 = getCRC32(buf, fileSize);
            fclose(file2);
        }

        // TODO add a trap for infinite loop - exit with error.
   } while (crc1 != crc2); // If the file couldn't be read, file_size will be 0 and the CRCs will both be 0xFFFFFFFF

   if (crc) *crc=crc1;

   return fileSize;
}

void NoGameSelected(void)
{
    unsigned short dmaVal = *(bgGetMapPtr(bg1b)+24*32);
    while (keysCurrent()  & (KEY_START | KEY_A));
    dmaFillWords(dmaVal | (dmaVal<<16),(void*) bgGetMapPtr(bg1b)+5*32*2,32*18*2);
    DSPrint(1,10,0,("    NO MASTER GAME SELECTED   "));
    DSPrint(1,12,0,("    USE CART1 OR DISK ICON    "));
    DSPrint(1,14,0,("  TO LOAD AND CONFIGURE GAME  "));
    WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;
    while (!(keysCurrent()  & (KEY_START | KEY_A | KEY_B | KEY_X)));
    while (keysCurrent()  & (KEY_START | KEY_A | KEY_B | KEY_X));
}

void CheckIfGameHasMusicMapper(void)
{
    if (myConfig.expansion == MUSIC_SCC)
    {
        // If the new Config tells us this is an SCC+ game, that slides into CART2
        memset(&MyMedia[MEDIA_CART2], 0x00, sizeof(MyMedia[MEDIA_CART2]));
        strcpy(MyMedia[MEDIA_CART2].filename, "SCC-I CARTRIDGE");
        MyMedia[MEDIA_CART2].filecrc = 0xFFFFFFFF;
    }
    else if (myConfig.expansion == MUSIC_MSX)
    {
        // If the new Config tells us this is an MSX-MUSIC game, that slides into CART2
        memset(&MyMedia[MEDIA_CART2], 0x00, sizeof(MyMedia[MEDIA_CART2]));
        strcpy(MyMedia[MEDIA_CART2].filename, "FM-PAC CARTRIDGE");
        MyMedia[MEDIA_CART2].filecrc = 0xFFFFFFFF;
    }
    else if (MyMedia[MEDIA_CART2].filecrc == 0xFFFFFFFF) // Was music mapped in?
    {
        memset(&MyMedia[MEDIA_CART2], 0x00, sizeof(MyMedia[MEDIA_CART2]));
    }
}


// --------------------------------------------------------------------
// Let the user select new options for the currently loaded game...
// --------------------------------------------------------------------
void HachibittoMainMenu(void)
{
    u8 bPlayGame=0;
    u8 screenTouched = 1;

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
    BottomScreenMainMenu();

    // Display games as we have them
    DisplayFileNames();

    while (!bPlayGame)
    {
        if (keysCurrent() & KEY_START)
        {
            while (keysCurrent()) swiWaitForVBlank();
            bPlayGame = 1;
        }

        // If the touch-screen is pressed... react to it
        if  ((keysCurrent() & KEY_TOUCH) && !screenTouched)
        {
            screenTouched = 5;
            touchPosition touch;
            touchRead(&touch);

            if ((touch.py >= 22) && (touch.py < 60)) // CART 1
            {
                if ((touch.px >= 220) && MyMedia[MEDIA_CART1].filecrc) // Delete existing mount?
                {
                    memset(&MyMedia[MEDIA_CART1], 0x00, sizeof(MyMedia[MEDIA_CART1]));
                    memset(ROM_Memory, 0xFF, (MAX_CART_SIZE_KB/2) * 1024);
                    if (MyMedia[MEDIA_CART2].filecrc == 0xFFFFFFFF) // Was Music Mapper mapped in?
                    {
                        memset(&MyMedia[MEDIA_CART2], 0x00, sizeof(MyMedia[MEDIA_CART2]));
                    }
                    // If disk is still mapped in, find the Config for that now as the master
                    if (MyMedia[MEDIA_DISK].filecrc != 0)
                    {
                        FindConfig();
                    }
                }
                else if (touch.px < 220)
                {
                    BottomScreenOptions();
                    HachibittoChooseFile(MEDIA_CART1, 1);
                    if (ucGameChoice != -1)
                    {
                        LoadGameIntoMedia(MEDIA_CART1, gpFic[ucGameChoice].szName);
                        CheckIfGameHasMusicMapper();
                    }
                }
                BottomScreenMainMenu();
                DisplayFileNames();
            }
            else if ((touch.py >= 60) && (touch.py < 98)) // CART 2
            {
                if ((myConfig.expansion != MUSIC_SCC) && (myConfig.expansion != MUSIC_MSX)) // Expanded Music mapper has to be booted out by Options...
                {
                    if ((touch.px >= 220) && MyMedia[MEDIA_CART2].filecrc) // Delete existing mount?
                    {
                        memset(&MyMedia[MEDIA_CART2], 0x00, sizeof(MyMedia[MEDIA_CART2]));
                        memset(ROM_Memory + (MAX_CART_SIZE_KB/2) * 1024, 0xFF, (MAX_CART_SIZE_KB/2) * 1024);
                    }
                    else if (touch.px < 220)
                    {
                        BottomScreenOptions();
                        HachibittoChooseFile(MEDIA_CART2, 1);
                        if (ucGameChoice != -1)
                        {
                            LoadGameIntoMedia(MEDIA_CART2, gpFic[ucGameChoice].szName);
                        }
                    }
                    BottomScreenMainMenu();
                    DisplayFileNames();
                }
            }
            else if ((touch.py >= 98) && (touch.py < 145)) // DISK
            {
                if ((touch.px >= 220) && MyMedia[MEDIA_DISK].filecrc) // Delete existing mount?
                {
                    memset(&MyMedia[MEDIA_DISK], 0x00, sizeof(MyMedia[MEDIA_DISK]));
                    memset(DISK_Memory, 0xFF, MAX_DISK_SIZE_KB * 1024);
                    if (MyMedia[MEDIA_CART2].filecrc == 0xFFFFFFFF) // Was Music Mapper mapped in?
                    {
                        if (MyMedia[MEDIA_CART1].filecrc == 0) // If no Master CART1
                        {
                            memset(&MyMedia[MEDIA_CART2], 0x00, sizeof(MyMedia[MEDIA_CART2]));
                        }
                    }
                }
                else if (touch.px < 220)
                {
                    BottomScreenOptions();
                    HachibittoChooseFile(MEDIA_DISK, 1);
                    if (ucGameChoice != -1)
                    {
                        LoadGameIntoMedia(MEDIA_DISK, gpFic[ucGameChoice].szName);
                        CheckIfGameHasMusicMapper();
                    }
                }
                BottomScreenMainMenu();
                DisplayFileNames();
            }
            else if ((touch.py >= 145) && (touch.py < 192)) // Main Menu Icons
            {
              if ((touch.px >= 20) && (touch.px < 92)) // Options
              {
                  if (GetMasterCRC())
                  {
                      BottomScreenOptions();
                      HachibittoGameOptions(false);
                      CheckIfGameHasMusicMapper();
                      BottomScreenMainMenu();
                      DisplayFileNames();
                  }
                  else
                  {
                      BottomScreenOptions();
                      NoGameSelected();
                      BottomScreenMainMenu();
                      DisplayFileNames();
                  }
              }
              else if ((touch.px >= 92) && (touch.px < 167)) // Play Game
              {
                  if (GetMasterCRC())
                  {
                      bPlayGame = 1;
                  }
                  else
                  {
                      BottomScreenOptions();
                      NoGameSelected();
                      BottomScreenMainMenu();
                      DisplayFileNames();
                  }
              }
              else if ((touch.px >= 167) && (touch.px < 245)) // Controller Map
              {
                  if (GetMasterCRC())
                  {
                      BottomScreenOptions();
                      HachibittoChangeKeymap();
                      BottomScreenMainMenu();
                      DisplayFileNames();
                  }
                  else
                  {
                      BottomScreenOptions();
                      NoGameSelected();
                      BottomScreenMainMenu();
                      DisplayFileNames();
                  }
              }
            }
        }
        else
        {
            if (screenTouched) screenTouched--;
        }

        ShowRandomPreviewSnaps();
        swiWaitForVBlank();
    }
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

// Just a bit faster than the generic DSPrint() when dealing with numbers only
void DSPrint_fps(u16 fps)
{
    u16 *pusScreen,*pusMap;
    char tmpStr[4];

    if (fps/100) tmpStr[0] = '0' + fps/100;
    else tmpStr[0] = ' ';
    tmpStr[1] = '0' + (fps%100) / 10;
    tmpStr[2] = '0' + (fps%100) % 10;
    tmpStr[3] = 0;

    pusScreen=(u16*) bgGetMapPtr(bg1b);
    pusMap=(u16*) bgGetMapPtr(bg0b)+24*32;
    char *cPtr = tmpStr;

    while((*cPtr )!='\0' )
    {
        char ch = *cPtr++;
        *pusScreen++=*(pusMap+(ch)-' ');          // Number from 0-9 or punctuation
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
