<#
.SYNOPSIS
    deploy_c5.ps1 - Dedicated bounded Bluetooth SPP client for Nokia C5-00.2.

.DESCRIPTION
    Speaks the Symbian deploy daemon protocol over Bluetooth Serial Port Profile (RFCOMM).
    Designed specifically for Nokia C5-00.2 (which lacks WiFi hardware).
    Features bounded operations, 128 KB receive buffer for RFCOMM stability,
    finite timeouts, pacing delays on large PUT transfers, atomic GET downloads
    via temporary .part files, SHA-1 digest calculation for C:\sys\hash\<name>,
    and a screen geometry guard (screen 240x320) to guard against misdirected commands.

.PARAMETER Com
    Mandatory. Serial COM port name (e.g. COM5, COM4) bound to the C5-00.2 Bluetooth SPP service.
    Must be specified explicitly; there is no default port.

.EXAMPLE
    # Check device info (screen resolution, free RAM, drive space):
    .\tools-patches\deploy_c5.ps1 -Com COM5 -Info

.EXAMPLE
    # Download a log or benchmark file from C5:
    .\tools-patches\deploy_c5.ps1 -Com COM5 -GetFile "E:\PS1\logs\ps1_stage1.jsonl" -OutFile ".\ps1_stage1.jsonl"

.EXAMPLE
    # Deploy executable with guarded SHA-1 hash to C:\sys\hash:
    .\tools-patches\deploy_c5.ps1 -Com COM5 `
        -PutFile "..\S60_3rd_FP1\Epoc32\release\gcce\urel\ps1stage1.exe" `
        -Remote "E:\sys\bin\ps1stage1.exe" `
        -HashRemote "C:\sys\hash\ps1stage1.exe"

.EXAMPLE
    # Run a test scenario and retrieve the output log:
    .\tools-patches\deploy_c5.ps1 -Com COM5 -Scenario ".\scenario.txt" -OutFile ".\scenario.log"

.EXAMPLE
    # Probe transport roundtrip with a disposable temporary file:
    .\tools-patches\deploy_c5.ps1 -Com COM5 -ProbeRoundtrip
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Com,

    [int]$Baud = 115200,
    [int]$ReadTimeoutMs = 15000,
    [int]$WriteTimeoutMs = 30000,
    [int]$ReadBufferSize = 131072, # 128 KB buffer for RFCOMM stability

    [string]$PutFile,
    [string]$Remote,
    [string]$HashRemote,
    [string]$GetFile,
    [string]$OutFile,
    [string]$DelFile,
    [string]$Ls,
    [string]$Stat,
    [string]$Scenario,
    [string]$ScenarioRemoteDir = "E:\tester",
    [int]$ScenarioTimeoutSec = 300,
    [switch]$Info,
    [switch]$Status,
    [string[]]$Cmd,
    [switch]$ProbeRoundtrip
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($Com)) {
    throw "Explicit COM port is required (e.g. -Com COM5). Refusing empty port."
}

# --- Parameter Validation & Conflict Prevention ---
if ($GetFile -and $Scenario) {
    throw "Conflicting parameters: -GetFile and -Scenario cannot be used together."
}

if ($GetFile -and [string]::IsNullOrWhiteSpace($OutFile)) {
    throw "Parameter -GetFile requires -OutFile to specify the local destination path."
}

if ($OutFile -and -not $GetFile -and -not $Scenario) {
    throw "Parameter -OutFile was specified without -GetFile or -Scenario."
}

if ($PutFile -and [string]::IsNullOrWhiteSpace($Remote)) {
    throw "Parameter -PutFile requires -Remote to specify destination path on device."
}

if ($Remote -and [string]::IsNullOrWhiteSpace($PutFile)) {
    throw "Parameter -Remote was specified without -PutFile."
}

if ($HashRemote -and [string]::IsNullOrWhiteSpace($PutFile)) {
    throw "Parameter -HashRemote was specified without -PutFile."
}

if ($HashRemote -and $Remote -and ($HashRemote -eq $Remote)) {
    throw "-HashRemote must differ from -Remote: it specifies where the SHA-1 digest is stored (e.g. C:\sys\hash\<name>)"
}

$hasAction = $Info -or $Status -or $Ls -or $Stat -or ($Cmd -and $Cmd.Count -gt 0) -or $PutFile -or $GetFile -or $DelFile -or $Scenario -or $ProbeRoundtrip
if (-not $hasAction) {
    throw "No action specified. Provide at least one operation (-Info, -Status, -Ls, -Stat, -Cmd, -PutFile, -GetFile, -DelFile, -Scenario, -ProbeRoundtrip)."
}

$script:BlockVerbs = @('LS', 'PS', 'WG', 'INFO', 'TOP', 'AP')

function Open-BoundedSerialPort($com, $baud, $readTimeoutMs, $writeTimeoutMs, $bufSize) {
    $sp = New-Object System.IO.Ports.SerialPort $com, $baud, 'None', 8, 'One'
    $sp.ReadTimeout = $readTimeoutMs
    $sp.WriteTimeout = $writeTimeoutMs
    $sp.ReadBufferSize = $bufSize
    $sp.DtrEnable = $true
    $sp.RtsEnable = $true
    $sp.Open()
    try { $sp.DiscardInBuffer(); $sp.DiscardOutBuffer() } catch {}
    return $sp
}

function Read-Line-Bounded($stream, [int]$timeoutMs = 15000) {
    $bytes = New-Object System.Collections.Generic.List[byte]
    $oldTimeout = $stream.ReadTimeout
    $stream.ReadTimeout = $timeoutMs
    try {
        while ($true) {
            $b = $stream.ReadByte()
            if ($b -lt 0) { throw "connection closed while reading reply" }
            if ($b -eq 10) { break }        # \n
            if ($b -ne 13) { $bytes.Add([byte]$b) }  # skip \r
        }
    } finally {
        $stream.ReadTimeout = $oldTimeout
    }
    return [System.Text.Encoding]::ASCII.GetString($bytes.ToArray())
}

function Read-Exact-Bounded($stream, [int]$n, $outFs, [int]$chunkTimeoutMs = 15000) {
    $buf = New-Object byte[] 4096
    $remaining = $n
    $lastReport = 0
    $prevTimeout = $stream.ReadTimeout
    $stream.ReadTimeout = $chunkTimeoutMs
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        while ($remaining -gt 0) {
            $take = [Math]::Min($remaining, $buf.Length)
            $got = 0
            try {
                $got = $stream.Read($buf, 0, $take)
            } catch [System.TimeoutException] {
                $elapsedSec = $sw.Elapsed.TotalSeconds
                $rate = if ($elapsedSec -gt 0) { ($n - $remaining) / $elapsedSec / 1024.0 } else { 0 }
                throw ("GET read timed out after {0}s ({1:N0}/{2:N0} bytes received, rate {3:N1} KB/s)" -f ($chunkTimeoutMs/1000), ($n - $remaining), $n, $rate)
            } catch [System.IO.IOException] {
                throw "GET read IO error after $($n-$remaining)/$n bytes received: $($_.Exception.Message)"
            }
            if ($got -le 0) { throw "connection closed after $($n-$remaining)/$n bytes" }
            $outFs.Write($buf, 0, $got)
            $remaining -= $got
            $received = $n - $remaining
            if (($received - $lastReport) -ge 65536 -or $remaining -eq 0) {
                $elapsedSec = $sw.Elapsed.TotalSeconds
                $rate = if ($elapsedSec -gt 0) { $received / $elapsedSec / 1024.0 } else { 0 }
                Write-Host ("[net] .. {0:N0}/{1:N0} bytes ({2:P1}) @ {3:N1} KB/s" -f $received, $n, ($received / $n), $rate)
                $lastReport = $received
            }
        }
    } finally {
        $stream.ReadTimeout = $prevTimeout
    }
}

function Send-Text($stream, [string]$line) {
    $b = [System.Text.Encoding]::ASCII.GetBytes($line)
    $stream.Write($b, 0, $b.Length)
    $stream.Flush()
}

function Invoke-Cmd-Bounded($stream, [string]$line, [switch]$Quiet) {
    $trimmed = $line.Trim()
    if (-not $Quiet) { Write-Host "[net] > $trimmed" }
    Send-Text $stream ($trimmed + "`n")
    $verb = ($trimmed -split '\s+')[0].ToUpper()

    $first = Read-Line-Bounded $stream 15000
    $lines = @($first)
    if ($script:BlockVerbs -contains $verb -and $first.StartsWith("OK")) {
        while ($true) {
            $l = Read-Line-Bounded $stream 15000
            if ($l -eq 'END') { break }
            $lines += $l
        }
    }
    if (-not $Quiet) { $lines | ForEach-Object { Write-Host "[net] < $_" } }
    return $lines
}

function Assert-Ok($lines, [string]$what) {
    $first = @($lines)[0]
    if (-not $first.StartsWith("OK")) { throw "$what failed: $first" }
}

function Do-Put-Bounded($stream, [string]$local, [string]$remote, [int]$timeoutMs = 20000) {
    if (-not (Test-Path $local)) { throw "local file not found: $local" }
    $localPath = (Resolve-Path $local).Path
    $fi = Get-Item $localPath
    $size = $fi.Length
    Write-Host ("[net] PUT {0} -> {1} ({2:N0} bytes)" -f $localPath, $remote, $size)
    Send-Text $stream ("PUT {0} {1}`n" -f $remote, $size)

    $fs = [System.IO.File]::OpenRead($localPath)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $buf = New-Object byte[] 4096
        $sent = 0
        $lastReport = 0
        while ($true) {
            $r = $fs.Read($buf, 0, $buf.Length)
            if ($r -le 0) { break }
            try {
                $stream.Write($buf, 0, $r)
                # Pacing sleep for RFCOMM stability on large files
                if ($size -gt 65536) {
                    Start-Sleep -Milliseconds 4
                }
            } catch {
                $early = $null
                try {
                    $early = Read-Line-Bounded $stream 3000
                } catch {}
                if ($early) { throw "PUT refused after $sent bytes: $early" }
                throw
            }
            $sent += $r
            if (($sent - $lastReport) -ge 65536 -or $sent -eq $size) {
                $elapsedSec = $sw.Elapsed.TotalSeconds
                $rate = if ($elapsedSec -gt 0) { $sent / $elapsedSec / 1024.0 } else { 0 }
                Write-Host ("[net] .. {0:N0}/{1:N0} bytes ({2:P1}) @ {3:N1} KB/s" -f $sent, $size, ($sent / $size), $rate)
                $lastReport = $sent
            }
        }
        $stream.Flush()
    } finally {
        $fs.Close()
    }

    $deadline = (Get-Date).AddSeconds(20)
    $resp = $null
    while ((Get-Date) -lt $deadline) {
        $r = Read-Line-Bounded $stream $timeoutMs
        if ($r -match '^OK\s+\d+' -or $r.StartsWith("ERR")) {
            $resp = $r
            break
        }
        Write-Host "[net] (ignored leftover before PUT reply: $r)"
    }
    if (-not $resp) { throw "PUT failed: timeout waiting for reply" }
    if (-not $resp.StartsWith("OK")) { throw "PUT failed: $resp" }
    $elapsed = $sw.Elapsed.TotalSeconds
    $rate = if ($elapsed -gt 0) { $size / $elapsed / 1024.0 } else { 0 }
    Write-Host ("[net] PUT complete: {0:N0} bytes in {1:N2}s ({2:N1} KB/s)" -f $size, $elapsed, $rate)
}

function Do-PutHash-Bounded($stream, [string]$local, [string]$hashRemote, [int]$timeoutMs = 15000) {
    if (-not (Test-Path $local)) { throw "local file not found for hash calculation: $local" }
    $localPath = (Resolve-Path $local).Path
    $sha1 = [System.Security.Cryptography.SHA1]::Create()
    $bytes = $sha1.ComputeHash([System.IO.File]::ReadAllBytes($localPath))
    $hashHex = [BitConverter]::ToString($bytes) -replace '-'
    Write-Host ("[net] PUT hash -> {0} (20 bytes, SHA1={1})" -f $hashRemote, $hashHex)
    Send-Text $stream ("PUT {0} 20`n" -f $hashRemote)
    $stream.Write($bytes, 0, 20)
    $stream.Flush()
    
    $deadline = (Get-Date).AddSeconds(15)
    $resp = $null
    while ((Get-Date) -lt $deadline) {
        $r = Read-Line-Bounded $stream $timeoutMs
        if ($r -match '^OK\s+\d+' -or $r.StartsWith("ERR")) {
            $resp = $r
            break
        }
        Write-Host "[net] (ignored leftover before PUT-HASH reply: $r)"
    }
    if (-not $resp) { throw "PUT hash failed: timeout waiting for reply" }
    if (-not $resp.StartsWith("OK")) { throw "PUT hash failed: $resp" }
    Write-Host "[net] $resp"
}

function Do-Get-Bounded($stream, [string]$remote, [string]$out, [int]$chunkTimeoutMs = 15000) {
    if (-not [System.IO.Path]::IsPathRooted($out)) {
        $out = Join-Path (Get-Location).Path $out
    }
    Write-Host "[net] GET $remote -> $out"
    Send-Text $stream ("GET {0}`n" -f $remote)
    
    $size = -1
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        $resp = Read-Line-Bounded $stream 15000
        if ($resp -match '^OK\s+(\d+)$') {
            $size = [int]$Matches[1]
            break
        }
        if ($resp.StartsWith("ERR")) {
            throw "GET failed: $resp"
        }
        Write-Host "[net] (ignored leftover before GET reply: $resp)"
    }
    if ($size -lt 0) { throw "GET failed: no valid OK <size> reply received" }

    $dir = Split-Path $out -Parent
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    
    $partOut = $out + ".part"
    if (Test-Path $partOut) { Remove-Item $partOut -Force }
    $fs = [System.IO.File]::Create($partOut)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $success = $false
    try {
        Read-Exact-Bounded $stream $size $fs $chunkTimeoutMs
        $success = $true
    } finally {
        $fs.Close()
        if (-not $success) {
            try {
                if (Test-Path $partOut) { Remove-Item $partOut -Force }
            } catch {}
        }
    }
    if (Test-Path $out) { Remove-Item $out -Force }
    Move-Item $partOut $out -Force
    $elapsed = $sw.Elapsed.TotalSeconds
    $rate = if ($elapsed -gt 0) { $size / $elapsed / 1024.0 } else { 0 }
    Write-Host ("[net] got {0:N0} bytes in {1:N2}s ({2:N1} KB/s)" -f $size, $elapsed, $rate)
}

function Do-Del-Bounded($stream, [string]$remote) {
    foreach ($r in ($remote -split ',')) {
        $t = $r.Trim()
        if (-not $t) { continue }
        # Note: RFs::Delete returns ERR delete -1 if file is not found.
        # In cleanup workflows, removing non-existent files is standard and non-fatal.
        [void](Invoke-Cmd-Bounded $stream ("DEL {0}" -f $t))
    }
}

function Do-Scenario-Bounded($stream, [string]$local, [string]$remoteDir, [int]$timeoutSec, [string]$outLog) {
    if (-not (Test-Path $local)) { throw "scenario not found: $local" }
    $name = Split-Path $local -Leaf
    if (-not $remoteDir.EndsWith('\')) { $remoteDir += '\' }
    $remote = $remoteDir + $name
    $remoteLog = [System.IO.Path]::ChangeExtension($remote, '.log')

    Do-Put-Bounded $stream $local $remote
    $lines = Invoke-Cmd-Bounded $stream ("SCRIPT {0} {1}" -f $remote, $remoteLog)
    Assert-Ok $lines "SCRIPT"

    $deadline = (Get-Date).AddSeconds($timeoutSec)
    $lastStatus = ''
    $terminalStatus = $null
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        $st = $null
        try {
            $stLines = @(Invoke-Cmd-Bounded $stream "STATUS" -Quiet)
            $st = $stLines[0]
        } catch {
            Write-Host "[net] status poll notice: $($_.Exception.Message)"
            continue
        }
        if ($st -ne $lastStatus) { Write-Host "[net] $st"; $lastStatus = $st }
        if ($st -match '^OK\s+(DONE|FAIL|CANCELLED|IDLE)') {
            $terminalStatus = $st
            break
        }
    }

    # If requested, attempt to pull and show scenario log
    if ($outLog) {
        try {
            Do-Get-Bounded $stream $remoteLog $outLog
            Get-Content $outLog | ForEach-Object { Write-Host "[log] $_" }
        } catch {
            if ($terminalStatus -and ($terminalStatus -match '^OK\s+DONE')) {
                # For a successful run, failure to retrieve the requested log is fatal
                throw
            } else {
                # For failed/timed out scenario, preserve root failure rather than masking with log GET error
                Write-Warning "[net] Could not retrieve scenario log: $($_.Exception.Message)"
            }
        }
    }

    if (-not $terminalStatus) {
        throw ("Scenario timed out after {0}s (last status: '{1}')" -f $timeoutSec, $lastStatus)
    }

    if ($terminalStatus -notmatch '^OK\s+DONE') {
        throw ("Scenario failed with terminal status: {0}" -f $terminalStatus)
    }

    return $remoteLog
}

# --- Connection & Execution ---
Write-Host "[net] Connecting C5 serial $Com @ $Baud baud (ReadTimeout: ${ReadTimeoutMs}ms, BufSize: ${ReadBufferSize}b)..."
$script:serial = Open-BoundedSerialPort $Com $Baud $ReadTimeoutMs $WriteTimeoutMs $ReadBufferSize
$script:stream = $script:serial.BaseStream

try {
    $stream = $script:stream

    # Guard: verify C5 screen geometry (screen 240x320).
    # Note: Screen 240x320 is a model geometry guard against accidentally connecting
    # to other Symbian devices (e.g. N95 320x240, 701 640x360), not a cryptographic device signature.
    $infoLines = @(Invoke-Cmd-Bounded $stream "INFO" -Quiet)
    $isC5 = $false
    foreach ($l in $infoLines) {
        if ($l -match '^screen\s+240x320') { $isC5 = $true }
    }
    if (-not $isC5) {
        throw "Target device screen guard failed: device does not report screen 240x320. Info: $($infoLines -join '; ')"
    }
    Write-Host "[net] Verified target device screen guard (screen 240x320)"

    if ($ProbeRoundtrip) {
        Write-Host "[net] Performing disposable small file roundtrip probe..."
        $probeRemote = "E:\tester\_transport_probe.bin"
        $probeData = [System.Text.Encoding]::ASCII.GetBytes("C5_TRANSPORT_PROBE_" + (Get-Date).ToString("yyyyMMdd_HHmmss_fff"))
        $probeLocal = [System.IO.Path]::GetTempFileName()
        [System.IO.File]::WriteAllBytes($probeLocal, $probeData)
        try {
            Do-Put-Bounded $stream $probeLocal $probeRemote
            $probeGot = [System.IO.Path]::GetTempFileName()
            try {
                Do-Get-Bounded $stream $probeRemote $probeGot
                $gotBytes = [System.IO.File]::ReadAllBytes($probeGot)
                if ($gotBytes.Length -ne $probeData.Length) {
                    throw "Probe length mismatch: sent $($probeData.Length)b, got $($gotBytes.Length)b"
                }
                for ($i = 0; $i -lt $probeData.Length; $i++) {
                    if ($probeData[$i] -ne $gotBytes[$i]) {
                        throw "Probe byte mismatch at index $i"
                    }
                }
                Write-Host "[net] Probe roundtrip succeeded: exact byte match ($($probeData.Length) bytes)"
            } finally {
                if (Test-Path $probeGot) { Remove-Item $probeGot -Force }
            }
            Do-Del-Bounded $stream $probeRemote
            Write-Host "[net] Probe remote file deleted successfully"
        } finally {
            if (Test-Path $probeLocal) { Remove-Item $probeLocal -Force }
        }
    }

    if ($PutFile -and $Remote)      { Do-Put-Bounded $stream $PutFile $Remote }
    if ($PutFile -and $HashRemote)  { Do-PutHash-Bounded $stream $PutFile $HashRemote }

    if ($Info)   { $infoLines | ForEach-Object { Write-Host "[net] < $_" } }
    if ($Status) { [void](Invoke-Cmd-Bounded $stream "STATUS") }
    if ($Ls)     { [void](Invoke-Cmd-Bounded $stream ("LS {0}" -f $Ls)) }
    if ($Stat)   { [void](Invoke-Cmd-Bounded $stream ("STAT {0}" -f $Stat)) }

    foreach ($c in $Cmd) { [void](Invoke-Cmd-Bounded $stream $c) }

    if ($Scenario) {
        $remoteLog = Do-Scenario-Bounded $stream $Scenario $ScenarioRemoteDir $ScenarioTimeoutSec $OutFile
    }

    if ($GetFile -and $OutFile) {
        Do-Get-Bounded $stream $GetFile $OutFile $ReadTimeoutMs
    }

    if ($DelFile) { Do-Del-Bounded $stream $DelFile }

    Send-Text $stream "BYE`n"
    try { [void](Read-Line-Bounded $stream 2000) } catch {}
} finally {
    try { $script:serial.Close() } catch {}
}
Write-Host "[net] done"
