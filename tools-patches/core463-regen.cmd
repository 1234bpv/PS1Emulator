@echo off
REM ---------------------------------------------------------------------------
REM core463-regen.cmd - rebuild pcsxcore.lib with GCCE 4.6.3 instead of CSL 3.4.3.
REM
REM Same reasoning and the same three tricks as gpu463-regen.cmd: old ar plus an
REM explicit ranlib for the index, EABI Version5 stamped back to Version4 with
REM the attributes section dropped, and compat_aeabi.c in the EXE to supply the
REM __aeabi_idiv/__aeabi_uidiv that 4.6.3 calls and Symbian does not export.
REM
REM -O2 deliberately: -O3 grew the image by 25% and cost 5% of the frame on this
REM device (ARM1136 has a 16K instruction cache), -Os cost 20% of the rasteriser.
REM
REM The assembly is taken pre-processed from configs\core\asm - Symbian's build
REM hands .s (lower case) to the compiler without running cpp, so those files are
REM generated once by configs\core\asm\regen.cmd.
REM ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion

for %%I in ("%~dp0..") do set "PROJ=%%~fI"
for %%I in ("%PROJ%\..") do set "SYMBIANROOT=%%~fI"
set FORK=%PROJ%\pcsx_rearmed
set NEWBIN=%SYMBIANROOT%\toolchain\gcce\arm-2012.03\bin
set OLDBIN=%SYMBIANROOT%\toolchain\CSL_Arm_Toolchain\bin
set SDKINC=%SYMBIANROOT%\S60_3rd_FP1\Epoc32\include
set OUTLIB=%SYMBIANROOT%\S60_3rd_FP1\Epoc32\release\armv5\urel
set TMPDIR=%PROJ%\configs\core\obj463
set ZLIB=%FORK%\deps\libchdr\deps\zlib-1.3.1

if not exist "%TMPDIR%" mkdir "%TMPDIR%"
del /q "%TMPDIR%\*.o" 2>nul

REM The compiler's own include dir must come FIRST. Symbian's libc ships a
REM stdarg.h whose va_list is a plain char*, which 3.4.3 tolerates but 4.6.3
REM rejects outright ("incompatible type for argument 1 of __builtin_va_start"
REM in psxbios.c). Everything except stdarg/stddef still falls through to the SDK.
set GCCINC=%NEWBIN%\..\lib\gcc\arm-none-symbianelf\4.6.3\include
set INC=-I"%GCCINC%" -I"%PROJ%\src\compat" -I"%FORK%" -I"%FORK%\include" -I"%FORK%\libpcsxcore" -I"%FORK%\libpcsxcore\new_dynarec" -I"%ZLIB%" -I"%SDKINC%" -I"%SDKINC%\libc"
set FLAGS=-c -O2 -march=armv6 -mtune=arm1136j-s -fno-exceptions -fshort-wchar -include "%SDKINC%\gcce\gcce.h" -include "%PROJ%\src\compat\compat_va_gcce46.h" -D__SYMBIAN32__ -D__EPOC32__ -D__GCCE__ -D__EABI__ -D__MARM__ -D__MARM_ARMV5__ -D__SERIES60_3X__ -D__SERIES60_31__ -DBASE_ADDR_DYNAMIC=1 -D__SIZEOF_POINTER__=4 -DP_HAVE_MMAP=0 -DNDEBUG -DDISABLE_MEM_LUTS=1
if "%PS1_NO_DIAG%"=="1" set FLAGS=%FLAGS% -DPS1_NO_DIAG

for %%F in (cdriso cdrom cdrom-async cheat database decode_xa mdec misc plugins ppf psxbios psxcommon psxcounters psxdma psxevents psxhw psxinterpreter psxmem r3000a sio spu gpu pad gte gte_nf gte_divider) do (
  "%NEWBIN%\arm-none-symbianelf-gcc.exe" %FLAGS% %INC% -o "%TMPDIR%\%%F.o" "%FORK%\libpcsxcore\%%F.c" || exit /b 1
)
for %%F in (emu_if new_dynarec pcsxmem) do (
  "%NEWBIN%\arm-none-symbianelf-gcc.exe" %FLAGS% %INC% -o "%TMPDIR%\%%F.o" "%FORK%\libpcsxcore\new_dynarec\%%F.c" || exit /b 1
)
for %%F in (adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil) do (
  "%NEWBIN%\arm-none-symbianelf-gcc.exe" %FLAGS% %INC% -o "%TMPDIR%\z_%%F.o" "%ZLIB%\%%F.c" || exit /b 1
)
for %%F in (gte_arm gte_nf_arm linkage_arm) do (
  "%NEWBIN%\arm-none-symbianelf-gcc.exe" -c -march=armv6 %INC% -o "%TMPDIR%\%%F.o" "%PROJ%\configs\core\asm\%%F.s" || exit /b 1
)

for %%F in ("%TMPDIR%\*.o") do (
  "%NEWBIN%\arm-none-symbianelf-objcopy.exe" -R .ARM.attributes "%%F" || exit /b 1
  powershell -NoProfile -Command ^
    "$p='%%F'; $b=[IO.File]::ReadAllBytes($p); $b[0x24]=0; $b[0x25]=0; $b[0x26]=0; $b[0x27]=4; [IO.File]::WriteAllBytes($p,$b)" || exit /b 1
)

del /q "%OUTLIB%\pcsxcore.lib" 2>nul
"%OLDBIN%\arm-none-symbianelf-ar.exe" rc "%OUTLIB%\pcsxcore.lib" "%TMPDIR%\*.o" || exit /b 1
"%OLDBIN%\arm-none-symbianelf-ranlib.exe" "%OUTLIB%\pcsxcore.lib" || exit /b 1

echo [core463] pcsxcore.lib rebuilt with GCCE 4.6.3
endlocal
