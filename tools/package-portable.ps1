# package-portable.ps1 —— Windows 便携包打包（一次性可重复执行）。
# 产物布局（<OutDir> 根 = exe 同目录，命中 portableenvironment 便携分支）：
#   paleo.exe + DLL 闭包 + QtWebEngineProcess.exe
#   plugins/ qml/ resources/ translations/ share/qt6/resources(双保险)
#   share/gdal share/proj          ← GDAL/PROJ 数据（PROJ_DATA+PROJ_LIB 双指）
#   qgis/resources                 ← QGIS 前缀资源（QGIS_PREFIX_PATH=<pkg>\qgis）
#   office-editor/                 ← ranuts/document 静态编辑页（AGPL-3.0 派生）
#   python/                        ← conda 建 py3.10 + correlation requirements
#   correlation/独立Web项目_20261004/
#   启动paleo.bat                  ← 全套环境变量 + 启动前坐标自检（相对 %~dp0，可整体搬目录）
# 依赖来源：~/paleo-qgis-deps(conda Qt6.11.2) + ~/paleo-qgis-prefix + qtpdf 前缀 +
#   构建产物 paleo.exe + vendor/{ranuts-document,onnxruntime} + 独立Web项目_20261004。
# 用法：pwsh -File tools/package-portable.ps1 [-OutDir ...] [-Executable ...]
#       [-OfficeEditor ...] [-SkipPython] [-SkipCorrelation]
# OutDir 已存在即报错退出（保留旧版本，避免误删仍可用的历史包）。
# 打包完成后用包内 paleo.exe --check-runtime 实测 EPSG:32649/4326 与正反变换，
# 复制成功不等于坐标链路可用。
param(
  [string]$OutDir = "C:/Users/wangj.KEVIN/paleo-portable",
  [string]$Executable,            # 缺省 build/paleo.exe；worktree 布局回退 build/setup/out/paleo.exe
  [string]$OfficeEditor,          # 缺省 vendor/ranuts-document（worktree 无 vendor 时可指主仓库）
  [switch]$SkipPython,
  [switch]$SkipCorrelation
)
$ErrorActionPreference = 'Stop'
$Repo = Split-Path $PSScriptRoot -Parent
if (-not $Executable) {
  $Executable = @("$Repo/build/paleo.exe", "$Repo/build/setup/out/paleo.exe") |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
  if (-not $Executable) { throw "找不到构建产物 paleo.exe（build/ 或 build/setup/out/），请用 -Executable 指定" }
}
if (-not $OfficeEditor) { $OfficeEditor = "$Repo/vendor/ranuts-document" }
$BuildOut = [IO.Path]::GetFullPath((Split-Path $Executable -Parent))
$Deps = if ($env:PALEO_LOCAL_DEPS) { $env:PALEO_LOCAL_DEPS } else { "$HOME/paleo-qgis-deps" }
$Qgis = if ($env:PALEO_QGIS_PREFIX) { $env:PALEO_QGIS_PREFIX } else { "$HOME/paleo-qgis-prefix" }
$Qtpdf = if ($env:PALEO_QTPDF_PREFIX) { $env:PALEO_QTPDF_PREFIX } else { 'C:/deps/Qt/6.11.2/msvc2022_64' }
$WebProject = if ($env:PALEO_CORRELATION_PROJECT_DIR) { $env:PALEO_CORRELATION_PROJECT_DIR }
              else { 'C:/Users/wangj.KEVIN/projects/独立Web项目_20261004' }
$Conda = 'C:/ProgramData/anaconda3/Scripts/conda.exe'

function Info($m) { Write-Host "[pkg] $m" -ForegroundColor Cyan }
function Copy-Tree([string]$Source, [string]$Target, [Parameter(ValueFromRemainingArguments)]$Options) {
  if (-not (Test-Path -LiteralPath $Source -PathType Container)) { throw "缺少打包目录: $Source" }
  & robocopy.exe $Source $Target @Options /NFL /NDL /NJH /NJS | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "目录复制失败 ($LASTEXITCODE): $Source -> $Target" }
}

# ---- 0. 前置检查 + VS 环境（dumpbin 做 DLL 闭包）----
foreach ($p in @($Executable, "$Deps/Library/bin/Qt6Core.dll", "$Qgis/bin/qgis_core.dll",
                 "$Deps/Library/share/proj/proj.db", "$Qgis/resources/srs.db", "$Qgis/resources/symbology-style.xml",
                 "$OfficeEditor/index.html")) {
  if (-not (Test-Path -LiteralPath $p)) { throw "缺少打包输入: $p" }
}
$OutDir = [IO.Path]::GetFullPath($OutDir)
if (Test-Path -LiteralPath $OutDir) { throw "输出目录已存在，为保留旧版本请另选 -OutDir: $OutDir" }
Info "输出目录: $OutDir"
New-Item -ItemType Directory -Force "$OutDir" | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$devCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
$lines = & cmd.exe /d /c "`"$devCmd`" -arch=amd64 -host_arch=amd64 >nul && set"
foreach ($line in $lines) { if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') } }
$dumpbin = (Get-Command dumpbin.exe -ErrorAction Stop).Source

# ---- 1. 主程序 + ORT ----
Info "1/9 paleo.exe + onnxruntime"
Copy-Item -LiteralPath $Executable -Destination (Join-Path $OutDir "paleo.exe")
if (Test-Path "$Repo/vendor/onnxruntime/lib/onnxruntime.dll") { Copy-Item "$Repo/vendor/onnxruntime/lib/onnxruntime.dll" $OutDir -Force }

# ---- 2. DLL 闭包（dumpbin BFS；QtWebEngineProcess 的 DLL 面与主程序基本同源，一并覆盖）----
Info "2/9 DLL 闭包（dumpbin BFS）"
$searchDirs = @("$Deps/Library/bin", "$Qgis/bin", "$Qtpdf/bin", "$BuildOut") | ForEach-Object { [IO.Path]::GetFullPath($_) }
$queue = [System.Collections.Generic.Queue[string]]::new()
$queue.Enqueue([IO.Path]::GetFullPath("$OutDir/paleo.exe"))
$copied = @{}
while ($queue.Count -gt 0) {
  $pe = $queue.Dequeue()
  # 注意：PS 变量名不区分大小写——这里绝不能叫 $deps（会覆盖依赖根 $Deps）
  $depDlls = & $dumpbin /dependents $pe 2>$null | Select-String -Pattern '^\s+([A-Za-z0-9_.\-]+\.dll)\s*$' | ForEach-Object { $_.Matches[0].Groups[1].Value.ToLowerInvariant() }
  foreach ($dll in $depDlls) {
    if ($copied.ContainsKey($dll)) { continue }
    $src = $null
    foreach ($d in $searchDirs) { $c = Join-Path $d $dll; if (Test-Path $c) { $src = $c; break } }
    if (-not $src) { continue }  # 系统 DLL（System32）不进包
    Copy-Item $src $OutDir -Force
    $copied[$dll] = $src
    $queue.Enqueue((Join-Path $OutDir $dll))
  }
}
Info ("    闭包 DLL 数: " + $copied.Count)
if (-not $copied.ContainsKey('qt6webenginecore.dll')) { Write-Warning '闭包缺 Qt6WebEngineCore——office 编辑页将退系统浏览器' }

# ---- 3. Qt 插件 / QML / WebEngine 进程与资源 / 翻译 ----
Info "3/9 Qt plugins/qml/WebEngine 资源"
# 注意：Library/plugins 是 Qt5 的（本 env 同时含 qt5 包）——Qt6 插件在 lib/qt6/plugins
Copy-Tree "$Deps/Library/lib/qt6/plugins" "$OutDir/plugins" /E
Copy-Tree "$Deps/Library/lib/qt6/qml" "$OutDir/qml" /E
Copy-Item "$Deps/Library/lib/qt6/QtWebEngineProcess.exe" $OutDir -Force
# conda Qt 的资源/翻译在 share/qt6 下；两处都放（resources/ 供 QLibraryInfo 相对解析，share/qt6 保 conda 布局语义）
Copy-Tree "$Deps/Library/share/qt6/resources" "$OutDir/resources" /E
Copy-Tree "$Deps/Library/share/qt6/resources" "$OutDir/share/qt6/resources" /E
if (Test-Path "$Deps/Library/share/qt6/translations") { Copy-Tree "$Deps/Library/share/qt6/translations" "$OutDir/share/qt6/translations" /E }
Copy-Tree "$Deps/Library/translations" "$OutDir/translations" /E
# WebEngine 进程自身还有一小圈依赖（resources 目录里带 v8 snapshot 等），DLL 已在闭包内

# ---- 4. GDAL/PROJ 数据 ----
Info "4/9 GDAL/PROJ 数据目录"
Copy-Tree "$Deps/Library/share/gdal" "$OutDir/share/gdal" /E
Copy-Tree "$Deps/Library/share/proj" "$OutDir/share/proj" /E

# ---- 5. QGIS 前缀资源（bin DLL 已进闭包；lib/include 不进包）----
Info "5/9 QGIS resources"
Copy-Tree "$Qgis/resources" "$OutDir/qgis/resources" /E

# ---- 6. Office 编辑组件（ranuts/document，AGPL-3.0 派生——分发须随附对应源码）----
Info "6/9 office-editor"
Copy-Tree $OfficeEditor "$OutDir/office-editor" /E

# ---- 7. Python 运行时（conda 建 3.10 + correlation requirements；PALEO_PYTHON 同用）----
if (-not $SkipPython) {
  Info "7/9 python 3.10 运行时（conda create + pip install，数分钟）"
  & $Conda create -y -p "$OutDir/python" python=3.10 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'conda create 失败' }
  & "$OutDir/python/python.exe" -m pip install --no-warn-script-location -r "$WebProject/requirements.txt" 2>&1 | Select-Object -Last 3 | Write-Host
  if ($LASTEXITCODE -ne 0) { throw 'pip install 失败' }
} else { Info '7/9 跳过 python（-SkipPython）' }

# ---- 8. 地层对比独立项目 ----
if (-not $SkipCorrelation) {
  Info "8/9 correlation 项目（robocopy 1.2G，含数据）"
  Copy-Tree $WebProject "$OutDir/correlation/$(Split-Path $WebProject -Leaf)" /E /XD __pycache__ .venv
} else { Info '8/9 跳过 correlation（-SkipCorrelation）' }

# ---- 9. 启动器（内容必须纯 ASCII：cmd 在 chcp 切码后会错位重读批处理，
#      中文内容会把解析器打碎；中文说明进 README，correlation 目录运行时枚举）----
Info "9/9 启动器"
@"
@echo off
rem Paleo Workstation portable launcher - all paths relative to %~dp0, safe to move the whole folder
rem (Keep this file pure ASCII: cmd misreparses the batch after a mid-file chcp,
rem  and GBK codepage would corrupt non-ASCII bytes. Chinese docs live in README.)
set "ROOT=%~dp0"
set "PATH=%ROOT%;%ROOT%qgis\bin;%ROOT%python;%ROOT%python\Library\bin;%ROOT%python\Scripts;%PATH%"
set "QT_PLUGIN_PATH=%ROOT%plugins"
set "QML2_IMPORT_PATH=%ROOT%qml"
set "QTWEBENGINEPROCESS_PATH=%ROOT%QtWebEngineProcess.exe"
set "GDAL_DATA=%ROOT%share\gdal"
set "PROJ_DATA=%ROOT%share\proj"
set "PROJ_LIB=%ROOT%share\proj"
set "QGIS_PREFIX_PATH=%ROOT%qgis"
set "PALEO_OFFICE_EDITOR=%ROOT%office-editor"
set "PYTHONUTF8=1"
set "PALEO_PYTHON=%ROOT%python\python.exe"
set "PALEO_CORRELATION_PYTHON=%ROOT%python\python.exe"
rem coordinate runtime self-check: fail loudly instead of starting a broken map stack
set "REPORT=%TEMP%\paleo-runtime-check.json"
"%ROOT%paleo.exe" --check-runtime --report "%REPORT%"
if errorlevel 1 (
  echo Coordinate runtime check FAILED. See "%REPORT%".
  pause
  exit /b 1
)
rem correlation project dir: the single subfolder under correlation\ (name may be non-ASCII)
for /d %%D in ("%ROOT%correlation\*") do if not defined PALEO_CORRELATION_PROJECT_DIR set "PALEO_CORRELATION_PROJECT_DIR=%%D"
start "" "%ROOT%paleo.exe" %*
"@ | Out-File "$OutDir/启动paleo.bat" -Encoding ascii
@"
# Paleo Workstation 便携包

- 双击「启动paleo.bat」运行；命令行可附工程路径：`启动paleo.bat D:\path\project.qgz`
- 启动器会先做坐标运行时自检（EPSG:32649/4326 与 QGIS/GDAL 变换），失败时给出
  报告路径并不启动主程序。
- 地层对比工作台：ribbon 选择独立项目（已预指 correlation/）后「启动服务」；
  Python 已随包（python/），`PALEO_CORRELATION_*` 由启动器注入。
- Office 预览/编辑：内置 office-editor/（OnlyOffice WASM 静态页，AGPL-3.0 派生，
  分发本包须随附对应源码并保留 ONLYOFFICE 标识）。
- 需要目标机装有 VC++ 2015-2022 运行库（MSVC /MD）。
- 重新打包：`pwsh -File tools/package-portable.ps1`（仓库内）。
"@ | Out-File "$OutDir/README-便携包.md" -Encoding utf8

# ---- 10. 包内坐标链路实测（复制成功 ≠ 坐标可用）----
Info "验证包内 EPSG:32649 / QGIS / GDAL 坐标转换"
$reportPath = Join-Path $OutDir 'runtime-check.json'
& (Join-Path $OutDir 'paleo.exe') --check-runtime --report $reportPath
if ($LASTEXITCODE -ne 0) { throw "包内坐标自检失败，详见 $reportPath" }

Info "完成: $OutDir"
$info = Get-ChildItem $OutDir | ForEach-Object { $s = (Get-ChildItem $_.FullName -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum / 1MB; '{0,-18} {1,8:N0} MB' -f $_.Name, $s }
$info | Write-Host
