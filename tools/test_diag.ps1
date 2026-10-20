$deps = 'C:\Users\wangj.KEVIN\paleo-qgis-deps'
$qgis = 'C:\Users\wangj.KEVIN\paleo-qgis-prefix'
$qtpdf = 'C:\deps\Qt\6.11.2\msvc2022_64'
$env:PATH = ((Join-Path $deps 'Library\bin'), (Join-Path $qgis 'bin'), (Join-Path $qtpdf 'bin'), $deps, $env:PATH) -join ';'
$env:QT_PLUGIN_PATH = Join-Path $deps 'Library\plugins'
$env:QT_QPA_PLATFORM = 'offscreen'

$exe = Join-Path (Get-Location) 'build\tst_algorithms.exe'
Write-Host "Running $exe..."
& $exe
Write-Host "Exit code: $LASTEXITCODE"
