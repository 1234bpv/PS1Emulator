@echo off
REM ---------------------------------------------------------------------------
REM gpuasm-regen.cmd - assemble gpu_unai's hand-written ARM inner loops.
REM
REM Why this exists: gpu_arm.S holds the assembly rasteriser spans, and they are
REM what the profile is dominated by (gpuPolySpanFn<32> alone was 15.5% of the
REM frame). The bundled CSL 3.4.3 assembler cannot build it - that binutils does
REM not substitute macro parameters inside .if, and every macro invocation fails
REM with "bad expression". Rewriting hand-written assembly to dodge that is a lot
REM of untestable risk, so the file is assembled with the newer GCCE 4.6.3
REM binutils instead and handed to the abld link as a static library. Everything
REM else still builds with CSL 3.4.3.
REM
REM Run once after checkout, and again whenever gpu_arm.S changes.
REM ---------------------------------------------------------------------------
setlocal

set PROJ=%~dp0..
set FORK=%PROJ%\pcsx_rearmed
set NEWBIN=D:\VsCodeProjects\SymbianProjects\toolchain\gcce\arm-2012.03\bin
REM The archive must be packed by the OLD ar: the CSL 3.4.3 linker cannot read an
REM index written by binutils 2.21 at all - it reports "File format not
REM recognized". The .o itself is ordinary ELF and links without complaint.
set OLDBIN=D:\VsCodeProjects\SymbianProjects\toolchain\CSL_Arm_Toolchain\bin
set OUTLIB=D:\VsCodeProjects\SymbianProjects\S60_3rd_FP1\Epoc32\release\armv5\urel
set TMPDIR=%PROJ%\configs\gpu\asm

if not exist "%NEWBIN%\arm-none-symbianelf-gcc.exe" (
  echo [gpuasm] missing GCCE 4.6.3 at %NEWBIN%
  exit /b 1
)
if not exist "%TMPDIR%" mkdir "%TMPDIR%"

REM -march=armv6 is what lets arm_features.h turn on HAVE_ARMV6, which selects
REM the ARMv6-only span variants the switch in gpu_inner.h dispatches to.
"%NEWBIN%\arm-none-symbianelf-gcc.exe" -c -march=armv6 ^
  -I"%FORK%\plugins\gpu_unai" -I"%FORK%" -I"%FORK%\include" ^
  -o "%TMPDIR%\gpu_arm.o" "%FORK%\plugins\gpu_unai\gpu_arm.S" || exit /b 1

REM The 4.6.3 assembler marks the object EABI Version5, and the CSL 3.4.3
REM binutils reject anything above Version4 outright ("File format not
REM recognized" from ar, ranlib and ld alike). The emitted code is identical -
REM this is hand-written ARM with no EH and no floating point - so drop the
REM attributes section and stamp the header back to Version4.
"%NEWBIN%\arm-none-symbianelf-objcopy.exe" -R .ARM.attributes "%TMPDIR%\gpu_arm.o" || exit /b 1
powershell -NoProfile -Command ^
  "$p='%TMPDIR%\gpu_arm.o'; $b=[IO.File]::ReadAllBytes($p); $b[0x24]=0; $b[0x25]=0; $b[0x26]=0; $b[0x27]=4; [IO.File]::WriteAllBytes($p,$b)" || exit /b 1

del /q "%TMPDIR%\gpuasm.lib" 2>nul
"%OLDBIN%\arm-none-symbianelf-ar.exe" rc "%TMPDIR%\gpuasm.lib" "%TMPDIR%\gpu_arm.o" || exit /b 1
REM Explicit ranlib: "ar rcs" leaves this archive without a symbol index, and the
REM linker then refuses it with "Archive has no index".
"%OLDBIN%\arm-none-symbianelf-ranlib.exe" "%TMPDIR%\gpuasm.lib" || exit /b 1
copy /y "%TMPDIR%\gpuasm.lib" "%OUTLIB%\gpuasm.lib" >nul || exit /b 1

echo [gpuasm] gpuasm.lib -^> %OUTLIB%
endlocal
