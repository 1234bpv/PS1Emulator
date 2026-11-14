# Перевірки GTE aspect ratio: збирає gte.c, gte_nf.c, gte_divider.c разом з gte_aspect_test.c
# хостовим gcc, запускає результат і перевіряє код повернення та підсумок.
# Кожен запуск має межу часу. Проміжні файли й EXE лежать у тимчасовій теці, поза сховищем.
# Повертає 0 лише за повного успіху.
param(
	[int]$CompileTimeoutMs = 120000,
	[int]$RunTimeoutMs = 120000,
	[int]$TestTimeoutMs = 30000
)

$ErrorActionPreference = 'Stop'

$gcc = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\toolchain\perl\c\bin\gcc.exe'))
$pcsxDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\pcsx_rearmed'))
$testSrc = Join-Path $PSScriptRoot 'gte_aspect_test.c'
$gteSrc = Join-Path $pcsxDir 'libpcsxcore\gte.c'
$gteNfSrc = Join-Path $pcsxDir 'libpcsxcore\gte_nf.c'
$gteDivSrc = Join-Path $pcsxDir 'libpcsxcore\gte_divider.c'

if (-not (Test-Path $gcc)) {
	throw "run.ps1: MinGW gcc not found at $gcc"
}

# Тека збірки має бути ASCII-шляхом: mingw gcc мовчки завершується, коли TMP містить кирилицю.
Add-Type -Namespace Ps1EmuGte -Name Path -MemberDefinition '[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)] public static extern uint GetShortPathName(string aLong, System.Text.StringBuilder aShort, uint aSize);'
$tempRoot = [IO.Path]::GetTempPath().TrimEnd('\')
$shortBuf = New-Object System.Text.StringBuilder 260
if ([Ps1EmuGte.Path]::GetShortPathName($tempRoot, $shortBuf, 260) -gt 0) { $tempRoot = $shortBuf.ToString() }
if ($tempRoot -match '[^\x00-\x7F]') { throw "run.ps1: temp directory is not ASCII and has no short name: $tempRoot" }
$env:TMP = $tempRoot
$env:TEMP = $tempRoot

$work = Join-Path $tempRoot "ps1emu-gte-$PID"
New-Item -ItemType Directory -Force -Path $work | Out-Null

function Invoke-Limited {
	param([string]$Exe, [string[]]$Arguments, [int]$TimeoutMs, [string]$Label)
	$psi = New-Object System.Diagnostics.ProcessStartInfo
	$psi.FileName = $Exe
	$psi.Arguments = (($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' ')
	$psi.UseShellExecute = $false
	$psi.RedirectStandardOutput = $true
	$psi.RedirectStandardError = $true
	$psi.CreateNoWindow = $true
	$p = New-Object System.Diagnostics.Process
	$p.StartInfo = $psi
	try {
		[void]$p.Start()
	} catch {
		return [pscustomobject]@{ Code = -2; TimedOut = $false; Text = "cannot start ${Label}: $($_.Exception.Message)" }
	}
	$stdOutTask = $p.StandardOutput.ReadToEndAsync()
	$stdErrTask = $p.StandardError.ReadToEndAsync()
	if (-not $p.WaitForExit($TimeoutMs)) {
		try { $p.Kill() } catch { }
		return [pscustomobject]@{ Code = -1; TimedOut = $true; Text = "${Label}: time limit exceeded (${TimeoutMs} ms)" }
	}
	[System.Threading.Tasks.Task]::WaitAll($stdOutTask, $stdErrTask)
	$outText = $stdOutTask.Result
	$errText = $stdErrTask.Result
	$combined = @()
	if ($outText) { $combined += $outText }
	if ($errText) { $combined += $errText }
	return [pscustomobject]@{ Code = $p.ExitCode; TimedOut = $false; Text = ($combined -join "`n") }
}

$failures = @()

# 1. Хостове збирання
$exe = Join-Path $work 'gte_aspect_test.exe'
$inc1 = Join-Path $pcsxDir 'include'
$inc2 = Join-Path $pcsxDir 'libpcsxcore'

$savedPath = $env:PATH
$env:PATH = (Split-Path -Parent $gcc) + ';' + $savedPath

$compileArgs = @(
	'-std=gnu99',
	'-O2',
	"-I$inc1",
	"-I$inc2",
	$gteSrc,
	$gteNfSrc,
	$gteDivSrc,
	$testSrc,
	'-lm',
	'-o',
	$exe
)

Write-Output "Building GTE aspect test with MinGW gcc..."
$build = Invoke-Limited $gcc $compileArgs $CompileTimeoutMs 'host-build'
if ($build.TimedOut) { $failures += 'host-build: time limit exceeded' }
elseif ($build.Code -ne 0) { $failures += "host-build: exit code $($build.Code)`n$($build.Text)" }

if ($failures.Count -gt 0) {
	$env:PATH = $savedPath
	Write-Error ($failures -join "`n")
	exit 1
}

# 2. Запуск тесту
Write-Output "Running GTE aspect test..."
$run = Invoke-Limited $exe @() $RunTimeoutMs 'test-run'
$env:PATH = $savedPath

if ($run.TimedOut) { $failures += 'test-run: time limit exceeded' }
elseif ($run.Code -ne 0) { $failures += "test-run: exit code $($run.Code)" }

if ($run.Text.Trim()) {
	Write-Output $run.Text
}

# Прибирання тимчасових файлів
try { Remove-Item -Recurse -Force $work } catch { }

if ($failures.Count -gt 0) {
	Write-Error ($failures -join "`n")
	exit 1
}

Write-Output "PASS: GTE aspect ratio verification complete."
exit 0
