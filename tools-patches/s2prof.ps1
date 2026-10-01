# ---------------------------------------------------------------------------
# s2prof.ps1 - turns the stage 2 events of a bench run into the share table.
#
#   s2prof.ps1 -Log run1\ps1_stage1.jsonl,run2\ps1_stage1.jsonl -Map S2.map -Sym S2.nm.txt
#
# The device only counts: PC samples folded into 16-byte buckets (s2hist), a few
# coarse counters (s2cls) and the dynarec counters (s2dyn, s2slow). Everything that
# needs a name is resolved here, from the linker map (which object owns an address)
# and from nm of the ELF the link produced (which function owns it, local ones
# included). Both files must come from the same link as the EXE that ran; the three
# anchor functions logged in s2map are checked against nm to prove it.
#
# Several logs give a mean and a range per class; one log gives the range over its
# four time windows, which is a weaker spread (windows follow each other, they are
# not independent runs).
# ---------------------------------------------------------------------------
param(
  [Parameter(Mandatory)][string[]]$Log,
  [Parameter(Mandatory)][string]$Map,
  [Parameter(Mandatory)][string]$Sym,
  [int]$Top = 20
)
$ErrorActionPreference = 'Stop'
# pwsh -File hands a comma list over as one string.
$Log = @($Log | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture

# --- linker map: which object owns which text range -----------------------
function Short-Object([string]$path) {
  $p = $path.Trim()
  if ($p -match '([^\\/()]+)\(([^)]+)\)\s*$') { return ("{0}({1})" -f $Matches[1], $Matches[2]).ToLower() }
  return ([IO.Path]::GetFileName($p)).ToLower()
}
$secs = New-Object System.Collections.Generic.List[object]
$pendingName = $null
foreach ($l in [IO.File]::ReadLines($Map)) {
  if ($l -match '^ (\.text\S*)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$') {
    $secs.Add([pscustomobject]@{ A = [Convert]::ToInt64($Matches[2], 16); S = [Convert]::ToInt64($Matches[3], 16); O = Short-Object $Matches[4] })
    $pendingName = $null
  } elseif ($l -match '^ (\.text\S*)\s*$') {
    $pendingName = $Matches[1]
  } elseif ($pendingName -and $l -match '^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$') {
    $secs.Add([pscustomobject]@{ A = [Convert]::ToInt64($Matches[1], 16); S = [Convert]::ToInt64($Matches[2], 16); O = Short-Object $Matches[3] })
    $pendingName = $null
  } else { $pendingName = $null }
}
$secs = @($secs | Where-Object { $_.S -gt 0 } | Sort-Object A)
if ($secs.Count -lt 20) { throw "map $Map gave only $($secs.Count) text sections - wrong file?" }
$secA = [int64[]]($secs | ForEach-Object { $_.A })
$textLo = $secA[0]

# --- nm: which function owns an address -----------------------------------
$symList = New-Object System.Collections.Generic.List[object]
foreach ($l in [IO.File]::ReadLines($Sym)) {
  if ($l -match '^([0-9a-f]{8})\s+(?:([0-9a-f]{8})\s+)?([tTwW])\s+(.+)$') {
    $sz = if ($Matches[2]) { [Convert]::ToInt64($Matches[2], 16) } else { 0 }
    $symList.Add([pscustomobject]@{ A = [Convert]::ToInt64($Matches[1], 16); S = $sz; N = $Matches[4] })
  }
}
$syms = @($symList | Sort-Object A)
$symA = [int64[]]($syms | ForEach-Object { $_.A })
if ($syms.Count -lt 100) { throw "nm file $Sym gave only $($syms.Count) symbols" }

# Index of the last element of $arr that is <= $v, or -1.
function Find-Le([int64[]]$arr, [int64]$v) {
  $lo = 0; $hi = $arr.Length - 1; $r = -1
  while ($lo -le $hi) {
    $mid = [int](($lo + $hi) / 2)
    if ($arr[$mid] -le $v) { $r = $mid; $lo = $mid + 1 } else { $hi = $mid - 1 }
  }
  return $r
}

# Major / minor class of an object. Order matters: first match wins.
function Get-Class([string]$obj) {
  switch -Regex ($obj) {
    '^pcsxcore\.lib\(linkage_arm\.o\)'                      { return @('linkage asm', 'linkage_arm.S') }
    '^pcsxcore\.lib\(new_dynarec\.o\)'                      { return @('dynarec C', 'new_dynarec.o (compiler, hash, helpers)') }
    '^pcsxcore\.lib\((gte|gte_nf|gte_arm|gte_nf_arm|gte_divider)\.o\)' { return @('GTE', $obj) }
    '^gpuasm\.lib\('                                        { return @('GPU', 'gpu_arm (asm)') }
    '^gpuunai\.lib\(gpulib_if\.o\)'                         { return @('GPU', 'gpulib_if (gpu_unai C++)') }
    '^gpuunai\.lib\('                                       { return @('GPU', 'gpulib driver ' + $obj) }
    '^(spu|registers|dma|freeze|out|nullsnd|arm_utils)\.o$' { return @('SPU', 'dfsound ' + $obj) }
    '^pcsxcore\.lib\(spu\.o\)'                              { return @('SPU', 'dfsound glue') }
    '^snd_symbian\.o$'                                      { return @('SPU', 'snd_symbian (feed, WSOLA)') }
    '^pcsxcore\.lib\(mdec\.o\)'                             { return @('MDEC', 'mdec.o') }
    '^pcsxcore\.lib\((cdriso|cdrom-async|ppf|deflate|inflate|trees|inffast|inftrees|crc32|adler32|zutil|uncompr|gzread|gzwrite|gzlib|gzclose)\.o\)' { return @('cdriso/zlib', $obj) }
    '^pcsxcore\.lib\('                                      { return @('libpcsxcore other', $obj) }
    '^(stage1|vout_symbian|glrender|app_symbian|i18n|accel|pmu_symbian|ocengine|ndrc_mem_symbian)\.o$' { return @('frontend', $obj) }
    '^(imgconv|archive|arc7z|arcrar|7z.*|lzma.*|lzma2dec|bcj2|bra.*|delta|cpuarch)\.o$' { return @('import/archive', $obj) }
    default                                                 { return @('runtime libs', $obj) }
  }
}

function Resolve-Link([int64]$linkAddr) {
  $i = Find-Le $secA $linkAddr
  if ($i -lt 0 -or $linkAddr -ge $secs[$i].A + $secs[$i].S) { return $null }
  $c = Get-Class $secs[$i].O
  $j = Find-Le $symA $linkAddr
  $name = '(no symbol)'
  if ($j -ge 0) {
    $s = $syms[$j]
    # nm gives sizes for compiled functions; asm labels carry none and own up to the next symbol.
    if ($s.S -eq 0 -or $linkAddr -lt $s.A + $s.S) { $name = $s.N }
    else { $name = '(gap after) ' + $s.N }
  }
  return [pscustomobject]@{ Major = $c[0]; Minor = $c[1]; Obj = $secs[$i].O; Fn = $name }
}

# --- events ----------------------------------------------------------------
function Field([string]$msg, [string]$name) {
  if ($msg -match "(?:^|\s)$([regex]::Escape($name))=(\S+)") { return $Matches[1] }
  return $null
}
function Nums([string]$s) { if (-not $s) { return @() } return @($s -split '[,/]' | ForEach-Object { [int64]$_ }) }

function Read-Run([string]$path) {
  $ev = New-Object System.Collections.Generic.List[object]
  foreach ($l in [IO.File]::ReadLines($path)) {
    if ($l.Length -lt 10 -or $l[0] -ne '{') { continue }
    try { $o = $l | ConvertFrom-Json } catch { continue }
    $ev.Add($o)
  }
  $mode = $null
  foreach ($e in $ev) { if ($e.event -eq 's2prof' -or $e.event -eq 's2dyn') { $mode = ($e.msg -split ':')[0] } }
  if (-not $mode) { throw "$path has no s2prof/s2dyn event - not a -Stage2Profile run" }
  $mine = @($ev | Where-Object { $_.msg -like "${mode}:*" })
  return [pscustomobject]@{ Path = $path; Mode = $mode; Ev = $mine; All = $ev }
}

function Last-Msg($run, [string]$type) {
  $x = @($run.Ev | Where-Object { $_.event -eq $type })
  if ($x.Count) { return $x[-1].msg }
  return $null
}

function Analyse-Run($run) {
  $r = [ordered]@{ Path = $run.Path }
  $pass = Last-Msg $run 'pass'
  $r.PassMs = [int64](Field $pass 'ms')
  $prof = $run.All | Where-Object { $_.event -eq 'profile' -and $_.msg -like "$($run.Mode):*" } | Select-Object -Last 1
  $r.EmuMs = if ($prof -and $prof.msg -match 'emu=(\d+)ms') { [int64]$Matches[1] } else { 0 }
  $r.Prof = Last-Msg $run 's2prof'
  $r.Dyn = Last-Msg $run 's2dyn'
  $r.Slow = Last-Msg $run 's2slow'
  $r.Pmu = Last-Msg $run 'pmu'
  $r.Cls = $null
  if ($r.Prof) {
    $map = Last-Msg $run 's2map'
    $codeBase = [Convert]::ToInt64(((Field $map 'code') -split ',')[0], 16)
    $anch = (Field $map 'anchors') -split ','
    $names = 'ndrc_symbian_alloc', 'ps1SpuInit', 'voutSymbianSetDsa'
    $deltas = @()
    for ($k = 0; $k -lt 3; $k++) {
      $s = $syms | Where-Object { $_.N -eq $names[$k] -or $_.N -like "$($names[$k])(*" } | Select-Object -First 1
      if (-not $s) { throw "anchor $($names[$k]) is not in the nm file - it does not belong to this link" }
      $deltas += ([Convert]::ToInt64($anch[$k], 16) - $s.A)
    }
    if (($deltas | Sort-Object -Unique).Count -ne 1) {
      throw ("anchors disagree ({0}): the map/nm files are not from the EXE that ran" -f (($deltas | ForEach-Object { '{0:x}' -f $_ }) -join ','))
    }
    $delta = $deltas[0]
    $r.Delta = $delta
    $r.CodeCheck = ($codeBase - $textLo) -eq $delta
    $cls = Last-Msg $run 's2cls'
    $r.Cls = $cls
    $rows = @{}   # class key -> int64[4]
    $fn = @{}     # function key -> [pscustomobject]
    $tc = [int64[]](0, 0, 0, 0); $sys = [int64[]](0, 0, 0, 0)
    for ($w = 0; $w -lt 4; $w++) {
      $v = Nums (Field $cls "w$w")
      $tc[$w] = $v[0]; $sys[$w] = $v[2]
    }
    $exeTot = [int64[]](0, 0, 0, 0)
    $unres = [int64[]](0, 0, 0, 0)
    foreach ($e in ($run.Ev | Where-Object { $_.event -eq 's2hist' -and $_.msg -match 'kind=exe shift=4' })) {
      $body = ($e.msg -split 'rows=', 2)[1]
      foreach ($tok in ($body -split '\s+' | Where-Object { $_ })) {
        $p = $tok -split ':'
        $off = [Convert]::ToInt64($p[0], 16)
        $c = Nums $p[1]
        # device offset -> run-time address -> address in the link the map and nm describe
        $link = ($codeBase + $off) - $delta
        $res = Resolve-Link $link
        for ($w = 0; $w -lt 4; $w++) { $exeTot[$w] += $c[$w] }
        if (-not $res) { for ($w = 0; $w -lt 4; $w++) { $unres[$w] += $c[$w] }; continue }
        $ck = $res.Major + '|' + $res.Minor
        if (-not $rows.ContainsKey($ck)) { $rows[$ck] = [int64[]](0, 0, 0, 0) }
        $fk = $res.Fn + '|' + $res.Obj
        if (-not $fn.ContainsKey($fk)) { $fn[$fk] = [pscustomobject]@{ Fn = $res.Fn; Obj = $res.Obj; Major = $res.Major; C = [int64[]](0, 0, 0, 0) } }
        for ($w = 0; $w -lt 4; $w++) { $rows[$ck][$w] += $c[$w]; $fn[$fk].C[$w] += $c[$w] }
      }
    }
    # what the device could not fit into its row list
    $rest = [int64[]](0, 0, 0, 0)
    foreach ($e in ($run.Ev | Where-Object { $_.event -eq 's2hist' -and $_.msg -match 'kind=exe end' })) {
      $rr = Nums (Field $e.msg 'rest')
      for ($w = 0; $w -lt 4; $w++) { $rest[$w] += $rr[$w] }
    }
    $sysRows = @()
    foreach ($e in ($run.Ev | Where-Object { $_.event -eq 's2hist' -and $_.msg -match 'kind=sys shift=8' })) {
      $body = ($e.msg -split 'rows=', 2)[1]
      foreach ($tok in ($body -split '\s+' | Where-Object { $_ })) {
        $p = $tok -split ':'
        $c = Nums $p[1]
        $sysRows += [pscustomobject]@{ Addr = $p[0]; N = ($c | Measure-Object -Sum).Sum }
      }
    }
    $r.Rows = $rows; $r.Fn = $fn; $r.Tc = $tc; $r.Sys = $sys; $r.Rest = $rest; $r.Unres = $unres
    $r.SysRows = $sysRows
    $r.ExecN = ($tc | Measure-Object -Sum).Sum + ($sys | Measure-Object -Sum).Sum + ($exeTot | Measure-Object -Sum).Sum + ($rest | Measure-Object -Sum).Sum
  }
  return [pscustomobject]$r
}

function Pct($n, $d) { if ($d -le 0) { return 0.0 } return 100.0 * $n / $d }
function Sum4($a) { return ($a | Measure-Object -Sum).Sum }

function Show-Run($a) {
  Write-Host ""
  Write-Host "=== $($a.Path)"
  Write-Host ("pass {0} ms, emu {1} ms" -f $a.PassMs, $a.EmuMs)
  if ($a.Prof) {
    Write-Host ("s2prof  {0}" -f $a.Prof)
    Write-Host ("s2cls   {0}" -f $a.Cls)
    if (-not $a.CodeCheck) { Write-Host "WARNING: code base minus .text start differs from the anchor delta (anchors win)" }
    $n = $a.ExecN
    Write-Host ""
    Write-Host ("Shares of emu (exec-stage samples = {0}):  share%  [windows min-max]  95% binomial  est ms" -f $n)
    $lines = @()
    $win = @{}
    $win['dynarec: translation cache'] = $a.Tc
    $win['Symbian: ROM/kernel/other (no symbols)'] = $a.Sys
    $major = @{}
    foreach ($k in $a.Rows.Keys) {
      $mj = ($k -split '\|')[0]
      if (-not $major.ContainsKey($mj)) { $major[$mj] = [int64[]](0, 0, 0, 0) }
      for ($w = 0; $w -lt 4; $w++) { $major[$mj][$w] += $a.Rows[$k][$w] }
    }
    foreach ($mj in $major.Keys) { $win[$mj] = $major[$mj] }
    $win['(rows the device dropped)'] = $a.Rest
    if (Sum4 $a.Unres) { $win['(exe address without object)'] = $a.Unres }
    $tot4 = [int64[]](0, 0, 0, 0)
    foreach ($k in $win.Keys) { for ($w = 0; $w -lt 4; $w++) { $tot4[$w] += $win[$k][$w] } }
    foreach ($k in ($win.Keys | Sort-Object { -(Sum4 $win[$_]) })) {
      $c = Sum4 $win[$k]
      if (-not $c) { continue }
      $p = Pct $c $n
      $sh = @(0..3 | ForEach-Object { Pct $win[$k][$_] $tot4[$_] })
      $se = 1.96 * 100.0 * [math]::Sqrt(($p / 100.0) * (1 - $p / 100.0) / [math]::Max($n, 1))
      $mark = if ($p -ge 5.0) { '*' } else { ' ' }
      Write-Host ("{0} {1,-42} {2,6:N2}  [{3,5:N1}-{4,5:N1}]  +-{5,4:N2}  {6,7:N0}" -f $mark, $k, $p, ($sh | Measure-Object -Minimum).Minimum, ($sh | Measure-Object -Maximum).Maximum, $se, ($p / 100.0 * $a.EmuMs))
      if ($major.ContainsKey($k)) {
        foreach ($rk in ($a.Rows.Keys | Where-Object { $_ -like "$k|*" } | Sort-Object { -(Sum4 $a.Rows[$_]) })) {
          $rc = Sum4 $a.Rows[$rk]
          if ((Pct $rc $n) -lt 0.5) { continue }
          Write-Host ("    {0,-38} {1,6:N2}" -f (($rk -split '\|', 2)[1]), (Pct $rc $n))
        }
      }
    }
    Write-Host ""
    Write-Host ("Top {0} functions in the program image (share of emu):" -f $Top)
    $i = 0
    foreach ($f in ($a.Fn.Values | Sort-Object { -(Sum4 $_.C) } | Select-Object -First $Top)) {
      $i++
      Write-Host ("{0,2}. {1,6:N2}%  {2,-16} {3,-30} {4}" -f $i, (Pct (Sum4 $f.C) $n), $f.Major, $f.Obj, $f.Fn)
    }
    if ($a.SysRows.Count) {
      Write-Host ""
      Write-Host "Top addresses outside the program image and the translation cache (256-byte buckets):"
      foreach ($s in ($a.SysRows | Sort-Object { -$_.N } | Select-Object -First 8)) { Write-Host ("  {0,8}  {1,6:N2}%" -f $s.Addr, (Pct $s.N $n)) }
    }
  }
  if ($a.Dyn) {
    $rcMs = [int64](Field $a.Dyn 'rcMs')
    Write-Host ""
    Write-Host ("D2  {0}" -f $a.Dyn)
    Write-Host ("    recompile {0} ms = {1:N2}% of the pass ({2} ms); cache flushes {3} ms" -f $rcMs, (Pct $rcMs $a.PassMs), $a.PassMs, (Field $a.Dyn 'imbMs'))
    $live = [double](Field $a.Dyn 'htLive'); $h0 = [double](Field $a.Dyn 'htHit0'); $h1 = [double](Field $a.Dyn 'htHit1'); $sl = [double](Field $a.Dyn 'htSlow')
    if ($live -gt 0) { Write-Host ("    ndrc_get_addr_ht from running code: {0:N0} ({1:N0}/s), hash hit {2:N1}%+{3:N1}%, miss to get_addr {4:N2}%" -f $live, ($live * 1000 / [math]::Max($a.PassMs, 1)), (Pct $h0 $live), (Pct $h1 $live), (Pct $sl $live)) }
  }
  if ($a.Slow) {
    Write-Host ""
    Write-Host ("D1  {0}" -f $a.Slow)
    $rd = Nums (Field $a.Slow 'rd'); $wr = Nums (Field $a.Slow 'wr')
    $all = @(0..5 | ForEach-Object { $rd[$_] + $wr[$_] }); $t = Sum4 $all
    $names = 'scratch', 'scratchTail', 'io', 'bios', 'ram', 'other'
    for ($k = 0; $k -lt 6; $k++) { Write-Host ("    {0,-12} {1,12:N0}  {2,6:N2}%" -f $names[$k], $all[$k], (Pct $all[$k] $t)) }
    Write-Host ("    scratchpad share of dynamic slow-path accesses: {0:N2}% of {1:N0}" -f (Pct $all[0] $t), $t)
  }
  if ($a.Pmu) {
    Write-Host ""
    $c = [double](Field $a.Pmu 'cyc64'); $p0 = [double](Field $a.Pmu 'pmn0'); $p1 = [double](Field $a.Pmu 'pmn1')
    $cyc = $c * 64
    Write-Host ("PMU {0}" -f $a.Pmu)
    Write-Host ("    cycles {0:N0} = {1:N1} MHz over the pass (valid only if it matches the core clock); pmn0/cycle {2:N4}, pmn1/cycle {3:N4}" -f $cyc, ($cyc / [math]::Max($a.PassMs, 1) / 1000), ($p0 / [math]::Max($cyc, 1)), ($p1 / [math]::Max($cyc, 1)))
  }
}

$runs = @($Log | ForEach-Object { Analyse-Run (Read-Run $_) })
foreach ($a in $runs) { Show-Run $a }

if (($runs | Where-Object { $_.Prof }).Count -gt 1) {
  $withProf = @($runs | Where-Object { $_.Prof })
  Write-Host ""
  Write-Host ("=== {0} runs: share of emu per class (mean, min-max across runs)" -f $withProf.Count)
  $keys = @{}
  foreach ($a in $withProf) {
    $keys['dynarec: translation cache'] = 1; $keys['Symbian: ROM/kernel/other (no symbols)'] = 1
    foreach ($k in $a.Rows.Keys) { $keys[($k -split '\|')[0]] = 1 }
  }
  $tab = foreach ($k in $keys.Keys) {
    $v = foreach ($a in $withProf) {
      $c = switch ($k) {
        'dynarec: translation cache' { Sum4 $a.Tc }
        'Symbian: ROM/kernel/other (no symbols)' { Sum4 $a.Sys }
        default { $s = 0; foreach ($rk in $a.Rows.Keys) { if (($rk -split '\|')[0] -eq $k) { $s += Sum4 $a.Rows[$rk] } }; $s }
      }
      Pct $c $a.ExecN
    }
    [pscustomobject]@{ Class = $k; Mean = ($v | Measure-Object -Average).Average; Min = ($v | Measure-Object -Minimum).Minimum; Max = ($v | Measure-Object -Maximum).Maximum }
  }
  foreach ($t in ($tab | Sort-Object Mean -Descending)) {
    Write-Host ("{0} {1,-42} {2,6:N2}  [{3,5:N2}-{4,5:N2}]" -f $(if ($t.Mean -ge 5) { '*' } else { ' ' }), $t.Class, $t.Mean, $t.Min, $t.Max)
  }
}
