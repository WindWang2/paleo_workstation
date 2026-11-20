# paleo_dev_ps1_selftest.ps1 — paleo-dev.ps1 的跨平台单元自检（方向 81）。
# 不执行 paleo-dev.ps1 本身（会进 VS/vendor 流程）：先整文件语法解析，再按
# AST 抽出被测函数在本进程定义，喂合成夹具断言。Windows PowerShell 5.1 与
# pwsh 7（含 Linux）都可跑；ctest 在找到 pwsh/powershell 时注册
# paleo_dev_ps1_selftest（Windows CI 必跑）。
# -Mutate：跳过 OSGeo4W 锁戳验证，夹具应拒绝这个实现。
param([string]$Script, [switch]$Mutate)
$ErrorActionPreference = 'Stop'
if (-not $Script) { $Script = Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'paleo-dev.ps1' }

$tokens = $null; $errs = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Script, [ref]$tokens, [ref]$errs)
if ($errs.Count) {
  $errs | ForEach-Object { Write-Host ("PARSE ERROR: {0} @ line {1}" -f $_.Message, $_.Extent.StartLineNumber) }
  exit 1
}
$want = 'Get-Osgeo4wLockHash', 'Assert-Osgeo4wLockStamp', 'Enter-VendorEnvironment', 'Enter-DependencyEnvironment', 'Invoke-EnsureQgisResources', 'Assert-NoEnvDebtRegression'
$fns = $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true)
foreach ($f in $fns) { if ($want -contains $f.Name) { Invoke-Expression $f.Extent.Text } }
foreach ($n in $want) { if (-not (Get-Command $n -CommandType Function -ErrorAction SilentlyContinue)) { throw "function $n not found in $Script" } }
if ($Mutate) {
  function Assert-Osgeo4wLockStamp([string]$Prefix) {}
}

$failures = New-Object System.Collections.Generic.List[string]
function Check([bool]$cond, [string]$what) { if (-not $cond) { $failures.Add($what) } }
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("paleo-ps1-selftest-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $tmp | Out-Null
$savedEnv = @{}
foreach ($k in 'PALEO_PYTHON', 'QGIS_PREFIX_PATH', 'CMAKE_PREFIX_PATH', 'PATH',
               'GDAL_DATA', 'GDAL_DRIVER_PATH', 'PROJ_LIB', 'PROJ_DATA', 'QT_PLUGIN_PATH',
               'QT_QPA_PLATFORM_PLUGIN_PATH', 'QTWEBENGINEPROCESS_PATH',
               'QTWEBENGINE_RESOURCES_PATH', 'QTWEBENGINE_LOCALES_PATH') {
  $savedEnv[$k] = [Environment]::GetEnvironmentVariable($k, 'Process')
}
try {
  # 首轮已修项再红必须阻断；预算豁免和未知旧红仍交回各自判定。
  $manifest = Join-Path $tmp 'redset.json'
  Set-Content $manifest -Encoding utf8 -Value '{"entries":[{"test":"tst_runtime","status":"fixed"},{"test":"tst_cache_las","status":"exempt"}]}'
  $threw = $false
  try { Assert-NoEnvDebtRegression @('tst_runtime') $manifest } catch { $threw = $true }
  Check $threw 'fixed environment debt must not pass through direct-run retry'
  $ok = $true
  try { Assert-NoEnvDebtRegression @('tst_cache_las', 'tst_other') $manifest } catch { $ok = $false }
  Check $ok 'perf exemptions/legacy tests must reach their own classifier'

  # OSGeo4W 只复用匹配当前锁的前缀。安装数据库解析由 Python 夹具覆盖。
  $Vendor = Join-Path $tmp 'vendor'
  $prefix = Join-Path $Vendor 'osgeo4w'
  $lock = Join-Path $Vendor 'osgeo4w-offline.lock.json'
  $stamp = Join-Path $prefix '.paleo-osgeo4w-lock.sha256'
  New-Item -ItemType Directory -Force $prefix | Out-Null
  Set-Content -LiteralPath $lock -Value '{"schema":1}' -Encoding ascii
  $threw = $false
  try { Assert-Osgeo4wLockStamp $prefix } catch { $threw = $true }
  Check $threw 'OSGeo4W prefix without a receipt must be rejected'
  Set-Content -LiteralPath $stamp -Value 'stale' -Encoding ascii
  $threw = $false
  try { Assert-Osgeo4wLockStamp $prefix } catch { $threw = $true }
  Check $threw 'OSGeo4W prefix from a different lock must be rejected'
  Set-Content -LiteralPath $stamp -Value (Get-Osgeo4wLockHash) -Encoding ascii
  $ok = $true
  try { Assert-Osgeo4wLockStamp $prefix } catch { $ok = $false }
  Check $ok 'OSGeo4W prefix stamped for the current lock must be accepted'

  # The managed route binds Qt, GIS and the caller's explicitly chosen Python.
  $qgis = Join-Path $prefix 'apps/qgis'
  $headers = Join-Path $qgis 'include'
  New-Item -ItemType Directory -Force $headers | Out-Null
  Set-Content (Join-Path $headers 'qgsapplication.h') 'fixture'
  $env:PALEO_PYTHON = 'C:/custom/python.exe'
  Enter-DependencyEnvironment
  Check ($env:PALEO_PYTHON -eq 'C:/custom/python.exe') 'user PALEO_PYTHON overwritten'
  Check ((Get-Item -LiteralPath $env:QGIS_PREFIX_PATH).FullName -eq (Get-Item -LiteralPath $qgis).FullName) 'Qt and QGIS did not use the locked prefix'
  Check ($env:PROJ_DATA.Replace('\', '/') -eq (Join-Path $prefix 'share/proj').Replace('\', '/')) 'PROJ_DATA selected another prefix'

  New-Item -ItemType Directory -Force (Join-Path $qgis 'resources') | Out-Null
  Set-Content (Join-Path $qgis 'resources/srs.db') -Value 'x'
  $env:PALEO_PYTHON = Join-Path $tmp 'no-such-python.exe'
  $Root = Split-Path -Parent $Script
  $ok = $true; try { Invoke-EnsureQgisResources } catch { $ok = $false }
  Check $ok 'ensure-resources invoked Python although srs.db exists'
  Remove-Item -LiteralPath (Join-Path $qgis 'resources/srs.db') -Force
  $threw = $false; try { Invoke-EnsureQgisResources } catch { $threw = $true }
  Check $threw 'ensure-resources with missing inputs must fail clearly'

  $Vendor = Join-Path $tmp 'absent-vendor'
  $threw = $false; try { Enter-DependencyEnvironment } catch { $threw = $true }
  Check $threw 'an unlocked host must be rejected with bootstrap instructions'
} finally {
  foreach ($k in $savedEnv.Keys) { [Environment]::SetEnvironmentVariable($k, $savedEnv[$k], 'Process') }
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

if ($Mutate) {
  if ($failures.Count -eq 0) { Write-Host 'MUTATION SURVIVED: skipping the OSGeo4W lock check passed the fixtures'; exit 1 }
  Write-Host ("mutation caught ({0} failures, e.g. {1}) — PASS" -f $failures.Count, $failures[0])
  exit 0
}
if ($failures.Count) { $failures | ForEach-Object { Write-Host "FAIL $_" }; exit 1 }
Write-Host ("paleo-dev.ps1 selftest: PASS ({0} functions parsed)" -f $fns.Count)
exit 0
