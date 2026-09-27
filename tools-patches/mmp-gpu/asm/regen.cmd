@echo off
REM Regenerate preprocessed gpu_unai assembly for the Symbian build.
REM
REM Same reason as configs\core\asm\regen.cmd: the Symbian build system hands
REM assembly to the compiler as lowercase .s, and GCC does NOT preprocess .s
REM files. gpu_arm.S depends on macros from arm_features.h.
REM
REM Rerun this after updating the pcsx_rearmed fork.

setlocal
for %%I in ("%~dp0..\..\..") do set "PROJ=%%~fI"
set "PR=%PROJ%\pcsx_rearmed"
set "CSL=%PROJ%\..\toolchain\CSL_Arm_Toolchain\bin"
set OUT=%~dp0

"%CSL%\arm-none-symbianelf-gcc.exe" -E -x assembler-with-cpp -march=armv6 ^
  -D__SIZEOF_POINTER__=4 ^
  -I "%PR%\include" -I "%PR%\plugins\gpu_unai" ^
  "%PR%\plugins\gpu_unai\gpu_arm.S" -o "%OUT%gpu_arm.s"
if errorlevel 1 echo FAILED: gpu_arm

echo Done. Verify with: arm-none-symbianelf-gcc -c -march=armv6 gpu_arm.s -o nul
endlocal
