# PS1Emulator

<!-- Video demonstration: to be added here. -->

PS1Emulator is a PlayStation emulator for Nokia smartphones running Symbian
S60 3rd Edition and Symbian Belle. It is a port of
[PCSX-ReARMed](https://github.com/notaz/pcsx_rearmed) by notaz, adapted to the
ARM11 processors of these phones and to the platform security of Symbian 9.
Games run on a dynamic recompiler that translates PlayStation machine code
into ARM code while the game is running, which is what makes them playable on
a 330 MHz phone.

The first part of this document is written for people who want to install the
emulator and play. The second part, starting at
[How it works](#part-2-how-it-works), is written for people who want to know
what happens inside, check the measurements, or build the emulator
themselves.

# Part 1. Using the emulator

## What it gives you

The emulator runs real PlayStation games from disc images on the phone's
memory card, with sound, save states and the full PlayStation controller
mapped to the phone keys. How fast a game runs depends on the game and on the
phone. On the Nokia C5-00.2 and on an overclocked Nokia N95 8GB the twelve
games measured so far run at between 28% and about 100% of the speed of a real
PlayStation; on an N95 8GB at its factory clock they run at between 22% and
79%. The full table is in [Game speed](#game-speed).

The emulator offers the following:

- **Dynamic recompiler.** PlayStation code is translated into ARM code by the
  Ari64 recompiler of PCSX-ReARMed. An interpreter is included as a fallback
  for games the recompiler gets wrong.
- **Two ways to draw the picture.** The software rasteriser `gpu_unai` works on
  every phone, including those without a 3D chip. On phones with the PowerVR
  MBX chip (N95 8GB, E90) the polygons can be drawn by the graphics chip
  through OpenGL ES 1.1 instead.
- **Direct screen output.** The finished frame is written straight into the
  screen memory, bypassing the window server, and a frame the game did not
  change is not drawn again.
- **Sound** from the PlayStation sound chip emulation at 44.1 kHz stereo, with
  an optional mode that keeps the pitch when the game runs slower than real
  time.
- **Save states**, five slots per game, and PlayStation memory cards.
- **Disc images in several formats**, including compressed `.pbp`, and an
  importer on the phone itself that unpacks `.zip`, `.7z` and uncompressed
  `.rar` archives and compresses the images.
- **Configurable controls** with presets for racing, fighting and role-playing
  games, a hold mode that keeps a button pressed without holding the key, and
  tilt steering on the N95's accelerometer.
- **Per-game settings**, so that a speed setting needed by one game does not
  affect the others.
- **Fifteen interface languages**, chosen automatically from the phone's
  language setting or by hand, with a built-in help text and a hint for every
  menu row.

## Supported phones

| Phone | Platform | Processor | Graphics | Status |
|---|---|---|---|---|
| Nokia N95 8GB (RM-320) | S60 3rd FP1, Symbian 9.2 | OMAP 2420, ARM1136, 330 MHz | PowerVR MBX | reference phone, all twelve games measured |
| Nokia E90 (RA-6) | S60 3rd FP1, Symbian 9.2 | OMAP 2420, ARM1136, 330 MHz | PowerVR MBX | installed and tested, not part of the game speed series |
| Nokia C5-00.2 | S60 3rd FP2 | ARM11, about 600 MHz | none | all twelve games measured |
| Nokia 701 | Symbian Belle FP2 | BCM2763, 1 GHz | VideoCore IV | a target, the emulator has not been run on it yet |

Adaptation to more phones is planned: the touch-screen Nokia phones with the
Broadcom BCM2727 and BCM2763 chips, such as the Nokia N8, E7, 701 and 808, and
the Symbian 9.4 phones, such as the Nokia 5800 and N97. Most of them have few
or no game-friendly hardware keys, so the main work there is on-screen
controls and the screen sizes these phones use. Because this is a hobby
project, there is no schedule for it.

The N95 8GB and the E90 are built on the same chip, so what is measured on one
of them applies to the other, except where the E90's slower factory memory bus
matters. The C5-00.2 has no 3D chip at all; the software rasteriser and direct
screen output exist so that phones like it are fully supported.

## Game speed

Every game below was measured from the same saved moment of actual gameplay,
not from a menu or an intro, three times for exactly 1200 PlayStation frames.
The first number is the speed of the emulated PlayStation as a share of the
real console, where 100% is full speed. The second is the number of pictures
per second that reached the screen. All games are NTSC versions and the
emulator ran with its default settings and sound on. Each figure is the median
of three runs. The measurements were made with release alpha 1.3, on
2026-10-05 for the C5-00.2 and the overclocked N95 and on 2026-10-06 for the
N95 at its factory clock.

| Game | C5-00.2 | N95 8GB, 330 MHz | N95 8GB, 426 MHz |
|---|---|---|---|
| Castlevania: Symphony of the Night | 56.8% / 34.1 fps | 44.8% / 26.9 fps | 57.9% / 34.7 fps |
| Colin McRae Rally 2.0 | 32.0% / 9.5 fps | 22.3% / 6.6 fps | 28.5% / 8.5 fps |
| Colony Wars | 65.0% / 19.5 fps | 51.7% / 15.5 fps | 68.0% / 20.4 fps |
| Crash Bandicoot | 52.8% / 15.9 fps | 35.1% / 10.5 fps | 45.0% / 13.5 fps |
| Gran Turismo | 58.3% / 17.5 fps | 43.8% / 13.2 fps | 57.1% / 17.1 fps |
| Need for Speed: Porsche Unleashed | 46.7% / 13.8 fps | 34.8% / 10.3 fps | 44.9% / 13.3 fps |
| Rayman | 66.0% / 39.6 fps | 65.2% / 39.1 fps | 84.6% / 50.8 fps |
| RC Helicopter | 53.1% / 31.9 fps | 40.4% / 24.2 fps | 52.1% / 31.3 fps |
| Star Trek: Invasion | 84.7% / 25.4 fps | 71.1% / 21.3 fps | 93.0% / 27.9 fps |
| Warzone 2100 | 82.3% / 16.5 fps | 57.7% / 11.5 fps | 74.3% / 14.9 fps |
| Wing Commander III | 100.1% / 24.0 fps | 79.4% / 19.1 fps | 104.3% / 25.0 fps |
| WipEout | 68.5% / 20.6 fps | 52.6% / 15.8 fps | 67.9% / 20.4 fps |

The third column is the N95 8GB overclocked with
[SymbianOverclock](https://github.com/1234bpv/SymbianOverclock): multiplier
111, processor 426.24 MHz, memory bus 213.12 MHz, both 29% above factory. The
clock was read back from the processor registers before and after every series
of three runs, not taken from the setting.

Many games do not draw a new picture on every frame of the television signal.
Gran Turismo, Crash Bandicoot and WipEout, for example, run at 30 pictures per
second on a real PlayStation, so at 100% speed they would show 30 fps, not 60.
This is why the two numbers in a cell are not proportional across games.

Repeated runs of the same game on the same phone differed by up to 4%, so
differences smaller than that between the columns mean nothing. Wing
Commander III reaches full speed on the C5-00.2 and on the overclocked N95;
a figure slightly above 100% is full speed within that spread.

Raising the N95's processor and bus by 29% makes every one of the twelve games
between 28% and 32% faster, so the gain follows the clock almost exactly. The
C5-00.2, with its 600 MHz processor but no 3D chip, is between as fast as and
1.5 times as fast as the N95 at its factory clock, and within about 20% of the
overclocked N95 in either direction, depending on the game.

The factory column was measured twice. The first series, on 2026-10-05, took
21% to 80% longer than the repeat on 2026-10-06, on the same clock, the
same saved states and the same emulator file, and it is not used here. The
details are in
[How the game speed is measured](#how-the-game-speed-is-measured).

## Before you install

1. **The phone must accept self-signed packages.** It has to be prepared for
   homebrew software ("hacked") in the way usual for Symbian 9. On S60 3rd
   Edition up to FP1 this is done with `Dr Web RomPatcher plus light` and its
   `Open4all`, `universal` and `Install server` patches. On Symbian Belle FP2
   the antivirus route no longer works; there `SafeManager` is used first and
   `RomPatcher+` after it.
2. **A PlayStation BIOS is strongly recommended.** Put a dump of your own
   console's BIOS (for example `scph7502.bin`, `scph1001.bin` or
   `scph5500.bin`) into `E:\PS1\bios\`. Without one the emulator warns before
   starting and falls back to a built-in replacement, which is less compatible
   and less accurate in timing.
3. **Games** are disc images made from your own discs, placed anywhere on the
   memory card; the folder `E:\PS1\games\` is the usual one.

## Installation

1. Download a package from the [Releases](../../releases) page. Two are
   offered:
   - `ps1emu_full.sis` for a phone that has never had this emulator. It also
     installs the application resource; without that resource the launch
     fails with the panic `USER 23`.
   - `ps1emu_min.sis` for a phone that already has the emulator installed. It
     replaces the program, the icon, the registration resource and the help
     texts.
2. Copy the package to the phone by USB cable, Bluetooth or memory card, open
   it and confirm the installation.
3. The emulator appears in the phone menu with its own icon.

Both packages install the English and Ukrainian help texts into
`E:\PS1\help\`. To remove the emulator, use the phone's application manager.
Save states, memory cards and settings in `E:\PS1\` stay on the card.

## Your first game

On the first launch the emulator opens a short setup page. After that it shows
its main menu: a single list with six sections, `Game`, `Video`, `Speed`,
`Sound`, `Controls` and `System`, and the rows `Help` and `Exit`. Only one
section is open at a time, and the line at the bottom of the screen explains
the highlighted row.

1. Open the `Game` section and select `Game`. A file browser opens; choose a
   disc image. If the game is still inside an archive, use `Add games...`
   instead, which finds archives and images on the card and converts them.
2. Select `Start` at the top of the list.
3. The right soft key opens the pause menu during the game. The top row then
   reads `Resume`, or `Restart game` if a setting marked with `*` was changed,
   because such settings take effect only after a restart.

Before starting, the emulator checks that a game is chosen and a BIOS is
present, and warns if either is missing.

### Menu keys

- **Up and down** move between rows and wrap around at the ends.
- **Right or OK** opens a section, or changes the value of a setting.
- **Left** closes the open section; opening another section closes it too.
- **Clear (C)** closes the section and puts the cursor on its header.

## Controls

The default mapping, laid out for the N95 8GB:

| PlayStation | Phone key | | PlayStation | Phone key |
|---|---|---|---|---|
| D-pad | arrow keys | | L1 / R1 | `1` / `3` |
| Cross | OK, or `5` | | L2 / R2 | `4` / `6` |
| Circle | `0` | | Select | `2` |
| Square | `7` | | Start | `8` |
| Triangle | `9` | | | |

During a game the right soft key opens the pause menu, the left soft key saves
a screenshot, and the side volume keys or `#` and `*` change the volume.

Every button can be reassigned in `Controls...`. Select a button and press OK
to bind a key, or press left or right to switch the button into hold mode,
marked `hold`: one press latches the button down and the next releases it,
which is what keeps the accelerator pressed through a race. `Control preset`
offers ready layouts (`Default`, `Racing`, `Fighting`, `RPG`), and
`Keyboard: C5` additionally maps `2`, `4`, `6` and `8` to the D-pad for the
C5-00.2's keypad.

### Tilt control

On phones with an accelerometer the game can be steered by tilting the phone.
`Tilt control` in the `Controls` section turns the accelerometer into the
analogue stick of a DualShock controller, which suits racing and flying games
best. The way the phone is held at the moment tilt control is switched on
becomes the neutral position, so it can be played lying on a table or held at
an angle. Four settings appear while it is on: which stick axis follows
sideways tilt, which follows forward-and-back tilt (either can be inverted or
switched off), the dead zone around the neutral position, and the sensitivity.
The buttons stay on their keys while tilt is on.

Tilt control works on the Nokia N95 8GB, where it has been tested. The E90 and
the C5-00.2 have no accelerometer the emulator can find; there the setting
switches itself back to `off`. Other phones with an accelerometer have not been
tried yet.

## Settings

Settings marked with `*` take effect after a game restart.

### Game

- **`Game` \***: choose a disc image.
- **`Save state`** and **`Load state`**, slots 1 to 5, shown while a game is
  running. Left and right choose the slot; `[*]` marks a used slot and `[ ]`
  an empty one. States are stored in `E:\PS1\saves\`.
- **`Add games...`**: find disc images and archives on the card and convert
  them to compressed `.pbp`.

### Video

- **`Smooth scale`** (default `on`): filters the picture when the game uses a
  640-pixel-wide mode, so that thin letters do not drop out.
- **`Edge smooth`** (`off`, `1` to `5`, default `off`): smooths the edges of 3D
  geometry.
- **`FPS meter`**: shows the frame rate and frame time on screen.
- **`3D lighting`** and **`Transparency`**: switch off lighting and
  semi-transparent effects to save time where a game allows it.
- **`Skip same frames`**: does not redraw a frame the game left unchanged.
- **`Direct draw` \***: writes the picture straight into the screen memory.
- **`GL output` \***: shows the picture through OpenGL ES on phones with a 3D
  chip. With it on, **`Software draw`** chooses whether the software
  rasteriser draws the polygons and the 3D chip only shows the result, or the
  3D chip draws them itself.
- **`Interlace hack`** and **`Pixel skip`**: draw only every other line, or skip
  pixels that cannot be seen in wide modes, trading picture quality for speed.

### Speed

- **`Frameskip`** (`off`, `auto`, `1` to `3`): in `auto` the emulator skips
  drawing a frame when it falls behind.
- **`CPU` \***: `dynarec` (the recompiler) or `interpreter`.
- **`GTE fast mode` \***: skips part of the geometry calculation. It changes
  the geometry, and therefore the picture, and is not always faster: in Gran
  Turismo it costs about 6%, because the rasteriser then gets more work.
- **`PS1 CPU speed`** (58% to 175%, default 100%): the speed of the emulated
  PlayStation processor. Raising it helps only games that wait for the
  processor; in Gran Turismo 25% more costs 2.5% of time and draws under 1%
  more geometry.

### Sound

- **`Sound` \***: switches sound emulation and output on or off.
- **`Preserve pitch` \*** (default `off`): when the game runs slower than real
  time, stretches the sound without lowering its pitch. It costs about 6% of
  speed on the N95.

### Controls

`Control preset`, `Keyboard`, `Controls...` and `Tilt control` with its four
adjustments, as described in [Controls](#controls).

### System

- **`Language`**: one of fifteen interface languages; by default the phone's
  own.
- **`BIOS` \***: choose a BIOS dump from `E:\PS1\bios\` or extract one from an
  archive.
- **`Per-game profile`**: keep separate settings and key bindings for every
  game in `E:\PS1\cfg\<game>.txt`.
- **`Reset settings`**: return all settings, keys and the language to their
  defaults after a second press of OK. Saves and memory cards are not touched.

## Disc image formats

- **Disc images**: `.bin`, `.img`, `.iso`, with track sheets `.cue`, `.toc`,
  `.ccd` or `.mds`.
- **Compressed images**: `.pbp` (the PSP EBOOT format) and `.cbn` / `.cbin`
  (compressed ISO). `.pbp` is the recommended one: it is smaller and cheaper to
  read on the phone.
- **Copy protection data**: `.sub` and `.sbi` (LibCrypt).
- **Archives for import**: `.zip`, `.7z`, and `.rar` with uncompressed content.
  Compressed RAR archives are not supported, because the unRAR licence
  conflicts with GPL-2.0.

CHD images are not supported.

Compression is done on the phone itself, so no computer is needed to prepare a
game. `Add games...` in the `Game` section searches the memory card for disc
images and archives, unpacks the archives and writes each game as a compressed
`.pbp` next to it. A `.pbp` takes about two thirds of the space of the raw
image (Gran Turismo: 454 MB instead of 694 MB) and costs only about 1.4% of
speed to read during play, as measured on the N95.

## Questions and problems

**The emulator closes at once with `USER 23`.** The application resource is
missing. Install `ps1emu_full.sis` instead of `ps1emu_min.sis`.

**The emulator warns about a missing BIOS.** No dump was found in
`E:\PS1\bios\`. The game can still be started on the built-in replacement, but
some games behave differently or do not start.

**A game is too slow.** Try `Frameskip: auto` first. Switching off
`3D lighting` or `Transparency` helps where the game still looks acceptable
without them. On the N95 8GB and E90 an overclock gives the largest gain; see
[Game speed](#game-speed).

**A game shows wrong graphics or freezes.** Try `CPU: interpreter` for that
game together with `Per-game profile`, so that the slower setting stays with
that one game.

# Part 2. How it works

## Running generated code under Symbian platform security

A dynamic recompiler writes machine code into memory and then executes it.
Since Symbian 9.1, platform security forbids this for ordinary memory. Two
platform-dependent parts of PCSX-ReARMed are therefore replaced:

| Purpose | Linux, as in upstream | Symbian |
|---|---|---|
| memory for translated code, writable and executable | `mmap(PROT_EXEC\|PROT_WRITE)` | `RChunk` with execute permission |
| making the processor see freshly written code | `__clear_cache` / `cacheflush` | `User::IMB_Range()` |

Everything else in the core is upstream code, compiled for ARMv6. NEON code
paths are excluded throughout, because none of the target processors has
NEON. The user interface, file browser, sound output and screen output are a
Symbian shell written for this port; the upstream `frontend/` and `libpicofe`
are not used.

## Drawing and showing a frame

The PlayStation GPU is emulated by `gpu_unai`, a C++ software rasteriser with
ARM-specific routines. On phones with the PowerVR MBX chip a second
rasteriser, `glrender`, can draw the polygons through OpenGL ES 1.1 instead; it
batches primitives, caches PlayStation video memory pages as textures and
invalidates only what the game overwrote.

The finished frame reaches the screen in one of two ways. Direct Screen Access
writes it straight into the screen memory, bypassing the window server; this
is the default and the only path on phones without a 3D chip. On phones with
the MBX chip the frame can instead be scaled and shown through OpenGL ES. In
both cases a frame the game did not change is not presented again.

## Where the time goes

The measurements behind the following were made on the phones themselves,
with the clock read from the processor registers.

On the N95 8GB at factory clock, about 62% of the frame time follows the
processor clock, 19% follows the memory bus clock and 19% follows neither. A
51% faster processor with the bus unchanged made Gran Turismo 16% faster in
wall time. In a real race, the software rasteriser takes about half of the
run; semi-transparent blending costs almost nothing.

The C5-00.2 is about 1.8 times faster than the N95 at emulation and
rasterisation, which matches the ratio of the processor clocks, but its screen
output costs 3.6 times as much as on the N95 and takes 18% of the run instead
of 3%. On that phone the output path is therefore where further work pays off.

Many games show far fewer pictures than the emulator produces frames, because
the game itself decides when to flip its picture, and it counts emulated time
to do so. In the Gran Turismo race replay the game shows a new picture on only
41% of the emulated frames, independently of the phone's speed. Frameskip with
a fixed pattern makes this worse, not better: drawing every second frame cut
the pictures actually seen from 14.4 to 9.0 per second. Only the adaptive
`auto` mode, which decides frame by frame, is worth using.

## How the game speed is measured

Each game has one saved state, taken during actual gameplay and checked by a
screenshot, and the same state was used on every phone and every clock. A run
loads the state, waits until the emulator is in the foreground, sets the
volume to zero and lets the game run for exactly 1200 PlayStation frames, with
no screenshots, dumps or saves during that time. The PlayStation speed is
1200 divided by the run time and by the frame rate of the region (60 for
NTSC). The shown frame rate counts the frames copied to the screen by direct
output, so it is the rate of screen updates; it does not prove that every
update showed a different picture.

Every game ran three times on each configuration. The emulator file was the alpha 1.3 release, identical on both phones and
checked by its SHA-256 hash. The N95's clock registers were read before and
after every series of three runs; only read-only probes were used during the
series. Runs spoiled by a Bluetooth dialog over the picture, by a game image
with a wrong name, or by sound that was not yet muted were excluded and their
logs kept. An earlier attempt to measure a third N95 configuration, 495 MHz
with the factory bus, lost the connection to the phone every time and has no
accepted results.

The full record, with every run, the raw logs, the register readings and the
gameplay screenshots, is in
[`docs/measurements/f11-game-speed-2026-10-05.md`](docs/measurements/f11-game-speed-2026-10-05.md)
and its `evidence` folder.

### What these numbers do not show

The first factory-clock series on the N95, on 2026-10-05, made the overclock
look worth 55% to 136%, far more than a 29% clock change can give. A repeat on
2026-10-06, on the same clock read back from the registers and with the same
states, settings, script and emulator file, ran every game faster: the first
series had taken 21% to 80% longer, depending on the game. The emulated work was identical: in Gran Turismo both series
executed exactly 733 371 181 PlayStation cycles and showed 601 frames, and only
the time differed. The table uses the repeat, and against it the overclock is
worth 28% to 32%.

The slow state is not a one-off. In the repeat, the first of three Wing
Commander III runs took 53.2 seconds against 25.4 and 25.1 for the other two,
and it was slow evenly from its first second, emulating about 44 million
PlayStation cycles per second instead of 94 million. That run was discarded
and the game repeated: 25.2, 25.5 and 25.1 seconds. What puts the phone into
this state, about twice as slow on the same clock, has not been established.
A single run on the N95 therefore proves little, and a series whose three runs
disagree by more than a few percent has to be repeated.

The E90 was not part of this series, and no game has been measured on the
Nokia 701.

## Known limitations

- **No NEON.** The ARM11 processors of the target phones do not have NEON, so
  all NEON code is excluded and the emulator is built for ARMv6.
- **Full speed is not guaranteed.** Emulating the 33.8 MHz PlayStation
  processor together with its geometry and sound chips is heavy work for a
  330 MHz phone. Of the twelve games measured, only Wing Commander III reaches
  full speed, and only on the C5-00.2 and an overclocked N95.
- **Help texts in two languages.** The interface has fifteen languages, but the
  long help text exists in English and Ukrainian only; other languages fall
  back to English for it.
- **The Nokia 701 is untested.**

## Translations

The interface is available in fifteen languages: English, Ukrainian, Chinese,
German, French, Spanish, Russian, Portuguese, Italian, Polish, Turkish,
Japanese, Hindi, Arabic and Korean. The menu texts live in
`src/symbian/i18n.cpp`; the help texts and menu hints are UTF-8 files in
`help/<lang>.txt`, installed to `E:\PS1\help\`. A missing text falls back to
English.

## Building from source

The build requires the S60 3rd Edition FP1 SDK, two GCCE compilers and a Perl
older than 5.22 (the SDK's scripts use `defined(%hash)`); Strawberry Perl
5.20 portable works.

- **GCCE 4.6.3** (`arm-2012.03`) builds the emulator core `libpcsxcore` and
  the rasteriser `gpu_unai`. Since 2026-10-02 this is the default: it gave a
  2% to 4% shorter frame with identical video memory on three phones.
- **CSL ARM Toolchain** (GCC 3.4.3, bundled with the SDK) builds the Symbian
  shell `stage1` and everything else described by `.mmp` files.
- The assembly files are in GAS syntax, so RVCT and `armasm` cannot be used.
- The core is the fork
  [`pcsx_rearmed-symbian`](https://github.com/1234bpv/pcsx_rearmed-symbian),
  branch `symbian`, checked out into `pcsx_rearmed/`.

The steps:

1. Set up the environment with `tools-patches/symbian-env.cmd`.
2. Apply `tools-patches/cl_bpabi.pm.patch` and
   `tools-patches/makdeps.pm.patch` to the SDK's `Epoc32\tools\`.
3. Generate `revision.h` in the core fork.
4. Build the core, the rasteriser and the application:
   ```powershell
   pwsh -NoProfile -File tools-patches/run.ps1 -Core -Gpu -Release -NoPush
   ```
5. Package and sign both installers:
   ```powershell
   pwsh -NoProfile -File tools-patches/pack.ps1
   ```
   `pack.ps1` refuses a benchmark build unless `-AllowBench` is given. On a
   clean clone there is no signing key in `configs\sign`, so it generates a
   self-signed one; that is a different identity from earlier builds, so
   remove an earlier installation from the phone first.

`run.ps1 -CoreCc csl343` builds the core with the older compiler for
comparison. The scripts, patches and the delivery of builds to a phone are
described in [`tools-patches/README.md`](tools-patches/README.md). During
development, builds are delivered over WiFi or Bluetooth by the neighbouring
project `SymbianDeployDaemon`.

## Further documentation

| File | Contents |
|---|---|
| [`pcsx-rearmed-symbian-port-plan.md`](pcsx-rearmed-symbian-port-plan.md) | The port plan and the reasons behind its architectural decisions. |
| [`docs/measurements/f11-game-speed-2026-10-05.md`](docs/measurements/f11-game-speed-2026-10-05.md) | The game speed series of 2026-10-05, with every run and its evidence. |
| [`docs/measurements/f6-compat-matrix.md`](docs/measurements/f6-compat-matrix.md) | Which of the twelve test games start, reach gameplay and at what speed. |
| [`tools-patches/README.md`](tools-patches/README.md) | Build scripts, SDK patches and delivery to the phone. |
| [`work/state.md`](work/state.md) | Current state of the work. |

## Contributing

This is a hobby project, developed in spare time on the author's own phones.
Bug reports, fixes and reports from other Symbian phones are welcome as issues
or pull requests. For a game that runs wrongly, the most useful report names
the phone, the game image and the settings, and says whether
`CPU: interpreter` changes anything.

## License

GPL-2.0, the licence of upstream
[PCSX-ReARMed](https://github.com/notaz/pcsx_rearmed). The changes to the core
in the fork remain under the same licence.
