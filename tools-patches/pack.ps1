# ---------------------------------------------------------------------------
# pack.ps1 - build the two signed packages in one step.
#
#   pack.ps1                 # both packages from whatever EXE is in the SDK tree
#   pack.ps1 -OutDir .\out  # somewhere other than the project root
#
# min  - EXE plus the registration resource. Enough for a phone that already
#        carries the application resource (every phone the deploy daemon set up).
# full - the same plus \resource\apps\ps1stage1.rsc. A phone that has never seen
#        the daemon panics USER 23 on launch without it.
# Both carry ps1slide.dll, the media key anim of the Slider base, built by hand
# (src\symbian\slidekeys\group: bldmake bldfiles, abld build gcce urel).
#
# Both are self-signed with configs\sign\sha1.{cer,key}: the phone must be
# hacked to install them, which every target phone here is.
# ---------------------------------------------------------------------------
param(
  [string]$OutDir = '',
  [int]$LimitSec = 120,
  # Package a harness build on purpose - for trying the installer itself, never
  # for a release.
  [switch]$AllowBench
)

$ErrorActionPreference = 'Stop'
$Proj = Split-Path -Parent $PSScriptRoot
$Shared = Split-Path -Parent $Proj
$Sdk  = Join-Path $Shared 'S60_3rd_FP1'
$Tools = "$Sdk\Epoc32\tools"
$Exe  = "$Sdk\Epoc32\release\gcce\urel\ps1stage1.exe"
$Cer  = "$Proj\configs\sign\sha1.cer"
$Key  = "$Proj\configs\sign\sha1.key"
if (-not $OutDir) { $OutDir = $Proj }
# makesis reports only "file I/O fault" when the output directory is missing.
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

if (-not (Test-Path $Exe)) { throw "no EXE to package: $Exe" }
$SlideDll = "$Sdk\Epoc32\release\gcce\urel\ps1slide.dll"
if (-not (Test-Path $SlideDll)) { throw "no ps1slide.dll to package: build src\symbian\slidekeys\group" }
# configs\ is not in the repository, so a clean clone has neither the package
# descriptions nor the signing pair. Both are recoverable: tools-patches carries
# the descriptions, and a self-signed pair can be generated. A generated pair is
# a different identity from the recorded one, so a phone that already holds an
# install signed with the old certificate has to have it removed first.
$SignPass = 'ps1emu'
if (-not ((Test-Path $Cer) -and (Test-Path $Key))) {
  Write-Host "[pack] no signing pair in configs\sign - generating a self-signed one"
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Cer) | Out-Null
  & "$Tools\makekeys.exe" -cert -password $SignPass -len 1024 `
    -dname "CN=PS1Emulator OR=none CO=UA" $Key $Cer | Out-Null
  foreach ($f in @($Cer, $Key)) { if (-not (Test-Path $f)) { throw "makekeys produced no $f" } }
  Set-Content -Path "$Proj\configs\sign\passphrase.txt" -Value $SignPass -NoNewline
}
# The recorded pair has no passphrase; a generated one does, and signsis takes it
# as a fourth argument. The file written above is what tells them apart.
$PassFile = "$Proj\configs\sign\passphrase.txt"
$SignTail = if (Test-Path $PassFile) { @((Get-Content $PassFile -Raw).Trim()) } else { @() }
$exeInfo = [IO.FileInfo]$Exe
Write-Host "[pack] EXE $($exeInfo.Length) bytes, $($exeInfo.LastWriteTime)"

# The whole point of the PS1_BENCH flag is that the shipped binary carries no
# measurement harness, and nothing else downstream would notice a bench EXE in
# a release package. run.ps1 leaves the flavour of the last build here.
$FlavourFile = "$Proj\configs\stage1\group\.flavour"
$flavour = if (Test-Path $FlavourFile) { (Get-Content $FlavourFile -Raw).Trim() } else { '' }
if ($flavour -ne 'release' -and -not $AllowBench) {
  throw "the SDK tree holds a '$flavour' build, not a release one: run 'run.ps1 -Release' first, or pass -AllowBench to package it anyway"
}
if (-not $AllowBench) {
  foreach ($libName in @('pcsxcore', 'gpuunai')) {
    $lib = "$Sdk\Epoc32\release\armv5\urel\$libName.lib"
    $marker = "$Sdk\Epoc32\release\armv5\urel\.$libName.profile"
    if (-not ((Test-Path $lib) -and (Test-Path $marker))) {
      throw "$libName has no verified release profile: run 'run.ps1 -Release -NoPush'"
    }
    $profile = (Get-Content $marker -Raw).Trim()
    if ($profile -ne 'release' -or (Get-Item $lib).LastWriteTime -gt (Get-Item $marker).LastWriteTime) {
      throw "$libName profile is '$profile' or stale: rebuild the release EXE before packaging"
    }
    if ($exeInfo.LastWriteTime -lt (Get-Item $lib).LastWriteTime) {
      throw "EXE is older than $libName.lib: relink the release EXE before packaging"
    }
  }
  if ((Get-Item $FlavourFile).LastWriteTime -lt $exeInfo.LastWriteTime) {
    throw 'EXE is newer than its flavour marker: rebuild through run.ps1 before packaging'
  }
}
Write-Host "[pack] flavour $flavour"

function Invoke-Step {
  param([string]$File, [string[]]$ArgList, [string]$What)
  $job = Start-Job -ScriptBlock {
    param($f, $a, $cwd)
    Set-Location -LiteralPath $cwd
    & $f @a 2>&1
    $LASTEXITCODE
  } -ArgumentList $File, $ArgList, $Proj
  if (-not (Wait-Job $job -Timeout $LimitSec)) {
    Stop-Job $job; Remove-Job $job -Force
    throw "$What exceeded its ${LimitSec}s limit"
  }
  $out = Receive-Job $job
  Remove-Job $job
  $rc = $out[-1]
  if ($rc -ne 0) {
    $out | Select-Object -First 10 | ForEach-Object { Write-Host $_ }
    throw "$What failed with $rc"
  }
}

$made = @()
foreach ($v in @(@{ Pkg = 'stage1.pkg'; Name = 'ps1emu_min' },
                 @{ Pkg = 'stage1_full.pkg'; Name = 'ps1emu_full' })) {
  $pkg = "$Proj\configs\stage1\group\$($v.Pkg)"
  if (-not (Test-Path $pkg)) { $pkg = "$Proj\tools-patches\mmp-stage1\group\$($v.Pkg)" }
  if (-not (Test-Path $pkg)) { throw "no package description: $($v.Pkg)" }
  $unsigned = "$OutDir\$($v.Name)_unsigned.sis"
  $signed   = "$OutDir\$($v.Name).sis"
  Remove-Item $unsigned, $signed -Force -ErrorAction SilentlyContinue
  Invoke-Step "$Tools\makesis.exe" @($pkg, $unsigned) "makesis $($v.Pkg)"
  Invoke-Step "$Tools\signsis.exe" (@($unsigned, $signed, $Cer, $Key) + $SignTail) "signsis $($v.Name)"
  Remove-Item $unsigned -Force -ErrorAction SilentlyContinue
  # A signsis that writes nothing still exits zero on some SDK builds.
  if (-not (Test-Path $signed)) { throw "$($v.Name): no signed package produced" }
  $fi = [IO.FileInfo]$signed
  Write-Host "[pack] $($fi.Name) $($fi.Length) bytes"
  $made += $signed
}
Write-Host "[pack] done: $($made -join ', ')"
