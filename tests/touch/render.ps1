# Перегляд панелі на ПК: збирає render_panel.cpp разом із touchcore.cpp і touchdraw.cpp хостовим
# g++, запускає його й пише BMP-файли в теку, задану параметром -OutDir. Кожен запуск має межу часу.
# Повертає 0 лише тоді, коли збірка пройшла, кожен очікуваний файл створено й він непорожній.
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

# 1. Збирання. cc1plus знаходить свої DLL через PATH: без каталогу g++ він мовчки завершується з кодом 1.
$exe = Join-Path $work 'render_panel.exe'
$flags = @('-std=c++98', '-pedantic', '-Wall', '-Wextra', '-Werror', '-O2', "-I$srcDir")
$savedPath = $env:PATH
$env:PATH = (Split-Path -Parent $gxx) + ';' + $savedPath
$build = Invoke-Limited $gxx ($flags + @($viewSrc, $coreSrc, $drawSrc, '-o', $exe)) $CompileTimeoutMs 'host-build'
$env:PATH = $savedPath
if ($build.TimedOut) { $failures += 'host-build: time limit exceeded' }
elseif ($build.Code -ne 0) { $failures += "host-build: exit code $($build.Code)" }
if ($build.Text.Trim()) { Write-Output "--- host-build ---"; Write-Output $build.Text }

# 2. Запуск: пише файли в OutDir і звіряє 16- та 32-бітні шляхи.
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$outAbs = [IO.Path]::GetFullPath($OutDir)
if ($failures.Count -eq 0) {
	$run = Invoke-Limited $exe @($outAbs) $RunTimeoutMs 'render'
	Write-Output "--- render ---"
	Write-Output $run.Text
	if ($run.TimedOut) { $failures += 'render: time limit exceeded' }
	elseif ($run.Code -eq 1) { $failures += 'render: a BMP file could not be written' }
	elseif ($run.Code -eq 2) { $failures += 'render: 16-bit and 32-bit paths disagree beyond the tolerance' }
	elseif ($run.Code -ne 0) { $failures += "render: exit code $($run.Code)" }
	# Очікувано: 2 екрани × 2 розкладки × 3 фони × 3 стани × 2 непрозорості.
	# Рядки stdout закінчуються на CRLF (текстовий режим C), тож \r відокремлюємо від шляху.
	$written = @([regex]::Matches($run.Text, '(?m)^wrote ([^\r\n]+)') | ForEach-Object { $_.Groups[1].Value.Trim() })
	if ($written.Count -ne 72) { $failures += "render: $($written.Count) files reported, 72 expected" }
	foreach ($f in $written) {
		if (-not (Test-Path -LiteralPath $f)) { $failures += "render: file missing: $f"; continue }
		if ((Get-Item -LiteralPath $f).Length -le 0) { $failures += "render: empty file: $f" }
	}
}

if ($failures.Count -eq 0) {
	Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
	Write-Output "render.ps1: PASS ($($written.Count) files in $outAbs)"
	exit 0
}
foreach ($f in $failures) { Write-Output "render.ps1: FAIL: $f" }
Write-Output "render.ps1: work directory kept for diagnosis: $work"
exit 1
