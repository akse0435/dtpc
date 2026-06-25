# dtpc: DECtalk PC Emulator

## What is this

This is an emulator for the DECtalk PC ISA card, released in 1992, running in MAME. The emulator builds upon the DECtalk PC emulation already in MAME, by implementing module load and serial protocols, so that it may be used with applications outside MAME through a virtual serial cable, such as COM0COM.

The emulator has been tested with various screen reader drivers meant for the DECtalk Express, as well as with regular text, and it all works.

Please Note: A big part of this emulator has been coded with the help of AI, so not everything might be perfect, and there's definitely room for improvement. The emulator has been tested and found to be working by myself and others, and a few crashes have already been fixed. All contributions and feedback are very welcome.

## How to build

The following procedure has been tested in MSYS2 MINGW64 on Windows.

First, download and install [MSYS2](https://www.msys2.org/) on your computer. When the installation is complete, search for MINGW64 in the start menu, and launch MSYS2 MINGW64 as administrator.

Now you need to install the packages needed to build the code. Run the following command:

```bash
pacman -S --needed make git mingw-w64-x86_64-gcc mingw-w64-x86_64-python
```

This will install GCC, make, Python and Git. If you're asked "Proceed with installation?", press y and then enter to confirm. Then wait until it's done.

You now need to download a copy of the MAME source code, which is needed to build the emulator, since it needs the MAME core to run. First navigate to the root of your C drive with the following command:

```bash
cd /c
```

Then run:

```bash
git clone --depth 1 https://github.com/mamedev/mame.git
```

This will download the MAME source code to `C:\mame`. If you would like to download a specific version of MAME, e.g. MAME 0.287 for Windows 7 compatibility, you can add `--branch mame0xxx` before the URL in the command above. Then wait until it's done. This may take a while depending on your internet speed.

Now, copy `dtpc.cpp` from this repository to the following path:

```
C:\mame\src\mame\dec
```

You now need to register dtpc in MAME's driver list. Back in MINGW64, navigate to the root of the MAME directory with the following command:

```bash
cd /c/mame
```

Then run the following command to add dtpc to `mame.lst`:

```bash
grep -q "dec/dtpc.cpp" /c/mame/src/mame/mame.lst || printf '\n@source:dec/dtpc.cpp\ndtpc\n' >> /c/mame/src/mame/mame.lst
```

You're now ready to build. Run the following command:

```bash
make SUBTARGET=dtpc SOURCES=src/mame/dec/dtpc.cpp CONFIG=release STRIP_SYMBOLS=1 LTO=1 PYTHON_EXECUTABLE=/mingw64/bin/python.exe -j4
```

This will build a static binary with the DECtalk PC emulator and required components from MAME.

A few tips:

- `-j4` refers to the number of threads used to build, so you can use a higher value if you have more threads in your computer.
- You can shorten the building time by up to 20 minutes by removing `LTO=1` from the command, though it will also add about 10 MB to the file size.

## How to use

You need to download and set up a few things in order to get the emulator up and running:

1. [COM0COM](https://dectalk.nu/Software%20and%20Manuals/Software/Miscellaneous/setup_com0com_W7_x64_signed.exe): See below.
2. [DECtalk PC ROMs](https://dectalk.nu/Software%20and%20Manuals/Hardware/DECtalk/PC1/Software/dectalk_isa.zip): Rename the downloaded file to `dtpc.zip`, and put it into `C:\mame\roms`.
3. [DECtalk PC software modules](https://dectalk.nu/Software%20and%20Manuals/Hardware/DECtalk/PC1/Software/dtpc-modules.zip): This package contains modules for 7 different DECtalk versions. The emulator is optimized for version 4.2CD, but all versions will work. Create a folder in `C:\mame` called `modules`, and copy the files from the version you want into it.

### How to set up COM0COM

Install COM0COM as you would any other program. Nothing needs to be changed during the installation before the finish screen. If you get any warnings about installing new drivers during the installation, just press install. When you get to the finish screen, be sure to check the box called "Launch Setup", then press finish.

The COM0COM setup will now open, and you should be in the first of two text fields. In the first, enter the COM port the DECtalk PC sends data to, and in the second, enter the COM port you communicate through, i.e. the one you set in your DECtalk Express driver, or use when sending text to the emulator. Then navigate to the apply button and press enter. You should now be all set with COM0COM, and you may close the window.

### How to launch the DECtalk PC emulator

Before launching the emulator, you need to configure at least one variable: the COM port to use. See below for a complete list of available variables. Press Windows+R, and type cmd, then press enter. Now navigate to the MAME directory with the following command:

```bat
cd C:\mame
```

To set the emulator's COM port to COM8, as used in the example earlier, run the command:

```bat
set DTPC_PORT=COM8
```

I'd also recommend setting the audio gain to double, since the regular volume is a bit low. To do this, run:

```bat
set DTPC_GAIN=2
```

Now, to launch the emulator, I'd recommend using the following command:

```bat
dtpc dtpc -video none -samplerate 20000
```

For future use, you can make a bat file with your preferred commands and place it in the mame folder, for quick access to starting the emulator.

The DECtalk software will now be loaded to the DECtalk PC card, and you will then hear the startup message: "DECtalk PC is running.". You should now be able to send text to the DECtalk PC emulator through the other COM port you set up, COM9 in this example.

## List of available variables

The following is a complete list of variables that can be changed before launching the emulator. Every variable is set with the command `set DTPC_VARIABLE=VALUE`.

| Variable | Default value | Description |
| --- | --- | --- |
| DTPC_LOG | 0 | If set to 1, the emulator will log everything happening. This makes the emulation incredibly slow, and should only be used for debugging. |
| DTPC_MODULES | modules | This can be used to specify a different folder containing the software modules. |
| DTPC_PORT | none | This is used to specify the COM port that the emulator connects to, and is the only obligatory one. |
| DTPC_DSPMHZ | 80 | This can be used to change the speed of the DSP for testing purposes. The actual DECtalk PC card uses a speed of 20 MHz, but since this produces damaged audio in the emulator, 80 is the default. It takes a value between 5 and 160. |
| DTPC_GAIN | 1 | This can be used to adjust the gain of the audio output. It takes a value from 0.0 to 8.0. |
| DTPC_PUMPUS | 5 | This can be used to change the runtime polling interval in microseconds. It takes a value between 2 and 500. |

## Current status of the emulator

- All modules and the dictionary load correctly, proven by a RAM dump.
- A few crashes have been fixed.
- It has a latency of about 400 milliseconds from pressing a key in a screen reader (or sending text) until speech is heard.
- The code is currently a bit cluttered, and could use a clean-up.
