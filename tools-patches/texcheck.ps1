# ---------------------------------------------------------------------------
# texcheck.ps1 - unpack, on the host, exactly the texture pages the device
# renderer unpacked, using the VRAM dump from the same frame.
#
#   texcheck.ps1 -RunDir plans\runs\2026-08-24_..._texdesc_n95 -Frame 6000
#
# The run log carries a "glrtex" line per page the frame drew from (tba, cba,
# mode and the texture-window masks); ps1_vram<frame>.bin carries the VRAM the
# software rasteriser was reading at that instant. Decoding here with the same
# rules the device uses separates three explanations that look identical on a
# screenshot: a broken unpack, wrong texture coordinates, and a page that
# simply is that colour.
# ---------------------------------------------------------------------------
param(
  [Parameter(Mandatory = $true)][string]$RunDir,
  [int]$Frame = 6000
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$log  = Get-Content (Join-Path $RunDir 'ps1_stage1.jsonl') -Raw
$vram = [IO.File]::ReadAllBytes((Join-Path $RunDir "ps1_vram$Frame.bin"))
$off  = $vram.Length - 1024 * 512 * 2     # the dump carries a short text header

# The [int] casts matter: PowerShell keeps byte arithmetic in a byte, so a
# shifted byte silently evaluates to zero.
function Get-Texel { param([int]$Index)
  $i = $off + $Index * 2
  return ([int]$vram[$i] -bor ([int]$vram[$i + 1] -shl 8))
}

$rows = [regex]::Matches($log, 'slot=(\d+) tba=(\d+) cba=(-?\d+) mode=(\d+) mu=(\d+) mv=(\d+)')
if ($rows.Count -eq 0) { throw "no glrtex lines in $RunDir" }
Write-Host "[tex] $($rows.Count) сторінок"

foreach ($m in $rows) {
  $slot = [int]$m.Groups[1].Value
  $tba  = [int]$m.Groups[2].Value
  $cba  = [int]$m.Groups[3].Value
  $mode = [int]$m.Groups[4].Value
  $mu   = [int]$m.Groups[5].Value
  $mv   = [int]$m.Groups[6].Value
  $tpw  = if ($mode -eq 32) { 4 } elseif ($mode -eq 64) { 2 } else { 1 }

  $bmp  = New-Object System.Drawing.Bitmap(256, 256)
  $sum  = @(0, 0, 0)
  for ($y = 0; $y -lt 256; $y++) {
    $row = $tba + ($y -band $mv) * 1024
    for ($x = 0; $x -lt 256; $x++) {
      $sx = $x -band $mu
      $idx = $row + [int]($sx / $tpw)
      if ($idx -ge 1024 * 512) { continue }
      $w = Get-Texel $idx
      if ($mode -eq 32)     { $t = Get-Texel (($cba -shl 4) + (($w -shr (($sx -band 3) * 4)) -band 0xF)) }
      elseif ($mode -eq 64) { $t = Get-Texel (($cba -shl 4) + (($w -shr (($sx -band 1) * 8)) -band 0xFF)) }
      else                  { $t = $w }
      $r = (($t -band 0x1F) -shl 3); $g = ((($t -shr 5) -band 0x1F) -shl 3); $b = ((($t -shr 10) -band 0x1F) -shl 3)
      $sum[0] += $r; $sum[1] += $g; $sum[2] += $b
      $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, $r, $g, $b))
    }
  }
  $bmp.Save((Join-Path $RunDir "tex$slot.png"), [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  $bpp = if ($mode -eq 32) { 4 } elseif ($mode -eq 64) { 8 } else { 16 }
  "{0,2}  {1,2}bpp tba={2,6} cba={3,5} mask={4},{5}   середнє {6,4:N0}/{7,4:N0}/{8,4:N0}" -f `
    $slot, $bpp, $tba, $cba, $mu, $mv, ($sum[0] / 65536), ($sum[1] / 65536), ($sum[2] / 65536)
}
