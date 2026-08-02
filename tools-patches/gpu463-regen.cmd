@echo off
REM ---------------------------------------------------------------------------
REM gpu463-regen.cmd - rebuild gpuunai.lib with GCCE 4.6.3 instead of CSL 3.4.3.
REM
REM An A/B experiment, not the default build. Run "deploy.cmd stage1" afterwards
REM WITHOUT this and abld puts the 3.4.3 library back, which is how you switch
REM the comparison the other way.
REM
REM Two things the old toolchain needs from a 4.6.3 object (both learned the hard
REM way, see also gpuasm-regen.cmd):
REM   - the archive index must come from the old ar plus an explicit ranlib;
REM   - the header must be stamped back from EABI Version5 to Version4 and the
REM     attributes section dropped, or every old binutils tool answers
REM     "File format not recognized".
REM 4.6.3 also emits __aeabi_idiv/__aeabi_uidiv, which Symbian does not export -
REM src\compat\compat_aeabi.c supplies them on top of the *divmod that it does.
REM ---------------------------------------------------------------------------
setlocal

set PROJ=%~dp0..
set FORK=%PROJ%\pcsx_rearmed
set NEWBIN=D:\VsCodeProjects\SymbianProjects\toolchain\gcce\arm-2012.03\bin
set OLDBIN=D:\VsCodeProjects\SymbianProjects\toolchain\CSL_Arm_Toolchain\bin
set SDKINC=D:\VsCodeProjects\SymbianProjects\S60_3rd_FP1\Epoc32\include
set OUTLIB=D:\VsCodeProjects\SymbianProjects\S60_3rd_FP1\Epoc32\release\armv5\urel
set TMPDIR=%PROJ%\configs\gpu\obj463

if not exist "%TMPDIR%" mkdir "%TMPDIR%"
del /q "%TMPDIR%\*.o" 2>nul

set INC=-I"%PROJ%\src\compat" -I"%FORK%" -I"%FORK%\include" -I"%FORK%\libpcsxcore" -I"%FORK%\plugins\gpulib" -I"%FORK%\plugins\gpu_unai" -I"%SDKINC%" -I"%SDKINC%\libc"
REM gcce.h is the prelude abld force-includes; without it the Symbian libc
REM headers do not even parse (IMPORT_C is undefined).
set FLAGS=-c -O2 -march=armv6 -mtune=arm1136j-s -fno-exceptions -fno-rtti -fpermissive -fshort-wchar -include "%SDKINC%\gcce\gcce.h" -D__SYMBIAN32__ -D__EPOC32__ -D__GCCE__ -D__EABI__ -D__MARM__ -D__MARM_ARMV5__ -D__SERIES60_3X__ -D__SERIES60_31__ -DREARMED -DUSE_GPULIB=1 -D__SIZEOF_POINTER__=4 -DP_HAVE_MMAP=0

"%NEWBIN%\arm-none-symbianelf-gcc.exe" %FLAGS% %INC% -o "%TMPDIR%\gpu.o"       "%FORK%\plugins\gpulib\gpu.c"  || exit /b 1
"%NEWBIN%\arm-none-symbianelf-gcc.exe" %FLAGS% %INC% -o "%TMPDIR%\prim.o"      "%FORK%\plugins\gpulib\prim.c" || exit /b 1
"%NEWBIN%\arm-none-symbianelf-g++.exe" %FLAGS% %INC% -o "%TMPDIR%\gpulib_if.o" "%FORK%\plugins\gpu_unai\gpulib_if.cpp" || exit /b 1
"%NEWBIN%\arm-none-symbianelf-g++.exe" %FLAGS% %INC% -I"%FORK%\plugins\gpu_unai\old" -o "%TMPDIR%\oldif.o" "%FORK%\plugins\gpu_unai\old\if.cpp" || exit /b 1

for %%F in ("%TMPDIR%\*.o") do (
  "%NEWBIN%\arm-none-symbianelf-objcopy.exe" -R .ARM.attributes "%%F" || exit /b 1
  powershell -NoProfile -Command ^
    "$p='%%F'; $b=[IO.File]::ReadAllBytes($p); $b[0x24]=0; $b[0x25]=0; $b[0x26]=0; $b[0x27]=4; [IO.File]::WriteAllBytes($p,$b)" || exit /b 1
)

del /q "%OUTLIB%\gpuunai.lib" 2>nul
"%OLDBIN%\arm-none-symbianelf-ar.exe" rc "%OUTLIB%\gpuunai.lib" "%TMPDIR%\gpu.o" "%TMPDIR%\prim.o" "%TMPDIR%\gpulib_if.o" "%TMPDIR%\oldif.o" || exit /b 1
"%OLDBIN%\arm-none-symbianelf-ranlib.exe" "%OUTLIB%\gpuunai.lib" || exit /b 1

echo [gpu463] gpuunai.lib rebuilt with GCCE 4.6.3
endlocal
