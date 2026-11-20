# paleo-dev.ps1 — Windows sibling of ./paleo-dev (§44.1).
# Verbs: bootstrap [fetch-only] | bootstrap-native | build | test | selfcheck | checkenv | ensure-resources | clean-vendor <dep> | diagdeps <test>
#   bootstrap fetch-only : 只取依赖（OSGeo4W + ORT），不编译不自检——CI 用它让
#                          编译错误落在 Build 步骤而不是 Vendor 步骤（#134）。
#   build                : $env:CI 已设时 ninja -k 0，一次暴露全部编译错误。
#   test                 : $env:PALEO_CTEST_ARGS 透传给 ctest（如 "-LE perf"）。
#   diagdeps <test>      : #872 探针失败诊断——列 build 下 <test>.exe 的
#                          dumpbin /dependents，并在测试同款 PATH 下逐个
#                          定位（MISSING 即加载器 0xC0000135 的根因候选）。
#   checkenv             : Qt 编译/运行链一致性自检（方向 72 防回归）。
#   ensure-resources     : validate or restore the locked QGIS resource database.

param(
  [Parameter(Mandatory=$true, Position=0)][string]$Verb,
  [Parameter(Position=1)][string]$Arg
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Root 'build'
$Vendor = Join-Path $Root 'vendor'

function Enter-MsvcEnvironment {
  $env:VSLANG = '1033' # 有英文语言包时优先用英文；实际依赖前缀仍须验证。
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
  $env:VSLANG = '1033'
}

function Repair-NinjaMsvcDependencyPrefix([string]$BuildDirectory) {
  $script:MsvcDependencyPrefixNeedsClean = $false
  # CMake 3.29 的本地化探测可能把 UTF-8/系统代码页解错。只设 VSLANG
  # 不足：没有安装英文 clui.dll 时 cl 仍输出中文。探测实际输出字节。
  $rules = Join-Path $BuildDirectory 'CMakeFiles/rules.ninja'
  if (-not (Test-Path $rules)) { return }
  $source = Join-Path $BuildDirectory 'paleo-dependency-probe.cpp'
  $header = Join-Path $BuildDirectory 'paleo-dependency-probe.h'
  $object = Join-Path $BuildDirectory 'paleo-dependency-probe.obj'
  [IO.File]::WriteAllText($header, '#pragma once', [Text.Encoding]::ASCII)
  [IO.File]::WriteAllText($source, '#include "paleo-dependency-probe.h"', [Text.Encoding]::ASCII)
  $bytes = [Text.Encoding]::GetEncoding(28591) # 一字节映射，不改写原始依赖前缀。
  $probe = New-Object Diagnostics.Process
  $probe.StartInfo.FileName = (Get-Command cl.exe -ErrorAction Stop).Source
  $probe.StartInfo.Arguments = '/nologo /c /showIncludes /Fo"' + $object + '" "' + $source + '"'
  $probe.StartInfo.UseShellExecute = $false
  $probe.StartInfo.CreateNoWindow = $true
  $probe.StartInfo.RedirectStandardOutput = $true
  $probe.StartInfo.RedirectStandardError = $true
  $probe.StartInfo.StandardOutputEncoding = $bytes
  $null = $probe.Start()
  $output = $probe.StandardOutput.ReadToEnd()
  $errorText = $probe.StandardError.ReadToEnd()
  $probe.WaitForExit()
  if ($probe.ExitCode -ne 0) { throw "MSVC dependency probe failed: $errorText" }
  $prefix = [regex]::Match($output, '(?m)^([^\r\n]*?)[A-Za-z]:[\\/][^\r\n]*paleo-dependency-probe\.h\r?$')
  if (-not $prefix.Success) { throw 'MSVC dependency probe did not report its header' }
  # cl 直启和 Ninja 子进程可能使用不同控制台代码页，不能只相信显示的文字。
  # 用真实 Ninja 编译一张极小夹具，并要求 deps 数据库确实记录该头文件。
  $raw = $prefix.Groups[1].Value
  $systemEncoding = [Text.Encoding]::GetEncoding([Globalization.CultureInfo]::CurrentCulture.TextInfo.ANSICodePage)
  $candidates = @($raw,
    $bytes.GetString([Text.Encoding]::UTF8.GetBytes($systemEncoding.GetString($bytes.GetBytes($raw)))),
    $bytes.GetString($systemEncoding.GetBytes([Text.Encoding]::UTF8.GetString($bytes.GetBytes($raw))))) | Select-Object -Unique
  $verified = $null
  Push-Location $BuildDirectory
  try {
    foreach ($candidate in $candidates) {
      $fixture = "msvc_deps_prefix = $candidate`nrule compile`n  command = cl.exe /nologo /c /showIncludes /Fopaleo-dependency-probe.obj paleo-dependency-probe.cpp`n  deps = msvc`nbuild paleo-dependency-probe.obj: compile paleo-dependency-probe.cpp`n"
      [IO.File]::WriteAllBytes((Join-Path $BuildDirectory 'paleo-dependency-probe.ninja'), $bytes.GetBytes($fixture))
      & ninja -f paleo-dependency-probe.ninja -t clean | Out-Null
      & cmake --build $BuildDirectory --target paleo-dependency-probe.obj -- -f paleo-dependency-probe.ninja | Out-Null
      if ($LASTEXITCODE -ne 0) { throw 'Ninja dependency probe compile failed' }
      $dependencies = & ninja -f paleo-dependency-probe.ninja -t deps paleo-dependency-probe.obj
      if (($dependencies -join "`n") -match 'paleo-dependency-probe\.h') { $verified = $candidate; break }
    }
  } finally { Pop-Location }
  if ($null -eq $verified) { throw 'Ninja failed to record the dependency probe header' }
  $stamp = Join-Path $BuildDirectory '.paleo-msvc-dependency-prefix'
  $identity = [Convert]::ToBase64String($bytes.GetBytes($verified))
  $script:MsvcDependencyPrefixNeedsClean = -not (Test-Path $stamp) -or
      [IO.File]::ReadAllText($stamp) -ne $identity
  [IO.File]::WriteAllText($stamp, $identity, [Text.Encoding]::ASCII)
  $original = $bytes.GetString([IO.File]::ReadAllBytes($rules))
  $replacement = 'msvc_deps_prefix = ' + $verified
  $updated = [regex]::Replace($original, '(?m)^msvc_deps_prefix =[^\r\n]*', $replacement)
  if ($original -eq $updated) { return }
  [IO.File]::WriteAllBytes($rules, $bytes.GetBytes($updated))
  Write-Host 'Ninja MSVC include prefix aligned with compiler output bytes.'
}

function Get-Osgeo4wLockHash {
  $lock = Join-Path $Vendor 'osgeo4w-offline.lock.json'
  return (Get-FileHash -LiteralPath $lock -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-Osgeo4wLockStamp([string]$Prefix) {
  $stamp = Join-Path $Prefix '.paleo-osgeo4w-lock.sha256'
  if (!(Test-Path -LiteralPath $stamp) -or (Get-Content -LiteralPath $stamp -Raw).Trim() -ne (Get-Osgeo4wLockHash)) {
    throw "OSGeo4W prefix does not match the offline lock: $Prefix. Move it aside and run bootstrap again."
  }
}

function Install-LockedOsgeo4w([string]$Prefix, [string]$Cache, [string[]]$Packages, [switch]$Native) {
  $lock = [IO.File]::Open("$Prefix.bootstrap.lock", 'OpenOrCreate', 'ReadWrite', 'None')
  try { Install-Osgeo4wPayload @PSBoundParameters } finally { $lock.Dispose() }
}

function Install-Osgeo4wPayload([string]$Prefix, [string]$Cache, [string[]]$Packages, [switch]$Native) {
  $database = Join-Path $Prefix 'etc/setup/installed.db'
  $python = if ($env:PALEO_PYTHON) { $env:PALEO_PYTHON } else { 'python' }
  $tool = Join-Path $Root 'tools/osgeo4w_offline.py'
  $profile = @()
  if ($Native) { $profile += '--native' }
  & "$Vendor/build-osgeo4w-zstd.ps1" -Prefix $Prefix -PrepareOnly
  if (!$Native) {
    & "$Vendor/build-osgeo4w-opencl.ps1" -Prefix $Prefix -PrepareOnly
    & $python -I (Join-Path $Vendor 'fetch-onnxruntime.py')
    if ($LASTEXITCODE -ne 0) { throw 'ONNX Runtime bootstrap failed' }
  }
  $prepare = @('prepare', '--cache', $Cache) + $profile
  if ($env:PALEO_VENDOR_OFFLINE -eq '1') { $prepare += '--offline' }
  & $python -B $tool @prepare
  if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare the locked local OSGeo4W repository' }
  $fresh = !(Test-Path -LiteralPath $Prefix)
  $receipt = Join-Path $Root 'tools/osgeo4w_prefix.py'
  if (!$fresh) {
    Assert-Osgeo4wLockStamp $Prefix
    & $python -B $receipt verify $Prefix @profile
    if ($LASTEXITCODE -ne 0) { throw 'Prefix integrity check failed. Move it aside and bootstrap again.' }
  } else {
    $mirror = Join-Path $Cache 'archives'
    $setupArgs = @('-q','-A','-k','-n','-N','-d','-L',
                   '-l', ('"' + $mirror + '"'), '-R', ('"' + $Prefix + '"'),
                   '-P', ($Packages -join ','))
    $process = Start-Process -FilePath (Join-Path $Cache 'osgeo4w-setup.exe') `
      -ArgumentList $setupArgs -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0) {
      throw "OSGeo4W local setup failed ($($process.ExitCode)); see logs under $Prefix/var/log"
    }
  }
  & $python -B $tool check-installed $database @profile
  if ($LASTEXITCODE -ne 0) { throw 'Installed OSGeo4W prefix does not match the lock' }
  & "$Vendor/build-osgeo4w-zstd.ps1" -Prefix $Prefix
  if (!$Native) { & "$Vendor/build-osgeo4w-opencl.ps1" -Prefix $Prefix }
  if ($fresh) {
    & $python -B $receipt record $Prefix @profile
    if ($LASTEXITCODE -ne 0) { throw 'Could not record the new OSGeo4W prefix' }
  }
  [IO.File]::WriteAllText((Join-Path $Prefix '.paleo-osgeo4w-lock.sha256'),
                        (Get-Osgeo4wLockHash) + [Environment]::NewLine)
}

function Enter-VendorEnvironment {
  $osgeo = Join-Path $Vendor 'osgeo4w'
  if (-not (Test-Path -LiteralPath $osgeo)) { return $false }
  Assert-Osgeo4wLockStamp $osgeo
  if (!(Test-Path -LiteralPath (Join-Path $osgeo 'apps/qgis/include/qgsapplication.h'))) {
    throw "Incomplete OSGeo4W prefix: $osgeo. Move it aside and run bootstrap again."
  }
  $env:QGIS_PREFIX_PATH = Join-Path $osgeo 'apps\qgis'
  $qt = Join-Path $osgeo 'apps\qt6'
  $env:CMAKE_PREFIX_PATH = $qt
  $env:QT_PLUGIN_PATH = Join-Path $qt 'plugins'
  $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qt 'plugins\platforms'
  $env:QTWEBENGINEPROCESS_PATH = Join-Path $qt 'bin\QtWebEngineProcess.exe'
  $env:QTWEBENGINE_RESOURCES_PATH = Join-Path $qt 'resources'
  $env:QTWEBENGINE_LOCALES_PATH = Join-Path $qt 'translations\qtwebengine_locales'
  # OSGeo4W 的 bin/python.exe 需要其启动脚本设置 PYTHONHOME；CI 测试和源码
  # 闸门使用原有宿主解释器的绝对路径，不依赖 OSGeo4W Python 环境。
  if (-not $env:PALEO_PYTHON) {
    $hostPython = Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hostPython) { $env:PALEO_PYTHON = $hostPython.Source }
  }
  # vendored onnxruntime 必须排在 OSGeo4W bin 之前——OSGeo4W 自带
  # onnxruntime 1.17.1，我们的头文件是 1.30（ABI 30）；PATH 顺序错了
  # 会加载旧 DLL，会话构造直接段错误（tst_onnx 实锤）。
  $ortLib = Join-Path $Vendor 'onnxruntime\lib'
  $env:PATH = ($ortLib, (Join-Path $osgeo 'bin'), (Join-Path $osgeo 'apps\qgis\bin'),
               (Join-Path $osgeo 'apps\qt6\bin'), $env:PATH) -join ';'
  # GDAL/PROJ 数据目录：缺省时 GDAL 找不到 tms_NZTM2000.json、PROJ
  # 报 CRS 无大地基准（影响坐标变换类用例）。
  $gdalData = Join-Path $osgeo 'apps\gdal\share\gdal'
  $env:GDAL_DATA = $gdalData
  $env:GDAL_DRIVER_PATH = Join-Path $osgeo 'apps\gdal\lib\gdalplugins'
  $projData = Join-Path $osgeo 'share\proj'
  $env:PROJ_LIB = $projData
  $env:PROJ_DATA = $projData
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

function Invoke-EnsureQgisResources {
  $prefix = $env:QGIS_PREFIX_PATH
  if (-not $prefix) { throw 'A locked QGIS prefix is required. Run ./paleo-dev.ps1 bootstrap fetch-only.' }
  $database = Join-Path $prefix 'resources\srs.db'
  if (Test-Path -LiteralPath $database) {
    Write-Host "  OK  srs.db -> $database"
    return
  }
  $py = if ($env:PALEO_PYTHON) { $env:PALEO_PYTHON } else { 'python' }
  & $py (Join-Path $Root 'tools\ensure_qgis_resources.py') --prefix $prefix --layout win
  if ($LASTEXITCODE -ne 0) { throw 'QGIS resource recovery failed. Reprepare the locked prefix.' }
}

function Enter-DependencyEnvironment {
  if (-not (Enter-VendorEnvironment)) {
    throw 'Locked OSGeo4W prefix absent. Run ./paleo-dev.ps1 bootstrap fetch-only.'
  }
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
  'bootstrap-native' {
    Preflight
    Enter-MsvcEnvironment
    $pin = (Get-Content (Join-Path $Vendor 'manifest.json') -Raw | ConvertFrom-Json).deps.osgeo4w
    $prefix = Join-Path $Vendor 'osgeo4w-native'
    Install-LockedOsgeo4w -Prefix $prefix -Cache (Join-Path $Vendor 'cache/osgeo4w-native') `
      -Packages $pin.native_packages -Native
    Write-Host "Prepared native Qt and SDK closure: $prefix"
  }
  'bootstrap' {
    Preflight
    Write-Host "== vendor bootstrap (OSGeo4W route) =="
    $pin = (Get-Content (Join-Path $Vendor 'manifest.json') -Raw | ConvertFrom-Json).deps.osgeo4w
    if (-not $pin -or -not $pin.packages -or -not $pin.closure) {
      throw 'OSGeo4W pins incomplete in vendor/manifest.json（需 packages+closure）——跑 python3 tools/pin_osgeo4w.py 生成'
    }
    $osgeo = Join-Path $Vendor 'osgeo4w'
    $cache = Join-Path $Vendor 'cache/osgeo4w-offline'
    $installedDb = Join-Path $osgeo 'etc/setup/installed.db'
    $py = if ($env:PALEO_PYTHON) { $env:PALEO_PYTHON } else { 'python' }
    Install-LockedOsgeo4w -Prefix $osgeo -Cache $cache -Packages $pin.packages.PSObject.Properties.Name
    # Verify the resources and development layout before marking the prefix ready.
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
                           'apps\qt6\plugins\platforms\qoffscreen.dll',
                           'apps\qt6\plugins\sqldrivers\qsqlite.dll',
                           'apps\qt6\plugins\sqldrivers\qsqloci.dll',
                           'apps\qgis\resources\srs.db',
                           'apps\qgis\plugins\provider_spatialite.dll',
                           'share\proj\proj.db',
                           # qgscodeeditor.h（qgis-devel 头链）需要 QSci 头，
                           # 由 qscintilla-qt6-devel 提供在 Qt6 include 前缀下
                           'apps\Qt6\include\Qsci\qsciapis.h',
                           'apps\Qt6\lib\qscintilla2_qt6.lib',
                           'include\gdal.h', 'lib\gdal.lib',
                           'include\sqlite3.h')) {
      if (-not (Test-Path (Join-Path $osgeo $required))) { throw "OSGeo4W closure missing $required" }
    }

    Enter-VendorEnvironment
    if ($Arg -eq 'fetch-only') {
      Write-Host "== fetch-only: skip build/selfcheck (run 'build' then 'selfcheck') =="
    } else {
      $office = if ($env:PALEO_OFFICE_EDITOR) { $env:PALEO_OFFICE_EDITOR } else { Join-Path $Vendor 'ranuts-document' }
      $missingOffice = @(Get-Content -LiteralPath (Join-Path $Vendor 'office-editor-required.txt') | Where-Object {
        $asset = Get-Item -LiteralPath (Join-Path $office $_) -ErrorAction SilentlyContinue
        -not $asset -or $asset.PSIsContainer -or $asset.Length -eq 0
      })
      if ($missingOffice.Count) {
        if ($env:PALEO_OFFICE_EDITOR) { throw "PALEO_OFFICE_EDITOR 缺少组件文件：$($missingOffice -join ', ')" }
        $gitBash = Join-Path $env:ProgramFiles 'Git/bin/bash.exe'
        if (-not (Test-Path -LiteralPath $gitBash)) { throw 'Office 组件需要 Git Bash 或已就绪的 PALEO_OFFICE_EDITOR' }
        & $gitBash (Join-Path $Vendor 'fetch-ranuts-document.sh') --ensure
        if ($LASTEXITCODE -ne 0) { throw 'Office editor bootstrap failed' }
      }
      & $PSCommandPath build
      if ($LASTEXITCODE -ne 0) { throw 'Windows bootstrap build failed' }
      & $PSCommandPath selfcheck
      if ($LASTEXITCODE -ne 0) { throw 'Windows bootstrap selfcheck failed' }
    }
  }
  'build' {
    Enter-MsvcEnvironment
    Enter-DependencyEnvironment
    $buildType = if ($env:PALEO_CMAKE_BUILD_TYPE) { $env:PALEO_CMAKE_BUILD_TYPE } else { 'RelWithDebInfo' }
    $pythonArgs = @()
    if ($env:PALEO_PYTHON) {
      $pythonArgs += "-DPALEO_TEST_PYTHON=$env:PALEO_PYTHON"
      $pythonArgs += "-DPALEO_PYTHON3=$env:PALEO_PYTHON"
      $pythonArgs += "-DPython3_EXECUTABLE=$env:PALEO_PYTHON"
    }
    cmake -S $Root -B $Build -G Ninja "-DCMAKE_BUILD_TYPE=$buildType" "-DQGIS_PREFIX=$(Join-Path $Vendor 'osgeo4w')" @pythonArgs
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    Repair-NinjaMsvcDependencyPrefix $Build
    if ($script:MsvcDependencyPrefixNeedsClean) {
      # 首次修复不能沿用已经漏头依赖的对象；后续 configure 只补前缀，不反复清库。
      cmake --build $Build --target clean
      if ($LASTEXITCODE -ne 0) {
        Remove-Item -LiteralPath (Join-Path $Build '.paleo-msvc-dependency-prefix') -ErrorAction SilentlyContinue
        throw 'Clean after MSVC dependency-prefix repair failed'
      }
    }
    # CI 上 -k 0：ninja 不在第一个错误处停，一轮暴露全部 MSVC 编译错误（#134）。
    # #230：AGENTS.md「构建/测试一律 -j8 以内」——min(核数, 8)，PALEO_JOBS 可调低。
    $jobs = [Math]::Min([Environment]::ProcessorCount, 8)
    if ($env:PALEO_JOBS -match '^\d+$') { $jobs = [Math]::Max(1, [Math]::Min([int]$env:PALEO_JOBS, 8)) }
    if ($env:CI) { cmake --build $Build --parallel $jobs -- -k 0 } else { cmake --build $Build --parallel $jobs }
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed' }
  }
  'test' {
    Enter-DependencyEnvironment
    # #872：CI 下在 ctest 的父进程内设 SetErrorMode（可继承给测试子进程）：
    # SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX，
    # 缺 DLL/崩溃即带退出码返回而非弹模态框吊到 TIMEOUT。此前只在 ci.yml
    # 的 pwsh 步里设，本地/其他入口调 test 时缺这一层；这里收为单一真源
    # （ci.yml 保留同值冗余兜底）。本机交互调试要 JIT 弹窗时不设 CI 即可。
    if ($env:CI) {
      Add-Type -Namespace PaleoDev -Name Native -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);'
      [void][PaleoDev.Native]::SetErrorMode(0x8003)
    }
    $env:QT_QPA_PLATFORM = 'offscreen'
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
      # #760：崩溃 markers 与「只扫 ctest 结果行」的口径单一真源在
      # tools/ci-gate-rules.json，与 Linux tools/ci_ctest_gate.sh 同读一份。
      # python 不可用时退回内联副本；副本与 JSON 的一致性由 ctest 项 ci_gate_rules
      # 每轮看守（防两端悄悄漂移）。扫描范围收敛到 ctest 结果行——此前扫整份日志，
      # 测试自身输出里出现 "Illegal" 字样即假红。
      $py = if ($env:PALEO_PYTHON) { $env:PALEO_PYTHON } else { 'python' }
      $crashPattern = ''
      try { $crashPattern = (& $py (Join-Path $Root 'tools/ci_gate_rules.py') --regex 2>$null | Select-Object -First 1) } catch { }
      if (-not $crashPattern) {
        $crashPattern = '\*\*\*Exception|\*\*\*Timeout|Subprocess aborted|Child aborted|Subprocess killed|Illegal|SegFault'
      }
      $resultLineRegex = ''
      try { $resultLineRegex = (& $py (Join-Path $Root 'tools/ci_gate_rules.py') --result-line-regex 2>$null | Select-Object -First 1) } catch { }
      if (-not $resultLineRegex) { $resultLineRegex = 'Test +#[0-9]+:' }
      $crashLines = @(Select-String -Path $log -Pattern $resultLineRegex |
        Where-Object { $_.Line -match $crashPattern })
      if ($crashLines.Count -gt 0) {
        $crashLines | ForEach-Object { $_.Line }
        throw 'CTest failed (crash-class failure)'
      }
      $names = @( & ctest --test-dir $Build --rerun-failed -N @ctestExtra 2>$null |
        ForEach-Object { if ($_ -match 'Test\s+#\d+:\s+(\S+)') { $Matches[1] } } )
      # 方向 81：环境债红名单（tools/env_redset.json）先分类——豁免项（只限
      # 名单 routes 内的路线、且每条 FAIL! 命中预算断言正则）放行；已修项再红
      # / 名单外红照旧走下面的直跑复核。检查器不可用时保持原逻辑（全量直跑）。
      # #501：接 --strict——STALE（exempt 项本轮通过 → 名单可收缩）不再静默：
      # 过渡档只警告（名单僵化提醒），PALEO_REDSET_STALE=strict 升红；STALE
      # 不影响豁免红自身的放行（keep 集剔除 STALE 条目）。
      # #760：keep 集（不放行集合）即直跑集，与 Linux ci_ctest_gate.sh 的
      # rerun_names 同口径——EXEMPT/DOCUMENTED 既不直跑也不入 flaky 台账。
      $unexpectedFile = Join-Path $Build 'Testing/env-redset-unexpected.txt'
      Remove-Item $unexpectedFile -Force -ErrorAction SilentlyContinue
      $redsetExit = 1
      $redsetText = @()
      try {
        $redsetText = @(& $py (Join-Path $Root 'tools/check_env_redset.py') --build $Build --junit $junit `
          --route vendored --strict --emit-unexpected $unexpectedFile 2>&1)
        $redsetText | Tee-Object -FilePath $log -Append
        $redsetExit = $LASTEXITCODE
      } catch { "  (env_redset 检查器不可用：$_)" | Tee-Object -FilePath $log -Append }
      $staleNames = @($redsetText | Where-Object { $_ -match '^STALE\s+(\S+):' } |
        ForEach-Object { $Matches[1] })
      if ($staleNames.Count -gt 0) {
        if ($env:PALEO_REDSET_STALE -eq 'strict') {
          throw "env_redset STALE: $($staleNames -join ', ') 本轮通过——名单该收缩（#501 strict 档）"
        }
        Write-Warning "env_redset STALE: $($staleNames -join ', ') 本轮通过，名单可收缩（过渡档只警告；PALEO_REDSET_STALE=strict 升红）"
      }
      # 即使 Python 检查器不可用，已修项的首轮红也不能被直跑绿掩盖。
      Assert-NoEnvDebtRegression $names (Join-Path $Root 'tools/env_redset.json')
      if (Test-Path $unexpectedFile) {
        # --strict 会把 STALE 条目也写进不放行文件：STALE 项本轮是通过的
        #（不在 $names 里），从 keep 集剔除后不改变豁免红的放行语义。
        $keep = @(Get-Content $unexpectedFile | Where-Object { $_ -and $staleNames -notcontains $_ })
        if ($keep.Count -eq 0) {
          Write-Host '  env_redset: 全部红均为名单内豁免——放行'; return
        }
        $names = @($names | Where-Object { $keep -contains $_ })
      }
      $rerunFailed = $false
      # #501：首轮红、直跑绿的项 = 本轮 flaky 放行集合，离开本块前必须留痕。
      $rerunPassed = @()
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
        # #765：该 ctest 项的 TIMEOUT 属性，用作下面直跑的单测超时（缺省 300s）。
        $timeoutSec = 0
        try {
          $json = & ctest --test-dir $Build -R "^$([regex]::Escape($n))`$" --show-only=json-v1 2>$null | Out-String | ConvertFrom-Json
          foreach ($t in @($json.tests)) {
            foreach ($p in @($t.properties)) {
              if ($p.name -eq 'TIMEOUT') { $timeoutSec = [int]$p.value }
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
          # #765：直跑必须带单测超时。此前 `& $exe | Tee-Object` 无超时，挂死的
          # 测试会让 windows 车道干等到 job 级 360 分钟才被杀。这里复用该 ctest
          # 项的 TIMEOUT 属性（缺省 300s，与 Linux ctest 同源），超时即杀进程树
          # 并按红记（挂死不按负载抖动放行，与 Linux ci_ctest_gate.sh 的崩溃判红
          # 同口径）。stdout/stderr 走异步读（ReadToEndAsync + Task.WaitAll）——
          # 管道缓冲被写满时同步读会与「进程阻塞在写」互锁，造成假超时。
          if ($timeoutSec -le 0) { $timeoutSec = 300 }
          $psi = New-Object System.Diagnostics.ProcessStartInfo
          $psi.FileName = $exe
          $psi.Arguments = '-o "' + $qtout + ',txt"'
          $psi.WorkingDirectory = $Build
          $psi.UseShellExecute = $false
          $psi.CreateNoWindow = $true
          $psi.RedirectStandardOutput = $true
          $psi.RedirectStandardError = $true
          $proc = [System.Diagnostics.Process]::Start($psi)
          $outTask = $proc.StandardOutput.ReadToEndAsync()
          $errTask = $proc.StandardError.ReadToEndAsync()
          if ($proc.WaitForExit($timeoutSec * 1000)) {
            $rerunExit = $proc.ExitCode
          } else {
            try { & taskkill /PID $proc.Id /T /F 2>&1 | Out-Null } catch { }
            $null = $proc.WaitForExit(30000)
            $rerunExit = 1
            ("=== {0} (direct run) TIMED OUT after {1}s — process tree killed, counted red (#765) ===" -f $n, $timeoutSec) |
              Tee-Object -FilePath $log -Append
          }
          # WaitForExit(ms) 不等异步输出读完，这里收尾（有界等待，防读线程卡死）。
          $stdoutText = ''; $stderrText = ''
          if ([System.Threading.Tasks.Task]::WaitAll(@($outTask, $errTask), 30000)) {
            $stdoutText = $outTask.Result
            $stderrText = $errTask.Result
          }
          if ($stderrText) {
            "--- stderr ---" | Tee-Object -FilePath $log -Append
            $stderrText | Tee-Object -FilePath $log -Append
          }
          if ($stdoutText) {
            "--- stdout ---" | Tee-Object -FilePath $log -Append
            $stdoutText | Tee-Object -FilePath $log -Append
          }
        } catch {
          $rerunExit = 1
          "  (direct run failed: $_)" | Tee-Object -FilePath $log -Append
        } finally {
          Pop-Location
          foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
        }
        if ($rerunExit -ne 0) { $rerunFailed = $true } else { $rerunPassed += $n }
        if (Test-Path $qtout) {
          Get-Content $qtout | Tee-Object -FilePath $log -Append
          Remove-Item $qtout -Force
        }
      }
      if ($rerunFailed -or $unhandled) { throw 'CTest failed' }
      # #757：直跑全通过时 LastTestsFailed.log 仍是**首轮**失败名单（直跑不经
      # ctest，不会更新它）——残留名单上传后被 triage 当最终失败集，已洗白的
      # flaky 被误判成真红、flaky 汇总反而漏掉它（同 tools/ci_ctest_gate.sh 口径）。
      if (-not $rerunFailed -and -not $unhandled) {
        Remove-Item (Join-Path $Build 'Testing/Temporary/LastTestsFailed.log') -Force -ErrorAction SilentlyContinue
      }
      # #501：直跑放行记账（与 Linux tools/ci_ctest_gate.sh 同路径）——持久
      # JSONL 台账（累计命中由 check_flaky_ledger.py 聚合点名）+ step
      # summary / 逐项 annotation / 构建目录台账（PR 评审面可见）。放行不再无痕。
      if ($rerunPassed.Count -gt 0) {
        $flakyLedger = Join-Path $Root 'tools/flaky-ledger.jsonl'
        $utcNow = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        foreach ($t in $rerunPassed) {
          Add-Content -Path $flakyLedger -Encoding utf8 -Value (
            '{"test":"{0}","utc":"{1}","event":"serial-rerun-pass","lane":"ps1-direct"}' -f $t, $utcNow)
        }
        $ledgerDir = Join-Path $Build 'Testing'
        New-Item -ItemType Directory -Force $ledgerDir | Out-Null
        Add-Content -Path (Join-Path $ledgerDir 'flaky-ledger.txt') -Encoding UTF8 -Value @(
          "=== $(Get-Date -Format o) run=$($env:GITHUB_RUN_ID)"
          $rerunPassed
        )
        if ($env:GITHUB_STEP_SUMMARY) {
          Add-Content -Path $env:GITHUB_STEP_SUMMARY -Encoding UTF8 -Value @(
            "## flaky 直跑放行（#501，$($rerunPassed.Count) 项）", "",
            "首轮失败、直跑通过——已放行但**须复审**（同一测试反复入账应升级为硬红）：", "",
            ($rerunPassed | ForEach-Object { "- ``$_``" })
          )
        }
        foreach ($t in $rerunPassed) {
          Write-Host "::warning title=flaky 放行::$t 首轮失败、直跑通过——已记入 flaky 台账"
        }
        Write-Warning "直跑放行 $($rerunPassed.Count) 项（已逐条记入 tools/flaky-ledger.jsonl；同一测试累计 ≥3 次将被点名）"
        try {
          $flakyArgs = @()
          if ($env:PALEO_FLAKY_STRICT -eq '1') { $flakyArgs = @('--strict') }
          & $py (Join-Path $Root 'tools/check_flaky_ledger.py') @flakyArgs
        } catch { "  (flaky 台账检查器不可用：$_)" | Tee-Object -FilePath $log -Append }
      }
    }
  }
  'diagdeps' {
    # #872：探针（tst_runtime 等首个 C++ 测试）判红后的依赖清单。只读诊断，
    # 不改构建树；任何一步失败都只打 warning，不掩盖探针本身的红。
    if (-not $Arg) { throw 'usage: paleo-dev.ps1 diagdeps <test-name>' }
    $ErrorActionPreference = 'Continue'
    try { Enter-DependencyEnvironment } catch { Write-Warning "依赖环境装配失败（$_），按当前 PATH 定位" }
    try { Enter-MsvcEnvironment } catch { Write-Warning "MSVC 环境不可用（$_），跳过 dumpbin" }
    $exe = Get-ChildItem -Path $Build -Recurse -File -Filter "$Arg.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $exe) { Write-Warning "build 下找不到 $Arg.exe"; exit 0 }
    Write-Host "== $($exe.FullName) =="
    if (-not (Get-Command dumpbin.exe -ErrorAction SilentlyContinue)) {
      Write-Warning 'dumpbin.exe 不在 PATH，无法列依赖'; exit 0
    }
    $deps = @(& dumpbin.exe /nologo /dependents $exe.FullName |
      Where-Object { $_ -match '^\s+(\S+\.dll)\s*$' } | ForEach-Object { $Matches[1] })
    $missing = 0
    foreach ($d in $deps) {
      # 加载器搜索序近似：exe 所在目录 → System32 → PATH（不调 Get-Command，
      # 它对非 PATHEXT 扩展名的行为随 pwsh 版本漂移）。
      $searchDirs = @($exe.DirectoryName, [Environment]::SystemDirectory) + ($env:PATH -split ';' | Where-Object { $_ })
      $hit = $searchDirs | ForEach-Object { Join-Path $_ $d } | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
      if ($hit) { "  ok      $d -> $hit" }
      elseif ($d -match '^(api-ms-|ext-ms-)') { "  apiset  $d" }
      else { "  MISSING $d"; $missing++ }
    }
    if ($missing -gt 0) { Write-Host "::error::$Arg.exe 有 $missing 个直接依赖 DLL 在测试 PATH 下找不到（加载器 0xC0000135 候选根因）" }
    exit 0
  }
  'selfcheck' {
    Enter-DependencyEnvironment
    $env:QT_QPA_PLATFORM = 'offscreen'
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
