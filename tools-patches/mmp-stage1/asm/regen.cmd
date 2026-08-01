@echo off
REM Regenerate preprocessed assembly for the stage1 build (dfsound mix_chan).
REM
REM Why: the Symbian build system hands assembly to the compiler with a
REM lowercase .s extension, and GCC does NOT run the preprocessor on .s
REM (only on .S). dfsound's arm_utils.S relies on macros from arm_features.h,
REM so it must be preprocessed ahead of time. -march=armv6 defines HAVE_ARMV5,
REM which makes spu.c call the asm mix_chan/mix_chan_rvb from here.
REM
REM Rerun this after updating the pcsx_rearmed fork. Output goes to
REM configs\stage1\asm (copy this tree there first when setting up a machine).

setlocal
set PR=D:\VsCodeProjects\SymbianProjects\PS1Emulator\pcsx_rearmed
set CSL=D:\VsCodeProjects\SymbianProjects\toolchain\CSL_Arm_Toolchain\bin
set OUT=%~dp0

"%CSL%\arm-none-symbianelf-gcc.exe" -E -x assembler-with-cpp -march=armv6 ^
  -D__SIZEOF_POINTER__=4 ^
  -I "%PR%\plugins\dfsound" -I "%PR%\include" ^
  "%PR%\plugins\dfsound\arm_utils.S" -o "%OUT%arm_utils.s"
if errorlevel 1 echo FAILED: arm_utils

echo Done. Verify with: arm-none-symbianelf-gcc -c -march=armv6 arm_utils.s -o nul
endlocal
