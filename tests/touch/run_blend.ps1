# Перевірка змішування RGB565: повний перебір усіх значень каналів (R: 0..31, G: 0..63, B: 0..31)
# та рівнів альфа (0..255) для білого та чорного кольору.
# Порівнює формулу єдиного множення (1a882fd) з попередньою формулою (0a6e13b).
param(
	[int]$CompileTimeoutMs = 60000,
	[int]$RunTimeoutMs = 60000
)

$ErrorActionPreference = 'Stop'

$gxx = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\toolchain\perl\c\bin\g++.exe'))
$srcDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\src\ui'))
$testSrc = Join-Path $PSScriptRoot 'blend565_test.cpp'
$coreSrc = Join-Path $srcDir 'touchcore.cpp'

Add-Type -Namespace Ps1Emu -Name BlendPath -MemberDefinition '[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)] public static extern uint GetShortPathName(string aLong, System.Text.StringBuilder aShort, uint aSize);'
$tempRoot = [IO.Path]::GetTempPath().TrimEnd('\')
$shortBuf = New-Object System.Text.StringBuilder 260
if ([Ps1Emu.BlendPath]::GetShortPathName($tempRoot, $shortBuf, 260) -gt 0) { $tempRoot = $shortBuf.ToString() }
$work = Join-Path $tempRoot "ps1emu-blend-$PID"
New-Item -ItemType Directory -Force -Path $work | Out-Null

function Invoke-Limited {
	param([string]$Exe, [string[]]$Arguments, [int]$TimeoutMs, [string]$Label)
	$out = Join-Path $work "$Label.out.txt"
	$err = Join-Path $work "$Label.err.txt"
	if (Test-Path $out) { Remove-Item $out -Force }
	if (Test-Path $err) { Remove-Item $err -Force }
	$p = Start-Process -FilePath $Exe -ArgumentList $Arguments -NoNewWindow -PassThru `
		-RedirectStandardOutput $out -RedirectStandardError $err
	if (-not $p.WaitForExit($TimeoutMs)) {
		try { $p.Kill() } catch { }
		return [pscustomobject]@{ Code = -1; TimedOut = $true; Text = (Read-Text $out $err) }
	}
	return [pscustomobject]@{ Code = $p.ExitCode; TimedOut = $false; Text = (Read-Text $out $err) }
}

function Read-Text {
	param([string]$Out, [string]$Err)
	$parts = @()
	if (Test-Path $Out) { $parts += (Get-Content $Out -Raw) }
	if (Test-Path $Err) { $parts += (Get-Content $Err -Raw) }
	return (($parts | Where-Object { $_ }) -join "`n")
}

$failures = @()

$exe = Join-Path $work 'blend565_test.exe'
$flags = @('-std=c++98', '-pedantic', '-Wall', '-Wextra', '-Werror', '-O2', "-I$srcDir")
$savedPath = $env:PATH
$env:PATH = (Split-Path -Parent $gxx) + ';' + $savedPath
$build = Invoke-Limited $gxx ($flags + @($testSrc, $coreSrc, '-o', $exe)) $CompileTimeoutMs 'blend-build'
$env:PATH = $savedPath

if ($build.TimedOut) { $failures += 'blend-build: time limit exceeded' }
elseif ($build.Code -ne 0) { $failures += "blend-build: exit code $($build.Code)" }
if ($build.Text.Trim()) { Write-Output "--- blend-build ---"; Write-Output $build.Text }

if ($failures.Count -eq 0) {
	$run = Invoke-Limited $exe @() $RunTimeoutMs 'blend-run'
	Write-Output "--- blend-run ---"
	Write-Output $run.Text
	if ($run.TimedOut) { $failures += 'blend-run: time limit exceeded' }
	elseif ($run.Code -ne 0) { $failures += "blend-run: exit code $($run.Code)" }
}

if ($failures.Count -eq 0) {
	Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
	Write-Output "run_blend.ps1: PASS"
	exit 0
}

foreach ($f in $failures) { Write-Output "run_blend.ps1: FAIL: $f" }
exit 1
