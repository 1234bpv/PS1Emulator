@echo off
REM Regenerate preprocessed assembly for the Symbian build.
REM
REM Why: the Symbian build system hands assembly to the compiler with a
REM lowercase .s extension, and GCC does NOT run the preprocessor on .s
REM (only on .S). Upstream .S files rely on macros from arm_features.h,
REM so they must be preprocessed ahead of time.
REM
REM Rerun this after updating the pcsx_rearmed fork.

setlocal
set PR=D:\VsCodeProjects\SymbianProjects\PS1Emulator\pcsx_rearmed
set CSL=D:\VsCodeProjects\SymbianProjects\toolchain\CSL_Arm_Toolchain\bin
set OUT=%~dp0

for %%F in (gte_arm gte_nf_arm) do (
  "%CSL%\arm-none-symbianelf-gcc.exe" -E -x assembler-with-cpp -march=armv6 ^
    -D__SIZEOF_POINTER__=4 ^
    -I "%PR%\libpcsxcore" -I "%PR%\include" ^
    "%PR%\libpcsxcore\%%F.S" -o "%OUT%%%F.s"
  if errorlevel 1 echo FAILED: %%F
)

echo Done. Verify with: arm-none-symbianelf-gcc -c -march=armv6 gte_arm.s -o nul
endlocal
