# Перегляд панелі на ПК: збирає render_panel.cpp разом із touchcore.cpp і touchdraw.cpp хостовим
# g++ (статично, щоб exe не потребував DLL компілятора), запускає його з теки -OutDir і перевіряє,
# що з'явилися всі очікувані BMP-файли. Кожен запуск має межу часу. Повертає 0 лише тоді, коли
# збірка пройшла, перевірки проходять і кожен очікуваний файл створено й він непорожній.
# Працює під pwsh 7 і Windows PowerShell 5.1: процеси запускаються через ProcessStartInfo,
# бо Start-Process у 5.1 повертав порожній код виходу.
param(
	[Parameter(Mandatory = $true)][string]$OutDir,
	[int]$CompileTimeoutMs = 120000,
	[int]$RunTimeoutMs = 300000
)

$ErrorActionPreference = 'Stop'

$gxx = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\toolchain\perl\c\bin\g++.exe'))
$srcDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\src\ui'))
$viewSrc = Join-Path $PSScriptRoot 'render_panel.cpp'
$coreSrc = Join-Path $srcDir 'touchcore.cpp'
$drawSrc = Join-Path $srcDir 'touchdraw.cpp'

# Тека збірки має бути ASCII-шляхом: mingw g++ мовчки завершується, коли TMP містить кирилицю.
Add-Type -Namespace Ps1Emu -Name RenderPath -MemberDefinition '[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)] public static extern uint GetShortPathName(string aLong, System.Text.StringBuilder aShort, uint aSize);'
$tempRoot = [IO.Path]::GetTempPath().TrimEnd('\')
$shortBuf = New-Object System.Text.StringBuilder 260
if ([Ps1Emu.RenderPath]::GetShortPathName($tempRoot, $shortBuf, 260) -gt 0) { $tempRoot = $shortBuf.ToString() }
if ($tempRoot -match '[^\x00-\x7F]') { throw "render.ps1: temp directory is not ASCII and has no short name: $tempRoot" }
$env:TMP = $tempRoot
$env:TEMP = $tempRoot
$work = Join-Path $tempRoot "ps1emu-render-$PID"
New-Item -ItemType Directory -Force -Path $work | Out-Null

# Аргумент із пробілами береться в лапки: командний рядок склеюється з масиву.
function Join-Args {
	param([string[]]$Arguments)
	return (($Arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' ')
}

# Запуск з межею часу. Вивід читається асинхронно, щоб заповнений канал не зупинив процес.
# Після вбивства дочірні процеси (cc1plus) можуть тримати канали, тож чекання на них обмежене.
function Invoke-Limited {
	param([string]$Exe, [string[]]$Arguments, [int]$TimeoutMs, [string]$WorkDir)
	$psi = New-Object System.Diagnostics.ProcessStartInfo
	$psi.FileName = $Exe
	$psi.Arguments = (Join-Args $Arguments)
	$psi.WorkingDirectory = $WorkDir
	$psi.UseShellExecute = $false
	$psi.CreateNoWindow = $true
	$psi.RedirectStandardOutput = $true
	$psi.RedirectStandardError = $true
	$p = [System.Diagnostics.Process]::Start($psi)
	$outTask = $p.StandardOutput.ReadToEndAsync()
	$errTask = $p.StandardError.ReadToEndAsync()
	$timedOut = -not $p.WaitForExit($TimeoutMs)
	if ($timedOut) { try { $p.Kill() } catch { } }
	else { $p.WaitForExit() }
	$parts = @()
	foreach ($t in @($outTask, $errTask)) { if ($t.Wait(5000)) { $parts += $t.Result } }
	$code = -1
	if (-not $timedOut) { $code = $p.ExitCode }
	return [pscustomobject]@{ Code = $code; TimedOut = $timedOut; Text = (($parts | Where-Object { $_ }) -join "`n") }
}

$failures = @()

# 1. Збирання. cc1plus знаходить свої DLL через PATH: без каталогу g++ він мовчки завершується з кодом 1.
$exe = Join-Path $work 'render_panel.exe'
$flags = @('-std=c++98', '-pedantic', '-Wall', '-Wextra', '-Werror', '-O2', '-static', "-I$srcDir")
$savedPath = $env:PATH
$env:PATH = (Split-Path -Parent $gxx) + ';' + $savedPath
$build = Invoke-Limited $gxx ($flags + @($viewSrc, $coreSrc, $drawSrc, '-o', $exe)) $CompileTimeoutMs $PSScriptRoot
$env:PATH = $savedPath
if ($build.TimedOut) { $failures += 'host-build: time limit exceeded' }
elseif ($build.Code -ne 0) { $failures += "host-build: exit code $($build.Code)" }
if ($build.Text.Trim()) { Write-Output "--- host-build ---"; Write-Output $build.Text }

# 2. Запуск. exe працює з теки виводу й пише файли під ASCII-іменами "./name.bmp": шлях із
# не-ASCII символами не проходить ні через командний рядок, ні через розбір stdout.
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$outAbs = [IO.Path]::GetFullPath($OutDir)
$expected = @()
foreach ($s in @('640x360', '320x240')) {
	foreach ($l in @('twelve', 'sticks')) {
		foreach ($b in @('white', 'black', 'stripes')) {
			foreach ($st in @('rest', 'pressed', 'stick')) {
				foreach ($o in @(100, 40)) { $expected += "${s}_${l}_${b}_${st}_op${o}.bmp" }
			}
		}
	}
}
if ($failures.Count -eq 0) {
	$run = Invoke-Limited $exe @('.') $RunTimeoutMs $outAbs
	Write-Output "--- render ---"
	Write-Output $run.Text
	if ($run.TimedOut) { $failures += 'render: time limit exceeded' }
	elseif ($run.Code -eq 1) { $failures += 'render: a BMP file could not be written' }
	elseif ($run.Code -eq 2) { $failures += 'render: 16-bit and 32-bit paths disagree beyond the tolerance' }
	elseif ($run.Code -eq 3) { $failures += 'render: a layout or the art could not be built' }
	elseif ($run.Code -eq 4) { $failures += 'render: a geometry check failed (see the geometry lines above)' }
	elseif ($run.Code -ne 0) { $failures += "render: exit code $($run.Code)" }
	# Очікувано: 2 екрани × 2 розкладки × 3 фони × 3 стани × 2 непрозорості.
	foreach ($name in $expected) {
		$file = Join-Path $outAbs $name
		if (-not (Test-Path -LiteralPath $file)) { $failures += "render: file missing: $name"; continue }
		if ((Get-Item -LiteralPath $file).Length -le 0) { $failures += "render: empty file: $name" }
	}
}

if ($failures.Count -eq 0) {
	Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
	Write-Output "render.ps1: PASS ($($expected.Count) files in $outAbs)"
	exit 0
}
foreach ($f in $failures) { Write-Output "render.ps1: FAIL: $f" }
Write-Output "render.ps1: work directory kept for diagnosis: $work"
exit 1
