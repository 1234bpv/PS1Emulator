# ---------------------------------------------------------------------------
# run.ps1 - one command for the whole cycle: build, push, run, pull, decode.
#
#   run.ps1 -Dev n95 -Name texfix -Build -Gpu
#   run.ps1 -Dev n95 -RunArgs "gui play nosnd frames6100 light blend imggt.pbp"
#   run.ps1 -Dev e90 -Name ctl            # same EXE over Bluetooth
#   run.ps1 -List                         # what is reachable right now
#   run.ps1 -Dev c5 -Release -NoRun       # release EXE, no PS1_BENCH, push only
#
# Why this exists: every measurement used to be six manual steps (three builds,
# a push, a run, a pull) plus a hand-written pixel loop to look at the frame.
# Transport differs per device and that difference is the only thing -Dev hides:
# N95 answers over WiFi, E90 and C5-00.2 only over Bluetooth (see the shared
# SymbianProjects\CLAUDE.md). deploy.cmd is NOT used for building: its bldmake
# resolves to the wrong executable (see plans\progress.md).
#
# Artefacts land in plans\runs\<stamp>_<Name>_<dev>\ : the JSONL log, whatever
# ps1_gl<frame>.bin / ps1_vram<frame>.bin the run wrote, and a PNG per dump.
# ---------------------------------------------------------------------------
param(
  [string]$Dev = 'n95',
  [string]$RunArgs = 'gui play nosnd frames6100 glrender swoff light blend imggt.pbp',
  [string]$Name = 'run',
  [switch]$Build,          # relink stage1
  [switch]$Gpu,            # + gpu_unai (asm + GCCE 4.6.3); implies -Build
  [switch]$Core,           # + libpcsxcore; implies -Build
  [string]$CoreCc = '',     # which compiler builds libpcsxcore: csl343 or gcce463
  [switch]$Release,        # relink stage1 without PS1_BENCH; implies -Build
  [switch]$CfProfile,      # per-CF span/length histogram in GPU; bench only
  [switch]$CfTime,         # selected-CF primitive raster timing; bench only
  [switch]$SpuStageProfile,# per-stage timing in SPU; bench only
  [switch]$Stage2Profile,  # PC sampling, recompile timers, slow-path counters; bench only
  [int[]]$Shots = @(6000), # frames whose dumps to pull and decode
  [int]$Timeout = 900,
  [switch]$NoRun,          # build and push only
  [switch]$NoPush,         # build only; no phone needed
  [switch]$List,           # probe every transport and exit
  [string]$Ip,             # override the WiFi address
  [string]$Com             # override the Bluetooth port
)

$ErrorActionPreference = 'Stop'
$Proj = Split-Path -Parent $PSScriptRoot
$Shared = Split-Path -Parent $Proj
$Sdk  = Join-Path $Shared 'S60_3rd_FP1'
$Exe  = "$Sdk\Epoc32\release\gcce\urel\ps1stage1.exe"
$Net  = Join-Path $Shared 'SymbianDeployDaemon\tools\deploy_net.ps1'
$EnvCmd = "$PSScriptRoot\symbian-env.cmd"

# Transport per device. COM ports come from Bluetooth pairing and survive
# reboots, but they are per-PC: override with -Com if Windows renumbered them.
$Transport = @{
  n95 = @{ Kind = 'ip';  Addr = 'scan' }
  e90 = @{ Kind = 'com'; Addr = 'COM9'  }
  c5  = @{ Kind = 'com'; Addr = 'COM12' }
}

function Invoke-Net {
  param([string[]]$NetArgs)
  & powershell -NoProfile -ExecutionPolicy Bypass -File $Net @NetArgs
}

function Get-Link {
  param([string]$Which)
  if ($Ip)  { return @('-Ip', $Ip) }
  if ($Com) { return @('-Com', $Com) }
  $t = $Transport[$Which]
  if (-not $t) { throw "unknown device $Which - expected n95, e90 or c5" }
  if ($t.Kind -eq 'ip') { return @('-Ip', $t.Addr) }
  return @('-Com', $t.Addr)
}

if ($List) {
  foreach ($k in ($Transport.Keys | Sort-Object)) {
    $link = Get-Link $k
    Write-Host "[run] $k via $($link -join ' ') ..."
    try { Invoke-Net ($link + @('-Info')) } catch { Write-Host "[run]   unreachable" }
  }
  exit 0
}

# --- build ----------------------------------------------------------------
# Any library rebuild forces a stage1 relink: abld tracks only its own sources
# and would happily leave the previous EXE in place.
if ($Core -or $Gpu -or $CoreCc -or $CfProfile -or $CfTime -or $SpuStageProfile -or $Stage2Profile) { $Build = $true }
if ($Release) { $Build = $true }
if (($CfProfile -or $CfTime -or $SpuStageProfile -or $Stage2Profile) -and $Release) { throw 'CF/Stage diagnostics are bench-only' }
if ($SpuStageProfile -and $Stage2Profile) { throw '-SpuStageProfile and -Stage2Profile must be built separately' }
if ($CfProfile -and $CfTime) { throw '-CfProfile and -CfTime must be measured separately' }
$Flavour = if ($Release) { 'release' } elseif ($SpuStageProfile) { 'bench-spustage' } elseif ($Stage2Profile) { 'bench-stage2' } else { 'bench' }
$GpuProfile = if ($CfProfile) { 'bench-cf' } elseif ($CfTime) { 'bench-cftime' } else { if ($Release) { 'release' } else { 'bench' } }

function Build-Step {
  param([string]$Cmd, [string]$What, [int]$LimitSec = 1200)
  Write-Host "[run] === $What ==="
  # A build that hangs looks exactly like a build that is slow, and an
  # unattended cycle then waits for it forever.
  $job = Start-Job -ScriptBlock {
    param($c, $e)
    cmd /c "call `"$e`" >nul && $c" 2>&1
    $LASTEXITCODE
  } -ArgumentList $Cmd, $EnvCmd
  if (-not (Wait-Job $job -Timeout $LimitSec)) {
    Stop-Job $job; Remove-Job $job -Force
    throw "$What exceeded its ${LimitSec}s limit"
  }
  $out = @(Receive-Job $job)
  Remove-Job $job
  $rc = $out[-1]
  $out = @($out | Select-Object -SkipLast 1)
  $bad = $out | Select-String -Pattern 'rror|undefined reference|No such file'
  if ($rc -ne 0 -or $bad) {
    $out | Select-Object -Last 10 | ForEach-Object { Write-Host $_ }
    throw "$What failed with exit code $rc"
  }
}

# Two different toolchains can produce pcsxcore.lib, and for weeks the tree held
# one that neither of them had just built: measured 2026-09-20, the same sources
# linked to 1379008 bytes with CSL 3.4.3, 1443880 with GCCE 4.6.3, and what was
# actually in the tree gave 1386104. The marker records who built the library,
# and a library newer than its marker counts as unknown, not as the default.
$CoreMarker = "$Sdk\Epoc32\release\armv5\urel\.pcsxcore.cc"
$CoreProfileMarker = "$Sdk\Epoc32\release\armv5\urel\.pcsxcore.profile"
$CoreLib    = "$Sdk\Epoc32\release\armv5\urel\pcsxcore.lib"
function Get-CoreCc {
  if (-not (Test-Path $CoreLib)) { return '' }
  if (-not (Test-Path $CoreMarker)) { return 'unknown' }
  if ((Get-Item $CoreLib).LastWriteTime -gt (Get-Item $CoreMarker).LastWriteTime) { return 'unknown' }
  return (Get-Content $CoreMarker -Raw).Trim()
}
function Get-CoreProfile {
  if (-not (Test-Path $CoreLib)) { return '' }
  if (-not (Test-Path $CoreProfileMarker)) { return 'unknown' }
  if ((Get-Item $CoreLib).LastWriteTime -gt (Get-Item $CoreProfileMarker).LastWriteTime) { return 'unknown' }
  return (Get-Content $CoreProfileMarker -Raw).Trim()
}
function Build-Core {
  param([string]$Cc, [string]$Profile)
  $oldProfile = Get-CoreProfile
  $env:PS1_NO_DIAG = if ($Profile -eq 'release') { '1' } else { '0' }
  $env:PS1_SPU_STAGE_PROFILE = if ($Profile -eq 'bench-spustage') { '1' } else { '0' }
  # abld relinks the library only when an object changed, so a library dropped in
  # by the other toolchain survives "abld build" untouched and the marker would
  # then lie about it. Remove the target and check the timestamp moved.
  Remove-Item $CoreLib -Force -ErrorAction SilentlyContinue
  if ($Cc -eq 'gcce463') {
    Build-Step "call `"$PSScriptRoot\core463-regen.cmd`"" 'libpcsxcore gcce463'
  } else {
    $coreMmp = "$Proj\configs\core\group\pcsxcore.mmp"
    $coreBackup = "$coreMmp.profile.bak"
    if ($Profile -eq 'release') {
      Copy-Item $coreMmp $coreBackup -Force
      [IO.File]::AppendAllText($coreMmp, "`r`nMACRO           PS1_NO_DIAG`r`n")
    } elseif ($Profile -eq 'bench-spustage') {
      Copy-Item $coreMmp $coreBackup -Force
      [IO.File]::AppendAllText($coreMmp, "`r`nMACRO           PS1_SPU_STAGE_PROFILE`r`n")
    } elseif ($Profile -eq 'bench-stage2') {
      Copy-Item $coreMmp $coreBackup -Force
      [IO.File]::AppendAllText($coreMmp, "`r`nMACRO           PS1_STAGE2_PROFILE`r`n")
    }
    try {
      if ($oldProfile -ne $Profile) {
        Build-Step "cd /d `"$Proj\configs\core\group`" && call bldmake bldfiles && call .\ABLD.BAT clean gcce urel" 'libpcsxcore clean'
      }
      Build-Step "cd /d `"$Proj\configs\core\group`" && call bldmake bldfiles && call .\ABLD.BAT build gcce urel" 'libpcsxcore csl343'
    } finally {
      if ($Profile -eq 'release' -or $Profile -eq 'bench-spustage' -or $Profile -eq 'bench-stage2') {
        Copy-Item $coreBackup $coreMmp -Force
        Remove-Item $coreBackup -Force
      }
    }
  }
  if (-not (Test-Path $CoreLib)) { throw "libpcsxcore $Cc produced no pcsxcore.lib" }
  Set-Content -Path $CoreMarker -Value $Cc -NoNewline
  Set-Content -Path $CoreProfileMarker -Value $Profile -NoNewline
  Write-Host "[run] pcsxcore.lib $((Get-Item $CoreLib).Length) bytes, $Cc $Profile"
}

if ($CoreCc -and $CoreCc -ne 'csl343' -and $CoreCc -ne 'gcce463') {
  throw "-CoreCc takes csl343 or gcce463, not '$CoreCc'"
}
if ($Core -or $CoreCc) {
  $want = if ($CoreCc) { $CoreCc } else { 'csl343' }
  Build-Core $want $Flavour
} elseif ($Build) {
  $have = Get-CoreCc
  $haveProfile = Get-CoreProfile
  if ($have -eq 'unknown' -or $have -eq '') {
    Write-Host '[run] pcsxcore.lib provenance unknown - rebuilding with csl343'
    Build-Core 'csl343' $Flavour
  } elseif ($haveProfile -ne $Flavour) {
    Write-Host "[run] pcsxcore.lib profile $haveProfile -> $Flavour"
    Build-Core $have $Flavour
  }
}
$GpuLib = "$Sdk\Epoc32\release\armv5\urel\gpuunai.lib"
$GpuProfileMarker = "$Sdk\Epoc32\release\armv5\urel\.gpuunai.profile"
if ($Build -and ((-not (Test-Path $GpuLib)) -or (-not (Test-Path $GpuProfileMarker)) -or
    ((Get-Item $GpuLib).LastWriteTime -gt (Get-Item $GpuProfileMarker).LastWriteTime) -or
    ((Get-Content $GpuProfileMarker -Raw).Trim() -ne $GpuProfile))) {
  Write-Host "[run] gpuunai.lib profile unknown or different - rebuilding for $GpuProfile"
  $Gpu = $true
}
if ($Gpu) {
  $env:PS1_NO_DIAG = if ($Flavour -eq 'release') { '1' } else { '0' }
  $env:GPU_CF_PROFILE = if ($CfProfile) { '1' } else { '0' }
  $env:GPU_CF_TIME = if ($CfTime) { '1' } else { '0' }
  Build-Step "call `"$PSScriptRoot\gpuasm-regen.cmd`"" 'gpu_unai asm'
  Build-Step "call `"$PSScriptRoot\gpu463-regen.cmd`"" 'gpu_unai 4.6.3'
  if (-not (Test-Path $GpuLib)) { throw 'gpu_unai produced no gpuunai.lib' }
  Set-Content -Path $GpuProfileMarker -Value $GpuProfile -NoNewline
}
if ($Build) {
  # abld does not relink when only a library changed, so the previous EXE
  # would be measured instead of the new one. Remove it and let the link run.
  $started = Get-Date
  Remove-Item $Exe -Force -ErrorAction SilentlyContinue

  # -Release: build the same stage1.mmp with the bench-only sources and
  # bench-only sources stripped, so this link is what a real device install
  # would carry. SwEvent remains for side-key capture. Only stage1.mmp is
  # touched - core and GPU carry their own matching profile markers. A real on-disk
  # backup is kept, and finally restores it even if the build throws: a
  # release run must never leave the tree stuck without PS1_BENCH.
  $MmpPath = "$Proj\configs\stage1\group\stage1.mmp"
  $MmpBackup = "$MmpPath.bak"

  # Turning PS1_BENCH on or off changes how every stage1 object is compiled, but
  # abld decides what to recompile by source timestamp alone: without a clean it
  # relinks the previous flavour's objects. Measured 2026-09-19: the first
  # -Release build failed with six "undefined reference to pmuSymbian*" because
  # stage1.o still carried the macro while pmu_symbian.cpp had been dropped. The
  # marker records which flavour the object tree currently holds.
  $FlavourFile = "$Proj\configs\stage1\group\.flavour"
  $prevFlavour = if (Test-Path $FlavourFile) { (Get-Content $FlavourFile -Raw).Trim() } else { '' }
  # A build run by hand (plain abld, as a subagent may do) leaves objects the
  # marker knows nothing about. Anything newer than the marker means the tree
  # holds an unknown flavour, so clean rather than trust it.
  $projectTree = $Proj.Substring([IO.Path]::GetPathRoot($Proj).Length)
  $objDir = "$Sdk\Epoc32\build\$projectTree\configs\stage1\group"
  if ($prevFlavour -and (Test-Path $objDir)) {
    $markT = (Get-Item $FlavourFile).LastWriteTime
    $newer = Get-ChildItem -Path $objDir -Recurse -Filter *.o -ErrorAction SilentlyContinue |
             Where-Object { $_.LastWriteTime -gt $markT } | Select-Object -First 1
    if ($newer) {
      Write-Host "[run] objects newer than the flavour marker ($($newer.Name)) - flavour unknown"
      $prevFlavour = ""
    }
  }
  if ($prevFlavour -ne $Flavour) {
    Build-Step "cd /d `"$Proj\configs\stage1\group`" && call bldmake bldfiles && call .\ABLD.BAT clean gcce urel" 'stage1 clean'
  }

  if ($Release -or $SpuStageProfile -or $Stage2Profile) {
    Copy-Item $MmpPath $MmpBackup -Force
    $patched = [IO.File]::ReadAllText($MmpPath)
    if ($Release) {
      $patched = $patched.
        Replace("MACRO           PS1_BENCH", "// MACRO           PS1_BENCH").
        Replace("SOURCE          pmu_symbian.cpp", "// SOURCE          pmu_symbian.cpp").
        Replace("SOURCEPATH      ..\..\..\..\SymbianOverclock\src", "// SOURCEPATH      ..\..\..\..\SymbianOverclock\src").
        Replace("SOURCE          ocengine.cpp", "// SOURCE          ocengine.cpp")
      # Match the capability line by shape, not by its exact text: it was a literal
      # string here until adding SwEvent for the side keys made the match fail
      # silently, which would have shipped a release EXE with every capability the
      # harness build needs.
      # SwEvent is the one that stays. Without it CaptureKey answers -46 and no side
      # key reaches the game, so stripping it would ship the feature dead. The -46
      # that once looked like a capability refusal on C5-00.2 turned out to be the
      # missing SHA-1 in the sys hash directory, so that reason no longer holds.
      $patched = [regex]::Replace($patched, '(?m)^CAPABILITY\s+.*$', 'CAPABILITY      SwEvent')
      foreach ($must in @('CAPABILITY      SwEvent', '// MACRO           PS1_BENCH',
                          '// SOURCE          pmu_symbian.cpp')) {
        if ($patched -notmatch [regex]::Escape($must)) {
          throw "the release rewrite did not take: '$must' is missing from the patched mmp"
        }
      }
    }
    if ($SpuStageProfile) {
      $patched = $patched + "`r`nMACRO           PS1_SPU_STAGE_PROFILE`r`n"
    }
    if ($Stage2Profile) {
      $patched = $patched + "`r`nMACRO           PS1_STAGE2_PROFILE`r`n"
    }
    [IO.File]::WriteAllText($MmpPath, $patched)
  }
  try {
    Build-Step "cd /d `"$Proj\configs\stage1\group`" && call bldmake bldfiles && call .\ABLD.BAT build gcce urel" 'stage1'
  } finally {
    if ($Release -or $SpuStageProfile -or $Stage2Profile) {
      Copy-Item $MmpBackup $MmpPath -Force
      Remove-Item $MmpBackup -Force -ErrorAction SilentlyContinue
    }
  }
  # A link that quietly produced nothing leaves the tree looking built.
  if (-not (Test-Path $Exe)) { throw "stage1 produced no EXE" }
  $fi = [IO.FileInfo]$Exe
  if ($fi.LastWriteTime -lt $started) { throw "stage1 EXE is older than this build" }
  Write-Host "[run] exe $($fi.Length) bytes, $($fi.LastWriteTime)"
  Set-Content -Path $FlavourFile -Value $Flavour -NoNewline
  if ($Stage2Profile) {
    # s2prof.ps1 needs the symbols of exactly this link. The E32 image carries none;
    # the ELF the link produced before elf2e32 does, and its local symbols matter
    # because the rasteriser is almost all static functions.
    $elf = "$objDir\STAGE1\GCCE\urel\ps1stage1.exe"
    $nm = Join-Path $Shared 'toolchain\CSL_Arm_Toolchain\bin\arm-none-symbianelf-nm.exe'
    & $nm -S -n -C $elf | Set-Content -Path "$Exe.nm.txt" -Encoding ASCII
    if (-not (Test-Path "$Exe.map") -or (Get-Item "$Exe.map").LastWriteTime -lt $started) { throw 'the linker map is older than this build' }
    Write-Host "[run] symbols $((Get-Item "$Exe.nm.txt").Length) bytes, map $((Get-Item "$Exe.map").Length) bytes"
  }
}

if ($NoPush) { Write-Host "[run] built only"; exit 0 }

# --- push and run ---------------------------------------------------------
$link = Get-Link $Dev
$stamp = Get-Date -Format 'yyyy-MM-dd_HHmmss'
$RunDir = "$Proj\plans\runs\${stamp}_${Name}_${Dev}"
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null

$push = $link + @(
  '-PutFile', $Exe, '-Remote', 'E:\sys\bin\ps1stage1.exe',
  '-HashRemote', 'C:\sys\hash\ps1stage1.exe')
if (-not $NoRun) {
  $push += @('-Run', "E:\sys\bin\ps1stage1.exe $RunArgs", '-RunTimeoutSec', $Timeout,
             '-GetFile', 'E:\PS1\logs\ps1_stage1.jsonl',
             '-OutFile', "$RunDir\ps1_stage1.jsonl")
}
Invoke-Net $push | Select-Object -Last 6
if ($NoRun) { Write-Host '[run] pushed only'; exit 0 }

# --- pull the dumps and decode --------------------------------------------
$log = Get-Content "$RunDir\ps1_stage1.jsonl" -Raw

function Save-Png {
  param([byte[]]$Buf, [int]$Off, [int]$W, [int]$H, [int]$Stride,
        [bool]$Rgba, [bool]$FlipY, [string]$Path)
  Add-Type -AssemblyName System.Drawing
  $bmp = New-Object System.Drawing.Bitmap($W, $H)
  $rect = New-Object System.Drawing.Rectangle(0, 0, $W, $H)
  $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly,
                        [System.Drawing.Imaging.PixelFormat]::Format32bppRgb)
  $row = New-Object byte[] ($W * 4)
  for ($y = 0; $y -lt $H; $y++) {
    $sy = if ($FlipY) { $H - 1 - $y } else { $y }
    for ($x = 0; $x -lt $W; $x++) {
      if ($Rgba) {
        $i = $Off + ($sy * $Stride + $x) * 4
        $r = $Buf[$i]; $g = $Buf[$i + 1]; $b = $Buf[$i + 2]
      } else {
        # The [int] casts are load-bearing: PowerShell keeps byte arithmetic in
        # a byte, so "$Buf[$i+1] -shl 8" quietly evaluates to zero and every
        # frame comes out orange - the high half of the pixel simply vanishes.
        $i = $Off + ($sy * $Stride + $x) * 2
        $v = [int]$Buf[$i] -bor ([int]$Buf[$i + 1] -shl 8)
        $r = (($v -band 0x1F) -shl 3)
        $g = ((($v -shr 5) -band 0x1F) -shl 3)
        $b = ((($v -shr 10) -band 0x1F) -shl 3)
      }
      $o = $x * 4
      $row[$o] = $b; $row[$o + 1] = $g; $row[$o + 2] = $r; $row[$o + 3] = 255
    }
    [System.Runtime.InteropServices.Marshal]::Copy($row, 0,
      [IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $row.Length)
  }
  $bmp.UnlockBits($data)
  $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
}

foreach ($f in $Shots) {
  # What the software path actually put on the screen. This is the only dump the
  # non-GL run writes; ps1_gl<frame>.bin is left over from GL runs and comparing
  # it silently compares two stale files.
  $sc = "$RunDir\ps1_scr$f.bin"
  try { Invoke-Net ($link + @('-GetFile', "E:\PS1\logs\ps1_scr$f.bin", '-OutFile', $sc)) | Out-Null } catch { }
  if (Test-Path $sc) {
    $b = [IO.File]::ReadAllBytes($sc)
    $p = [regex]::Match($log, "pixB=(\d+) strideB=(\d+)")
    $pix = if ($p.Success) { [int]$p.Groups[1].Value } else { 4 }
    $str = if ($p.Success) { [int]$p.Groups[2].Value } else { 1280 }
    $w = [int]($str / $pix)
    $h = 240
    # The dump carries a short header, same as the VRAM one.
    $off = $b.Length - $str * $h
    Save-Png $b $off $w $h ($str / $pix) ($pix -eq 4) $false "$RunDir\scr$f.png"
    Write-Host "[run] scr$f.png ${w}x${h} pixB=$pix"
  }
  # What the GL renderer actually put on screen: RGBA, bottom-up, screen sized.
  $gl = "$RunDir\ps1_gl$f.bin"
  # Only under GL: otherwise the device still holds this file from an older GL
  # run, and pulling it hands back a picture this run never drew.
  if ($log -match "gl=1") {
  try { Invoke-Net ($link + @('-GetFile', "E:\PS1\logs\ps1_gl$f.bin", '-OutFile', $gl)) | Out-Null } catch { }
  }
  if (Test-Path $gl) {
    $m = [regex]::Match($log, "ps1_gl$f\.bin (\d+)x(\d+)")
    $w = if ($m.Success) { [int]$m.Groups[1].Value } else { 320 }
    $h = if ($m.Success) { [int]$m.Groups[2].Value } else { 240 }
    Save-Png ([IO.File]::ReadAllBytes($gl)) 0 $w $h $w $true $true "$RunDir\gl$f.png"
    Write-Host "[run] gl$f.png  ${w}x${h}"
  }
  # The software reference: a full VRAM dump, so cut out what the GPU scans out.
  $vr = "$RunDir\ps1_vram$f.bin"
  try { Invoke-Net ($link + @('-GetFile', "E:\PS1\logs\ps1_vram$f.bin", '-OutFile', $vr)) | Out-Null } catch { }
  if (Test-Path $vr) {
    $b = [IO.File]::ReadAllBytes($vr)
    $off = $b.Length - 1024 * 512 * 2      # the dump carries a short header
    $g = [regex]::Match($log, "frame=$f .{0,200}?scr=(\d+)x(\d+) src=(\d+),(\d+)")
    $w  = if ($g.Success) { [int]$g.Groups[1].Value } else { 320 }
    $h  = if ($g.Success) { [int]$g.Groups[2].Value } else { 240 }
    $sx = if ($g.Success) { [int]$g.Groups[3].Value } else { 0 }
    $sy = if ($g.Success) { [int]$g.Groups[4].Value } else { 0 }
    Save-Png $b ($off + ($sy * 1024 + $sx) * 2) $w $h 1024 $false $false "$RunDir\vram$f.png"
    Write-Host "[run] vram$f.png ${w}x${h} from ${sx},${sy}"
  }
}

# --- summary --------------------------------------------------------------
# What makes two runs comparable: the binary that was pushed, the clocks at the start
# and end of the pass, the output path (dsa/fb events), and the pass result itself.
Write-Host "[run] exe sha256 $((Get-FileHash -Algorithm SHA256 $Exe).Hash) $((Get-Item $Exe).Length) bytes, flavour $Flavour"
foreach ($pat in @('"event":"boot"', '"event":"exeid"', '"event":"clocks"', '"event":"fb"',
                   '"event":"dsa"', '"event":"pass"', '"event":"glr"', '"event":"profile"')) {
  Select-String -Path "$RunDir\ps1_stage1.jsonl" -SimpleMatch $pat |
    ForEach-Object { Write-Host $_.Line }
}
Write-Host "[run] -> $RunDir"
if ($Stage2Profile) {
  & pwsh -NoProfile -File "$PSScriptRoot\s2prof.ps1" -Log "$RunDir\ps1_stage1.jsonl" -Map "$Exe.map" -Sym "$Exe.nm.txt"
}
