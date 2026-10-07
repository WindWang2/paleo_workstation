# check_qt_env.ps1 — Qt 编译/运行链一致性自检（方向 72 防回归）。
# 用法：tools/check_qt_env.ps1 [-BuildDir <build>] [-PathOverride <PATH 串>]
# 逻辑：
#   1. 编译侧：build/CMakeCache.txt 的 Qt6Core_DIR → Qt 前缀 → bin/Qt6Core.dll
#      的文件版本（无 DLL 时读 Qt6CoreConfigVersionImpl.cmake 的 PACKAGE_VERSION）。
#   2. 运行侧：按 Windows DLL 搜索序（exe 目录 → System32 → PATH 逐项）解析
#      Qt6Core.dll，取第一个命中。
#   3. major.minor 不一致 → 错误（exit 1）；patch 不一致 → 警告（exit 3）。
#      混链特征（如 6.8 编 / 6.11 载）落在 major.minor 不一致上。
# -PathOverride 供 mutation 验证：注入一段故意错配的 PATH 检验告警路径。
param(
  [string]$BuildDir = (Join-Path $PSScriptRoot '..\build'),
  [string]$PathOverride = $null
)

$ErrorActionPreference = 'Stop'

function Get-QtCompileVersion([string]$qtCoreDir) {
  if (-not (Test-Path $qtCoreDir)) { return $null }
  # lib/cmake/Qt6Core → 前缀三级上翻；bin/Qt6Core.dll 的文件版本最直接。
  $dll = Join-Path (Split-Path (Split-Path (Split-Path $qtCoreDir -Parent) -Parent) -Parent) 'bin\Qt6Core.dll'
  if (Test-Path $dll) { return @{ Version = (Get-Item $dll).VersionInfo.FileVersion; Source = $dll } }
  $verFile = Join-Path $qtCoreDir 'Qt6CoreConfigVersionImpl.cmake'
  if (Test-Path $verFile) {
    $m = Select-String -Path $verFile -Pattern 'PACKAGE_VERSION\s+"([0-9.]+)"' | Select-Object -First 1
    if ($m) { return @{ Version = $m.Matches[0].Groups[1].Value; Source = $verFile } }
  }
  return $null
}

function Resolve-RuntimeQt6Core([string[]]$searchDirs) {
  foreach ($d in $searchDirs) {
    if (-not $d) { continue }
    $hit = Join-Path $d 'Qt6Core.dll'
    if (Test-Path $hit) {
      return @{ Dir = $d; Version = (Get-Item $hit).VersionInfo.FileVersion; Path = $hit }
    }
  }
  return $null
}

$cache = Join-Path $BuildDir 'CMakeCache.txt'
if (-not (Test-Path $cache)) { Write-Error "CMakeCache not found: $cache（先 build）"; exit 2 }
$qtDirLine = Select-String -Path $cache -Pattern '^Qt6Core_DIR:PATH=(.+)$' | Select-Object -First 1
if (-not $qtDirLine) { Write-Error 'CMakeCache 无 Qt6Core_DIR（未用 Qt 配置过）'; exit 2 }

$compile = Get-QtCompileVersion $qtDirLine.Matches[0].Groups[1].Value
if (-not $compile) { Write-Error "编译侧 Qt 版本无法读取：$($qtDirLine.Matches[0].Groups[1].Value)"; exit 2 }

# Windows DLL 搜索序（SafeDllSearchMode 开）：exe 目录 → System32 → Windows → PATH。
$effectivePath = if ($PathOverride) { $PathOverride } else { $env:PATH }
$searchDirs = @($BuildDir, "$env:WINDIR\System32", "$env:WINDIR") + ($effectivePath -split ';')
$runtime = Resolve-RuntimeQt6Core $searchDirs

Write-Host ("compile  Qt: {0}  [{1}]" -f $compile.Version, $compile.Source)
if (-not $runtime) {
  # 构建后运行期才需要 DLL；exe 还没链接出来时 PATH 上找不到不判死——
  # 但编译侧前缀自身的 bin 必须存在且算第一运行候选。
  $selfDll = Join-Path (Split-Path (Split-Path (Split-Path $qtDirLine.Matches[0].Groups[1].Value -Parent) -Parent) -Parent) 'bin\Qt6Core.dll'
  if (-not (Test-Path $selfDll)) { Write-Error "运行侧解析不到 Qt6Core.dll，且编译侧前缀无 bin DLL：$selfDll"; exit 1 }
  Write-Host "runtime  Qt: (PATH 未命中，回退编译侧前缀 $selfDll)"
  exit 0
}
Write-Host ("runtime  Qt: {0}  [{1}]" -f $runtime.Version, $runtime.Path)

$c = $compile.Version -split '\.'
$r = $runtime.Version -split '\.'
if ("$($c[0]).$($c[1])" -ne "$($r[0]).$($r[1])") {
  Write-Warning ("Qt 混链：编译 {0} / 运行 {1}（{2}）——major.minor 必须一致（方向 72 统一口径）" -f $compile.Version, $runtime.Version, $runtime.Dir)
  exit 1
}
if ("$($c[2])" -ne "$($r[2])") {
  Write-Warning ("Qt patch 不一致：编译 {0} / 运行 {1}——同 minor 内二进制兼容，但建议对齐" -f $compile.Version, $runtime.Version)
  exit 3
}
Write-Host "Qt 链一致：$($compile.Version)"
exit 0
