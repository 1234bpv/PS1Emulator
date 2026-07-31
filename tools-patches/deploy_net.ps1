# deploy_net.ps1 - PC-side client for the on-device deploy daemon (ps1daemon.exe).
#
# Speaks the daemon's line+binary protocol over WiFi/TCP so a full iteration runs
# without touching the phone screen: push the fresh EXE, launch it, pull the log.
#
#   deploy_net.ps1 -Ip 192.168.1.50 -PutFile <local> -Remote <devpath>
#   deploy_net.ps1 -Ip 192.168.1.50 -Run "E:\sys\bin\ps1stage1.exe 300"
#   deploy_net.ps1 -Ip 192.168.1.50 -GetFile <devpath> -OutFile <local>
#   deploy_net.ps1 -Ip ... -Cycle -PutFile <exe> -Remote <devpath> -Run "<cmdline>" `
#                  -GetFile <devlog> -OutFile <local>   # push -> run -> pull, in order
#
# The phone's IP is written by the daemon to E:\daemon_ip.txt; deploy.cmd caches it
# from the card so -Ip can be omitted downstream.

param(
    [Parameter(Mandatory=$true)][string]$Ip,
    [int]$Port = 2323,
    [string]$PutFile,
    [string]$Remote,
    [string]$HashRemote,
    [string]$Run,
    [string]$GetFile,
    [string]$OutFile,
    [switch]$Cycle,
    [int]$RunTimeoutSec = 180
)

$ErrorActionPreference = 'Stop'

function Connect-Daemon($ip, $port) {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.NoDelay = $true
    $c.Connect($ip, $port)
    return $c
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
    $buf = New-Object byte[] 65536
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

function Do-Put($stream, [string]$local, [string]$remote) {
    if (-not (Test-Path $local)) { throw "local file not found: $local" }
    $fi = Get-Item $local
    $size = $fi.Length
    Write-Host ("[net] PUT {0} -> {1} ({2:N0} bytes)" -f $local, $remote, $size)
    Send-Text $stream ("PUT {0} {1}`n" -f $remote, $size)
    $fs = [System.IO.File]::OpenRead($local)
    try {
        $buf = New-Object byte[] 65536
        while ($true) {
            $r = $fs.Read($buf, 0, $buf.Length)
            if ($r -le 0) { break }
            $stream.Write($buf, 0, $r)
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
    $stream.ReadTimeout = $timeoutSec * 1000
    Send-Text $stream ("RUN {0}`n" -f $cmdline)
    $resp = Read-Line $stream
    $stream.ReadTimeout = -1
    if ($resp.StartsWith("EXIT 0")) { Write-Host "[net] $resp" }
    elseif ($resp.StartsWith("EXIT")) { Write-Warning "[net] $resp (non-zero exit)" }
    else { throw "RUN failed: $resp" }
}

function Do-Get($stream, [string]$remote, [string]$out) {
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

$client = Connect-Daemon $Ip $Port
try {
    $stream = $client.GetStream()

    if ($PutFile -and $Remote)      { Do-Put $stream $PutFile $Remote }
    if ($PutFile -and $HashRemote)  { Do-PutHash $stream $PutFile $HashRemote }
    if ($Run)                       { Do-Run $stream $Run $RunTimeoutSec }
    if ($GetFile -and $OutFile)     { Do-Get $stream $GetFile $OutFile }

    Send-Text $stream "BYE`n"
    try { [void](Read-Line $stream) } catch {}
} finally {
    $client.Close()
}
Write-Host "[net] done"
