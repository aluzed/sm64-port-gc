# Super Mario 64 Port

- This repo contains a full decompilation of Super Mario 64 (J), (U), (E), and (SH).
- Naming and documentation of the source code and data structures are in progress.
- Beyond Nintendo 64, it can also target Linux and Windows natively.
- This fork additionally targets **Nintendo GameCube and Wii** (PowerPC) through devkitPPC,
  producing a `.dol` executable. See [GameCube / Wii](#gamecube--wii-devkitppc) below and the
  [port roadmap](docs/stories/README.md).

This repo does not include all assets necessary for compiling the game.
A prior copy of the game is required to extract the assets.
**No copyrighted asset is distributed here** — ROM archives and images are excluded from
version control, and compiled binaries must not be redistributed.

## Building native executables

### Linux

1. Install prerequisites (Ubuntu): `sudo apt install -y git build-essential pkg-config libusb-1.0-0-dev libsdl2-dev`.
2. Clone the repo: `git clone https://github.com/sm64-port/sm64-port.git`, which will create a directory `sm64-port` and then **enter** it `cd sm64-port`.
3. Place a Super Mario 64 ROM called `baserom.<VERSION>.z64` into the repository's root directory for asset extraction, where `VERSION` can be `us`, `jp`, or `eu`.
4. Run `make` to build. Qualify the version through `make VERSION=<VERSION>`. Add `-j4` to improve build speed (hardware dependent based on the amount of CPU cores available).
5. The executable binary will be located at `build/<VERSION>_pc/sm64.<VERSION>.f3dex2e`.

### Windows

1. Install and update MSYS2, following all the directions listed on https://www.msys2.org/.
2. From the start menu, launch MSYS2 MinGW and install required packages depending on your machine (do **NOT** launch "MSYS2 MSYS"):
  * 64-bit: Launch "MSYS2 MinGW 64-bit" and install: `pacman -S git make python3 mingw-w64-x86_64-gcc`
  * 32-bit (will also work on 64-bit machines): Launch "MSYS2 MinGW 32-bit" and install: `pacman -S git make python3 mingw-w64-i686-gcc`
  * Do **NOT** by mistake install the package called simply `gcc`.
3. The MSYS2 terminal has a _current working directory_ that initially is `C:\msys64\home\<username>` (home directory). At the prompt, you will see the current working directory in yellow. `~` is an alias for the home directory. You can change the current working directory to `My Documents` by entering `cd /c/Users/<username>/Documents`.
4. Clone the repo: `git clone https://github.com/sm64-port/sm64-port.git`, which will create a directory `sm64-port` and then **enter** it `cd sm64-port`.
5. Place a *Super Mario 64* ROM called `baserom.<VERSION>.z64` into the repository's root directory for asset extraction, where `VERSION` can be `us`, `jp`, or `eu`.
6. Run `make` to build. Qualify the version through `make VERSION=<VERSION>`. Add `-j4` to improve build speed (hardware dependent based on the amount of CPU cores available).
7. The executable binary will be located at `build/<VERSION>_pc/sm64.<VERSION>.f3dex2e.exe` inside the repository.

#### Troubleshooting

1. If you get `make: gcc: command not found` or `make: gcc: No such file or directory` although the packages did successfully install, you probably launched the wrong MSYS2. Read the instructions again. The terminal prompt should contain "MINGW32" or "MINGW64" in purple text, and **NOT** "MSYS".
2. If you get `Failed to open baserom.us.z64!` you failed to place the baserom in the repository. You can write `ls` to list the files in the current working directory. If you are in the `sm64-port` directory, make sure you see it here.
3. If you get `make: *** No targets specified and no makefile found. Stop.`, you are not in the correct directory. Make sure the yellow text in the terminal ends with `sm64-port`. Use `cd <dir>` to enter the correct directory. If you write `ls` you should see all the project files, including `Makefile` if everything is correct.
4. If you get any error, be sure MSYS2 packages are up to date by executing `pacman -Syu` and `pacman -Su`. If the MSYS2 window closes immediately after opening it, restart your computer.
5. When you execute `gcc -v`, be sure you see `Target: i686-w64-mingw32` or `Target: x86_64-w64-mingw32`. If you see `Target: x86_64-pc-msys`, you either opened the wrong MSYS start menu entry or installed the incorrect gcc package.
6. When switching between building for other platforms, run `make -C tools clean` first to allow for the tools to recompile on the new platform. This also helps when switching between shells like WSL and MSYS2.

### GameCube / Wii (devkitPPC)

> **Status: playable, not finished.** `make TARGET_WII=1` and `make TARGET_GC=1` produce a
> `.dol` that boots and runs at the correct 30 fps under Dolphin, in 60 Hz and in 50 Hz alike.
> In place: build system, video (libogc VIDEO/GX), GX renderer with textures and the colour
> combiner translated to TEV, distance fog, 32 kHz stereo audio, GameCube controller, and
> saves on both a memory card and SD.
>
> Still missing: **Wii Remote support**, video mode selection and 16:9, and two combiner
> effects (noise dithering, Z decal validation). Two rendering defects are open: the water
> surface flickers out for an instant, and some surfaces occasionally lose their texture.
>
> **Nothing has ever run on real hardware.** Dolphin hides exactly the class of fault that
> bites on a console. Track progress in [`docs/stories/`](docs/stories/README.md).

#### Dependencies

The GameCube/Wii build is a **cross-compilation**: it needs the devkitPro toolchain in
addition to the host tools already required by the PC build (Python 3, `make`, and a host
C compiler used to build the asset-processing tools in `tools/`).

| Dependency | What it provides | Notes |
|---|---|---|
| **devkitPPC** | `powerpc-eabi-gcc` and binutils for the Gekko/Broadway CPU | the cross compiler |
| **libogc** | console runtime: `VIDEO`, `GX`, `AUDIO`, `PAD`, `WPAD`, `SYS` | equivalent of SDL here |
| **libfat** | FAT filesystem on SD / USB, for saves and config | `sd:/apps/sm64/` |
| **libwiiuse**, **libbte** | Wii Remote / Nunchuk / Classic Controller support | Wii target only |
| **elf2dol** | converts the linked ELF into a bootable `.dol` | ships in `devkitPro/tools/bin` |

Install them with the devkitPro package manager, which pulls everything through two
metapackages:

- **Windows** — run the [devkitPro graphical installer](https://github.com/devkitPro/installer/releases)
  and tick **GameCube Development** and/or **Wii Development**. It installs devkitPPC, libogc,
  libfat and its own MSYS2 environment (default location `C:\devkitPro`).
- **Linux / macOS** — install `pacman` from devkitPro
  ([instructions](https://devkitpro.org/wiki/devkitPro_pacman)), then:
  ```sh
  sudo dkp-pacman -S gamecube-dev   # GameCube: devkitPPC + libogc + libfat
  sudo dkp-pacman -S wii-dev        # Wii: adds libwiiuse, libbte
  ```

You also need a **host compiler** and **Python 3** to build `tools/` and extract the assets.
On Windows there is no need for a second MSYS2 install: devkitPro's own MSYS2 ships `pacman`
with the `[msys]` repository, so from `C:\devkitPro\msys2\msys2_shell.bat`:

```sh
pacman -Syu                 # relaunch the shell if it asks you to
pacman -S gcc python git    # make is already installed
```

The host compiler only builds the asset tools (`n64graphics`, `mio0`, `skyconv`, `armips`…),
which run on the PC. The game itself is compiled by devkitPPC, so MSYS2's native gcc is fine
here — the upstream warning about "the package called simply `gcc`" only applies to building
the *game* for Windows.

The build expects these environment variables (set automatically by the installer and by
devkitPro's MSYS2 shell):

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
```

> **Launch the shell in MSYS mode: `msys2_shell.bat -msys`.**
> By default it starts with `MSYSTEM=MINGW64`, which makes `uname` report `MINGW64_NT-…`.
> `tools/Makefile` then takes its MinGW branch and passes `-municode` to `armips`, which the
> Cygwin-targeting gcc rejects with
> `unrecognized command-line option '-municode'`. In MSYS mode everything builds.

Git Bash will not work: it has neither `gcc` nor `python3`, and does not mount `/opt/devkitpro`.

Versions validated so far: devkitPPC 16.1.0, libogc from the current `wii-dev`/`gamecube-dev`
metapackages, host gcc 15.3.0, Python 3.12.13. Record yours with
`dkp-pacman -Q devkitPPC libogc`: devkitPro packages move fast and occasionally break
backwards compatibility.

#### Building

1. Place a ROM named `baserom.<VERSION>.z64` in the repository root, as for any other target:
   ```sh
   unzip -p "Super Mario 64 (USA).zip" > baserom.us.z64
   ```
2. Build for your console:
   ```sh
   make TARGET_WII=1 -j8        # Wii
   make TARGET_GC=1  -j8        # GameCube
   ```
3. The output lands in `build/<VERSION>_wii/sm64.<VERSION>.dol` (resp. `_gc`), around 12 MiB.
4. Package it:
   ```sh
   make TARGET_WII=1 dist      # dist/sm64/     boot.dol + meta.xml + icon.png
   make TARGET_GC=1  dist      # dist/sm64-gc/  sm64.dol + README.txt
   ```
   `meta.xml` takes its version from `git describe`, so a build made from uncommitted changes
   is marked `-dirty` — worth having when a `.dol` turns up on a memory card months later.

`dist/` is git-ignored on purpose: the binaries in it have ROM-extracted assets compiled in
and must not be redistributed. Share the source and let people build their own.

PC and console builds use separate build directories, so they coexist without `make clean`.
If you switch host platforms, run `make -C tools clean` so the host tools are rebuilt.

#### Running

- **Wii** — copy `dist/sm64/` to `sd:/apps/sm64/` and launch it from the Homebrew Channel.
  The binary must be named `boot.dol`.
- **GameCube** — launch the `.dol` with Swiss from an SD Gecko / SD2SP2.
- **Dolphin** — `make TARGET_WII=1 run` starts the freshly built `.dol`. Override the
  emulator path with `make ... run DOLPHIN=/path/to/Dolphin.exe`.

Dolphin is the fast iteration loop, but it is more forgiving than real hardware: it hides
missing `DCFlushRange` calls, misaligned buffers and audio DMA underruns. Validate each
milestone on a real console — see
[STORY-017](docs/stories/017-testing-dolphin-hardware.md).

#### Where progress is saved

Nothing has to be configured; the game picks the first thing it finds.

| Console | Order | Result |
|---|---|---|
| GameCube | memory card slot 1, then slot 2 | `sm64_save_file.bin`, two blocks, listed in the console's card manager |
| GameCube | then Serial Port 2 (SD2SP2), then an SD Gecko | `/sm64/` on the card |
| Wii | SD card, then USB | `sd:/sm64/` |

Two blocks rather than one because the file is written alternately across them: a power cut
during a save costs the new one and never the one before it. The SD path gets the same
guarantee through a temporary file and a rename.

The SD format is byte-identical to the PC build's, so a save can be carried between them; the
memory card format cannot. Build with `-DSTORAGE_OGC_PREFER_FAT=1` to keep saves on SD even
when a memory card is present.

With no storage at all the game runs normally and simply does not save.

#### Controls

| GameCube | Wii Classic | Wiimote + Nunchuk | N64 |
|---|---|---|---|
| Main stick | Left stick | Nunchuk stick | Analog stick |
| A | a | A | A |
| B | b | B | B |
| Z or L trigger | ZL / ZR | Z (Nunchuk) | Z |
| R trigger | Right trigger | C (Nunchuk) | R |
| Start | + | + | Start |
| C-stick | Right stick | D-pad | C buttons |
| D-pad | D-pad | — | D-pad |

A Wii Remote without a Nunchuk is not supported: SM64 needs an analog stick.

#### Reporting a crash

On a CPU exception the console shows the faulting address (SRR0). Translate it to a function
name with the map file produced by the build:

```sh
powerpc-eabi-addr2line -e build/us_wii/sm64.us.elf <address>
```

### Debugging

The code can be debugged using `gdb`. On Linux install the `gdb` package and execute `gdb <executable>`. On MSYS2 install by executing `pacman -S winpty gdb` and execute `winpty gdb <executable>`. The `winpty` program makes sure the keyboard works correctly in the terminal. Also consider changing the `-mwindows` compile flag to `-mconsole` to be able to see stdout/stderr as well as be able to press Ctrl+C to interrupt the program. In the Makefile, make sure you compile the sources using `-g` rather than `-O2` to include debugging symbols. See any online tutorial for how to use gdb.

## ROM building

It is possible to build N64 ROMs as well with this repository. See https://github.com/n64decomp/sm64 for instructions.

## Project Structure
	
	sm64
	├── actors: object behaviors, geo layout, and display lists
	├── asm: handwritten assembly code, rom header
	│   └── non_matchings: asm for non-matching sections
	├── assets: animation and demo data
	│   ├── anims: animation data
	│   └── demos: demo data
	├── bin: C files for ordering display lists and textures
	├── build: output directory
	├── data: behavior scripts, misc. data
	├── docs: project documentation
	│   └── stories: GameCube/Wii port roadmap (one file per story)
	├── doxygen: documentation infrastructure
	├── enhancements: example source modifications
	├── include: header files
	├── levels: level scripts, geo layout, and display lists
	├── lib: SDK library code
	├── rsp: audio and Fast3D RSP assembly code
	├── sound: sequences, sound samples, and sound banks
	├── src: C source code for game
	│   ├── audio: audio code
	│   ├── buffers: stacks, heaps, and task buffers
	│   ├── engine: script processing engines and utils
	│   ├── game: behaviors and rest of game source
	│   ├── goddard: Mario intro screen
	│   ├── menu: title screen and file, act, and debug level selection menus
	│   └── pc: port code, audio and video renderer
	├── text: dialog, level names, act names
	├── textures: skybox and generic texture data
	└── tools: build tools

## Contributing

Pull requests are welcome. For major changes, please open an issue first to
discuss what you would like to change.

Run `clang-format` on your code to ensure it meets the project's coding standards.

Official Discord: https://discord.gg/7bcNTPK
