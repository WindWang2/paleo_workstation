# paleo-dev.ps1 — Windows sibling of ./paleo-dev (§44.1).
# Verbs: bootstrap | build | test | selfcheck | clean-vendor <dep>
# Binary-vendor route: OSGeo4W qgis-devel-4.2.x + dep closure, MSVC v14x /MD.
param(
  [Parameter(Mandatory=$true, Position=0)][string]$Verb,
  [Parameter(Position=1)][string]$Arg
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Root 'build'
$Vendor = Join-Path $Root 'vendor'

function Enter-MsvcEnvironment {
  if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { throw 'Visual Studio Build Tools not found' }
  $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $vsRoot) { throw 'MSVC C++ tools not installed' }
  $devCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
  $devCommand = '"' + $devCmd + '" -arch=amd64 -host_arch=amd64 >nul && set'
  $lines = & cmd.exe /d /c $devCommand
  if ($LASTEXITCODE -ne 0) { throw 'VsDevCmd failed' }
  foreach ($line in $lines) {
    if ($line -match '^([^=]+)=(.*)$') {
      [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
  }
}

function Enter-VendorEnvironment {
  $osgeo = Join-Path $Vendor 'osgeo4w'
  if (-not (Test-Path (Join-Path $osgeo 'apps\qgis\include\qgsapplication.h'))) { return }
  $env:QGIS_PREFIX_PATH = Join-Path $osgeo 'apps\qgis'
  $env:CMAKE_PREFIX_PATH = Join-Path $osgeo 'apps\qt6'
  $env:PATH = ((Join-Path $osgeo 'bin'), (Join-Path $osgeo 'apps\qgis\bin'),
               (Join-Path $osgeo 'apps\qt6\bin'), $env:PATH) -join ';'
}

function Preflight {
  Write-Host "== preflight =="
  Enter-MsvcEnvironment
  foreach ($tool in 'cmake','ninja','cl') {
    $found = Get-Command $tool -ErrorAction SilentlyContinue
    if ($found) { Write-Host ("  OK  {0} -> {1}" -f $tool, $found.Source) }
    else        { Write-Host ("  MISSING {0}" -f $tool); $script:failed = $true }
  }
  $free = (Get-PSDrive (Split-Path $Root -Qualifier).TrimEnd(':')).Free / 1GB
  Write-Host ("  disk free: {0:N0} GB" -f $free)
  if ($free -lt 20) { Write-Host "  WARN: vendor closure needs ~20GB"; }
  if ($script:failed) { throw "preflight failed — install missing tools (VS Build Tools + cmake + ninja)" }
}

switch ($Verb) {
  'bootstrap' {
    Preflight
    Write-Host "== vendor bootstrap (OSGeo4W route) =="
    $pin = (Get-Content (Join-Path $Vendor 'manifest.json') -Raw | ConvertFrom-Json).deps.osgeo4w
    if (-not $pin) { throw 'OSGeo4W pin missing from vendor/manifest.json' }
    $setup = Join-Path $Vendor 'osgeo4w-setup.exe'
    Invoke-WebRequest -Uri $pin.installer_url -OutFile $setup
    $actual = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $pin.installer_sha256) { throw "OSGeo4W installer SHA256 mismatch: $actual" }
    $osgeo = Join-Path $Vendor 'osgeo4w'
    $cache = Join-Path $Vendor 'cache\osgeo4w'
    New-Item -ItemType Directory -Force $osgeo, $cache | Out-Null
    $setupArgs = @('-q', '-A', '-k', '-n', '-N', '-d', '-O', '-s', $pin.site,
                   '-R', ('"' + $osgeo + '"'), '-l', ('"' + $cache + '"'),
                   '-P', ($pin.packages -join ','))
    # The installer detaches when invoked directly; wait for the entire setup
    # process tree before inspecting installed.db or configuring CMake.
    $setupProcess = Start-Process -FilePath $setup -ArgumentList $setupArgs -Wait -PassThru
    if ($setupProcess.ExitCode -ne 0) { throw "OSGeo4W setup failed: $($setupProcess.ExitCode)" }
    $installedDb = Join-Path $osgeo 'etc\setup\installed.db'
    if (-not (Test-Path $installedDb) -or
        -not (Select-String -Path $installedDb -Pattern '^qgis\s+qgis-4\.2\.' -Quiet) -or
        -not (Select-String -Path $installedDb -Pattern '^qgis-devel\s+qgis-devel-4\.2\.' -Quiet)) {
      throw 'OSGeo4W did not install QGIS and development headers from the 4.2.x family'
    }
    foreach ($required in @('apps\qgis\include\qgsapplication.h',
                           'apps\qgis\lib\qgis_core.lib',
                           'apps\qgis\lib\qgis_gui.lib',
                           'apps\qt6\lib\cmake\Qt6\Qt6Config.cmake',
                           'apps\qt6\plugins\sqldrivers\qsqloci.dll',
                           # qgscodeeditor.h（qgis-devel 头链）需要 QSci 头，
                           # 由 qscintilla-qt6-devel 提供在 Qt6 include 前缀下
                           'apps\Qt6\include\Qsci\qsciapis.h',
                           'apps\Qt6\lib\qscintilla2_qt6.lib',
                           'include\gdal.h', 'lib\gdal.lib',
                           'include\sqlite3.h')) {
      if (-not (Test-Path (Join-Path $osgeo $required))) { throw "OSGeo4W closure missing $required" }
    }

    $ortPin = (Get-Content (Join-Path $Vendor 'manifest.json') -Raw | ConvertFrom-Json).deps.onnxruntime_win
    if ($ortPin) {
      $ortDest = Join-Path $Vendor 'onnxruntime'
      if (-not (Test-Path (Join-Path $ortDest 'include\onnxruntime_c_api.h'))) {
        Write-Host "Fetching ONNX Runtime Windows archive..."
        $ortZip = Join-Path $Vendor 'onnxruntime-win-x64.zip'
        Invoke-WebRequest -Uri $ortPin.url -OutFile $ortZip
        $actualOrt = (Get-FileHash $ortZip -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualOrt -ne $ortPin.archive.sha256) { throw "ONNX Runtime Windows archive SHA256 mismatch: $actualOrt" }
        $tmpOrt = Join-Path $Vendor 'tmp-ort'
        Expand-Archive -Path $ortZip -DestinationPath $tmpOrt -Force
        $inner = Get-ChildItem -Path $tmpOrt | Select-Object -First 1
        if (Test-Path $ortDest) { Remove-Item -Recurse -Force $ortDest }
        Move-Item -Path $inner.FullName -Destination $ortDest -Force
        Remove-Item -Recurse -Force $tmpOrt, $ortZip
      }
    }

    Enter-VendorEnvironment
    & $PSCommandPath build
    if ($LASTEXITCODE -ne 0) { throw 'Windows bootstrap build failed' }
    & $PSCommandPath selfcheck
    if ($LASTEXITCODE -ne 0) { throw 'Windows bootstrap selfcheck failed' }
  }
  'build' {
    Enter-MsvcEnvironment
    Enter-VendorEnvironment
    cmake -S $Root -B $Build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DQGIS_PREFIX=$(Join-Path $Vendor 'osgeo4w')"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    cmake --build $Build
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed' }
  }
  'test' {
    Enter-VendorEnvironment
    $env:QT_QPA_PLATFORM = 'offscreen'
    $logDir = Join-Path $Vendor 'logs'
    New-Item -ItemType Directory -Force $logDir | Out-Null
    $log = Join-Path $logDir 'ctest.log'
    ctest --test-dir $Build --output-on-failure 2>&1 | Tee-Object -FilePath $log
    if ($LASTEXITCODE -ne 0) {
      # ctest 的 --output-on-failure 在 Windows runner 上回收不到子进程
      # 输出；失败测试逐个直跑，QtTest 的 FAIL/Loc 行直落日志与控制台
      # （控制台可见性：日志文件在 artifact 里，排障不应多一跳）。
      $names = & ctest --test-dir $Build --rerun-failed -N 2>$null |
        ForEach-Object { if ($_ -match 'Test\s+#\d+:\s+(\S+)') { $Matches[1] } }
      foreach ($n in $names) {
        "=== $n (direct run) ===" | Tee-Object -FilePath $log -Append
        & (Join-Path $Build "$n.exe") 2>&1 | Tee-Object -FilePath $log -Append
      }
      throw 'CTest failed'
    }
  }
  'selfcheck' {
    Enter-VendorEnvironment
    $env:QT_QPA_PLATFORM = 'offscreen'
    & (Join-Path $Build 'paleo_selfcheck.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Selfcheck failed' }
  }
  'clean-vendor' {
    if (-not $Arg) {
      foreach ($TargetPath in @((Join-Path $Vendor 'build'), (Join-Path $Vendor 'prefix'), (Join-Path $Vendor 'onnxruntime'))) {
        if (Test-Path $TargetPath) { Remove-Item -Recurse -Force $TargetPath }
      }
      Write-Host "vendor build tree cleared (archives kept)"
    } else {
      if ($Arg -notmatch '^[A-Za-z0-9._-]+$' -or $Arg -in @('.', '..')) {
        throw "invalid vendor dependency name '$Arg'"
      }
      $target = if ($Arg -eq 'onnxruntime') { Join-Path $Vendor 'onnxruntime' } else { Join-Path $Vendor $Arg }
      if (Test-Path $target) { Remove-Item -Recurse -Force $target; Write-Host "removed $target" }
    }
  }
  default { throw "unknown verb '$Verb' — bootstrap|build|test|selfcheck|clean-vendor" }
}
