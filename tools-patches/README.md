# tools-patches — Build Infrastructure

Without this, the build is **impossible to reproduce** on another machine. Project docs and configs are local (CLAUDE.md §8), so the most critical items are collected here.

## Contents

| File | Purpose |
|---|---|
| `symbian-env.cmd` | build environment: `EPOCROOT`, `PATH`, drive D: |
| `deploy.cmd` | build → sign → copy to device → collect logs (single command) |
| `cl_bpabi.pm.patch` | SDK fix: vista patch without hardcoded path to CSL |
| `makdeps.pm.patch` | SDK fix: preprocessor for dependency scanning |
| `mmp-core/` | `bld.inf` + `pcsxcore.mmp` for the core (Stage 1) |
| `mmp-gpu/` | `gpuunai.mmp` — gpulib + gpu_unai without assembler (Stage 2) |
| `mmp-stage0/` | `stage0.mmp` — executable memory probe (Stage 0) |
| `mmp-stage1/` | `stage1.mmp` — wrapper linking the core and GPU |

The deployment daemon together with the PC client `deploy_net.ps1` lives as a separate project one
level up — `..\..\SymbianDeployDaemon` — because it is needed by any application for
Symbian, not just this one. Build it from there; `deploy.cmd ... wifi` invokes the client
at this path.

## Prerequisites

1. **S60 3rd Edition FP1 SDK** at `D:\VsCodeProjects\SymbianProjects\S60_3rd_FP1`
2. **CSL Arm Toolchain** (GCC 3.4.3, SDK bundle) at `D:\VsCodeProjects\SymbianProjects\toolchain\CSL_Arm_Toolchain`
3. **Strawberry Perl 5.20.x portable** at `D:\VsCodeProjects\SymbianProjects\toolchain\perl`
   Version is critical: `E32Plat.pm:289,387` use `defined(%hash)`, which is **fatal with Perl 5.22**. Msys-perl from Git Bash is fundamentally unsuitable.
4. Core fork — `github.com/1234bpv/pcsx_rearmed-symbian`, branch `symbian`

## Applying SDK Patches

```cmd
cd D:\VsCodeProjects\SymbianProjects\S60_3rd_FP1\Epoc32\tools
copy cl_bpabi.pm cl_bpabi.pm.orig
copy makdeps.pm  makdeps.pm.orig
patch -u cl_bpabi.pm < <path>\cl_bpabi.pm.patch
patch -u makdeps.pm  < <path>\makdeps.pm.patch
```

Patches are constructed as `diff -u <file>.orig <file>`, so they are applied to the **stock** SDK files.

### Why Each is Needed

**`cl_bpabi.pm`** — official vista patch is applied, but with fixes. As shipped, the patch contains a hardcoded `C:\Program Files\CSL Arm Toolchain\` in three functions (`GCCInstallPath`, `GCCLibPath`, `GCCRuntimePath`); it triggers if the installation path does not literally contain `CSL Arm Toolchain` (in our case — `CSL_Arm_Toolchain`, with underscores), and the build breaks. The hardcoded path has been removed — the path is determined from the compiler itself. Additionally, lines 207-209 were aligned with line 240: the former expected the root, the latter — `...\bin`.

**`makdeps.pm`** — two changes. (1) Dependency scanning is performed by cpp from CSL 3.4.3: stock `Epoc32\gcc\bin\cpp.exe` is a cygwin build from 2003 that crashes on C99 variadic macros (`libpcsxcore/system.h:51`). (2) Unescaping `\\` in the `-M` output of this cpp.

`Preprocessor.pm` is deliberately **not** touched: `prepfile.pm`/`epocrc.pl` invoke cpp with the `-+` flag, which CSL-cpp does not accept.

## Generated `revision.h`

`libpcsxcore/misc.c:38` includes `"revision.h"`, which upstream generates via Makefile and **ignores in git** (`.gitignore:15`). In a fresh clone it is missing → `misc.c:691: parse error before "REV"`. Create it manually:

```cmd
cd <fork>
git describe --always > tmp && set /p R=<tmp && echo #define REV "%R%" > include\revision.h && del tmp
```
or simply write a line like `#define REV "f8c6f9c"`.

## Building the Core

```cmd
call symbian-env.cmd
cd <...>\configs\core\group
bldmake bldfiles
call .\ABLD.BAT build gcce urel
```
Expected result — `Epoc32\release\armv5\urel\pcsxcore.lib` (~685 KB, 42 object files).

Invoke `abld` specifically as `call .\ABLD.BAT` — cmd cannot find plain `abld`.

## After Modifying the Core, Always Rebuild EXE with `reallyclean`

`abld` tracks only the target's own source files. Modifying `pcsxcore.lib` or `gpuunai.lib` **does not trigger** relinking: `abld build` for `stage1` silently does nothing, and the EXE remains with the old library inside.

```cmd
cd <...>\configs\core\group  & call .\ABLD.BAT build gcce urel
cd <...>\configs\stage1\group & call .\ABLD.BAT reallyclean gcce urel & bldmake bldfiles & call .\ABLD.BAT build gcce urel
```

There is one check: **the modification time of `ps1stage1.exe` must be later than that of `pcsxcore.lib`.** Otherwise, a build without your changes will be deployed to the device, and debugging will go down the wrong path — the fix will look as if it had no effect.

## Packaging SIS for a Physical Device

```cmd
call symbian-env.cmd
cd <...>\configs\stage0\group
makesis stage0.pkg ps1stage0.sis
openssl genrsa -out sha1.key 1024
openssl req -new -x509 -sha1 -key sha1.key -out sha1.cer -days 3650 -subj "/CN=PS1Emulator Dev"
signsis -s ps1stage0.sis ps1stage0_sha1.sis sha1.cer sha1.key
signsis -o ps1stage0_sha1.sis
```

**The certificate must be RSA + SHA-1, and openssl generates it, not `makekeys`.** This was verified on Nokia N95 8GB (S60v3 FP1, Symbian 9.2, firmware v20.0.016 and v35.0.001): SHA-1 installs, SHA-256 results in **"file corrupted"**.

Symbian 9.2 is from 2006, its SWInstall does not know `sha256WithRSAEncryption`. The certificate is not parsed at all, so the installer does not even reach the validity date check and reports file corruption. The error points to the package contents even though only the certificate is broken — it is easy to waste a lot of time on this misleading hint.

Diagnostic rule distinguishing the two cases:
- **"file corrupted"** — certificate was not parsed (SHA-256, or DSA from `makekeys`);
- **"expired certificate"** — certificate was parsed, device reached the expiration date. This is how original PIPS 1.7 behaves, signed by Nokia via Symbian CA until 2020-08-05.

Reference standard for verification — Ensymble certificate (`ensymble/utils/defaultcert.py`, zlib+base64): RSA-1024, `sha1WithRSAEncryption`, 2006-2036. A package signed with it installs, so when in doubt, it is used to check whether the issue is with the certificate or the SIS itself.

`makekeys` from this SDK is fundamentally unsuitable: it only produces DSA (no way to specify RSA), and DSA should not be relied upon. The example in its own help suggests 2048 bits — it was designed for a newer version with RSA.

**`-s` is required.** Without it, `signsis` silently does not sign and the return code is zero — appearing as success. There is one check: `signsis -o` must print `Issued by`/`Issued to`; empty output means an unsigned file.

**`.pkg` must have CRLF line endings and be in ASCII.** With LF, `makesis` silently fails to see the vendor block and crashes with `No vendor names found` — the message points to the wrong cause. Editors saving in LF break the build invisibly.

The key and certificate (`*.key`, `*.cer`) are not tracked in git — they are generated locally using the command above. Self-signing is sufficient because the UID is in the range `0xE0000000-0xEFFFFFFF` (development) and `CAPABILITY None`.

## Deploying to Device: `deploy.cmd`

Automates the entire cycle to avoid manually repeating steps, each of which has a silent failure mode:

```cmd
deploy.cmd stage1        REM core+gpu+stage1 -> reallyclean -> signsis -> SIS to phone
deploy.cmd stage0        REM stage0 -> signsis -> SIS to phone
deploy.cmd stage1 fast   REM EXE only + recalculate \sys\hash (without SIS reinstallation)
deploy.cmd collect       REM retrieve ps1_*.jsonl + ps1_vram.bin into plans\runs\<timestamp>\
```

Connect the phone in **Mass storage** mode — the memory card mounts as a removable drive and
is detected automatically by the presence of `card_content.cid` in the root.

The script exists because of three traps, each having cost an entire session:
1. `abld` does not relink the EXE when only `.lib` changed → `reallyclean` is performed for stage1.
2. `signsis` without `-s` returns 0 and writes an **unsigned** file → verified via `signsis -o`
   (issuer must be present).
3. An old build on the device makes a correct fix look "ineffective" → EXE timestamp is compared
   against `pcsxcore.lib`/`gpuunai.lib`, and a stale build halts deployment.

Signing key — `configs\sign\sha1.{key,cer}` (the same RSA/SHA-1; not committed, if
missing it is generated automatically using the same recipe as above).

**`fast` mode is deprecated.** The idea was to overwrite the exe in `\sys\bin` via mass storage, but
S60v3 verifies the removable media exe against the hash on the **system drive `C:\sys\hash\<name>`**, and C:
is unreachable from PC in mass storage mode (only card E: is visible). Without updating this hash,
the loader rejects with `-46`. In-place replacement is reliably done only by the daemon over WiFi (`wifi`),
because it runs on the phone and has access to C:. `deploy.cmd ... fast` now only displays this
explanation and redirects to `wifi`.

## WiFi Deployment via Daemon

`ps1daemon.exe` (project `..\..\SymbianDeployDaemon`) is a resident application on the phone
that receives files and launches programs over TCP. It covers the full cycle
**without touching the screen**: the keypress itself before a run corrupted measurements (the same code yielded
9,326 and 22,314 ms depending on the background).

One-time setup:
1. `..\..\SymbianDeployDaemon\tools\build.cmd` — build and sign `ps1daemon_sha1.sis`;
   place it on the card (first installation is done from the device — see daemon README).
2. Install it on the phone, launch `PS1 Deploy Daemon`. On startup it may ask once
   for an access point — select home WiFi. The IP and port (2323) will appear on the screen;
   the same IP is written to `E:\daemon_ip.txt`.

Then every iteration without cable modes:
```cmd
deploy.cmd stage1 wifi 192.168.x.y     REM build -> PUT exe (+hash on C:) -> RUN -> GET log
```
The IP can be omitted if cached (`deploy.cmd collect` copies `daemon_ip.txt` from
the card to `tools-patches\.daemon_ip`). The fourth argument is the application argument string;
**empty by default**, in which case stage1 performs a full benchmark (both modes +
`compare`). An argument like `drc` would run a single pass. The log is saved to `plans\runs\<timestamp>_wifi\`.

The client uploads the removable media hash to `C:\sys\hash\ps1stage1.exe` (not on the card) — that is where
the loader looks for it. Verified on live WiFi: without it, `RUN` results in `-46`.

Protocol (ASCII string + raw bytes): `PUT <path> <size>`, `GET <path>`,
`RUN <exe> <arguments>` (daemon waits for completion and returns the exit code — meaning the radio stays idle
during computation), `STOP`/`BYE`. Client implementation —
`..\..\SymbianDeployDaemon\tools\deploy_net.ps1`.

Daemon listens, PC connects — so the PC firewall does not interfere. `CAPABILITY NetworkServices
ReadUserData` (self-signed set). Writing to `\sys\bin` with `wifi`/`fast` relies on
Open4All (hack), which removes `\sys` protection.
