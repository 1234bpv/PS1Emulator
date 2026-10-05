# deploy_net.ps1 - PC-side client for the on-device deploy daemon / tester
# (ps1daemon.exe).
#
# Speaks the daemon's line+binary protocol over WiFi/TCP so a full iteration runs
# without touching the phone screen: push the fresh EXE, launch it, press keys,
# grab a screenshot, pull the log.
#
#   deploy_net.ps1 -PutFile <local> -Remote <devpath> [-HashRemote <devpath>]
#   deploy_net.ps1 -GetFile <devpath> -OutFile <local>
#   deploy_net.ps1 -Run "E:\sys\bin\app.exe 300"          # blocks; "300" is an
#         ARGUMENT TO THE APP, not a timeout. The ceiling is the session default
#         (120 s, changeable with SET RUNTIMEOUT); to set one explicitly use
#         EXEC: -Cmd "EXEC 30000 E:\sys\bin\app.exe".
#         Never RUN anything that takes the screen: a fullscreen window parks
#         the radio, the AP drops the phone, and the daemon is stuck in
#         WaitForRequest with no way to reconnect. Use a SCRIPT for those.
#   deploy_net.ps1 -Start "E:\sys\bin\app.exe"            # returns at once
#   deploy_net.ps1 -App 0xE0001010                        # launch by UID3
#   deploy_net.ps1 -Key ok -Key ok -Key rsk               # press keys in order
#   deploy_net.ps1 -Do "KEY down; KEY down; KEY ok; WAITFG hexen 5000"
#         runs each ';'-separated command in order, over ONE connection, and
#         stops at the first ERR instead of pressing on blind past it.
#   deploy_net.ps1 -Observe -OutFile .\look.png
#         one screenshot, pulled down and turned into a PNG, printed next to
#         FG and the screen size from INFO. Meant to follow -Do in the same
#         call: act through a sequence, then look once at the result.
#   deploy_net.ps1 -Shot "E:\tester\menu.bmp" -OutFile .\menu.bmp
#   deploy_net.ps1 -Info
#   deploy_net.ps1 -GetTime
#   deploy_net.ps1 -SetTime "2026-09-16 12:00:00"
#   deploy_net.ps1 -SyncTime
#   deploy_net.ps1 -Ls "E:\tester\"
#   deploy_net.ps1 -Cmd "WG" -Cmd "STAT E:\Hexen2\stdout.log"
#   deploy_net.ps1 -Scenario .\scenarios\hexen2_smoke.txt -PullTo .\out\shots
#
# Everything runs over ONE connection in the order the switches are listed below
# (put -> hash -> app/start/run -> keys -> shot -> get -> pull -> del), so a whole
# iteration costs one LAN scan instead of one per step.
#
# The phone's IP is written by the daemon to E:\daemon_ip.txt and cached here in
# .daemon_ip, so -Ip can normally be omitted.

param(
    # IP of the daemon, or "scan"/"auto"/empty to auto-discover it on the LAN.
    [string]$Ip = "",
    [int]$Port = 2323,

    # Device label. The address cache becomes .daemon_ip.<label>, so two phones
    # on the same LAN stop overwriting each other's cached address.
    [string]$Device = "",

    # Bluetooth transport. Give the outgoing SPP COM port of the paired phone
    # (e.g. -Com COM7) and the whole WiFi/TCP path is bypassed: the same text
    # protocol runs over the serial stream. For E90 (WiFi unreachable) and C5
    # (no WiFi). Baud is ignored by Bluetooth SPP but SerialPort needs a value.
    [string]$Com = "",
    [int]$Baud = 115200,

    # --- files ---
    [string]$PutFile,
    [string]$Remote,
    [string]$HashRemote,
    [string]$GetFile,
    [string]$OutFile,
    # One or more device paths to delete, comma-separated. Runs last, so a cycle
    # can pull a log and then drop the scratch files it came from.
    [string]$DelFile,
    [string]$Ls,

    # --- processes ---
    [string]$Run,
    [int]$RunTimeoutSec = 180,
    [string]$Start,
    [string]$App,

    # --- device control ---
    [string[]]$Key,
    [string]$Text,
    [string]$Tap,
    [string]$Shot,
    [int]$ShotStep = 1,
    [switch]$Info,
    [switch]$Wg,

    # --- agent mode: act through a sequence, then look once ---
    # -Do runs its ';'-separated commands in order, over the same connection,
    # and stops at the first ERR. -Observe takes one screenshot afterwards
    # (or on its own) and reports FG + screen size next to the PNG. -OutFile
    # picks where that PNG lands, same as for -Shot.
    [string]$Do,
    [switch]$Observe,

    # --- clock ---
    [switch]$GetTime,
    [string]$SetTime,
    [switch]$SyncTime,

    # --- scenarios ---
    # Local scenario file: uploaded, run on the phone, then its log is pulled.
    [string]$Scenario,
    [string]$ScenarioRemoteDir = "E:\tester",
    [int]$ScenarioTimeoutSec = 600,
    [switch]$Status,
    [switch]$Cancel,
    [switch]$Reboot,
    [switch]$RebootCold,
    [switch]$Restart,

    # Pull every file from a device directory (screenshots, logs) into -PullTo.
    [string]$Pull,
    [string]$PullTo,

    # Raw commands, executed in order. The escape hatch for anything the
    # switches above do not cover - the protocol is plain text on purpose.
    [string[]]$Cmd,

    [switch]$Cycle
)

$ErrorActionPreference = 'Stop'

# The daemon writes BMP because that needs no encoder on the phone (see README).
# On this side a BMP is awkward to look at, so every pulled .bmp also lands as a
# .png next to it. Failure here is not fatal: the BMP is the real result, the
# PNG only saves a manual conversion step.
function ConvertTo-Png([string]$bmpPath) {
    if (-not $bmpPath -or -not ($bmpPath.ToLower().EndsWith('.bmp'))) { return $null }
    if (-not (Test-Path $bmpPath)) { return $null }
    $png = [System.IO.Path]::ChangeExtension($bmpPath, '.png')
    try {
        Add-Type -AssemblyName System.Drawing -ErrorAction Stop
        $img = [System.Drawing.Image]::FromFile($bmpPath)
        try { $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png) } finally { $img.Dispose() }
        Write-Host "[net] png $png"
        return $png
    } catch {
        Write-Host "[net] png conversion skipped: $($_.Exception.Message)"
        return $null
    }
}

# Verbs whose reply is a block of lines terminated by a literal "END".
$script:BlockVerbs = @('LS', 'PS', 'WG', 'INFO', 'TOP', 'AP')

function Connect-Daemon($ip, $port) {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.NoDelay = $true
    $c.Connect($ip, $port)
    return $c
}

# Bluetooth transport: open the phone's outgoing SPP COM port. Returns the
# SerialPort; its BaseStream is used exactly like a TCP NetworkStream, because
# the protocol code only touches ReadByte/Read/Write/Flush/ReadTimeout, which a
# serial stream supports too. A default ReadTimeout matters: the serial stream
# starts at InfiniteTimeout, and a stuck read would then hang forever.
function Open-SerialPort($com, $baud) {
    $sp = New-Object System.IO.Ports.SerialPort $com, $baud, 'None', 8, 'One'
    $sp.ReadTimeout  = -1        # InfiniteTimeout; callers set finite where needed
    $sp.WriteTimeout = 120000
    $sp.ReadBufferSize = 1048576 # Restoration readback: larger host serial buffer
    $sp.Open()
    # A freshly opened SPP port can carry stale bytes from a dropped session.
    try { $sp.DiscardInBuffer(); $sp.DiscardOutBuffer() } catch {}
    return $sp
}

# Confirm the host on $ip:$port is our daemon (not some other service on 2323):
# BYE must be answered with a line starting 'B'. Closes the socket either way.
function Test-IsDaemon($ip, $port) {
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $c.BeginConnect($ip, $port, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne(1500)) { return $false }
        $c.EndConnect($iar)
        $s = $c.GetStream(); $s.ReadTimeout = 2000
        $b = [System.Text.Encoding]::ASCII.GetBytes("BYE`n")
        $s.Write($b, 0, $b.Length); $s.Flush()
        return ($s.ReadByte() -eq 66)   # 'B' from "BYE\n"
    } catch { return $false } finally { $c.Close() }
}

# Auto-discover the daemon: scan each local IPv4 /24 in parallel for an open 2323,
# then verify by the BYE handshake. Returns the IP string, or $null.
function Find-Daemon($port) {
    $locals = @()
    try {
        $locals = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
            Where-Object { $_.IPAddress -ne '127.0.0.1' -and $_.IPAddress -notlike '169.254.*' } |
            Select-Object -ExpandProperty IPAddress
    } catch {
        $locals = [System.Net.Dns]::GetHostAddresses([System.Net.Dns]::GetHostName()) |
            Where-Object { $_.AddressFamily -eq 'InterNetwork' } | ForEach-Object { $_.ToString() }
    }
    # Prefer private LAN ranges; keep others as fallback.
    $locals = @($locals | Where-Object { $_ -like '192.168.*' -or $_ -like '10.*' -or $_ -like '172.*' }) +
              @($locals | Where-Object { -not ($_ -like '192.168.*' -or $_ -like '10.*' -or $_ -like '172.*') })
    $seen = @{}
    foreach ($local in ($locals | Select-Object -Unique)) {
        $o = $local.Split('.')
        if ($o.Count -ne 4) { continue }
        $base = "{0}.{1}.{2}." -f $o[0], $o[1], $o[2]
        if ($seen.ContainsKey($base)) { continue }
        $seen[$base] = $true
        $self = [int]$o[3]
        Write-Host "[net] scanning ${base}0/24 for the daemon on port $port ..."
        $pending = @()
        for ($h = 1; $h -le 254; $h++) {
            if ($h -eq $self) { continue }
            $ip = $base + $h
            $c = New-Object System.Net.Sockets.TcpClient
            $iar = $c.BeginConnect($ip, $port, $null, $null)
            $pending += [pscustomobject]@{ Ip = $ip; Client = $c; Iar = $iar }
        }
        # 600 ms was not enough: the phone answers over WiFi slower than wired
        # hosts, so the scan "found nothing" with the daemon plainly running.
        # This costs exactly one pause per subnet sweep.
        Start-Sleep -Milliseconds 2500  # let LAN connects race to completion
        $hits = @()
        foreach ($p in $pending) {
            if ($p.Iar.IsCompleted) {
                try { $p.Client.EndConnect($p.Iar); $hits += $p.Ip } catch { }
            }
            $p.Client.Close()
        }
        $found = @()
        foreach ($ip in $hits) {
            if (Test-IsDaemon $ip $port) { $found += $ip }
        }
        if ($found.Count) { return [string[]]$found }
    }
    return @()
}

# One INFO line worth showing when several daemons answer: the screen size tells
# the phones apart (N95 320x240, C5 240x320, 701 640x360) without any extra
# bookkeeping on this side.
function Get-DaemonTag($ip, $port) {
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $c.BeginConnect($ip, $port, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne(1500)) { return "?" }
        $c.EndConnect($iar)
        $s = $c.GetStream(); $s.ReadTimeout = 3000
        $b = [System.Text.Encoding]::ASCII.GetBytes("INFO`n")
        $s.Write($b, 0, $b.Length); $s.Flush()
        $r = New-Object System.IO.StreamReader($s, [System.Text.Encoding]::ASCII)
        $tag = @()
        for ($i = 0; $i -lt 40; $i++) {
            $line = $r.ReadLine()
            if ($null -eq $line -or $line -eq "END") { break }
            if ($line -match "^screen (.+)$" -or $line -match "^ap (.+)$") { $tag += $Matches[1] }
        }
        if ($tag.Count) { return ($tag -join " ") } else { return "?" }
    } catch { return "?" } finally { $c.Close() }
}

# Read one text line (up to \n), return without trailing CR/LF.
function Read-Line($stream) {
    $bytes = New-Object System.Collections.Generic.List[byte]
    while ($true) {
        $b = $stream.ReadByte()
        if ($b -lt 0) { throw "connection closed while reading reply" }
        if ($b -eq 10) { break }        # \n
        if ($b -ne 13) { $bytes.Add([byte]$b) }  # skip \r
    }
    return [System.Text.Encoding]::ASCII.GetString($bytes.ToArray())
}

function Read-Exact($stream, [int]$n, $outFs) {
    $buf = New-Object byte[] 8192
    $remaining = $n
    while ($remaining -gt 0) {
        $take = [Math]::Min($remaining, $buf.Length)
        $got = $stream.Read($buf, 0, $take)
        if ($got -le 0) { throw "connection closed after $($n-$remaining)/$n bytes" }
        $outFs.Write($buf, 0, $got)
        $remaining -= $got
    }
}

function Send-Text($stream, [string]$line) {
    $b = [System.Text.Encoding]::ASCII.GetBytes($line)
    $stream.Write($b, 0, $b.Length)
    $stream.Flush()
}

# Verbs after which the link is torn down on purpose: the daemon answers first
# and drops the connection immediately, so the reply often never makes it out.
# Waiting for it forever is the documented trap - the default ReadTimeout is
# infinite, so a lost line hangs the client instead of failing.
function Test-ResetsLink([string]$line) {
    $parts = $line -split '\s+', 2
    $verb = $parts[0].ToUpper()
    if ($verb -eq 'NETRESET') { return $true }
    # RESTART answers once and then the daemon exits, so the link goes away by
    # design - the successor is a new process with a new connection.
    if ($verb -eq 'RESTART') { return $true }
    # "AP" alone only lists; "AP <name>" saves and reconnects.
    if ($verb -eq 'AP' -and $parts.Count -gt 1 -and $parts[1].Trim()) { return $true }
    return $false
}

# Send one text command and collect its reply. Block replies (LS/PS/WG/INFO) are
# read until the "END" sentinel; everything else is a single line.
function Invoke-Cmd($stream, [string]$line, [switch]$Quiet) {
    if (-not $Quiet) { Write-Host "[net] > $line" }
    Send-Text $stream ($line + "`n")
    $verb = ($line -split '\s+')[0].ToUpper()

    if (Test-ResetsLink $line) {
        # Bounded wait: the answer is welcome but not required, because the
        # command's whole point is to drop the link it would travel over.
        $old = $stream.ReadTimeout
        $stream.ReadTimeout = 8000
        try {
            $first = Read-Line $stream
            if (-not $Quiet) { Write-Host "[net] < $first" }
            return @($first)
        } catch {
            Write-Host "[net] < (no reply - the link is being reset, as asked)"
            return @("OK")
        } finally { $stream.ReadTimeout = $old }
    }

    $first = Read-Line $stream
    $lines = @($first)
    if ($script:BlockVerbs -contains $verb -and $first.StartsWith("OK")) {
        while ($true) {
            $l = Read-Line $stream
            if ($l -eq 'END') { break }
            $lines += $l
        }
    }
    if (-not $Quiet) { $lines | ForEach-Object { Write-Host "[net] < $_" } }
    # Returned plain, NOT as ,$lines. The comma wraps the array in another array,
    # and a caller writing @(Invoke-Cmd ...) then gets a one-element array whose
    # single element is the real array - $lines[0] is an array, $lines.Count is 1,
    # and a listing silently reads as empty. Callers wrap with @() instead, which
    # also restores the array when PowerShell unrolls a one-line reply to a string.
    return $lines
}

# REBOOT answers in two beats: an acknowledgement, then - only if the firmware
# refused - an error line. Success is therefore silence followed by the link
# going away, which is why this needs its own handler rather than -Cmd.
function Do-Reboot($stream, [switch]$Cold) {
    $cmd = if ($Cold) { "REBOOT cold" } else { "REBOOT" }
    Write-Host "[net] > $cmd"
    Send-Text $stream ($cmd + "`n")
    Write-Host "[net] < $(Read-Line $stream)"
    $stream.ReadTimeout = 6000
    try {
        $second = Read-Line $stream
        Write-Warning "[net] < $second - the phone did NOT reboot"
        if ($second -match '-5$') {
            Write-Host "[net] -5 is KErrNotSupported: this firmware does not implement"
            Write-Host "[net] UserSvr::ResetMachine (measured on the N95 8GB). Reboot by hand."
        }
        return $false
    } catch {
        Write-Host "[net] no second line - the phone is rebooting"
        return $true
    } finally { try { $stream.ReadTimeout = -1 } catch {} }
}

# RESTART answers once - "OK restart requested" - and the process then exits so
# a successor can start; the link goes away by design (Test-ResetsLink already
# knows this). The daemon is headless on purpose, so whether the successor
# actually comes back is not something to leave to a human glancing at INFO
# later: reconnect here and poll for it, and fail loudly if it never shows up.
#
# Measured now that the daemon listens with SO_REUSEADDR: the successor starts
# listening again ~5s after RESTART, and the client sees it at ~5.6s on the E90
# over Bluetooth and ~11s on the N95 over WiFi - the gap above 5s there is this
# loop's own poll step, not the network coming back. The old "up to a minute"
# on WiFi was TIME_WAIT on the port before SO_REUSEADDR existed, not
# reassociation. The 90s default is headroom for a bad day, not the expected
# wait - do not shrink it to match the measured numbers.
function Do-Restart($stream, [int]$TimeoutSec = 90) {
    Assert-Ok (Invoke-Cmd $stream "RESTART") "RESTART"
    & $script:closeConn
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Milliseconds 500
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        try {
            $s = & $script:reconnect
            $s.ReadTimeout = 4000
            $info = @(Invoke-Cmd $s "INFO" -Quiet)
            if ($info[0].StartsWith("OK")) {
                foreach ($l in $info) {
                    if ($l -match '\bby (\w)\b' -and $Matches[1] -eq 'r') {
                        Write-Host ("[net] restarted - back after {0:N1}s (INFO: by r)" -f $sw.Elapsed.TotalSeconds)
                        return
                    }
                }
            }
        } catch {
            # Not up yet, or mid-reassociation on WiFi - keep polling.
        }
        Start-Sleep -Milliseconds 1500
    }
    throw ("RESTART failed: no daemon answering INFO with 'by r' within {0}s" -f $TimeoutSec)
}

function Assert-Ok($lines, [string]$what) {
    $first = @($lines)[0]
    if (-not $first.StartsWith("OK")) { throw "$what failed: $first" }
}

function Do-Put($stream, [string]$local, [string]$remote) {
    if (-not (Test-Path $local)) { throw "local file not found: $local" }
    # Same trap as in Do-Get: .NET resolves a relative path against the PROCESS
    # working directory, while Test-Path/Get-Item use PowerShell's location - so
    # the checks pass and the read then fails on a different file.
    $local = (Resolve-Path $local).Path
    $fi = Get-Item $local
    $size = $fi.Length
    Write-Host ("[net] PUT {0} -> {1} ({2:N0} bytes)" -f $local, $remote, $size)
    Send-Text $stream ("PUT {0} {1}`n" -f $remote, $size)
    $fs = [System.IO.File]::OpenRead($local)
    try {
        $buf = New-Object byte[] 8192
        $sent = 0
        while ($true) {
            $r = $fs.Read($buf, 0, $buf.Length)
            if ($r -le 0) { break }
            try { $stream.Write($buf, 0, $r) }
            catch {
                # The daemon answers a refused PUT (ERR open -46 and the like) before
                # the body arrives, then chokes on the rest of it. Without reading that
                # answer the refusal looks like a link that dies mid-transfer.
                $early = $null
                try { $prev = $stream.ReadTimeout; $stream.ReadTimeout = 3000
                      $early = Read-Line $stream } catch {} finally { try { $stream.ReadTimeout = $prev } catch {} }
                if ($early) { throw "PUT refused after $sent bytes: $early" }
                throw
            }
            $sent += $r
            if (($sent % 262144) -lt $buf.Length) { Write-Host ("[net] .. {0:N0}/{1:N0}" -f $sent, $size) }
        }
        $stream.Flush()
    } finally { $fs.Close() }
    $resp = Read-Line $stream
    if (-not $resp.StartsWith("OK")) { throw "PUT failed: $resp" }
    Write-Host "[net] $resp"
}

# On removable media S60v3 validates an EXE against \sys\hash\<name> (20-byte SHA-1).
# After overwriting the EXE we must rewrite that digest or the loader returns -46.
function Do-PutHash($stream, [string]$local, [string]$hashRemote) {
    $local = (Resolve-Path $local).Path
    $sha1 = [System.Security.Cryptography.SHA1]::Create()
    $bytes = $sha1.ComputeHash([System.IO.File]::ReadAllBytes($local))
    Write-Host ("[net] PUT hash -> {0} (20 bytes)" -f $hashRemote)
    Send-Text $stream ("PUT {0} 20`n" -f $hashRemote)
    $stream.Write($bytes, 0, 20)
    $stream.Flush()
    $resp = Read-Line $stream
    if (-not $resp.StartsWith("OK")) { throw "PUT hash failed: $resp" }
    Write-Host "[net] $resp"
}

function Do-Run($stream, [string]$cmdline, [int]$timeoutSec) {
    Write-Host "[net] RUN $cmdline"
    # The daemon now has its own ceiling too; ours is deliberately longer so the
    # device-side timeout is what fires, and we get a reply instead of a hang.
    $stream.ReadTimeout = ($timeoutSec + 30) * 1000
    Send-Text $stream ("SET runtimeout {0}`n" -f ($timeoutSec * 1000))
    [void](Read-Line $stream)
    Send-Text $stream ("RUN {0}`n" -f $cmdline)
    $resp = Read-Line $stream
    $stream.ReadTimeout = -1
    if ($resp.StartsWith("EXIT 0")) { Write-Host "[net] $resp" }
    elseif ($resp.StartsWith("EXIT")) { Write-Warning "[net] $resp (non-zero exit)" }
    elseif ($resp.StartsWith("TIMEOUT")) { Write-Warning "[net] $resp (killed on the phone)" }
    else { throw "RUN failed: $resp" }
}

function Do-Del($stream, [string]$remote) {
    foreach ($r in ($remote -split ',')) {
        $t = $r.Trim()
        if (-not $t) { continue }
        # Not fatal: deleting something already gone is the normal cleanup case.
        [void](Invoke-Cmd $stream ("DEL {0}" -f $t))
    }
}

function Do-Get($stream, [string]$remote, [string]$out) {
    # Absolute path before touching .NET file APIs: they resolve a relative path
    # against the PROCESS working directory, which is not PowerShell's location.
    # A relative -OutFile then lands somewhere else and the caller cannot find
    # the file it just downloaded.
    if (-not [System.IO.Path]::IsPathRooted($out)) {
        $out = Join-Path (Get-Location).Path $out
    }
    Write-Host "[net] GET $remote -> $out"
    Send-Text $stream ("GET {0}`n" -f $remote)
    $resp = Read-Line $stream
    if (-not $resp.StartsWith("OK")) { throw "GET failed: $resp" }
    $size = [int]($resp.Split(' ')[1])
    $dir = Split-Path $out -Parent
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    $fs = [System.IO.File]::Create($out)
    try { Read-Exact $stream $size $fs } finally { $fs.Close() }
    Write-Host ("[net] got {0:N0} bytes" -f $size)
}

# Copy a whole device directory down. Files only, one level - which is all the
# screenshots-and-logs case needs.
function Do-Pull($stream, [string]$devDir, [string]$localDir) {
    if (-not $devDir.EndsWith('\')) { $devDir += '\' }
    $lines = @(Invoke-Cmd $stream ("LS {0}" -f $devDir) -Quiet)
    if (-not $lines[0].StartsWith("OK")) { Write-Warning "[net] pull: $($lines[0])"; return }
    if (-not (Test-Path $localDir)) { New-Item -ItemType Directory -Path $localDir | Out-Null }
    $n = 0
    foreach ($l in $lines[1..($lines.Count - 1)]) {
        $parts = $l -split '\s+', 3
        if ($parts.Count -lt 3 -or $parts[0] -ne 'F') { continue }
        $name = $parts[2]
        $local = Join-Path $localDir $name
        Do-Get $stream ($devDir + $name) $local
        [void](ConvertTo-Png $local)
        $n++
    }
    Write-Host "[net] pulled $n file(s) from $devDir"
}

# -Do: a ';'-separated action sequence, run in order over one connection, for
# an agent stepping the phone through a UI flow. Stops at the first failed
# step rather than pressing on blind - that is exactly what once sent keys
# into the wrong foreground app and launched something unintended. Returns an
# object, not a bare bool: { Ok; LinkReset }. Ok mirrors the old $true/$false
# contract (returned instead of thrown, so a caller that also asked for
# -Observe still gets to see the screen the failed step left behind). LinkReset
# tells the caller whether the sequence's actual last step tore the connection
# down (NETRESET to close out a game is the documented case) - -Observe needs
# to know that to reconnect before it tries to use the same dead $stream.
#
# "Failed" is judged by the shape of the reply, not by a per-verb allowlist -
# that list would have to be kept in step with the firmware by hand and would
# drift silently. Do NOT go back to "does not start with OK": LOG and ECHO
# hand back their raw text with no OK/ERR prefix at all (tester_exec.cpp), so
# that rule flags every successful LOG as a failed step and stops the whole
# sequence there - measured on the E90, and it also swallowed a genuine ERR
# on the step right after, because the sequence never reached it. The
# protocol's actual contract (README: a reply starts with OK, ERR, EXIT,
# TIMEOUT or BYE) reserves ERR for failure, so that is the check. TIMEOUT -
# RUN/EXEC hitting their ceiling - counts as failure too: a step that never
# finished is not a step that succeeded. EXIT <code> <category> - RUN/EXEC's
# own way of answering - is judged by the code: 0 is a clean exit, anything
# else is the program having failed.
function Do-Sequence($stream, [string]$do) {
    $steps = @($do -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })

    # A link-resetting command (RESTART, NETRESET, AP <name> - Test-ResetsLink
    # already knows exactly which, reused here rather than a second list that
    # could drift from it) leaves nothing for the next step to read from:
    # Invoke-Cmd's Read-Line throws on the dead connection, uncaught, and the
    # promised "stop cleanly, return $false" turns into a crash instead. As the
    # LAST step it is fine - NETRESET-to-finish is the documented way to end a
    # -Do run - so this only forbids it earlier in the sequence, checked before
    # any step runs at all, so a bad sequence refuses outright instead of
    # doing some of its steps and then dying mid-way.
    for ($i = 0; $i -lt $steps.Count - 1; $i++) {
        if (Test-ResetsLink $steps[$i]) {
            Write-Warning ("[net] -Do refused: step {0}/{1} '{2}' resets the link and is not the last step" -f ($i + 1), $steps.Count, $steps[$i])
            return [pscustomobject]@{ Ok = $false; LinkReset = $false }
        }
    }

    for ($i = 0; $i -lt $steps.Count; $i++) {
        $lines = @(Invoke-Cmd $stream $steps[$i])
        $first = $lines[0]
        $failed = $first.StartsWith("ERR") -or $first.StartsWith("TIMEOUT")
        if (-not $failed -and $first.StartsWith("EXIT")) {
            $failed = (($first -split '\s+')[1]) -ne '0'
        }
        if ($failed) {
            Write-Warning ("[net] -Do stopped at step {0}/{1} '{2}': {3}" -f ($i + 1), $steps.Count, $steps[$i], $first)
            return [pscustomobject]@{ Ok = $false; LinkReset = $false }
        }
    }
    # The preflight loop above already guarantees a link-resetting step can
    # only be the last one, so checking just that one tells the whole story.
    $linkReset = ($steps.Count -gt 0) -and (Test-ResetsLink $steps[$steps.Count - 1])
    return [pscustomobject]@{ Ok = $true; LinkReset = $linkReset }
}

# -Observe: one screenshot for an agent to look at, taken after a completed
# step rather than after every keypress. Shot to a scratch file under
# E:\tester\ (the daemon MkDirAll's that at startup, so it is always there),
# pull it down, drop the scratch copy, and print FG + screen size next to the
# PNG so the caller does not need a second round trip just to orient itself.
function Do-Observe($stream, [string]$outFile) {
    $remoteShot = 'E:\tester\_observe.bmp'
    $localBmp = if ($outFile) { [System.IO.Path]::ChangeExtension($outFile, '.bmp') }
                else { Join-Path $PSScriptRoot 'observe.bmp' }
    Assert-Ok (Invoke-Cmd $stream ("SHOT {0}" -f $remoteShot)) "SHOT"
    # Clean up the scratch file even when Do-Get throws, so a failed pull does
    # not leave it behind on the phone. The inner try/catch matters: Do-Del
    # runs over the same (possibly now unhappy) connection, and a PowerShell
    # exception raised inside a finally REPLACES the one already in flight -
    # so without it, a failed cleanup would hide the real reason Do-Get failed.
    try { Do-Get $stream $remoteShot $localBmp } finally { try { Do-Del $stream $remoteShot } catch {} }
    $png = ConvertTo-Png $localBmp
    $fg = @(Invoke-Cmd $stream "FG" -Quiet)[0]
    Write-Host "[net] $fg"
    $info = @(Invoke-Cmd $stream "INFO" -Quiet)
    $screenLine = @($info | Where-Object { $_ -match '^screen ' })
    if ($screenLine.Count) { Write-Host "[net] $($screenLine[0])" }
    Write-Host "[net] observe: $(if ($png) { $png } else { $localBmp })"
}

# Reconnect after a -Do sequence whose last step tore the link down on purpose
# (NETRESET is the documented way to close out a game before looking at the
# result; RESTART and "AP <name>" count too). NETRESET drives the very same
# "network lost - reconnecting" path inside the daemon that an unplanned WiFi
# drop does (deploy_daemon.cpp: both cases break out of the receive loop into
# the same re-attach handling), so it can be just as slow to come back as the
# WiFi leg of a RESTART - measured there at ~11s worst case, with 90s kept as
# headroom rather than the expected wait. Bounded for the same reason as
# Do-Restart: this must give up eventually instead of hanging a caller that
# only wanted to look at the screen. Returns the new stream, or $null if the
# daemon never answered again within the deadline.
function Wait-Relink([int]$TimeoutSec = 90) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        try {
            $s = & $script:reconnect
            $s.ReadTimeout = 4000
            $info = @(Invoke-Cmd $s "INFO" -Quiet)
            if ($info[0].StartsWith("OK")) { return $s }
        } catch {
            # Not back yet, mid re-attach - keep polling.
        }
        Start-Sleep -Milliseconds 1500
    }
    return $null
}

# Upload a scenario, run it on the phone, wait for it to finish, bring back the log.
#
# Why on the phone at all: a full-screen app (our case: the game) takes the WiFi
# radio down with it, so a PC-driven sequence dies the moment the app starts. A
# scenario executing on the device survives that - we just collect the evidence
# once the link is back.
function Do-Scenario($stream, [string]$local, [string]$remoteDir, [int]$timeoutSec) {
    if (-not (Test-Path $local)) { throw "scenario not found: $local" }
    $name = Split-Path $local -Leaf
    if (-not $remoteDir.EndsWith('\')) { $remoteDir += '\' }
    $remote = $remoteDir + $name
    $remoteLog = [IO.Path]::ChangeExtension($remote, '.log')

    Do-Put $stream $local $remote
    $lines = Invoke-Cmd $stream ("SCRIPT {0} {1}" -f $remote, $remoteLog)
    Assert-Ok $lines "SCRIPT"

    $deadline = (Get-Date).AddSeconds($timeoutSec)
    $lastStatus = ''
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        $st = $null
        try {
            # A finite read timeout is what makes the reconnect below reachable.
            # Without it the socket read blocks forever the moment the scenario
            # brings a full-screen app up and the radio parks - the poll loop
            # never sees an error, never reconnects, and never times out.
            $stream.ReadTimeout = 8000
            $st = @(Invoke-Cmd $stream "STATUS" -Quiet)[0]
            $stream.ReadTimeout = -1
        } catch {
            # Expected while the scenario runs a full-screen app: the radio parks
            # and the link drops. Reconnect and keep watching rather than give up.
            #
            # The reconnect must itself be guarded. Unguarded, a failed Connect
            # threw straight out of this catch and killed the whole watch - which
            # is precisely the situation it exists to survive, since the phone is
            # off the network for as long as the app holds the screen.
            Write-Host "[net] link lost while scripting - reconnecting ..."
            Start-Sleep -Seconds 5
            try { $stream = & $script:reconnect } catch { }
            continue
        }
        if ($st -ne $lastStatus) { Write-Host "[net] $st"; $lastStatus = $st }
        if ($st -match '^OK (DONE|FAIL|CANCELLED|IDLE)') { break }
    }
    # The loop can exit with the socket dead - on the deadline, or after a
    # reconnect that did not succeed. The caller goes straight on to fetch the
    # log, so hand it back a live connection instead of a disposed stream.
    try { [void]$script:stream.CanRead } catch {
        try { [void](& $script:reconnect) } catch {}
    }
    return $remoteLog
}

# --- resolve the IP ----------------------------------------------------------
# Explicit value, else the cache from last time, else scan the LAN. If the
# address does not answer (DHCP moved the phone), fall back to a scan.
$cacheFile = "$PSScriptRoot\.daemon_ip"
if ($Device) { $cacheFile = "$cacheFile.$Device" }

# Pick one daemon out of everything the scan found. With a single phone this is
# the old behaviour; with several it refuses to guess. Picking one at random is
# how a command ends up in the wrong phone, and that is not visible in the reply.
function Select-Daemon($port, $prefer) {
    $all = @(Find-Daemon $port)
    if ($all.Count -eq 0) { return $null }
    # An address named explicitly wins over the ambiguity guard: the guard
    # exists to stop a blind guess, not to override an answer already given.
    if ($prefer -and $all -contains $prefer) { return $prefer }
    if ($all.Count -eq 1) { Write-Host "[net] found daemon at $($all[0])"; return $all[0] }
    Write-Host "[net] several daemons answered:"
    foreach ($a in $all) { Write-Host ("[net]   {0}  {1}" -f $a, (Get-DaemonTag $a $port)) }
    throw "more than one daemon on the LAN - name the phone with -Ip <addr> (and -Device <label> to cache it)"
}

# Two transports, one protocol. $script:reconnect reopens the live one (used by
# Do-Scenario when a link drops); $script:closeConn tears it down at the end.
if ($Com) {
    # Bluetooth / serial: no address, no scan.
    $script:serial = Open-SerialPort $Com $Baud
    $script:stream = $script:serial.BaseStream
    $script:reconnect = {
        try { $script:serial.Close() } catch {}
        Start-Sleep -Milliseconds 500
        $script:serial = Open-SerialPort $Com $Baud
        $script:stream = $script:serial.BaseStream
        $script:stream
    }
    $script:closeConn = { try { $script:serial.Close() } catch {} }
    Write-Host "[net] serial $Com @ $Baud baud"
} else {
    if (-not $Ip -and (Test-Path $cacheFile)) {
        $Ip = (Get-Content $cacheFile -TotalCount 1).Trim()
        if ($Ip) { Write-Host "[net] trying cached $Ip" }
    }
    if (-not $Ip -or $Ip -in @('scan', 'auto')) {
        $Ip = Select-Daemon $Port
        if (-not $Ip) { throw "no daemon found on the LAN - is it running and on WiFi?" }
    } elseif (-not (Test-IsDaemon $Ip $Port)) {
        # An address given on the command line names one phone. Falling back to
        # whatever else answers on the LAN once sent commands meant for the N95 to
        # an E90 in the middle of someone else's work, so only a cached guess scans.
        if ($PSBoundParameters.ContainsKey('Ip')) {
            throw "daemon not responding at $Ip (explicit -Ip, not scanning; use -Ip scan to search)"
        }
        Write-Host "[net] $Ip not responding, scanning the LAN ..."
        $found = Select-Daemon $Port $Ip
        if (-not $found) { throw "daemon not at $Ip and none found on the LAN" }
        $Ip = $found
    }

    $script:Ip = $Ip
    $script:client = Connect-Daemon $Ip $Port

    # Rewrite the IP cache as plain ASCII. The phone writes E:\daemon_ip.txt as
    # UTF-16 (Symbian descriptors), so it cannot be used verbatim from a batch file.
    try {
        [System.IO.File]::WriteAllText($cacheFile, "$Ip`r`n", [System.Text.Encoding]::ASCII)
    } catch { }

    $script:stream = $script:client.GetStream()
    $script:reconnect = {
        try { $script:client.Close() } catch {}
        $script:client = Connect-Daemon $script:Ip $Port
        $script:stream = $script:client.GetStream()
        $script:stream
    }
    $script:closeConn = { try { $script:client.Close() } catch {} }
}

try {
    $stream = $script:stream

    # -HashRemote names where the 20-byte digest is WRITTEN, not the file to be
    # checked. Aimed at the EXE itself it overwrites the binary with the digest;
    # on C:\sys\bin\ps1daemon.exe that destroys the delivery channel, and only a
    # daemon still alive in RAM makes the mistake recoverable.
    if ($HashRemote -and $Remote -and ($HashRemote -eq $Remote)) {
        throw "-HashRemote must differ from -Remote: it says where the digest goes, so pointing it at the file overwrites the file"
    }

    if ($PutFile -and $Remote)      { Do-Put $stream $PutFile $Remote }
    if ($PutFile -and $HashRemote)  { Do-PutHash $stream $PutFile $HashRemote }

    if ($Info)   { [void](Invoke-Cmd $stream "INFO") }
    if ($Wg)     { [void](Invoke-Cmd $stream "WG") }
    if ($Ls)     { [void](Invoke-Cmd $stream ("LS {0}" -f $Ls)) }
    if ($Status) { [void](Invoke-Cmd $stream "STATUS") }
    if ($Cancel) { [void](Invoke-Cmd $stream "CANCEL") }

    if ($SyncTime) {
        $SetTime = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    }
    if ($SetTime) { Assert-Ok (Invoke-Cmd $stream ("TIME {0}" -f $SetTime)) "TIME" }
    if ($GetTime) { Assert-Ok (Invoke-Cmd $stream "TIME") "TIME" }

    if ($App)    { Assert-Ok (Invoke-Cmd $stream ("APP {0}" -f $App)) "APP" }
    if ($Start)  { Assert-Ok (Invoke-Cmd $stream ("START {0}" -f $Start)) "START" }
    if ($Run)    { Do-Run $stream $Run $RunTimeoutSec }

    foreach ($k in $Key) { [void](Invoke-Cmd $stream ("KEY {0}" -f $k)) }
    if ($Text)   { [void](Invoke-Cmd $stream ("TEXT {0}" -f $Text)) }
    if ($Tap)    { [void](Invoke-Cmd $stream ("TAP {0}" -f $Tap)) }

    # Act, then look: -Do's whole sequence runs first, -Observe only after it,
    # so a failed step still gets its screenshot instead of the run aborting
    # blind. A failed -Do fails the whole call, but only once -Observe (if
    # asked for) has had its chance to show what the failed step left behind.
    $doOk = $true
    $doLinkReset = $false
    if ($Do) {
        $doResult = Do-Sequence $stream $Do
        $doOk = $doResult.Ok
        $doLinkReset = $doResult.LinkReset
    }
    if ($Observe) {
        if ($doLinkReset) {
            # -Do's last step (NETRESET, most commonly) took the link down on
            # purpose - that is the documented way to end a run that just
            # closed a game, and -Observe right after it is the main reason
            # -Do accepts a link-resetting step as the last one at all. Do
            # NOT hand $stream to Do-Observe as-is here: it is dead, and
            # Do-Observe's first command would surface that as a raw socket
            # exception instead of a message a caller can act on.
            Write-Host "[net] -Do's last step reset the link - reconnecting before -Observe ..."
            $relinked = Wait-Relink
            if (-not $relinked) {
                throw "-Observe failed: -Do's last step reset the link and the daemon did not answer again within 90s - it may still be re-attaching to the network, or the reset itself failed"
            }
            $stream = $relinked
        }
        Do-Observe $stream $OutFile
    }
    if (-not $doOk) { throw "-Do sequence failed - see the stopped step above" }

    foreach ($c in $Cmd) { [void](Invoke-Cmd $stream $c) }

    if ($Scenario) {
        $remoteLog = Do-Scenario $stream $Scenario $ScenarioRemoteDir $ScenarioTimeoutSec
        $stream = $script:stream          # Do-Scenario may have reconnected
        $localLog = if ($OutFile) { $OutFile } else { ".\$(Split-Path $Scenario -Leaf).log" }
        Do-Get $stream $remoteLog $localLog
        # The scenario's own output is the point of the exercise: show it.
        Get-Content $localLog | ForEach-Object { Write-Host "[log] $_" }
    }

    if ($Shot) {
        $lines = Invoke-Cmd $stream ("SHOT {0} {1}" -f $Shot, $ShotStep)
        Assert-Ok $lines "SHOT"
        if ($OutFile -and -not $Scenario) { Do-Get $stream $Shot $OutFile; [void](ConvertTo-Png $OutFile) }
    }

    if ($GetFile -and $OutFile -and -not $Shot -and -not $Scenario) {
        Do-Get $stream $GetFile $OutFile
    }
    if ($Pull -and $PullTo) { Do-Pull $stream $Pull $PullTo }
    if ($DelFile)           { Do-Del $stream $DelFile }

    # Last, deliberately: everything else has already been done and collected.
    # RESTART comes before REBOOT for the same reason both come last - each one
    # takes the link away, so nothing may be queued behind them.
    if ($Restart) {
        Do-Restart $stream
        return
    }

    if ($Reboot -or $RebootCold) {
        [void](Do-Reboot $stream -Cold:$RebootCold)
        return
    }

    Send-Text $stream "BYE`n"
    try { [void](Read-Line $stream) } catch {}
} finally {
    & $script:closeConn
}
Write-Host "[net] done"

