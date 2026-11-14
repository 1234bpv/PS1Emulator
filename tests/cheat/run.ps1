# Перевірки чит-кодів: збирає cheatfile_test.cpp та cheatfind_test.cpp хостовим g++,
# запускає результат і перевіряє рядки підсумку; окремо збирає реалізації компілятором
# телефона. Кожен запуск має межу часу. Проміжні файли й EXE лежать у тимчасовій теці, поза сховищем.
# Повертає 0 лише за повного успіху. -SrcDir задає іншу копію реалізації (для мутаційної перевірки).
param(
	[int]$CompileTimeoutMs = 120000,
	[int]$RunTimeoutMs = 120000,
	[int]$TestTimeoutMs = 30000,
	[int]$SuiteTimeoutMs = 240000,
	[string]$SrcDir = ''
)

$ErrorActionPreference = 'Stop'

$gxx = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\toolchain\perl\c\bin\g++.exe'))
$armGxx = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\toolchain\CSL_Arm_Toolchain\bin\arm-none-symbianelf-g++.exe'))
$srcDir = if ($SrcDir) { [IO.Path]::GetFullPath($SrcDir) } else { [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\src\ui')) }

# Тека збірки має бути ASCII-шляхом: mingw g++ мовчки завершується, коли TMP містить кирилицю.
Add-Type -Namespace Ps1EmuCheat -Name Path -MemberDefinition '[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)] public static extern uint GetShortPathName(string aLong, System.Text.StringBuilder aShort, uint aSize);'
$tempRoot = [IO.Path]::GetTempPath().TrimEnd('\')
$shortBuf = New-Object System.Text.StringBuilder 260
if ([Ps1EmuCheat.Path]::GetShortPathName($tempRoot, $shortBuf, 260) -gt 0) { $tempRoot = $shortBuf.ToString() }
if ($tempRoot -match '[^\x00-\x7F]') { throw "run.ps1: temp directory is not ASCII and has no short name: $tempRoot" }
$env:TMP = $tempRoot
$env:TEMP = $tempRoot
$work = Join-Path $tempRoot "ps1emu-cheat-$PID"
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

$suites = @(
	@{
		Name = 'cheatfile'
		TestSrc = Join-Path $PSScriptRoot 'cheatfile_test.cpp'
		ImplSrcs = @((Join-Path $srcDir 'cheatfile.cpp'))
		Tag = 'cheatfile'
	},
	@{
		Name = 'cheatfind'
		TestSrc = Join-Path $PSScriptRoot 'cheatfind_test.cpp'
		ImplSrcs = @((Join-Path $srcDir 'cheatfile.cpp'), (Join-Path $srcDir 'cheatfind.cpp'))
		Tag = 'cheatfind'
	}
)

# 1. Хостове збирання та запуск для кожної тестової програми.
$flags = @('-std=c++98', '-pedantic', '-Wall', '-Wextra', '-Werror', '-O2', '-static', "-I$srcDir")
$savedPath = $env:PATH
$env:PATH = (Split-Path -Parent $gxx) + ';' + $savedPath

foreach ($s in $suites) {
	if ($failures.Count -ne 0) { break }

	foreach ($imp in $s.ImplSrcs) {
		if (-not (Test-Path $imp)) {
			$failures += "$($s.Name): implementation not found: $imp"
			break
		}
	}
	if ($failures.Count -ne 0) { break }

	$exe = Join-Path $work "$($s.Name)_test.exe"
	$compileArgs = $flags + @($s.TestSrc) + $s.ImplSrcs + @('-o', $exe)
	$build = Invoke-Limited $gxx $compileArgs $CompileTimeoutMs "host-build-$($s.Name)"
	if ($build.TimedOut) { $failures += "host-build-$($s.Name): time limit exceeded" }
	elseif ($build.Code -ne 0) { $failures += "host-build-$($s.Name): exit code $($build.Code)" }
	if ($build.Text.Trim()) { Write-Output "--- host-build-$($s.Name) ---"; Write-Output $build.Text }
	if ($failures.Count -ne 0) { break }

	$list = Invoke-Limited $exe @('--list') $RunTimeoutMs "list-$($s.Name)"
	$names = @()
	foreach ($line in ($list.Text -split "`n")) {
		if ($line -match '^TEST (\S+)') { $names += $matches[1] }
	}
	if ($list.TimedOut -or $list.Code -ne 0 -or $names.Count -eq 0) {
		$failures += "list-$($s.Name): no test names (time limit, exit code or empty list)"
		break
	}

	$defined = ([regex]::Matches([IO.File]::ReadAllText($s.TestSrc), '(?m)^static void (Test\w+)\(\)')).Count
	if ($names.Count -ne $defined) {
		$failures += "list-$($s.Name): $($names.Count) tests in --list, $defined Test functions in $(Split-Path -Leaf $s.TestSrc)"
		break
	}

	$total = 0
	$failed = 0
	Write-Output "--- run $($s.Name) ---"
	$clock = [Diagnostics.Stopwatch]::StartNew()
	for ($i = 0; $i -lt $names.Count; $i++) {
		$name = $names[$i]
		if ($clock.ElapsedMilliseconds -gt $SuiteTimeoutMs) {
			$failed += 1
			Write-Output ("FAIL [suite] time budget {0} ms exceeded: {1} of {2} tests not run" -f $SuiteTimeoutMs, ($names.Count - $i), $names.Count)
			break
		}
		$one = Invoke-Limited $exe @('--only', $name) $TestTimeoutMs "run-$name"
		$regexPattern = "(?m)^$([regex]::Escape($s.Tag)): (\d+) checks, (\d+) failed\r?$"
		$m = [regex]::Match($one.Text, $regexPattern)
		if ($one.TimedOut) {
			$failed += 1
			$total += 1
			Write-Output "FAIL [$name] time limit exceeded: no result in $TestTimeoutMs ms"
		} elseif (-not $m.Success) {
			$failed += 1
			$total += 1
			Write-Output "FAIL [$name] no summary line, exit code $($one.Code)"
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
	Write-Output "$($s.Tag): $total checks, $failed failed"
	if ($total -eq 0) { $failures += "$($s.Name): no checks executed" }
	if ($failed -ne 0) { $failures += "$($s.Name): $failed failed checks or tests" }
}

$env:PATH = $savedPath

# 2. Збирання реалізацій компілятором телефона.
$armFiles = @('cheatfile.cpp', 'cheatfind.cpp')
foreach ($af in $armFiles) {
	if ($failures.Count -ne 0) { break }
	$afSrc = Join-Path $srcDir $af
	if (Test-Path $afSrc) {
		$armObj = Join-Path $work "$([IO.Path]::GetFileNameWithoutExtension($af))_arm.o"
		$arm = Invoke-Limited $armGxx @('-O2', '-Wall', '-Werror', '-fno-exceptions', '-fno-rtti', "-I$srcDir", '-c', $afSrc, '-o', $armObj) $CompileTimeoutMs "arm-build-$af"
		Write-Output "--- arm-build $af ---"
		if ($arm.Text.Trim()) { Write-Output $arm.Text }
		if ($arm.TimedOut) { $failures += "arm-build-${af}: time limit exceeded" }
		elseif ($arm.Code -ne 0) { $failures += "arm-build-${af}: exit code $($arm.Code)" }
		elseif ($arm.Text.Trim()) { $failures += "arm-build-${af}: compiler printed diagnostics" }
	}
}

if ($failures.Count -eq 0) {
	Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
	Write-Output 'run.ps1: PASS (host-build, run, arm-build)'
	exit 0
}
foreach ($f in $failures) { Write-Output "run.ps1: FAIL: $f" }
Write-Output "run.ps1: work directory kept for diagnosis: $work"
exit 1
