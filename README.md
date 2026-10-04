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
  - Pitch-preserving time-scale stretching via WSOLA (`Preserve pitch`) when emulation drops below real-time speed.
  - Hardware-level 10-step master volume control.
- **Timing and Frameskip**:
  - Adaptive frameskip advice mechanism synchronized with guest framebuffer swapping.
  - Configurable fixed frameskip levels.
  - Adjustable guest CPU cycle multiplier.
- **State Management**: Five save-state slots per game, written to and read from `E:\PS1\saves\`.
- **Localised Interface**: Fifteen interface languages, chosen in the menu or taken from the phone's own language setting.
- **Input & Controls**:
  - Complete 14-button digital PlayStation controller mapping with ready-made presets (Default, Racing, Fighting, RPG).
  - In-app key remapping interface with secondary key assignment and C5 directional keypad options.
  - Key toggle/hold mode for sustaining button states (e.g., vehicle acceleration).
  - Optional tilt control on phones with an accelerometer (Nokia N95): the tilt drives an analogue stick, with an adjustable dead zone, sensitivity, axis assignment and inversion. The position the phone is held in when it is switched on becomes the neutral one.
- **Native User Interface**:
  - Dedicated application icon in the phone menu (multi-resolution 88x88, 44x44, and 22x22).
  - Full-screen AVKON interface with custom rendering.
  - Two-level accordion menu in a single list with contextual status line hints for each item (localized, English fallback).
  - Built-in help reader with word wrapping and scrolling (`E:\PS1\help\<lang>.txt`).
  - First-time setup wizard on initial launch when configuration is uninitialized (`intro=1`).
  - Pre-start verification with warnings if no game is selected or when running on the built-in HLE BIOS replacement.
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

### Menu Navigation

- **Up / Down**: Navigate through items with wrap-around between first and last rows.
- **Right / OK**: Expand accordion section header, or cycle/toggle a setting value.
- **Left**: Collapse the open accordion section (opening another section closes the active one).
- **Clear (C)**: Collapse active section and return cursor to its header.
- **Status Line**: Displays a contextual explanatory hint for the highlighted item in the active interface language (English fallback).

### Controls Configuration
The `Controls...` menu allows assigning custom scan codes to each button and toggling the hold/toggle mode per button. Select a button and press OK to bind a key, or Left/Right to switch that button into hold mode, marked `hold` in the row. In hold mode one press latches the button down and the next press releases it, which is what keeps Cross held through a race without holding the key.

### Starting a Game
The top-level execution item launches the game: it displays `Start` before emulation has begun, `Resume` once a game is running, or `Restart game` if restart-only settings marked with `*` were modified. OK on any other row opens or alters that row, preventing accidental session restarts. Before starting, the emulator checks for a valid game disc image and BIOS dump; if no game is selected or if no BIOS file is found in `E:\PS1\bios\`, a warning prompt is displayed (falling back to the built-in HLE BIOS replacement if accepted). On initial launch when configuration is uninitialized (missing `intro=1`), the first-time setup page opens instead.

## 6. Settings

The settings menu uses a two-level accordion structure in a single list. The top level contains the execution command (`Start` / `Resume` / `Restart game`), six expandable sections (`Game`, `Video`, `Speed`, `Sound`, `Controls`, `System`), followed by `Help` and `Exit`. Only one section is open at a time. The status line at the bottom displays a localized hint for the highlighted row (`help/<lang>.txt`, with English fallback). Items marked with `*` require an emulation restart to take effect when modified from the in-game pause menu.

### Top-Level Items
- **`Start` / `Resume` / `Restart game`**: Begins emulation, resumes the active game, or restarts the session when restart-only settings (`*`) have been changed. Displays warnings if no game is selected or if no BIOS file is present.
- **`Help`**: Opens the built-in help viewer with word wrapping and smooth scrolling, loading topic texts from `E:\PS1\help\<lang>.txt`.
- **`Exit`**: Saves settings and terminates the emulator.

### GAME Section
- **`Game` \***: Opens the file browser to select a disc image (`.bin`, `.img`, `.iso`, `.pbp`, `.cbn`, `.cbin`). Changing it requires a game restart.
- **`Save state`** (slots `1`–`5`): Saves the running machine state to `E:\PS1\saves\`, using a file name derived from the disc image and slot. Visible only while a game is running. Left/Right selects the slot; `[*]` marks an occupied slot and `[ ]` marks an empty slot.
- **`Load state`** (slots `1`–`5`): Restores machine state from the selected slot. Visible only while a game is running.
- **`Add games...`**: Launches the batch import and extraction utility to scan storage drives for disc images and archives (`.zip`, `.7z`, `.rar`, `.cue`) and convert them to compressed `.pbp` format.

### VIDEO Section
- **`Smooth scale`** (`on` / `off`, default `on`): Bilinear downsampling filter for high horizontal resolution modes (such as 640-wide text) to prevent thin character strokes from dropping out.
- **`Edge smooth`** (`off`, `1`–`5`, default `off`): Fast FXAA-style edge smoothing post-processor for 3D geometry. Higher levels increase smoothing strength.
- **`FPS meter`** (`on` / `off`): Displays real-time frames per second and frame calculation time overlaid on screen.
- **`3D lighting`** (`on` / `off`): Controls texture lighting and Gouraud shading modulation in `gpu_unai`.
- **`Transparency`** (`on` / `off`): Enables or disables semi-transparency blending (glass, glows, shadows).
- **`Skip same frames`** (`on` / `off`): Dirty-only display presentation based on GPU updates. Skips redrawing unchanged frames to save blitting time (~6.7 ms per frame).
- **`Direct draw` \*** (`on` / `off`): Direct Screen Access (DSA / direct framebuffer writes), bypassing Window Server composite operations (~0.93 ms saved per frame).
- **`GL output` \*** (`on` / `off`): Hardware-accelerated OpenGL ES scaling and presentation via PowerVR MBX.
- **`Software draw`** (`on` / `off`): Available when `GL output` is active. When `on`, the software rasterizer renders the frame and GL presents it; when `off`, the graphics chip rasterizes primitives.
- **`Interlace hack`** (`on` / `off`): Skips rendering alternate scanlines to boost software rasterization frame rates.
- **`Pixel skip`** (`on` / `off`): Skips rasterizing invisible pixels in high horizontal resolution modes.

### SPEED Section
- **`Frameskip`** (`off`, `auto`, `1`–`3`): GPU rendering frameskip. In `auto` mode, the emulator dynamically drops frames based on timing advice. Fixed levels (1–3) skip that many frames between drawn ones.
- **`CPU` \*** (`dynarec` / `interpreter`): Selects the CPU execution engine (Ari64 dynamic recompiler or interpreter fallback).
- **`GTE fast mode` \*** (`on` / `off`): Skips GTE flag calculation in the dynamic recompiler. It changes the geometry the GTE produces, so it alters the picture and is not always faster: measured on Gran Turismo it costs about 6 percent, because the rasteriser then receives more work.
- **`PS1 CPU speed`** (`58%` to `175%`, default `100%`): Speed of the emulated PlayStation processor, shown as a percentage where a larger number is faster. Internally it is the cycle multiplier - how many cycles one guest instruction is charged - which is why the steps are uneven. Measured on Gran Turismo, a 25 percent overclock costs 2.5 percent of wall time and buys under 1 percent more drawn geometry, so treat it as a per-game remedy rather than a general speed-up.

### SOUND Section
- **`Sound` \*** (`on` / `off`): Enables or disables SPU sound emulation and audio output.
- **`Preserve pitch` \*** (`on` / `off`, default `off`): When emulation drops below full speed, stretches sound without changing pitch using WSOLA (Waveform Similarity Overlap-Add). Costs ~6% emulation performance on Nokia N95.

### CONTROLS Section
- **`Control preset`**: Ready-made key layout presets (`Default`, `Racing`, `Fighting`, `RPG`). Selecting a preset replaces current key assignments.
- **`Keyboard`**: Base keypad layout selection (`N95` or `C5`). `N95` is default; `C5` additionally maps numeric keys `2`, `4`, `6`, `8` as secondary D-pad direction controls.
- **`Controls...`**: Opens the button mapping menu to configure primary/secondary scan codes and hold/toggle flags for all 14 controller buttons.
- **`Tilt control`** (`on` / `off`): Steers from the accelerometer on supported devices (Nokia N95). Reports as a DualShock controller and moves an analog stick. Reverts to `off` on devices without an accelerometer. The following four settings appear only while tilt control is enabled:
  - **`Tilt: sideways`** (`LX`, `LY`, `RX`, `RY`, inverted, `off`, default `LX`): Stick axis mapped to sideways phone tilt.
  - **`Tilt: forward-back`** (`LX`, `LY`, `RX`, `RY`, inverted, `off`, default `LY`): Stick axis mapped to forward/backward phone tilt.
  - **`Tilt dead zone`** (`0` to `40`, default `10`): Neutral dead zone threshold before stick displacement begins.
  - **`Tilt sensitivity`** (`40%` to `300%`, default `100%`): Analog stick deflection sensitivity.

### SYSTEM Section
- **`Language`**: Interface language selection (15 languages available; initial value follows the phone's language setting). Menu labels and help texts switch immediately.
- **`BIOS` \***: Selects a PlayStation BIOS ROM dump from `E:\PS1\bios\` or extracts one from an archive (default: `scph7502.bin`). When no dump is present, emulation falls back to the built-in HLE BIOS replacement.
- **`Per-game profile`** (`on` / `off`): Enables storing and loading independent configuration settings and keymaps for each game under `E:\PS1\cfg\<game>.txt`.
- **`Reset settings`**: Restores all configuration settings, keys, and language to default values (requires confirmation, press OK twice). Existing saves and memory cards are not touched.

## 7. Building

### Prerequisites
- **SDK**: S60 3rd Edition FP1 SDK (`S60_3rd_FP1`).
- **Compilers**:
  - **GCCE 4.6.3** (`arm-2012.03` BPABI) for the core library (`libpcsxcore`) and GPU library (`gpu_unai`).
  - **CSL Arm Toolchain** (GCC 3.4.3, SDK bundle) for the `stage1` application shell and `.mmp` components.
- **Perl**: Strawberry Perl 5.20.x portable (Perl 5.22+ is incompatible with Symbian SDK build scripts due to `defined(%hash)` syntax).
- **PCSX Core Fork**: `pcsx_rearmed-symbian` repository (`symbian` branch).

### Build Procedure
As of 2026-10-02, the core library (`libpcsxcore`) and the GPU library (`gpu_unai`) are built by default with GCCE 4.6.3 via `tools-patches/core463-regen.cmd` and `tools-patches/gpu463-regen.cmd` (reducing frame time by 2–4% with identical VRAM output across test devices). The `stage1` shell executable is compiled with CSL Arm Toolchain GCC 3.4.3. The parameter `-CoreCc csl343` in `run.ps1` remains available for benchmark comparisons.

1. Set up the build environment using `tools-patches/symbian-env.cmd`.
2. Apply SDK patches (`tools-patches/cl_bpabi.pm.patch` and `tools-patches/makdeps.pm.patch`) to `Epoc32\tools\`.
3. Generate the core revision header `revision.h` in the PCSX core fork.
4. Compile the libraries and application via `tools-patches/run.ps1`:
   ```powershell
   pwsh -NoProfile -File tools-patches/run.ps1 -Core -Gpu -Release -NoPush
   ```
   Alternatively, invoke individual regeneration scripts:
   - Run `tools-patches/core463-regen.cmd` to build `pcsxcore.lib`.
   - Run `tools-patches/gpu463-regen.cmd` to build `gpuunai.lib`.
   - Build `stage1` via `configs\stage1\group\ABLD.BAT build gcce urel`.
5. Package and sign the SIS installer in one step:
   ```powershell
   pwsh -NoProfile -File tools-patches/pack.ps1
   ```
   This produces two self-signed packages. It refuses a harness build: run
   `run.ps1 -Release` first, or pass `-AllowBench` when testing the installer.

Detailed build scripts, patches, and deployment instructions are documented in [tools-patches/README.md](tools-patches/README.md).

### Installing on a Phone

Both packages are self-signed, so the phone must be hacked to accept them: `Dr Web RomPatcher plus light` with the `Open4all`, `universal` and `Install server` patches on S60v3 up to FP1, and `SafeManager` followed by `RomPatcher+` on Symbian Belle FP2, where the antivirus route no longer works.

- `ps1emu_min.sis` carries the executable, application icon (`ps1stage1_aif.mbm`), registration resource and the help texts. Use it on a phone that already holds `\resource\apps\ps1stage1.rsc` — every phone the deploy daemon has set up does.
- `ps1emu_full.sis` carries the application resource as well. Use it on a phone that has never had this application: without that resource the launch panics with `USER 23`.

Both are built in one step by `tools-patches\pack.ps1`, which refuses a harness build unless you pass `-AllowBench`. On a clean clone there is no signing pair in `configs\sign`, so it generates a self-signed one and writes the passphrase next to it. That is a different identity from the one used before, so remove any earlier install from the phone first.

**Help texts**: both packages install `help/en.txt` and `help/uk.txt` to `E:\PS1\help\`. The help viewer and the menu hints read `E:\PS1\help\<lang>.txt` (UTF-8) and fall back to English when a language file or entry is missing. A phone set up through the deploy daemon needs these two files copied there by hand.

## 8. Known Limitations

- **No ARM NEON Instructions**: The target ARMv6 architecture (ARM1136, BCM2763) does not support NEON SIMD extensions. All NEON code paths are excluded from compilation.
- **ARMv6 Instruction Set**: Compiled specifically for ARMv6 processors.
- **PlayStation BIOS Dump**: A valid PS1 BIOS ROM image (such as `scph7502.bin`, `scph1001.bin`, or `scph5500.bin`) placed in `E:\PS1\bios\` is strongly recommended. Without a BIOS dump, the emulator issues a warning before launch and runs on its built-in HLE replacement with lower compatibility and timing fidelity.
- **Hardware Performance**: Emulating a 33.8 MHz MIPS R3000A CPU along with GTE and SPU on ~330 MHz ARM11 hardware (OMAP 2420) is resource-demanding. While many games achieve playable frame rates using the dynamic recompiler, frameskip, and hardware rasterization, full-speed (60 FPS) execution across all games is not guaranteed.

## 9. License

This project is licensed under the **GNU General Public License v2.0 (GPL-2.0)**, matching the license of upstream [PCSX-ReARMed](https://github.com/notaz/pcsx_rearmed).
