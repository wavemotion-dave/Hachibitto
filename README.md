# Hachibitto - MSX2 Emulator

Welcome to Hachibitto (pronounced Ha-Chi-Bee-Toe). A Fantasy MSX/MSX2 Console for your DS/DSi/XL/LL handheld.

![Loading Screen](arm9/gfx_data/pdev_bg0.png)

# Donations Welcome (but never required!)

These hobby emulators have been a labor of love. An embarassing amount of development time has gone into it as I've strived to provide an enjoyable retro experience on the DS handheld. It's free to use and always will be, however if you feel inclined to buy me a virtual coffee for the effort, that would be beyond amazing!

[<img src="https://github.com/user-attachments/assets/ab67686c-2168-46a3-b39f-77b5f5fef5d0">](https://ko-fi.com/wavemotiondave)

Features :
-----------------------
* Rock-solid 60Hz sync to the DSi/XL/LL for a tear-free experience (DS-Lite/Phat supported with light frameskip).
* Loads .ROM cartridge files up to 1.25MB (DS-Lite/Phat) or 4MB (DSi).
* Loads .DSK disk files that are single side (360K) or double sided (720K) with in-game disk swap.
* Emulated machine is 128K of User RAM and 128K of VRAM.
* Fully configurable keys for the 12 NDS keys to any combination of joystick/keyboard.
* Save and Restore states so you can pick up where you left off on a per-game basis.
* Standard PSG, 2x PSG, SCC, SCC+ and MSX-MUSIC available.
* SRAM carts backed to SD card automatically.
* Slide-n-Glide style Joystick configuration to make climbing ladders in games like Chuckie-Egg more forgiving (try it - you'll like it!).
* Arkanoid paddle controllers supported (only 2 games known to make use of it - hold B button to speed up paddle movement).
* High Score saving for 10 scores with initials, date/time.
* Favorites list so you can mark games as 'like' or 'love' - a yellow or red heart icon will mark your favorite games.
* Solid Z80 core that passes the ZEXDOC test suite (covering everything except undocumented flags which are partially supported for the few games that need them).
* Minimal design aesthetic - pick game, play game. Runs unpatched from your SD card via TWL++ or similar.

Copyright :
-----------------------
Hachibitto is Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)

As long as there is no commercial use (i.e. no profit is made),
copying and distribution of this emulator, it's source code
and associated readme files, with or without modification, 
are permitted in any medium without royalty provided this 
copyright notice is used and wavemotion-dave (Hachibitto),
and Marat Fayzullin (fMSX core) are thanked profusely.

The sound driver (ay38910 and scc) are libraries from FluBBa (Fredrik Ahlström) 
and those copyrights remain his.

lzav compression (for save states) is Copyright (c) 2023-2025 Aleksey 
Vaneev and used by permission of the generous MIT license.

The MSX/MSX2 logos are used without permission but with the maximum
of respect and love.

The Hachibitto emulator is offered as-is, without any warranty.

Special Thanks :
-----------------------
Thanks to Flubba for the AY38910 and SCC sound cores. You can seek out his latest and greatest at https://github.com/FluBBaOfWard

Also thanks to Marat Fayzullin, as the author of fMSX - which is where the CZ80 CPU core and initial VDP9938 handling came from.

And Nishi.

Options :
-----------------------
![Hachibitto Options](images/Options.png)

Known Issues :
-----------------------
* MSX-MUSIC is vastly simplified processing so that it will run on the older DS hardware. Sound is passable but nowhere near authentic.
* Lubeck with MSX-MUSIC enabled crashes. Reason is unknown.

The Hachibitto HB-8 Fantasy Console :
-----------------------
![Hachibitto Console](images/Hachibito-HB8-Console.jpeg)

![Hachibitto Backplate](images/Hachibito-HB8-Backplate.jpeg)

![Hachibitto Memory Map](images/MemoryMap.png)

<img width="582" height="606" alt="image" src="https://github.com/user-attachments/assets/c82c24e7-9706-43c8-bb50-04ae3cf569a5" />

Version History :
-----------------------
Version 1.0 - 29-Sep-2026 by wavemotion-dave
* First major version released.
* Hotfix 1.0a with fix for Save Config on DS-Lite/Phat.
* Hotfix 1.0b with new Z80 handling to improve speed 5%

