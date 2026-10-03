# paleo-dev.ps1 — Windows sibling of ./paleo-dev (§44.1).
# Verbs: bootstrap [fetch-only] | build | test | selfcheck | clean-vendor <dep>
#   bootstrap fetch-only : 只取依赖（OSGeo4W + ORT），不编译不自检——CI 用它让
#                          编译错误落在 Build 步骤而不是 Vendor 步骤（#134）。
#   build                : $env:CI 已设时 ninja -k 0，一次暴露全部编译错误。
#   test                 : $env:PALEO_CTEST_ARGS 透传给 ctest（如 "-LE perf"）。
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
  # vendored onnxruntime 必须排在 OSGeo4W bin 之前——OSGeo4W 自带
  # onnxruntime 1.17.1，我们的头文件是 1.30（ABI 30）；PATH 顺序错了
  # 会加载旧 DLL，会话构造直接段错误（tst_onnx 实锤）。
  $ortLib = Join-Path $Vendor 'onnxruntime\lib'
  $env:PATH = ($ortLib, (Join-Path $osgeo 'bin'), (Join-Path $osgeo 'apps\qgis\bin'),
               (Join-Path $osgeo 'apps\qt6\bin'), $env:PATH) -join ';'
  # GDAL/PROJ 数据目录：缺省时 GDAL 找不到 tms_NZTM2000.json、PROJ
  # 报 CRS 无大地基准（影响坐标变换类用例）。
  $gdalData = Join-Path $osgeo 'apps\gdal\share\gdal'
  if (Test-Path $gdalData) { $env:GDAL_DATA = $gdalData }
  $projData = Join-Path $osgeo 'share\proj'
  if (Test-Path $projData) { $env:PROJ_LIB = $projData }
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
    if ($Arg -eq 'fetch-only') {
      Write-Host "== fetch-only: skip build/selfcheck (run 'build' then 'selfcheck') =="
    } else {
      & $PSCommandPath build
      if ($LASTEXITCODE -ne 0) { throw 'Windows bootstrap build failed' }
      & $PSCommandPath selfcheck
      if ($LASTEXITCODE -ne 0) { throw 'Windows bootstrap selfcheck failed' }
    }
  }
  'build' {
    Enter-MsvcEnvironment
    Enter-VendorEnvironment
    cmake -S $Root -B $Build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DQGIS_PREFIX=$(Join-Path $Vendor 'osgeo4w')"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    # CI 上 -k 0：ninja 不在第一个错误处停，一轮暴露全部 MSVC 编译错误（#134）。
    if ($env:CI) { cmake --build $Build -- -k 0 } else { cmake --build $Build }
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed' }
    # vendored onnxruntime.dll 拷进 build（应用目录在 DLL 搜索序中永远
    # 第一）：PATH 排序压不住 OSGeo4W 自带的 1.17.1（qgis 依赖链的解析
    # 机制绕过 PATH），ABI 1.30 头对 1.17 运行时即段错误。
    $ortLib = Join-Path $Vendor 'onnxruntime\lib'
    if (Test-Path $ortLib) {
      Copy-Item (Join-Path $ortLib '*.dll') $Build -Force
      Write-Host ("  ort dll staged -> {0} ({1})" -f $Build,
        (Get-Item (Join-Path $Build 'onnxruntime.dll')).VersionInfo.FileVersion)
    }
  }
  'test' {
    Enter-VendorEnvironment
    $env:QT_QPA_PLATFORM = 'offscreen'
    $logDir = Join-Path $Vendor 'logs'
    New-Item -ItemType Directory -Force $logDir | Out-Null
    $log = Join-Path $logDir 'ctest.log'
    $ctestExtra = @()
    if ($env:PALEO_CTEST_ARGS) { $ctestExtra = $env:PALEO_CTEST_ARGS.Trim() -split '\s+' }
    ctest --test-dir $Build --output-on-failure @ctestExtra 2>&1 | Tee-Object -FilePath $log
    if ($LASTEXITCODE -ne 0) {
      # ctest 的 --output-on-failure 在 Windows runner 上回收不到子进程
      # 输出；失败测试逐个直跑，QtTest 的 FAIL/Loc 行直落日志与控制台
      # （控制台可见性：日志文件在 artifact 里，排障不应多一跳）。
      $names = & ctest --test-dir $Build --rerun-failed -N @ctestExtra 2>$null |
        ForEach-Object { if ($_ -match 'Test\s+#\d+:\s+(\S+)') { $Matches[1] } }
      foreach ($n in $names) {
        # 有些 ctest 项不是可执行文件（layering 是 python 脚本）——跳过，
        # 否则 & 不存在的 .exe 会终止整个直跑循环。
        $exe = Join-Path $Build "$n.exe"
        if (-not (Test-Path $exe)) { continue }
        "=== $n (direct run) ===" | Tee-Object -FilePath $log -Append
        # QtTest 在无控制台的 Windows 上把结果走 OutputDebugString，
        # stdout 重定向收不到——用 -o 落文件再回放（崩溃栈仍走 stderr）。
        $qtout = Join-Path $logDir "$n.qtout.txt"
        & $exe -o "$qtout,txt" 2>&1 | Tee-Object -FilePath $log -Append
        if (Test-Path $qtout) {
          Get-Content $qtout | Tee-Object -FilePath $log -Append
          Remove-Item $qtout -Force
        }
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
  default { throw "unknown verb '$Verb' — bootstrap [fetch-only]|build|test|selfcheck|clean-vendor" }
}
