@echo off
REM ---------------------------------------------------------------------------
REM Symbian S60 3rd Edition FP1 build environment (GCCE / CSL Arm Toolchain)
REM Usage:  call symbian-env.cmd     then  bldmake bldfiles  &  abld build gcce urel
REM
REM EPOCROOT contract (enforced by Epoc32\tools\E32env.pm lines 15-21):
REM   - must be set, spelled in CAPITALS
REM   - must NOT contain a drive letter
REM   - must start with a backslash and END with a backslash
REM   - must not be a UNC path
REM Because the drive letter is absent, the build must run from drive D:.
REM ---------------------------------------------------------------------------

set SDKROOT=D:\VsCodeProjects\SymbianProjects\S60_3rd_FP1
set TCROOT=D:\VsCodeProjects\SymbianProjects\toolchain

set EPOCROOT=\VsCodeProjects\SymbianProjects\S60_3rd_FP1\

REM Perl 5.20 (portable). Must be < 5.22: E32Plat.pm:289,387 use defined(%hash),
REM which is a fatal error from Perl 5.22 onwards.
set PERLBIN=%TCROOT%\perl\perl\bin

REM Primary compiler: CSL Arm Toolchain (GCC 3.4.3), the SDK's bundled GCCE.
set CSLBIN=%TCROOT%\CSL_Arm_Toolchain\bin

REM cpp replacement for dependency scanning (makdeps.pm). The stock
REM Epoc32\gcc\bin\cpp.exe is a 2003 cygwin build that rejects C99 variadic
REM macros. Must come BEFORE Epoc32\gcc\bin. See cppshim\gcc\bin\cpp.bat.
set CPPSHIM=%TCROOT%\cppshim\gcc\bin

path=%PERLBIN%;%SDKROOT%\Epoc32\tools;%CPPSHIM%;%CSLBIN%;%SDKROOT%\Epoc32\gcc\bin;%PATH%

D:

echo [symbian-env] EPOCROOT=%EPOCROOT%
echo [symbian-env] perl    : & perl -e "print $^V" & echo.
echo [symbian-env] compiler:
arm-none-symbianelf-g++ -dumpversion
