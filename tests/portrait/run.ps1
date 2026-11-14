# Перевірки згладжування для portrait mode (1D downscale smoothing):
# збирає portrait_test.cpp хостовим g++ або clang++, запускає результат і
# перевіряє попіксельну точність виходу, захист меж буфера (канарки) та патерни.
# Кожен запуск має межу часу. Проміжні файли й EXE лежать у тимчасовій теці, поза сховищем.
# Повертає 0 лише за повного успіху.
param(
	[int]$CompileTimeoutMs = 120000,
	[int]$RunTimeoutMs = 120000,
	[int]$TestTimeoutMs = 30000,
	[int]$SuiteTimeoutMs = 240000
)

$ErrorActionPreference = 'Stop'

$gxx = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\toolchain\perl\c\bin\g++.exe'))
if (-not (Test-Path $gxx)) {
	$sysGxx = Get-Command g++, clang++ -ErrorAction SilentlyContinue | Select-Object -First 1
	if ($sysGxx) {
		$gxx = $sysGxx.Source
	} else {
		throw "run.ps1: neither MinGW g++ at $gxx nor system g++/clang++ found"
	}
}

$testSrc = Join-Path $PSScriptRoot 'portrait_test.cpp'
if (-not (Test-Path $testSrc)) {
	throw "run.ps1: test source not found: $testSrc"
}

# Тека збірки має бути ASCII-шляхом: mingw g++ мовчки завершується, коли TMP містить кирилицю.
Add-Type -Namespace Ps1EmuPortrait -Name Path -MemberDefinition '[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)] public static extern uint GetShortPathName(string aLong, System.Text.StringBuilder aShort, uint aSize);'
$tempRoot = [IO.Path]::GetTempPath().TrimEnd('\')
$shortBuf = New-Object System.Text.StringBuilder 260
if ([Ps1EmuPortrait.Path]::GetShortPathName($tempRoot, $shortBuf, 260) -gt 0) { $tempRoot = $shortBuf.ToString() }
if ($tempRoot -match '[^\x00-\x7F]') { throw "run.ps1: temp directory is not ASCII and has no short name: $tempRoot" }
$env:TMP = $tempRoot
$env:TEMP = $tempRoot
$work = Join-Path $tempRoot "ps1emu-portrait-$PID"
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
	$outTask = $p.StandardOutput.ReadToEndAsync()
	$errTask = $p.StandardError.ReadToEndAsync()
	$finished = $p.WaitForExit($TimeoutMs)
	if ($finished) {
		$p.WaitForExit()
	} else {
		try { $p.Kill() } catch { }
		[void]$p.WaitForExit(5000)
	}
	[void]$outTask.Wait(5000)
	[void]$errTask.Wait(5000)
	$text = @($outTask.Result, $errTask.Result) | Where-Object { $_ -and $_.Trim() }
	$code = -1
	if ($finished) { $code = $p.ExitCode }
	return [pscustomobject]@{ Code = $code; TimedOut = (-not $finished); Text = (($text) -join "`n") }
}

$failures = @()

# 1. Хостове збирання тесту.
$exe = Join-Path $work 'portrait_test.exe'
$flags = @('-std=c++98', '-pedantic', '-Wall', '-Wextra', '-Werror', '-O2', '-static')
$savedPath = $env:PATH
$env:PATH = (Split-Path -Parent $gxx) + ';' + $savedPath

$build = Invoke-Limited $gxx ($flags + @($testSrc, '-o', $exe)) $CompileTimeoutMs 'host-build-portrait'
if ($build.TimedOut) { $failures += 'host-build-portrait: time limit exceeded' }
elseif ($build.Code -ne 0) { $failures += "host-build-portrait: exit code $($build.Code)" }
if ($build.Text.Trim()) { Write-Output '--- host-build-portrait ---'; Write-Output $build.Text }

# 2. Отримання списку тестів.
if ($failures.Count -eq 0) {
	$list = Invoke-Limited $exe @('--list') $RunTimeoutMs 'list-portrait'
	$names = @()
	foreach ($line in ($list.Text -split "`n")) {
		if ($line -match '^TEST (\S+)') { $names += $matches[1] }
	}
	if ($list.TimedOut -or $list.Code -ne 0 -or $names.Count -eq 0) {
		$failures += 'list-portrait: no test names (time limit, exit code or empty list)'
	}

	$defined = ([regex]::Matches([IO.File]::ReadAllText($testSrc), '(?m)^static void (Test\w+)\(\)')).Count
	if ($names.Count -ne $defined) {
		$failures += "list-portrait: $($names.Count) tests in --list, $defined Test functions in $(Split-Path -Leaf $testSrc)"
	}
}

# 3. Виконання тестів по одному з таймером.
if ($failures.Count -eq 0) {
	$total = 0
	$failed = 0
	Write-Output '--- run portrait ---'
	$clock = [Diagnostics.Stopwatch]::StartNew()
	for ($i = 0; $i -lt $names.Count; $i++) {
		$name = $names[$i]
		if ($clock.ElapsedMilliseconds -gt $SuiteTimeoutMs) {
			$failed += 1
			Write-Output ("FAIL [suite] time budget {0} ms exceeded: {1} of {2} tests not run" -f $SuiteTimeoutMs, ($names.Count - $i), $names.Count)
			break
		}
		$one = Invoke-Limited $exe @('--only', $name) $TestTimeoutMs "run-$name"
		$regexPattern = "(?m)^portrait: (\d+) checks, (\d+) failed\r?$"
		$m = [regex]::Match($one.Text, $regexPattern)
		if ($one.TimedOut) {
			$failed += 1
			$total += 1
			Write-Output "FAIL [$name] time limit exceeded: no result in $TestTimeoutMs ms"
		} elseif (-not $m.Success) {
			$failed += 1
			$total += 1
			Write-Output "FAIL [$name] no summary line, exit code $($one.Code)"
			if ($one.Text.Trim()) { Write-Output $one.Text }
		} else {
			$total += [int]$m.Groups[1].Value
			$failed += [int]$m.Groups[2].Value
			if ([int]$m.Groups[1].Value -eq 0) {
				$failed += 1
				Write-Output "FAIL [$name] zero checks executed: the test checks nothing"
			}
			foreach ($l in ($one.Text -split "`n")) {
				if ($l -like 'FAIL *') { Write-Output $l.TrimEnd() }
			}
			if ($one.Code -ne 0 -and [int]$m.Groups[2].Value -eq 0) {
				$failed += 1
				Write-Output "FAIL [$name] exit code $($one.Code) with no failed checks"
			}
		}
	}
	Write-Output "portrait: $total checks, $failed failed"
	if ($total -eq 0) { $failures += 'portrait: no checks executed' }
	if ($failed -ne 0) { $failures += "portrait: $failed failed checks or tests" }
}

$env:PATH = $savedPath

if ($failures.Count -eq 0) {
	Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
	Write-Output 'run.ps1: PASS'
	exit 0
}
foreach ($f in $failures) { Write-Output "run.ps1: FAIL: $f" }
Write-Output "run.ps1: work directory kept for diagnosis: $work"
exit 1
