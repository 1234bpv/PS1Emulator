$ErrorActionPreference = 'Stop'
$script:RunRoot = $PSScriptRoot
$script:NetClient = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..\SymbianDeployDaemon\tools\deploy_net.ps1'))
function Invoke-BoundedNet {
    param([string]$Name, [string[]]$NetArgs, [int]$LimitSec = 45)
    $stdout = Join-Path $script:RunRoot "$Name.stdout.txt"
    $stderr = Join-Path $script:RunRoot "$Name.stderr.txt"
    $marker = Join-Path $script:RunRoot "$Name.done.json"
    $argsList = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$script:NetClient) + $NetArgs
    $quoted = $argsList | ForEach-Object { '"' + [regex]::Replace([regex]::Replace($_, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"' }
    $started = Get-Date
    $p = Start-Process -FilePath powershell.exe -ArgumentList ($quoted -join ' ') -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $timeout = -not $p.WaitForExit($LimitSec * 1000)
    if ($timeout) { Stop-Process -Id $p.Id -Force; $p.WaitForExit() }
    $result = [ordered]@{ name=$Name; started=$started.ToString('o'); finished=(Get-Date).ToString('o'); limitSec=$LimitSec; timeout=$timeout; exitCode=$p.ExitCode; args=$NetArgs }
    $result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $marker -Encoding UTF8
    Get-Content -LiteralPath $stdout,$stderr
    $result | ConvertTo-Json -Compress
}
