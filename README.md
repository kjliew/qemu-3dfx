# QEMU MESA GL/3Dfx Glide Pass-Through
Copyright (C) 2018-2022<br>
KJ Liew \<liewkj@yahoo.com\>
## Content
    qemu-0/hw/3dfx      - Overlay for QEMU source tree to add 3Dfx Glide pass-through device model
    qemu-1/hw/mesa      - Overlay for QEMU source tree to add MESA GL pass-through device model
    scripts/sign_commit - Script for stamping commit id
    wrappers/3dfx       - Glide wrappers for supported guest OS/environment (DOS/Windows/DJGPP/Linux)
    wrappers/mesa       - MESA GL wrapper for supported guest OS/environment (Windows)
## Patch
    00-qemu82x-mesa-glide.patch - Patch for QEMU version 8.2.x (MESA & Glide)
    01-qemu72x-mesa-glide.patch - Patch for QEMU version 7.2.x (MESA & Glide)
    02-qemu620-mesa-glide.patch - Patch for QEMU version 6.2.0 (MESA & Glide)
## QEMU Windows Guests Glide/OpenGL/Direct3D Acceleration
Witness, experience and share your thoughts on modern CPU/GPU prowess for retro Windows games on Apple Silicon macOS, Windows 10/11 and modern Linux. Most games can be installed and played in pristine condition without the hassle of hunting down unofficial, fan-made patches to play them on Windows 10/later or modern Linux/Wine.
- YouTube channel (https://www.youtube.com/channel/UCl8InhZs1ixZBcLrMDSWd0A/videos)
- VOGONS forums (https://www.vogons.org)
- Wiki (https://github.com/kjliew/qemu-3dfx/wiki)
## Building QEMU
Following instructions are based on `MSYS2/mingw-w64` BASH shell environment on Windows 10/11. It is meant to be simple and minor variations are inevitable due to different flavors of Linux distributions.

Simple guide to apply the patch:<br>
(using `00-qemu82x-mesa-glide.patch`)

    $ mkdir ~/myqemu && cd ~/myqemu
    $ git clone https://github.com/kjliew/qemu-3dfx.git
    $ cd qemu-3dfx
    $ wget https://download.qemu.org/qemu-8.2.1.tar.xz
    $ tar xf qemu-8.2.1.tar.xz
    $ cd qemu-8.2.1
    $ rsync -r ../qemu-0/hw/3dfx ../qemu-1/hw/mesa ./hw/
    $ patch -p0 -i ../00-qemu82x-mesa-glide.patch
    $ bash ../scripts/sign_commit
    $ mkdir ../build && cd ../build
    $ ../qemu-8.2.1/configure && make

## Building Guest Wrappers
**Requirements:**
 - `base-devel` (make, sed, xxd etc.)
 - `gendef, shasum`
 - `mingw32` cross toolchain (`binutils, gcc, windres, dlltool`) for WIN32 DLL wrappers
 - `Open-Watcom-1.9/v2.0` or `Watcom C/C++ 11.0` for DOS32 OVL wrapper
 - `{i586,i686}-pc-msdosdjgpp` cross toolchain (`binutils, gcc, dxe3gen`) for DJGPP DXE wrappers
<br>

    $ cd ~/myqemu/qemu-3dfx/wrappers/3dfx
    $ mkdir build && cd build
    $ bash ../../../scripts/conf_wrapper
    $ make && make clean

    $ cd ~/myqemu/qemu-3dfx/wrappers/mesa
    $ mkdir build && cd build
    $ bash ../../../scripts/conf_wrapper
    $ make && make clean

## Installing Guest Wrappers
**For Win9x/ME:**  
 - Copy `FXMEMMAP.VXD` to `C:\WINDOWS\SYSTEM`  
 - Copy `GLIDE.DLL`, `GLIDE2X.DLL` and `GLIDE3X.DLL` to `C:\WINDOWS\SYSTEM`  
 - Copy `GLIDE2X.OVL` to `C:\WINDOWS`  
 - Copy `OPENGL32.DLL` to `Game Installation` folders

**For Win2k/XP:**  
 - Copy `FXPTL.SYS` to `%SystemRoot%\system32\drivers`  
 - Copy `GLIDE.DLL`, `GLIDE2X.DLL` and `GLIDE3X.DLL` to `%SystemRoot%\system32`  
 - Run `INSTDRV.EXE`, require Administrator Priviledge  
 - Copy `OPENGL32.DLL` to `Game Installation` folders
 
## Donation
If this project helps you relive the nostalgic memory of Good Old Windows Games, you can now donate in the course of supporting **Games Preservation** with QEMU. Your donation also motivates and encourages further research in making QEMU the ultimate platform for Retro Windows Games.

For $89.99 donation, you will deserve the following donor's privileges:
- QEMU binary package built for platform of your choice (choose **ONE**: Windows 10/11, Ubuntu, etc.)
- QEMU-enhanced OpenGLide **Host-side wrappers** built for platform of your choice (choose **ONE**: Windows 10/11, Ubuntu, etc.)
- QEMU-enhanced [**WineD3D libraries for Win98/2K/ME/XP VMs**](https://www.winehq.org) for DirectDraw/Direct3D games up to DirectX 9.0c
- Game controllers support with [**QEMU USB Gamepad**](https://github.com/kjliew/qemu-3dfx/wiki/QEMU-USB-Gamepad)
- SDL2 clipboard sharing through built-in [**QEMU vdagent**](https://www.kraxel.org/blog/2021/05/qemu-cut-paste/)
- OpenGLide **Guest-side wrappers** for Windows
- Elect up to 5 games for priority support and your name as the honorary sponsor in the supported & tested list of games.

[![paypal](https://www.paypalobjects.com/en_US/i/btn/btn_donateCC_LG.gif)](https://www.paypal.com/cgi-bin/webscr?cmd=_s-xclick&hosted_button_id=XE47KTASERX4A)
### A Note to Donation
The purpose of the donation is for preserving retailed CD/DVD games in their originality. It may be used to purchase the game online or from local thrift shops. The donation is **NOT** the ticket for one to learn how to use QEMU Virtual Machine in general. Sometimes, it may be difficult to get virtualization acceleration working and that would result in serious degradation of game experience with QEMU. It is a **willing donation pledge and non-refundable**. Many classic Windows games also have re-releases from GOG/Steam that work on Windows 10/11 and modern Linux. It can be an option to consider before making a donation.

Donations without leaving notes on **Platform of Choice** are regarded as upper-class donors who have no desire in exercising donor's privileges. A measure to avoid unneccessary spamming on emails. Donors are expected to proactively follow up the communication to exercise donor's privileges as wished. All donations are tracked in PayPal transaction history. Only **"ONE"** platform of choice per donation. Upgrades eligibility are limited to the **SAME** platform of choice.
### About Game Election
The game election serves the purpose of allocating additional focus and resources to make them work. Sometimes, it means considerable efforts in researching, debugging and tracing the games to root cause the failures and come up with solutions. It is **optional** to make game election upon donation. My YouTube channel has video demos of games which already worked and more may be showing up periodically. It is typically a safe assumption that games using the same engine (IdTech1/2/3, LithTech, Unreal etc.) would also work, too. The _N_ counts of eligibility would only be accounted once the game were made to work. If upgrades were neccessary, it would be a **free upgrade** for QEMU binary packages.
