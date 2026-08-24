# ---------------------------------------------------------------------------
# run.ps1 - one command for the whole cycle: build, push, run, pull, decode.
#
#   run.ps1 -Dev n95 -Name texfix -Build -Gpu
#   run.ps1 -Dev n95 -RunArgs "gui play nosnd frames6100 light blend imggt.pbp"
#   run.ps1 -Dev e90 -Name ctl            # same EXE over Bluetooth
#   run.ps1 -List                         # what is reachable right now
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
  [int[]]$Shots = @(6000), # frames whose dumps to pull and decode
  [int]$Timeout = 900,
  [switch]$NoRun,          # build and push only
  [switch]$List,           # probe every transport and exit
  [string]$Ip,             # override the WiFi address
  [string]$Com             # override the Bluetooth port
)

$ErrorActionPreference = 'Stop'
$Proj = Split-Path -Parent $PSScriptRoot
$Sdk  = 'D:\VsCodeProjects\SymbianProjects\S60_3rd_FP1'
$Exe  = "$Sdk\Epoc32\release\gcce\urel\ps1stage1.exe"
$Net  = 'D:\VsCodeProjects\SymbianProjects\SymbianDeployDaemon\tools\deploy_net.ps1'
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
if ($Core -or $Gpu) { $Build = $true }

function Build-Step {
  param([string]$Cmd, [string]$What)
  Write-Host "[run] === $What ==="
  $out = cmd /c "call `"$EnvCmd`" >nul & $Cmd" 2>&1
  $bad = $out | Select-String -Pattern 'rror|undefined reference|No such file'
  if ($bad) {
    $bad | Select-Object -First 10 | ForEach-Object { Write-Host $_ }
    throw "$What failed"
  }
}

if ($Core) {
  Build-Step "cd /d `"$Proj\configs\core\group`" & call bldmake bldfiles & call .\ABLD.BAT build gcce urel" 'libpcsxcore'
}
if ($Gpu) {
  Build-Step "call `"$PSScriptRoot\gpuasm-regen.cmd`"" 'gpu_unai asm'
  Build-Step "call `"$PSScriptRoot\gpu463-regen.cmd`"" 'gpu_unai 4.6.3'
}
if ($Build) {
  Build-Step "cd /d `"$Proj\configs\stage1\group`" & call bldmake bldfiles & call .\ABLD.BAT build gcce urel" 'stage1'
  $fi = [IO.FileInfo]$Exe
  Write-Host "[run] exe $($fi.Length) bytes, $($fi.LastWriteTime)"
}

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
  # What the GL renderer actually put on screen: RGBA, bottom-up, screen sized.
  $gl = "$RunDir\ps1_gl$f.bin"
  try { Invoke-Net ($link + @('-GetFile', "E:\PS1\logs\ps1_gl$f.bin", '-OutFile', $gl)) | Out-Null } catch { }
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
foreach ($pat in @('"event":"boot"', '"event":"glr"', '"event":"profile"')) {
  Select-String -Path "$RunDir\ps1_stage1.jsonl" -SimpleMatch $pat |
    ForEach-Object { Write-Host $_.Line }
}
Write-Host "[run] -> $RunDir"
