# Звіряє work/features.json із work/state.md і зі сховищем.
# Падає на розходженні, щоб протокол завершення сесії не лишався обіцянкою.
# Запуск: pwsh -NoProfile -File work/check-state.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$errors = @()

$fPath = Join-Path $root 'work/features.json'
$sPath = Join-Path $root 'work/state.md'
$states = @('невиконано', 'в роботі', 'виконано', 'закрито')

$f = Get-Content -Raw -Encoding utf8 $fPath | ConvertFrom-Json
$ids = @()
foreach ($x in $f.функції) {
    foreach ($k in @('id', 'назва', 'стан', 'рівень', 'кроки', 'перевірка')) {
        if (-not $x.PSObject.Properties.Name.Contains($k)) { $errors += "функція $($x.id): немає поля $k" }
    }
    if ($states -notcontains $x.стан) { $errors += "функція $($x.id): невідомий стан '$($x.стан)'" }
    $ids += $x.id
}
foreach ($x in $f.функції) {
    foreach ($d in $x.залежить_від) {
        if ($ids -notcontains $d) { $errors += "функція $($x.id): залежить від невідомого $d" }
    }
}
if (($ids | Sort-Object -Unique).Count -ne $ids.Count) { $errors += 'у переліку є повторені id' }

# Пункт, який колись був у сховищі, вилучати не можна (розділ 16 методології).
$head = & git -C $root show HEAD:work/features.json 2>$null
if ($LASTEXITCODE -eq 0 -and $head) {
    foreach ($old in ($head | ConvertFrom-Json).функції.id) {
        if ($ids -notcontains $old) { $errors += "функцію $old вилучено з переліку" }
    }
}

$state = Get-Content -Raw -Encoding utf8 $sPath
foreach ($field in @('Крок', 'Функції', 'Останній прогін перевірок', 'Незакриті зауваження', 'Наступний крок')) {
    if ($state -notmatch "(?m)^- $([regex]::Escape($field)):\s*\S") { $errors += "state.md: немає заповненого поля '$field'" }
}
foreach ($field in @('Крок', 'Наступний крок')) {
    if ($state -match "(?m)^- $([regex]::Escape($field)):\s*(.+?)\s*$") {
        $v = $Matches[1]
        if ($v -ne 'немає' -and $ids -notcontains $v) { $errors += "state.md: поле '$field' називає невідому функцію '$v'" }
    }
}
$done = @($f.функції | Where-Object { $_.стан -eq 'виконано' -or $_.стан -eq 'закрито' }).Count
if ($state -match '(?m)^- Функції:\s*(\d+)\s+з\s+(\d+)') {
    if ([int]$Matches[1] -ne $done) { $errors += "state.md: заявлено виконаних $($Matches[1]), у переліку $done" }
    if ([int]$Matches[2] -ne $ids.Count) { $errors += "state.md: заявлено функцій $($Matches[2]), у переліку $($ids.Count)" }
}

if ($errors.Count -gt 0) {
    $errors | ForEach-Object { Write-Output "РОЗХОДЖЕННЯ: $_" }
    exit 1
}
Write-Output "стан узгоджений: функцій $($ids.Count), виконано $done"
exit 0
