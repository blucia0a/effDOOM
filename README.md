PureDOOM clone/port metarepo for Efficient Electron E1 (https://efficient.computer) 
-----------------------------------------------------------------------------------
Disclaimer: this is not an official Efficient software release and was
independently developed.

The goal of this project is to preserve as much PureDOOM code as possible, with
minimal changes to deal with mainly EVK memory size constraints.  

Props to https://github.com/fragglet/squashware and fragglet for the WAD and to
https://github.com/Daivuk/PureDOOM and Daivuk for PureDOOM.

This repository contains everything needed to build, run, and host-side render
DOOM while running on the Efficient E1x Evaluation Kit.  The code runs and
streams pixel data over UART to your host where you can display it using the
supplied `doom_render.py` renderer.

You need pyserial, pygame, and numpy installed for the host-side renderer to
work.

To build, set your EFFCC, EFFSDK, and EFFSDKBLD locations in the Makefile
and then run `make`.  You should then see bld/doom.hex, which is 
built to flash onto Efficient Electron E1x.

Connect your EVK and flash the binary as usual:

`eff-flash -t SRAM -f bld/doom.hex`

then (in another window) run:

`python3 doom_render.py newdoom1_1lev.wad /dev/ttyACM2`

You should see an SDL window appear rendering frames received over UART
assuming that /dev/ttyACM2 is the serial port associated with the STDIO UART on
your EVK.


