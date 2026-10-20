$cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
$deps = 'C:\Users\wangj.KEVIN\paleo-qgis-deps'
$qgis = 'C:\Users\wangj.KEVIN\paleo-qgis-prefix'
$qtpdf = 'C:\deps\Qt\6.11.2\msvc2022_64'
$env:PATH = ((Join-Path $deps 'Library\bin'), (Join-Path $qgis 'bin'), (Join-Path $qtpdf 'bin'), $deps, $env:PATH) -join ';'
$env:QT_PLUGIN_PATH = Join-Path $deps 'Library\plugins'

$exe = Join-Path (Get-Location) 'build\paleo.exe'
& $cdb -c 'lmv m libcrypto*; lmv m libssl*; q' $exe
