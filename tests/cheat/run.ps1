# Перевірки cheatfile: збирає src\ui\cheatfile.cpp разом з cheatfile_test.cpp хостовим g++,
# запускає результат і перевіряє рядок підсумку; окремо збирає cheatfile.cpp компілятором
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
$testSrc = Join-Path $PSScriptRoot 'cheatfile_test.cpp'
$implSrc = Join-Path $srcDir 'cheatfile.cpp'

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

# Запуск через System.Diagnostics.Process: у Windows PowerShell 5.1 Start-Process з
# перенаправленням віддає порожній ExitCode. Вивід читається асинхронно, щоб великий вивід
# не заблокував процес. Після влучення в межу часу процес знищують.
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

if (-not (Test-Path $implSrc)) {
	$failures += "implementation not found: $implSrc"
}

if ($failures.Count -eq 0) {
	# 1. Хостове збирання тесту разом із реалізацією і запуск.
	$exe = Join-Path $work 'cheatfile_test.exe'
	# -static: EXE не залежить від DLL компілятора (без нього запуск віддає 0xC0000135, DLL не знайдено).
	$flags = @('-std=c++98', '-pedantic', '-Wall', '-Wextra', '-Werror', '-O2', '-static', "-I$srcDir")
	# cc1plus.exe знаходить свої DLL через PATH; без каталогу g++ він мовчки завершується з кодом 1.
	$savedPath = $env:PATH
	$env:PATH = (Split-Path -Parent $gxx) + ';' + $savedPath
	$build = Invoke-Limited $gxx ($flags + @($testSrc, $implSrc, '-o', $exe)) $CompileTimeoutMs 'host-build'
	$env:PATH = $savedPath
	if ($build.TimedOut) { $failures += 'host-build: time limit exceeded' }
	elseif ($build.Code -ne 0) { $failures += "host-build: exit code $($build.Code)" }
	if ($build.Text.Trim()) { Write-Output '--- host-build ---'; Write-Output $build.Text }
}

if ($failures.Count -eq 0) {
	$list = Invoke-Limited $exe @('--list') $RunTimeoutMs 'list'
	$names = @()
	foreach ($line in ($list.Text -split "`n")) {
		if ($line -match '^TEST (\S+)') { $names += $matches[1] }
	}
	if ($list.TimedOut -or $list.Code -ne 0 -or $names.Count -eq 0) {
		$failures += 'list: no test names (time limit, exit code or empty list)'
	}
	# Кількість функцій Test* у файлі має збігатися з --list: закоментований RUN інакше пройшов би непоміченим.
	$defined = ([regex]::Matches([IO.File]::ReadAllText($testSrc), '(?m)^static void (Test\w+)\(\)')).Count
	if ($names.Count -ne $defined) {
		$failures += "list: $($names.Count) tests in --list, $defined Test functions in cheatfile_test.cpp (RUN missing or commented out)"
	}
	$total = 0
	$failed = 0
	Write-Output '--- run ---'
	$clock = [Diagnostics.Stopwatch]::StartNew()
	for ($i = 0; $i -lt $names.Count; $i++) {
		$name = $names[$i]
		if ($clock.ElapsedMilliseconds -gt $SuiteTimeoutMs) {
			$failed += 1
			Write-Output ("FAIL [suite] time budget {0} ms exceeded: {1} of {2} tests not run" -f $SuiteTimeoutMs, ($names.Count - $i), $names.Count)
			break
		}
		$one = Invoke-Limited $exe @('--only', $name) $TestTimeoutMs ("run-$name")
		$m = [regex]::Match($one.Text, '(?m)^cheatfile: (\d+) checks, (\d+) failed\r?$')
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
	Write-Output "cheatfile: $total checks, $failed failed"
	if ($total -eq 0) { $failures += 'run: no checks executed' }
	if ($failed -ne 0) { $failures += "run: $failed failed checks or tests" }
}

# 2. Збирання cheatfile.cpp компілятором телефона: будь-яке попередження чи помилка — невдача.
if ($failures.Count -eq 0 -or (Test-Path $implSrc)) {
	$armObj = Join-Path $work 'cheatfile_arm.o'
	$arm = Invoke-Limited $armGxx @('-O2', '-Wall', '-Werror', '-fno-exceptions', '-fno-rtti', '-c', $implSrc, '-o', $armObj) $CompileTimeoutMs 'arm-build'
	Write-Output '--- arm-build ---'
	Write-Output $arm.Text
	if ($arm.TimedOut) { $failures += 'arm-build: time limit exceeded' }
	elseif ($arm.Code -ne 0) { $failures += "arm-build: exit code $($arm.Code)" }
	elseif ($arm.Text.Trim()) { $failures += 'arm-build: compiler printed diagnostics' }
}

if ($failures.Count -eq 0) {
	Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
	Write-Output 'run.ps1: PASS (host-build, run, arm-build)'
	exit 0
}
foreach ($f in $failures) { Write-Output "run.ps1: FAIL: $f" }
Write-Output "run.ps1: work directory kept for diagnosis: $work"
exit 1
