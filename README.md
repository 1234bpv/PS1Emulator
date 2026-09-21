# PS1Emulator (PCSX-ReARMed for Symbian)

A PlayStation 1 (PS1) emulator port for Nokia smartphones running Symbian S60v3 and Symbian Belle. Based on the [PCSX-ReARMed](https://github.com/notaz/pcsx_rearmed) core by notaz, adapted for ARMv6 (ARM11) hardware and Symbian OS platform security requirements.

## 1. Overview

This project ports PCSX-ReARMed to Symbian OS devices powered by ARM11 processors. The primary goal is achieving playable emulation performance on resource-constrained mobile hardware through dynamic binary translation (MIPS R3000A to ARM) and optimized rendering pipelines.

To comply with Symbian platform security (Symbian OS 9.1 and newer), standard Linux memory management mechanisms have been replaced with platform-native equivalents:
- **Executable Translation Cache**: `RChunk` allocation with executable permissions replaces `mmap(PROT_EXEC|PROT_WRITE)`.
- **Instruction Cache Synchronization**: `User::IMB_Range()` replaces `__clear_cache` / `cacheflush` after code generation.

## 2. Supported Devices

| Device | Platform | SoC / CPU Core | Clock Speed | Graphics Hardware |
|---|---|---|---|---|
| **Nokia N95 8GB** *(Reference)* | S60v3 FP1, Symbian 9.2 | OMAP 2420, ARM1136 | 332 MHz | PowerVR MBX |
| Nokia E90 | S60v3 FP1, Symbian 9.2 | OMAP 2420, ARM1136 | 330 MHz | PowerVR MBX |
| Nokia C5-00.2 | S60v3 FP2 | ARMv6 | ~600 MHz | None (No 3D accelerator) |
| Nokia 701 | Symbian Belle | BCM2763, VideoCore IV | 1000 MHz | VideoCore IV (OpenGL ES 2.0) |

- **Nokia N95 8GB** is the primary reference and baseline test device. Hardware performance measurements directly translate to its architectural twin, the Nokia E90.
- **Nokia C5-00.2** lacks a dedicated 3D graphics accelerator. Software rasterization with direct screen writing (Direct Screen Access / CFbsBitmap) is fully supported for devices without GPU hardware.

## 3. Features

All listed features are implemented and verified in the codebase:

- **Dynamic Recompiler (Dynarec)**: Ari64 MIPS R3000A to ARM dynarec adapted for Symbian memory protection (`RChunk` and `User::IMB_Range()`). An interpreter fallback is also included.
- **Software GPU Rendering (`gpu_unai`)**: ARM-optimized C++ software rasterizer with custom assembly routines (excluding NEON).
- **Hardware GPU Rasterizer (`glrender`)**: OpenGL ES 1.1 hardware rasterizer designed for PowerVR MBX hardware, featuring primitive batching, VRAM page texture caching, and partial state cache invalidation.
- **Display Output Subsystem**:
  - Direct Screen Access (DSA) for direct framebuffer writes, bypassing Window Server blitting overhead.
  - Latch-based dirty frame filtering (`Skip same frames`) to avoid redundant presentation passes.
  - Hardware-accelerated OpenGL ES scaling and presentation.
- **Visual Enhancement Filters**:
  - `Smooth scale`: Bilinear downsampling filter for high horizontal resolution modes (such as 640x480 text menus).
  - `Edge smooth`: Fast FXAA-style edge smoothing post-processor for 3D geometry.
  - Performance hacks: `Interlace hack` (alternate scanline skipping), `Pixel skip` (skipping off-screen pixels in high-res modes), texture lighting modulation, and semi-transparency blending toggles.
- **Audio Subsystem**:
  - `dfsound` SPU emulation core outputting 16-bit stereo 44.1 kHz PCM.
  - Asynchronous audio playback via Symbian `CMdaAudioOutputStream` with dynamic rate pacing.
  - Hardware-level 10-step master volume control.
- **Timing and Frameskip**:
  - Adaptive frameskip advice mechanism synchronized with guest framebuffer swapping.
  - Configurable fixed frameskip levels.
  - Adjustable guest CPU cycle multiplier.
- **State Management**: Five save-state slots per game, written to and read from `E:\PS1\saves\`.
- **Localised Interface**: Fifteen interface languages, chosen in the menu or taken from the phone own language setting.
- **Input & Controls**:
  - Complete 14-button digital PlayStation controller mapping.
  - In-app key remapping interface with secondary key assignment.
  - Key toggle/hold mode for sustaining button states (e.g., vehicle acceleration).
  - Optional tilt control on phones with an accelerometer (Nokia N95): the tilt drives an analogue stick, with an adjustable dead zone, sensitivity, axis assignment and inversion. The position the phone is held in when it is switched on becomes the neutral one.
- **Native User Interface**:
  - Full-screen AVKON interface with custom rendering.
  - Built-in file browser with game auto-detection.
  - Per-game configuration profiles (`E:\PS1\cfg\<game>.txt`).
  - Real-time on-screen FPS and frame time performance overlay.
  - In-game screenshot capture.
- **On-Device Disc Import & Compression**:
  - Built-in conversion utility to extract and compress disc images into `.pbp` format directly on the device.

## 4. Supported Image Formats

### Disc Images and Metadata
- **Uncompressed Disc Images**: `.bin`, `.img`, `.iso`
- **Compressed Disc Images**:
  - `.pbp` (PlayStation Portable EBOOT format with 16-sector DEFLATE block compression)
  - `.cbn` / `.cbin` (Compressed ISO format)
- **Track Sheets and Metadata**: `.cue`, `.toc`, `.ccd`, `.mds`
- **Subchannel and Protection Data**: `.sub` (subchannel data), `.sbi` (LibCrypt copy protection data)

*Note: CHD format is not supported.*

### Archive Formats (On-Device Batch Import & Decompression)
- **`.zip`**: Store and DEFLATE compression via zlib.
- **`.7z`**: LZMA, LZMA2, and uncompressed streams via the LZMA SDK.
- **`.rar`**: Stored (uncompressed) streams in RAR4 and RAR5 formats. Compressed RAR archives are not supported due to licensing restrictions in the unRAR library that conflict with GPL-2.0.

## 5. Controls

### Default Key Mapping (Nokia N95 8GB Layout)

| PlayStation Button | Primary Key | Secondary Key |
|---|---|---|
| D-Pad Up | Up Arrow | - |
| D-Pad Down | Down Arrow | - |
| D-Pad Left | Left Arrow | - |
| D-Pad Right | Right Arrow | - |
| Cross | D-Pad Center / OK (Enter) | Key `5` |
| Circle | Key `0` | - |
| Square | Key `7` | - |
| Triangle | Key `9` | - |
| L1 | Key `1` | - |
| R1 | Key `3` | - |
| L2 | Key `4` | - |
| R2 | Key `6` | - |
| Select | Key `2` | - |
| Start | Key `8` | - |

### In-Game Shortcuts

- **Right Softkey (RSK)**: Open in-game pause and settings menu.
- **Left Softkey (LSK)**: Capture on-demand screenshot.
- **Side Volume Keys** or **Key `#` (Volume Up)** / **Key `*` (Volume Down)**: Adjust audio output volume.

### Controls Configuration
The `Controls...` menu allows assigning custom scan codes to each button and toggling the hold/toggle mode per button. Select a button and press OK to bind a key, or Left/Right to switch that button into hold mode, marked `hold` in the row. In hold mode one press latches the button down and the next press releases it, which is what keeps Cross held through a race without holding the key.

### Starting a Game
Only the `Start` item launches the game (it reads `Resume` once a game is running). OK on any other row opens the game manager, so a stray press cannot restart a race.

## 6. Settings

Settings are divided into logical sections within the menu. Items marked with `*` require an emulation restart to take effect when modified from the in-game pause menu.

### GAME Section
- **`Start` / `Resume` / `Restart game`**: Begins emulation, resumes the active game from pause, or restarts the session when restart-only settings have been altered.
- **`Game` \***: Opens the file browser to select a disc image (`.bin`, `.img`, `.iso`, `.pbp`, `.cbn`, `.cbin`).
- **`Save state`** / **`Load state`** (slots `1`-`5`): Saves or restores the whole machine state to `E:\PS1\saves\`, under a name derived from the disc image and the slot number. Both items appear only while a game is running; left and right pick the slot.
- **`Add games...`**: Launches the batch import and extraction utility to scan storage drives for disc images and archives (`.zip`, `.7z`, `.rar`, `.cue`) and convert them to compressed `.pbp` format.
- **`BIOS` \***: Selects a custom PlayStation BIOS ROM dump from `E:\PS1\bios\` or extracts one from an archive (default: `scph7502.bin`).
- **`Per-game profile`** (`on` / `off`): Enables storing and loading independent configuration settings and keymaps for each game under `E:\PS1\cfg\`.

### VIDEO Section
- **`3D lighting`** (`on` / `off`): Controls texture lighting and Gouraud shading modulation in `gpu_unai`.
- **`Transparency`** (`on` / `off`): Enables or disables semi-transparency blending.
- **`Interlace hack`** (`on` / `off`): Skips rendering alternate scanlines to boost software rasterization frame rates.
- **`Pixel skip`** (`on` / `off`): Skips rasterizing invisible pixels in high horizontal resolution modes.
- **`Smooth scale`** (`on` / `off`): Applies a bilinear downscaling filter to high-resolution display modes (such as 640-wide text) to prevent thin character strokes from dropping out.
- **`Edge smooth`** (`off`, `1`–`5`): Fast post-processing edge smoothing filter (FXAA). Higher values increase smoothing strength.
- **`Skip same frames`** (`on` / `off`): Dirty-only display presentation based on GPU updates. Skips redrawing unchanged frames to save blitting time (~6.7 ms per frame).
- **`Direct draw` \*** (`on` / `off`): Direct Screen Access (DSA / direct framebuffer writes), bypassing Window Server composite operations.
- **`GL output` \*** (`on` / `off`): Hardware-accelerated OpenGL ES scaling and presentation via PowerVR MBX.
- **`Software draw`** (`on` / `off`): Toggles software rasterization pass when OpenGL ES hardware rendering is active.

### SPEED Section
- **`CPU` \*** (`dynarec` / `interpreter`): Selects the CPU execution engine (Ari64 dynamic recompiler or interpreter).
- **`GTE fast mode` \*** (`on` / `off`): Skips GTE flag calculation in the dynamic recompiler. It changes the geometry the GTE produces, so it alters the picture and is not always faster: measured on Gran Turismo it costs about 6 percent, because the rasteriser then receives more work.
- **`Frameskip`** (`off`, `auto`, `1`–`3`): GPU rendering frameskip. In `auto` mode, the emulator dynamically drops frames based on timing advice.
- **`PS1 CPU speed`** (`58%` to `175%`, default `100%`): Speed of the emulated PlayStation processor, shown as a percentage where a larger number is faster. Internally it is the cycle multiplier - how many cycles one guest instruction is charged - which is why the steps are uneven. Measured on Gran Turismo, a 25 percent overclock costs 2.5 percent of wall time and buys under 1 percent more drawn geometry, so treat it as a per-game remedy rather than a general speed-up.
- **`FPS meter`** (`on` / `off`): Displays real-time frames per second and frame calculation time on screen.

### SOUND Section
- **`Sound` \*** (`on` / `off`): Enables or disables SPU sound emulation and audio output.

### CONTROLS Section
- **`Tilt control`** (`on` / `off`): Steers from the accelerometer. While it is on the controller reports itself as a DualShock and the tilt moves an analogue stick, which is what makes a partial turn possible at all. It reverts to `off` on a phone that has no sensor, so the switch never sits on `on` doing nothing. Four rows below it appear only while it is on:
- **`Tilt: sideways`** and **`Tilt: forward-back`**: where each axis of the sensor goes. `LX` and `LY` are the left stick, `RX` and `RY` the right one, a leading minus reverses the direction, and `off` unbinds the axis. The defaults, `LX` and `LY`, make the stick follow the tilt the way a marble on the screen would.
- **`Tilt dead zone`** (`0` to `40`): how far the phone may stray from the neutral position before the stick moves at all. The default is `10`, about two degrees.
- **`Tilt sensitivity`** (`40%` to `300%`): a larger number needs a smaller angle for full deflection. The default, `100%`, reaches the stops at about eighteen degrees.
- **`Controls...`**: Opens the button mapping menu to configure scan codes and hold/toggle flags for all 14 controller buttons.

### SYSTEM Section
- **`Language`**: Interface language. Fifteen languages are available; the initial value follows the phone own language setting.
- **`Reset settings`**: Restores all configuration settings to default values (requires confirmation).
- **`Exit`**: Shuts down the emulator and returns to the Symbian application launcher.

## 7. Building

### Prerequisites
- **SDK**: S60 3rd Edition FP1 SDK (`S60_3rd_FP1`).
- **Compiler**: CSL Arm Toolchain (GCC 3.4.3) or GCCE 4.6.3 (BPABI).
- **Perl**: Strawberry Perl 5.20.x portable (Perl 5.22+ is incompatible with Symbian SDK build scripts due to `defined(%hash)` syntax).
- **PCSX Core Fork**: `pcsx_rearmed-symbian` repository (`symbian` branch).

### Build Procedure
1. Set up the build environment using `tools-patches/symbian-env.cmd`.
2. Apply SDK patches (`tools-patches/cl_bpabi.pm.patch` and `tools-patches/makdeps.pm.patch`) to `Epoc32\tools\`.
3. Generate the core revision header `revision.h` in the PCSX core fork.
4. Compile the core library (`pcsxcore.lib`) and GPU library (`gpuunai.lib`):
   ```cmd
   cd configs\core\group
   bldmake bldfiles
   call .\ABLD.BAT build gcce urel
   ```
5. Compile the `stage1` application executable:
   ```cmd
   cd configs\stage1\group
   bldmake bldfiles
   call .\ABLD.BAT build gcce urel
   ```
6. Package and sign the SIS installer in one step:
   ```cmd
   pwsh -NoProfile -File tools-patches\pack.ps1
   ```
   This produces two self-signed packages. It refuses a harness build: run
   `run.ps1 -Release` first, or pass `-AllowBench` when the point is to try the
   installer rather than to ship.

Detailed build scripts, patches, and deployment instructions are documented in [tools-patches/README.md](tools-patches/README.md).

### Installing on a Phone

Both packages are self-signed, so the phone must be hacked to accept them: `Dr Web RomPatcher plus light` with the `Open4all`, `universal` and `Install server` patches on S60v3 up to FP1, and `SafeManager` followed by `RomPatcher+` on Symbian Belle FP2, where the antivirus route no longer works.

- `ps1emu_min.sis` carries the executable and its registration resource. Use it on a phone that already holds `\resource\apps\ps1stage1.rsc` — every phone the deploy daemon has set up does.
- `ps1emu_full.sis` carries the application resource as well. Use it on a phone that has never had this application: without that resource the launch panics with `USER 23`.

Both are built in one step by `tools-patches\pack.ps1`, which refuses a harness build unless you pass `-AllowBench`. On a clean clone there is no signing pair in `configs\sign`, so it generates a self-signed one and writes the passphrase next to it. That is a different identity from the one used before, so remove any earlier install from the phone first.

## 8. Known Limitations

- **No ARM NEON Instructions**: The target ARMv6 architecture (ARM1136, BCM2763) does not support NEON SIMD extensions. All NEON code paths are excluded from compilation.
- **ARMv6 Instruction Set**: Compiled specifically for ARMv6 processors.
- **PlayStation BIOS Dump Required**: A valid PS1 BIOS ROM image (such as `scph7502.bin`, `scph1001.bin`, or `scph5500.bin`) must be provided by the user in `E:\PS1\bios\`.
- **Hardware Performance**: Emulating a 33.8 MHz MIPS R3000A CPU along with GTE and SPU on ~330 MHz ARM11 hardware (OMAP 2420) is resource-demanding. While many games achieve playable frame rates using the dynamic recompiler, frameskip, and hardware rasterization, full-speed (60 FPS) execution across all games is not guaranteed.

## 9. License

This project is licensed under the **GNU General Public License v2.0 (GPL-2.0)**, matching the license of upstream [PCSX-ReARMed](https://github.com/notaz/pcsx_rearmed).
