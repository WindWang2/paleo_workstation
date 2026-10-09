# paleo-dev.ps1 — Windows sibling of ./paleo-dev (§44.1).
# Verbs: bootstrap [fetch-only] | build | test | selfcheck | checkenv | ensure-resources | clean-vendor <dep>
#   bootstrap fetch-only : 只取依赖（OSGeo4W + ORT），不编译不自检——CI 用它让
#                          编译错误落在 Build 步骤而不是 Vendor 步骤（#134）。
#   build                : $env:CI 已设时 ninja -k 0，一次暴露全部编译错误。
#   test                 : $env:PALEO_CTEST_ARGS 透传给 ctest（如 "-LE perf"）。
#   checkenv             : Qt 编译/运行链一致性自检（方向 72 防回归）。
#   ensure-resources     : 补齐 QGIS 前缀 resources/**（含 srs.db；方向 81，
#                          localdeps 的 test/selfcheck 缺 srs.db 时自动调用）。
# 依赖路线两条（Enter-DependencyEnvironment 自动择一，vendored 优先）：
#   vendored ：OSGeo4W qgis-devel-4.2.x 闭包（vendor/osgeo4w，CI 口径）。
#   localdeps：本机 conda 依赖根（默认 ~/paleo-qgis-deps，qt6-main 6.11.2 全家
#              + GDAL/GEOS 闭包）+ QGIS 4.2.0 前缀（~/paleo-qgis-prefix，按
#              Qt 6.11.2 构建）+ qtpdf 官方 6.11.2 覆盖层（默认
#              C:/deps/Qt/6.11.2/msvc2022_64——conda 闭包不带 Qt6Pdf，经
#              QT_ADDITIONAL_PACKAGES_PREFIX_PATH 补位）。编译与运行同链
#              6.11.2，治「exe 按 6.8 编、QGIS DLL 按 6.11 载」的混链（方向 72）。
#              根位置可被 PALEO_LOCAL_DEPS / PALEO_QGIS_PREFIX / PALEO_QTPDF_PREFIX 覆盖。
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
  if (-not (Test-Path (Join-Path $osgeo 'apps\qgis\include\qgsapplication.h'))) { return $false }
  $script:DepsRoute = 'vendored'
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
  return $true
}

function Enter-LocalDepsEnvironment {
  # 方向 72：本机 conda 依赖路线。Qt 6.11.2 编译（deps 头/cmake）与运行
  # （deps DLL，PATH 前置）同链；QGIS prefix 与 deps 的 Qt 同为 6.11.2 族。
  # 探测基准：deps 根有 Qt6Core.dll、QGIS 前缀有 qgis_core.lib——都不在则
  # 静默让位（返回 $false），build/test 会落到系统兜底并保持原报错语义。
  $deps = if ($env:PALEO_LOCAL_DEPS) { $env:PALEO_LOCAL_DEPS } else { Join-Path $env:USERPROFILE 'paleo-qgis-deps' }
  $qgis = if ($env:PALEO_QGIS_PREFIX) { $env:PALEO_QGIS_PREFIX } else { Join-Path $env:USERPROFILE 'paleo-qgis-prefix' }
  $qtpdf = if ($env:PALEO_QTPDF_PREFIX) { $env:PALEO_QTPDF_PREFIX } else { 'C:/deps/Qt/6.11.2/msvc2022_64' }
  if (-not (Test-Path (Join-Path $deps 'Library\bin\Qt6Core.dll'))) { return $false }
  if (-not (Test-Path (Join-Path $qgis 'lib\qgis_core.lib'))) { return $false }
  $script:DepsRoute = 'localdeps'
  $script:LocalQgis = $qgis; $script:LocalQtpdf = $qtpdf
  $env:QGIS_PREFIX_PATH = $qgis
  $env:CMAKE_PREFIX_PATH = Join-Path $deps 'Library'
  # Qt6Pdf 系不在 conda 闭包里：官方 6.11.2 qtpdf 扩展作为独立前缀补位
  # （Qt6Config 只在自己前缀内找组件，QT_ADDITIONAL_PACKAGES_PREFIX_PATH
  # 是 Qt 官方的「模块独立前缀」入口）。
  $env:QT_ADDITIONAL_PACKAGES_PREFIX_PATH = $qtpdf
  # QScintilla 头随 conda Qt 走（include/qt6/Qsci）——旧的
  # C:/deps/qscintilla-install 按 6.8 环境构建，统一链下退役。
  $env:QSCINTILLA_PREFIX_PATH = Join-Path $deps 'Library\include\qt6'
  # DLL 搜索序：deps 运行时在前（exe 与 QGIS DLL 共用的 6.11.2），QGIS 前缀
  # bin 随后（exe 静态导入 qgis_core/gui/analysis），qtpdf 覆盖层补 Qt6Pdf*.dll；
  # deps 根随后（conda 布局的 python.exe 在根不在 Library/bin——tst_pythonrepl/
  # tst_scriptrunner 的 QProcess 启动依赖它）；任何 6.8 系目录都不许在前面。
  $env:PATH = ((Join-Path $deps 'Library\bin'), (Join-Path $qgis 'bin'),
               (Join-Path $qtpdf 'bin'), $deps, $env:PATH) -join ';'
  # GDAL/PROJ 数据目录（与 vendored 路线同语义）：缺省时 GDAL 找不到
  # tms_NZTM2000.json、PROJ 报 CRS 无大地基准。
  $gdalData = Join-Path $deps 'Library\share\gdal'
  if (Test-Path $gdalData) { $env:GDAL_DATA = $gdalData }
  $projData = Join-Path $deps 'Library\share\proj'
  if (Test-Path $projData) { $env:PROJ_LIB = $projData }
  # 方向81：PALEO_PYTHON 显式指向 deps 根 python.exe（findBasePython 的最高
  # 优先）——不靠 PATH 发现（WindowsApps 商店桩会抢先）。build 时它作为
  # -DPALEO_TEST_PYTHON 进 cache，ctest 对每个测试注入同值并断言。用户已设
  # 的 PALEO_PYTHON 不覆盖。
  $depsPython = Join-Path $deps 'python.exe'
  if (-not $env:PALEO_PYTHON -and (Test-Path $depsPython)) { $env:PALEO_PYTHON = $depsPython }
  return $true
}

function Assert-NoEnvDebtRegression([string[]]$FailedTests, [string]$ManifestPath) {
  $fixedTests = @((Get-Content $ManifestPath -Raw -Encoding utf8 | ConvertFrom-Json).entries |
    Where-Object { $_.status -eq 'fixed' } | ForEach-Object { $_.test })
  $regressed = @($FailedTests | Where-Object { $fixedTests -contains $_ })
  if ($regressed.Count -gt 0) {
    throw ("CTest failed (fixed environment debt regressed; first-run evidence retained): {0}" -f
      ($regressed -join ', '))
  }
}

function Invoke-EnsureQgisResources([switch]$Auto) {
  # 方向81 srs.db 族：localdeps 的 QGIS 前缀只有 bin/include/lib，缺
  # resources/srs.db（tst_runtime / tst_boot 断言）。按 vendor/deb-closure.lock
  # 同源取 qgis-providers-common + qgis-common（_all 包）解出 resources/**，
  # srs.db = cp srs-template.db（Debian postinst 同义）。已齐备即零动作。
  # -Auto（test/selfcheck 自动调用）：失败只告警不中断，红留给测试本身。
  $prefix = $env:QGIS_PREFIX_PATH
  if (-not $prefix) {
    if ($Auto) { return }
    throw 'ensure-resources: 未探测到 QGIS 前缀（vendored/localdeps 都不在）'
  }
  if (Test-Path (Join-Path $prefix 'resources\srs.db')) {
    if (-not $Auto) { Write-Host ("  OK  srs.db -> {0}" -f (Join-Path $prefix 'resources\srs.db')) }
    return
  }
  $py = if ($env:PALEO_PYTHON) { $env:PALEO_PYTHON } else { 'python' }
  $rc = 1
  try {
    & $py (Join-Path $Root 'tools\ensure_qgis_resources.py') --prefix $prefix --layout win
    $rc = $LASTEXITCODE
  } catch { Write-Warning "ensure-resources: 无法运行 $py（$_）" }
  if ($rc -ne 0) {
    $msg = "ensure-resources failed（$py tools\ensure_qgis_resources.py --prefix $prefix）"
    if ($Auto) { Write-Warning $msg } else { throw $msg }
  }
}

function Enter-DependencyEnvironment {
  # vendored 优先（CI 与本地 vendor 装机一致）；否则本机 localdeps；都缺则
  # 保持系统兜底（$script:DepsRoute 留空，build 走原 -DQGIS_PREFIX=osgeo4w
  # 报错语义）。
  if (Enter-VendorEnvironment) { return }
  if (Enter-LocalDepsEnvironment) { return }
  $script:DepsRoute = 'system'
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
    if (-not $pin -or -not $pin.packages -or -not $pin.closure) {
      throw 'OSGeo4W pins incomplete in vendor/manifest.json（需 packages+closure）——跑 python3 tools/pin_osgeo4w.py 生成'
    }
    # #231：包集钉版（packages=直接包 / closure=requires 全闭包快照）。已装的
    # 安装树与钉版一致时跳过 setup（CI 缓存命中场景，零网络零漂移）；不一致
    # 才跑安装器，装后逐包校验——上游任何发版不再静默流进，漂移如实打红，
    # 刷新 = python3 tools/pin_osgeo4w.py 提交（同 deb-closure.lock 口径）。
    $osgeo = Join-Path $Vendor 'osgeo4w'
    $cache = Join-Path $Vendor 'cache\osgeo4w'
    New-Item -ItemType Directory -Force $osgeo, $cache | Out-Null
    $installedDb = Join-Path $osgeo 'etc\setup\installed.db'

    function Read-InstalledDbMap([string]$path) {
      # installed.db：首行「INSTALLED.DB 2」是格式头（不是包）；其余每行
      # 「名称 名称-版本.tar.bz2 0」——第二列是包文件名，剥掉「名称-」前缀与
      # .tar.* 后缀才是 setup.ini/manifest 的 version 口径（方向 81：旧实现把
      # 文件名当版本、把格式头当包，114 包全报「漂移」+「未钉版包 INSTALLED.DB」，
      # Windows CI 在 Vendor 步必红）。同口径参考实现与 selftest 见
      # tools/pin_osgeo4w.py installed_db_map。返回 @{名称=版本}，空/缺返回 $null。
      if (-not (Test-Path $path)) { return $null }
      $map = @{}
      foreach ($line in (Get-Content $path -Encoding utf8)) {
        $f = @($line.Trim() -split '\s+')
        if ($f.Count -lt 2 -or -not $f[0] -or $f[0] -eq 'INSTALLED.DB') { continue }
        $ver = $f[1] -replace '\.tar\.(bz2|xz|gz|zst|lz4)$', ''
        $prefix = $f[0] + '-'
        if ($ver.StartsWith($prefix, [System.StringComparison]::Ordinal)) { $ver = $ver.Substring($prefix.Length) }
        $map[$f[0]] = $ver
      }
      if ($map.Count -eq 0) { return $null }
      return $map
    }

    function Compare-PinnedClosure($installed, $closure) {
      # 双向比对：装了的必须钉、钉了的必须装且版本一致。返回差异描述列表。
      $diffs = @()
      foreach ($p in $closure.PSObject.Properties) {
        $v = $null
        if ($installed.ContainsKey($p.Name)) { $v = $installed[$p.Name] }
        if ($null -eq $v) { $diffs += "缺包 $($p.Name)（钉 $($p.Value)）" }
        elseif ($v -ne $p.Value) { $diffs += "版本漂移 $($p.Name)：装 $v / 钉 $($p.Value)" }
      }
      foreach ($k in $installed.Keys) {
        if (-not $closure.PSObject.Properties.Name.Contains($k)) {
          $diffs += "未钉版包装入 $k $($installed[$k])"
        }
      }
      return $diffs
    }

    $installed = Read-InstalledDbMap $installedDb
    $closureMatches = $false
    if ($null -ne $installed -and $null -ne $pin.closure) {
      $closureMatches = (@(Compare-PinnedClosure $installed $pin.closure)).Count -eq 0
    }

    if ($closureMatches) {
      Write-Host "  OSGeo4W 安装树与 manifest 钉版一致（$(@($pin.closure.PSObject.Properties).Count) 包）——跳过 setup"
    } else {
      $setup = Join-Path $Vendor 'osgeo4w-setup.exe'
      Invoke-WebRequest -Uri $pin.installer_url -OutFile $setup
      $actual = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLowerInvariant()
      if ($actual -ne $pin.installer_sha256) { throw "OSGeo4W installer SHA256 mismatch: $actual" }
      $setupArgs = @('-q', '-A', '-k', '-n', '-N', '-d', '-O', '-s', $pin.site,
                     '-R', ('"' + $osgeo + '"'), '-l', ('"' + $cache + '"'),
                     '-P', (($pin.packages.PSObject.Properties.Name) -join ','))
      # The installer detaches when invoked directly; wait for the entire setup
      # process tree before inspecting installed.db or configuring CMake.
      $setupProcess = Start-Process -FilePath $setup -ArgumentList $setupArgs -Wait -PassThru
      if ($setupProcess.ExitCode -ne 0) { throw "OSGeo4W setup failed: $($setupProcess.ExitCode)" }
      # 装后校验（#231）：漂移不再静默——逐包比对闭包钉版，不一致即红并给
      # 出机械刷新路径。
      $installed = Read-InstalledDbMap $installedDb
      if ($null -eq $installed) { throw "OSGeo4W installed.db 缺失或为空：$installedDb" }
      $diffs = @(Compare-PinnedClosure $installed $pin.closure)
      if ($diffs.Count -gt 0) {
        Write-Host "::error::OSGeo4W 包集与 manifest 钉版不符（上游已发版或钉版过期）："
        foreach ($d in $diffs) { Write-Host "::error::  $d" }
        throw 'OSGeo4W pin drift — 跑 python3 tools/pin_osgeo4w.py 刷新 vendor/manifest.json 并提交'
      }
    }
    # 家族闸（方向 71 单一来源 + #231 精确钉版双口径）：qgis_family 正则从
    # manifest 派生，不硬编码第二份 "4.2"；精确版本由上面的闭包比对覆盖
    # （装了的必须钉、钉了的必须装且版本一致——比家族闸更严，二者并存是纵深）。
    if ($pin.qgis_family -notmatch '^(\d+\.\d+)(\.x)?$') {
      throw "unparseable qgis_family in vendor/manifest.json: '$($pin.qgis_family)' (expect e.g. '4.2.x')"
    }
    $fam = [regex]::Escape($Matches[1])
    if (-not (Test-Path $installedDb) -or
        -not (Select-String -Path $installedDb -Pattern ('^qgis\s+qgis-' + $fam + '\.') -Quiet) -or
        -not (Select-String -Path $installedDb -Pattern ('^qgis-devel\s+qgis-devel-' + $fam + '\.') -Quiet)) {
      throw "OSGeo4W did not install QGIS and development headers from the $($pin.qgis_family) family"
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

    # LibreOffice（方向 94）：legacy 转换 API 的 Windows vendored 兜底。
    # 与 Linux vendor/bootstrap.sh 同语义（存在即跳过）；下载/校验/msiexec
    # 解包/剪裁/完整性归一单一来源在 vendor/fetch-libreoffice.sh（bash——
    # CI 与开发机都有 Git Bash；脚本内 OSTYPE 分派 Windows 段）。
    $loSoffice = Join-Path $Vendor 'libreoffice\program\soffice.exe'
    if (-not (Test-Path $loSoffice)) {
      $bash = Get-Command bash -ErrorAction SilentlyContinue
      if (-not $bash) { throw 'vendored LibreOffice 需要bash（Git Bash）跑 vendor/fetch-libreoffice.sh' }
      Write-Host "Fetching vendored LibreOffice (manifest deps.libreoffice_win)..."
      & $bash.Source (Join-Path $Vendor 'fetch-libreoffice.sh')
      if ($LASTEXITCODE -ne 0) { throw "fetch-libreoffice.sh failed (exit $LASTEXITCODE)" }
      if (-not (Test-Path $loSoffice)) { throw "fetch-libreoffice.sh 完成但 $loSoffice 缺失" }
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
    Enter-DependencyEnvironment
    # localdeps：configure 显式钉统一链四元组（cache 粘住，防无 env 的
    # shell 重配时静默丢前缀——与 QGIS_PREFIX 进 cache 同一纪律）。
    if ($script:DepsRoute -eq 'localdeps') {
      cmake -S $Root -B $Build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
        "-DCMAKE_PREFIX_PATH=$env:CMAKE_PREFIX_PATH" `
        "-DQT_ADDITIONAL_PACKAGES_PREFIX_PATH=$env:QT_ADDITIONAL_PACKAGES_PREFIX_PATH" `
        "-DQGIS_PREFIX=$script:LocalQgis" `
        "-DQSCINTILLA_PREFIX=$env:QSCINTILLA_PREFIX_PATH" `
        "-DPALEO_TEST_PYTHON=$env:PALEO_PYTHON"
    } else {
      cmake -S $Root -B $Build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DQGIS_PREFIX=$(Join-Path $Vendor 'osgeo4w')"
    }
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    # CI 上 -k 0：ninja 不在第一个错误处停，一轮暴露全部 MSVC 编译错误（#134）。
    # #230：AGENTS.md「构建/测试一律 -j8 以内」——min(核数, 8)，PALEO_JOBS 可调低。
    $jobs = [Math]::Min([Environment]::ProcessorCount, 8)
    if ($env:PALEO_JOBS -match '^\d+$') { $jobs = [Math]::Max(1, [Math]::Min([int]$env:PALEO_JOBS, 8)) }
    if ($env:CI) { cmake --build $Build --parallel $jobs -- -k 0 } else { cmake --build $Build --parallel $jobs }
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
    Enter-DependencyEnvironment
    $env:QT_QPA_PLATFORM = 'offscreen'
    # 方向 72：本机沙箱策略对「镜像位于仓库树内的进程」实施文件监狱——只能
    # 写树内路径。%TEMP% 默认在树外 → QTemporaryDir/QFile 全线「拒绝访问」
    # （历代 Windows 环境红的真身）。localdeps 路线下把 TEMP/TMP 重定向到
    # build 内一层浅目录（比 CMakeLists 避让的 ctest-home 深路径短）。
    # vendored/CI 路线无此策略，保持原样。
    if ($script:DepsRoute -eq 'localdeps') {
      $treeTmp = Join-Path $Build 'paleo-tmp'
      New-Item -ItemType Directory -Force $treeTmp | Out-Null
      $env:TEMP = $treeTmp; $env:TMP = $treeTmp
      # 同一监狱的第二个落点：SgyIndexCache 在 Windows 解析到 %LOCALAPPDATA%
      # （树外）→「Cannot create cache file」（tst_seismic_* 家族）。其解析链
      # 的最高优先覆盖是 SEISMIC_INDEX_CACHE_DIR——指到树内。
      $env:SEISMIC_INDEX_CACHE_DIR = Join-Path $treeTmp 'seismic-index'
      New-Item -ItemType Directory -Force $env:SEISMIC_INDEX_CACHE_DIR | Out-Null
      Invoke-EnsureQgisResources -Auto
    }
    # Python 门禁脚本在 Windows 默认 cp1252 下读写含中文的源码/输出会抛
    # UnicodeEncodeError（ui_invariants_selftest）——统一 UTF-8 模式。
    $env:PYTHONUTF8 = '1'
    $logDir = Join-Path $Vendor 'logs'
    New-Item -ItemType Directory -Force $logDir | Out-Null
    $firstRunDir = Join-Path $Build 'Testing/qtest-first-run'
    New-Item -ItemType Directory -Force $firstRunDir | Out-Null
    $junit = Join-Path $Build 'Testing/ctest-junit.xml'
    # 本轮启动失败时不得拿上一轮预算失败输出或 JUnit 当豁免证据。
    Remove-Item (Join-Path $firstRunDir '*.txt') -Force -ErrorAction SilentlyContinue
    Remove-Item $junit -Force -ErrorAction SilentlyContinue
    $log = Join-Path $logDir 'ctest.log'
    $ctestExtra = @()
    if ($env:PALEO_CTEST_ARGS) { $ctestExtra = $env:PALEO_CTEST_ARGS.Trim() -split '\s+' }
    ctest --test-dir $Build --output-on-failure --output-junit $junit @ctestExtra 2>&1 | Tee-Object -FilePath $log
    $ctestExit = $LASTEXITCODE
    if ($ctestExit -ne 0) {
      # ctest 的 --output-on-failure 在 Windows runner 上回收不到子进程
      # 输出；失败测试逐个直跑，QtTest 的 FAIL/Loc 行直落日志与控制台。
      # 与 Linux gate 对齐：崩溃/abort 不按一次性环境抖动放行。
      $crashPattern = '\*\*\*Exception|\*\*\*Timeout|Subprocess aborted|Child aborted|Subprocess killed|Illegal|SegFault'
      if (Select-String -Path $log -Pattern $crashPattern -Quiet) {
        throw 'CTest failed (crash-class failure)'
      }
      $names = @( & ctest --test-dir $Build --rerun-failed -N @ctestExtra 2>$null |
        ForEach-Object { if ($_ -match 'Test\s+#\d+:\s+(\S+)') { $Matches[1] } } )
      # 方向 81：环境债红名单（tools/env_redset.json）先分类——豁免项（只限
      # 名单 routes 内的路线、且每条 FAIL! 命中预算断言正则）放行；已修项再红
      # / 名单外红照旧走下面的直跑复核。检查器不可用时保持原逻辑（全量直跑）。
      $py = if ($env:PALEO_PYTHON) { $env:PALEO_PYTHON } else { 'python' }
      $unexpectedFile = Join-Path $Build 'Testing/env-redset-unexpected.txt'
      Remove-Item $unexpectedFile -Force -ErrorAction SilentlyContinue
      $redsetExit = 1
      try {
        & $py (Join-Path $Root 'tools/check_env_redset.py') --build $Build --junit $junit `
          --route $script:DepsRoute --emit-unexpected $unexpectedFile 2>&1 | Tee-Object -FilePath $log -Append
        $redsetExit = $LASTEXITCODE
      } catch { "  (env_redset 检查器不可用：$_)" | Tee-Object -FilePath $log -Append }
      # 即使 Python 检查器不可用，已修项的首轮红也不能被直跑绿掩盖。
      Assert-NoEnvDebtRegression $names (Join-Path $Root 'tools/env_redset.json')
      if (Test-Path $unexpectedFile) {
        if ($redsetExit -eq 0) { Write-Host '  env_redset: 全部红均为名单内豁免——放行'; return }
        $keep = @(Get-Content $unexpectedFile | Where-Object { $_ })
        if ($keep.Count -gt 0) { $names = @($names | Where-Object { $keep -contains $_ }) }
      }
      $rerunFailed = $false
      $unhandled = $names.Count -eq 0
      foreach ($n in $names) {
        # 有些 ctest 项不是可执行文件（layering 是 python 脚本）；未处理的
        # 失败项必须保留红，不能把它误当成可重跑通过。
        $exe = Join-Path $Build "$n.exe"
        if (-not (Test-Path $exe)) { $unhandled = $true; continue }
        "=== $n (direct run) ===" | Tee-Object -FilePath $log -Append
        # QtTest 在无控制台的 Windows 上把结果走 OutputDebugString，
        # stdout 重定向收不到——用 -o 落文件再回放（崩溃栈仍走 stderr）。
        $qtout = Join-Path $logDir "$n.qtout.txt"
        # 直跑复现 ctest 环境（该测试的 ENVIRONMENT 属性 + 工作目录=构建目录）。
        $saved = @{}
        try {
          $json = & ctest --test-dir $Build -R "^$([regex]::Escape($n))`$" --show-only=json-v1 2>$null | Out-String | ConvertFrom-Json
          foreach ($t in @($json.tests)) {
            foreach ($p in @($t.properties)) {
              if ($p.name -ne 'ENVIRONMENT') { continue }
              foreach ($kv in @($p.value)) {
                $i = $kv.IndexOf('=')
                if ($i -le 0) { continue }
                $k = $kv.Substring(0, $i)
                $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process')
                [Environment]::SetEnvironmentVariable($k, $kv.Substring($i + 1), 'Process')
              }
            }
          }
        } catch { "  (ctest env 解析失败：$_)" | Tee-Object -FilePath $log -Append }
        $rerunExit = 1
        Push-Location $Build
        try {
          & $exe -o "$qtout,txt" 2>&1 | Tee-Object -FilePath $log -Append
          $rerunExit = $LASTEXITCODE
        } finally {
          Pop-Location
          foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
        }
        if ($rerunExit -ne 0) { $rerunFailed = $true }
        if (Test-Path $qtout) {
          Get-Content $qtout | Tee-Object -FilePath $log -Append
          Remove-Item $qtout -Force
        }
      }
      if ($rerunFailed -or $unhandled) { throw 'CTest failed' }
    }
  }
  'selfcheck' {
    Enter-DependencyEnvironment
    $env:QT_QPA_PLATFORM = 'offscreen'
    if ($script:DepsRoute -eq 'localdeps') {
      # 同 test：树内进程的 TEMP 重定向（selfcheck 也用 QTemporaryDir）。
      $treeTmp = Join-Path $Build 'paleo-tmp'
      New-Item -ItemType Directory -Force $treeTmp | Out-Null
      $env:TEMP = $treeTmp; $env:TMP = $treeTmp
      Invoke-EnsureQgisResources -Auto
    }
    & (Join-Path $Build 'paleo_selfcheck.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Selfcheck failed' }
  }
  'ensure-resources' {
    Enter-DependencyEnvironment
    Invoke-EnsureQgisResources
  }
  'checkenv' {
    # 方向 72 防回归：编译链（CMakeCache 的 Qt6Core_DIR）vs 运行链（PATH
    # 解析到的 Qt6Core.dll）版本比对；不一致即非零退出。详见 BUILDING.md。
    Enter-DependencyEnvironment
    & (Join-Path $Root 'tools\check_qt_env.ps1') -BuildDir $Build
    if ($LASTEXITCODE -ne 0) { throw 'Qt environment check failed' }
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
  default { throw "unknown verb '$Verb' — bootstrap [fetch-only]|build|test|selfcheck|checkenv|ensure-resources|clean-vendor" }
}
