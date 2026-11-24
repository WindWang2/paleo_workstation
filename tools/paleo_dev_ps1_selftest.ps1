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
$want = 'Get-Osgeo4wLockHash', 'Assert-Osgeo4wLockStamp', 'Enter-VendorEnvironment', 'Enter-DependencyEnvironment', 'Invoke-EnsureQgisResources', 'Assert-NoEnvDebtRegression', 'Assert-CTestNonEmpty', 'Invoke-TestManifestCheck', 'Invoke-FlakyLedgerCheck'
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
  $threw = $false; try { Invoke-EnsureQgisResources } catch { $threw = $true }
  Check $threw 'an existing srs.db must still be verified against its archive'
  Remove-Item -LiteralPath (Join-Path $qgis 'resources/srs.db') -Force
  $threw = $false; try { Invoke-EnsureQgisResources } catch { $threw = $true }
  Check $threw 'ensure-resources with missing inputs must fail clearly'

  $Vendor = Join-Path $tmp 'absent-vendor'
  $threw = $false; try { Enter-DependencyEnvironment } catch { $threw = $true }
  Check $threw 'an unlocked host must be rejected with bootstrap instructions'

  # #957：空测试集兜底与 flaky strict 退出码传播。
  $emptyLog = Join-Path $tmp 'empty-ctest.log'
  Set-Content -LiteralPath $emptyLog -Encoding ascii -Value 'No tests were found!!!'
  $threw = $false; try { Assert-CTestNonEmpty $emptyLog '-R no_such_test' } catch { $threw = $true }
  Check $threw 'empty CTest selection log must fail non-zero (#957)'
  $okLog = Join-Path $tmp 'ok-ctest.log'
  Set-Content -LiteralPath $okLog -Encoding ascii -Value '100% tests passed, 0 tests failed out of 1'
  $ok = $true; try { Assert-CTestNonEmpty $okLog '-R tst_runtime' } catch { $ok = $false }
  Check $ok 'non-empty CTest log must pass Assert-CTestNonEmpty (#957)'

  $flakyRepo = Join-Path $tmp 'flaky-repo'
  $flakyTools = Join-Path $flakyRepo 'tools'
  New-Item -ItemType Directory -Force $flakyTools | Out-Null
  Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $Script) 'tools/check_flaky_ledger.py') -Destination (Join-Path $flakyTools 'check_flaky_ledger.py')
  $flakyJsonl = Join-Path $flakyTools 'flaky-ledger.jsonl'
  $threeRows = @(
    '{"test":"tst_flaky_957","utc":"2026-11-24T00:00:00Z","event":"serial-rerun-pass","lane":"ps1-direct"}'
    '{"test":"tst_flaky_957","utc":"2026-11-24T00:01:00Z","event":"serial-rerun-pass","lane":"ps1-direct"}'
    '{"test":"tst_flaky_957","utc":"2026-11-24T00:02:00Z","event":"serial-rerun-pass","lane":"ps1-direct"}'
  )
  Set-Content -LiteralPath $flakyJsonl -Encoding ascii -Value $threeRows
  $pyCmd = Get-Command python -ErrorAction SilentlyContinue
  if (-not $pyCmd) { $pyCmd = Get-Command python3 -ErrorAction SilentlyContinue }
  if ($pyCmd) {
    $flakyLog = Join-Path $tmp 'flaky.log'
    $threw = $false; try { Invoke-FlakyLedgerCheck $pyCmd.Source $flakyRepo $flakyLog $true } catch { $threw = $true }
    Check $threw 'three flaky records under strict mode must throw (#957)'
    $ok = $true; try { Invoke-FlakyLedgerCheck $pyCmd.Source $flakyRepo $flakyLog $false } catch { $ok = $false }
    Check $ok 'three flaky records under warning mode must not throw (#957)'
    Set-Content -LiteralPath $flakyJsonl -Encoding ascii -Value $threeRows[0..1]
    $ok = $true; try { Invoke-FlakyLedgerCheck $pyCmd.Source $flakyRepo $flakyLog $true } catch { $ok = $false }
    Check $ok 'below-threshold flaky records under strict mode must pass (#957)'
    $threw = $false; try { Invoke-FlakyLedgerCheck (Join-Path $tmp 'no-such-python.exe') $flakyRepo $flakyLog $true } catch { $threw = $true }
    Check $threw 'missing python under strict flaky mode must throw (#957)'

    # #965：配置期注册测试清单对账（缩减必须阻断）。
    $manifestBuild = Join-Path $tmp 'manifest-build'
    New-Item -ItemType Directory -Force (Join-Path $manifestBuild 'Testing') | Out-Null
    Set-Content -LiteralPath (Join-Path $manifestBuild 'Testing/registered-tests.json') -Encoding ascii -Value '{"schema":1,"platform":"windows","have_ort":false,"count":1,"tests":["only_one"],"perf_tests":[]}'
    $threw = $false; try { Invoke-TestManifestCheck $pyCmd.Source (Split-Path -Parent $Script) $manifestBuild $true } catch { $threw = $true }
    Check $threw 'shrunk registered-tests.json must fail Invoke-TestManifestCheck (#965)'
  }
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
