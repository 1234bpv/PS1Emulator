@echo off
setlocal EnableDelayedExpansion
REM ---------------------------------------------------------------------------
REM deploy.cmd - build, sign, deploy to the device and collect logs back.
REM
REM   deploy.cmd stage1          build core+gpu+stage1, sign, copy SIS to phone
REM   deploy.cmd stage0          build stage0, sign, copy SIS to phone
REM   deploy.cmd stage1 fast     copy the EXE only (needs the SIS installed once
REM                              before, and the loader check lifted - see below)
REM   deploy.cmd stage1 wifi [ip] [runargs]
REM                              over WiFi: push EXE, launch it, pull the log back -
REM                              no mass-storage juggling, no screen tap. Needs the
REM                              daemon running on the phone. ip defaults to the value
REM                              cached from the card (E:\daemon_ip.txt) on last collect,
REM                              then to "scan" - auto-discovering the daemon on the LAN.
REM                              Pass "scan" explicitly to force discovery. A stale
REM                              given/cached ip also falls back to a scan automatically.
REM   deploy.cmd collect         pull *.jsonl and ps1_vram.bin off the phone
REM
REM Mass-storage modes (stage*, fast, collect) need the phone in "Mass storage"
REM USB mode; the memory card then appears as a removable drive and is found
REM automatically. The wifi mode needs the phone OFF mass storage and the daemon
REM running instead.
REM
REM The daemon itself lives one level up, in ..\..\SymbianDeployDaemon - it is
REM shared with every other Symbian project here. Build and install it from
REM there; this script only talks to it, through that project's deploy_net.ps1.
REM
REM Three traps this script exists to close, all of them silent failures that
REM cost a debugging session each:
REM   1. abld does not relink the EXE when only a .lib changed -> reallyclean.
REM   2. signsis without -s returns 0 and produces an UNSIGNED file.
REM   3. a stale EXE on the phone makes a correct fix look ineffective.
REM ---------------------------------------------------------------------------

for %%I in ("%~dp0..") do set "PROJ=%%~fI"
for %%I in ("%PROJ%\..") do set "SYMBIANROOT=%%~fI"
set CONFIGS=%PROJ%\configs
set SIGNDIR=%CONFIGS%\sign
set RELDIR=%SYMBIANROOT%\S60_3rd_FP1\Epoc32\release\gcce\urel
set LIBDIR=%SYMBIANROOT%\S60_3rd_FP1\Epoc32\release\armv5\urel
set OPENSSL=C:\Program Files\Git\usr\bin\openssl.exe

set TARGET=%~1
set MODE=%~2
if "%TARGET%"=="" set TARGET=stage1

if /i "%TARGET%"=="collect" goto :find_phone

if /i "%TARGET%"=="daemon" (
  echo [deploy] the daemon moved out: build it in ..\..\SymbianDeployDaemon
  echo [deploy]   ..\..\SymbianDeployDaemon\tools\build.cmd
  exit /b 2
)
if /i not "%TARGET%"=="stage0" if /i not "%TARGET%"=="stage1" (
  echo [deploy] unknown target "%TARGET%" - expected stage0, stage1 or collect
  exit /b 2
)

call "%~dp0symbian-env.cmd"
if errorlevel 1 exit /b 1

REM --- build ----------------------------------------------------------------
if /i "%TARGET%"=="stage1" (
  echo.
  echo [deploy] === libpcsxcore ===
  pushd "%CONFIGS%\core\group"
  REM reallyclean so this always wins: core463-regen.cmd (opt-in experiment) drops
  REM a GCCE 4.6.3 pcsxcore.lib in the same place, and abld leaves a foreign
  REM library alone when its own objects are unchanged. Measured at -0.65% of the
  REM frame, which is not worth compiling the dynarec with a second compiler.
  call .\ABLD.BAT reallyclean gcce urel >nul 2>&1
  call bldmake bldfiles || (popd & echo [deploy] core bldmake FAILED & exit /b 1)
  call .\ABLD.BAT build gcce urel || (popd & echo [deploy] core build FAILED & exit /b 1)
  popd

  echo.
  REM gpu_unai is built outside abld, with GCCE 4.6.3 and the hand-written ARM
  REM inner loops. Measured against the abld/CSL 3.4.3 build on a fixed 9000-frame
  REM replay: rasteriser -8.3%, whole frame -3.0%, with every gpulib draw counter
  REM identical. configs\gpu\group\gpuunai.mmp is kept only as the record of the
  REM file list and macros - building it with abld would overwrite gpuunai.lib.
  echo [deploy] === gpu_unai (asm + GCCE 4.6.3) ===
  call "%~dp0gpuasm-regen.cmd" || (echo [deploy] gpu asm FAILED & exit /b 1)
  call "%~dp0gpu463-regen.cmd" || (echo [deploy] gpu 4.6.3 build FAILED & exit /b 1)
)

echo.
echo [deploy] === %TARGET% ===
pushd "%CONFIGS%\%TARGET%\group"
REM reallyclean is not optional: abld tracks only its own sources, so a changed
REM pcsxcore.lib or gpuunai.lib leaves the previous EXE in place without a word.
if /i "%TARGET%"=="stage1" call .\ABLD.BAT reallyclean gcce urel >nul 2>&1
call bldmake bldfiles || (popd & echo [deploy] bldmake FAILED & exit /b 1)
call .\ABLD.BAT build gcce urel || (popd & echo [deploy] %TARGET% build FAILED & exit /b 1)

set EXE=%RELDIR%\ps1%TARGET%.exe
if not exist "!EXE!" (popd & echo [deploy] missing !EXE! & exit /b 1)

REM --- staleness check: the EXE must be younger than every library it links ---
if /i "%TARGET%"=="stage1" (
  for %%L in (pcsxcore.lib gpuunai.lib) do (
    if exist "%LIBDIR%\%%L" (
      powershell -NoProfile -Command "exit [int]((Get-Item '!EXE!').LastWriteTime -lt (Get-Item '%LIBDIR%\%%L').LastWriteTime)"
      if errorlevel 1 (
        popd
        echo [deploy] STALE: ps1%TARGET%.exe is older than %%L - the link did not happen
        exit /b 1
      )
    )
  )
)

REM --- sign -----------------------------------------------------------------
REM RSA + SHA-1 is mandatory. Symbian 9.2 cannot parse a SHA-256 certificate at
REM all and reports "file is corrupted", which points at the wrong thing.
if not exist "%SIGNDIR%" mkdir "%SIGNDIR%"
if not exist "%SIGNDIR%\sha1.key" (
  echo [deploy] generating a self-signed RSA/SHA-1 certificate
  "%OPENSSL%" genrsa -out "%SIGNDIR%\sha1.key" 1024 || (popd & exit /b 1)
  "%OPENSSL%" req -new -x509 -sha1 -key "%SIGNDIR%\sha1.key" -out "%SIGNDIR%\sha1.cer" -days 3650 -subj "/CN=PS1Emulator Dev" || (popd & exit /b 1)
)

del /q ps1%TARGET%.sis ps1%TARGET%_sha1.sis 2>nul
call makesis %TARGET%.pkg ps1%TARGET%.sis || (popd & echo [deploy] makesis FAILED - check the .pkg is ASCII with CRLF & exit /b 1)
call signsis -s ps1%TARGET%.sis ps1%TARGET%_sha1.sis "%SIGNDIR%\sha1.cer" "%SIGNDIR%\sha1.key"
if not exist ps1%TARGET%_sha1.sis (popd & echo [deploy] signsis produced nothing & exit /b 1)

REM signsis without -s exits 0 and writes an unsigned file. The only proof that
REM signing happened is signsis -o printing an issuer.
call signsis -o ps1%TARGET%_sha1.sis | findstr /i "Issued" >nul
if errorlevel 1 (popd & echo [deploy] UNSIGNED: signsis -o shows no issuer & exit /b 1)
echo [deploy] signed ok
popd

REM --- WiFi path: talk to the on-device daemon, skip mass storage entirely ---
if /i "%MODE%"=="wifi" goto :wifi

REM --- find the phone -------------------------------------------------------
:find_phone
REM No pipes in this one-liner on purpose: inside for /f backquotes cmd hands the
REM caret through to PowerShell and the escaped pipe becomes a syntax error.
for /f "usebackq delims=" %%D in (`powershell -NoProfile -Command "$r=@(); foreach($d in (Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=2')){$r+=$d.DeviceID}; $p=''; foreach($x in $r){if(Test-Path ($x+'\card_content.cid')){$p=$x}}; if(-not $p -and $r.Count -eq 1){$p=$r[0]}; if($p){$p}"`) do set PHONE=%%D
if "%PHONE%"=="" (
  echo.
  echo [deploy] no phone found. Plug it in and pick "Mass storage" as the USB mode.
  echo [deploy] The build artefacts are ready; rerun "deploy.cmd collect" once connected.
  exit /b 3
)
echo [deploy] phone card on %PHONE%

REM Cache the daemon's last-known IP off the card so wifi mode can default to it.
REM The phone writes that file as UTF-16, and "for /f" below reads it as ASCII -
REM a plain copy yields "192.1" and the cache never works. Transcode it here.
if exist "%PHONE%\daemon_ip.txt" powershell -NoProfile -Command ^
  "$t=[System.IO.File]::ReadAllText('%PHONE%\daemon_ip.txt',[System.Text.Encoding]::Unicode).Trim();" ^
  "if($t -notmatch '^\d+\.\d+\.\d+\.\d+'){$t=[System.IO.File]::ReadAllText('%PHONE%\daemon_ip.txt').Trim()};" ^
  "[System.IO.File]::WriteAllText('%~dp0.daemon_ip',$t+[char]13+[char]10,[System.Text.Encoding]::ASCII)" >nul 2>&1

if /i "%TARGET%"=="collect" goto :collect

REM --- deliver --------------------------------------------------------------
if /i "%MODE%"=="fast" (
  REM Deprecated. In-place EXE replacement needs the removable-media hash on the
  REM SYSTEM drive C:\sys\hash\<name> rewritten too, and a PC in mass-storage mode
  REM cannot reach C: (only the card E: is exposed). Use wifi, whose daemon runs on
  REM the phone and can write C:. Verified: without the C: hash the loader rejects
  REM the launch with -46 (KErrPermissionDenied).
  echo [deploy] "fast" is deprecated - it cannot update C:\sys\hash from mass storage.
  echo [deploy] Use: deploy.cmd %TARGET% wifi ^<ip^>
  exit /b 4
) else (
  if not exist "%PHONE%\Downloads" mkdir "%PHONE%\Downloads"
  copy /y "%CONFIGS%\%TARGET%\group\ps1%TARGET%_sha1.sis" "%PHONE%\Downloads\" >nul || exit /b 1
  echo [deploy] ps1%TARGET%_sha1.sis -^> %PHONE%\Downloads
  echo [deploy] unplug, install it from the file manager, run it, plug back in,
  echo [deploy] then: deploy.cmd collect
)
REM Deliberately not collecting here: the logs on the card are still the previous
REM run's, and picking them up now would date-stamp stale numbers as fresh ones.
exit /b 0

REM --- collect --------------------------------------------------------------
:collect
set STAMP=
for /f "usebackq delims=" %%T in (`powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd_HHmmss"`) do set STAMP=%%T
set RUNDIR=%PROJ%\plans\runs\%STAMP%

set FOUND=0
for %%F in ("%PHONE%\ps1_*.jsonl" "%PHONE%\PS1\logs\ps1_*.jsonl" "%PHONE%\ps1_vram.bin") do if exist "%%~F" set FOUND=1
if "%FOUND%"=="0" (
  echo [deploy] nothing to collect on %PHONE% yet
  exit /b 0
)

mkdir "%RUNDIR%" 2>nul
for %%F in ("%PHONE%\ps1_*.jsonl" "%PHONE%\PS1\logs\ps1_*.jsonl" "%PHONE%\ps1_vram.bin") do (
  if exist "%%~F" (
    copy /y "%%~F" "%RUNDIR%\" >nul
    echo [deploy] collected %%~nxF  %%~zF bytes
  )
)
echo [deploy] -^> %RUNDIR%
exit /b 0

REM --- WiFi deploy over the on-device daemon --------------------------------
:wifi
REM ip = arg 3, else the value cached from the card on last collect, else "scan".
REM deploy_net.ps1 auto-discovers the daemon on the LAN when the ip is "scan" and
REM also falls back to a scan if a given/cached ip no longer answers (DHCP moved it).
set DIP=%~3
if "%DIP%"=="" (
  if exist "%~dp0.daemon_ip" for /f "usebackq tokens=1 delims=:" %%A in ("%~dp0.daemon_ip") do set DIP=%%A
)
if "%DIP%"=="" set DIP=scan

REM No run args by default: stage1 then runs BOTH interp and drc and emits the
REM compare line (the benchmark). An arg like "drc" would run a single pass instead.
set RUNARGS=%~4

set STAMP=
for /f "usebackq delims=" %%T in (`powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd_HHmmss"`) do set STAMP=%%T
set RUNDIR=%PROJ%\plans\runs\%STAMP%_wifi

REM stage1 moved its log under the E:\PS1\ layout; stage0 still writes to the root.
REM Pulling the old path silently returned a stale log instead of failing.
set LOGDEV=E:\ps1_%TARGET%.jsonl
if /i "%TARGET%"=="stage1" set LOGDEV=E:\PS1\logs\ps1_stage1.jsonl

REM Default 180 s of deploy_net.ps1 is shorter than the run: 6100 frames at 27 ms
REM is 170 s plus BIOS load, and the socket dropped RIGHT at the threshold of a useful
REM run - the device finished computing to the end, while the log remained on the card.
if not defined RUNTMO set RUNTMO=900
echo [deploy] wifi -^> %DIP%  (run args: "%RUNARGS%")
REM The app is installed on the card, so its EXE and log live on E: from the
REM phone's own side too. Push -> launch -> pull, all headless.
REM The removable-media hash lives on the SYSTEM drive C:\sys\hash\<name>, not on
REM the card - the loader checks it there. The daemon runs on the phone, so it can
REM reach C:; a PC in mass-storage mode cannot (that is why "fast" is deprecated).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\..\SymbianDeployDaemon\tools\deploy_net.ps1" ^
  -Ip %DIP% ^
  -PutFile "%RELDIR%\ps1%TARGET%.exe" -Remote "E:\sys\bin\ps1%TARGET%.exe" ^
  -HashRemote "C:\sys\hash\ps1%TARGET%.exe" ^
  -Run "E:\sys\bin\ps1%TARGET%.exe %RUNARGS%" -RunTimeoutSec %RUNTMO% ^
  -GetFile "%LOGDEV%" -OutFile "%RUNDIR%\ps1_%TARGET%.jsonl"
if errorlevel 1 (echo [deploy] wifi cycle FAILED & exit /b 1)
echo [deploy] wifi cycle done -^> %RUNDIR%
exit /b 0

endlocal
