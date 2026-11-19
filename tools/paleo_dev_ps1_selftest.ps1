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
$want = 'Get-Osgeo4wLockHash', 'Assert-Osgeo4wLockStamp', 'Enter-LocalDepsEnvironment', 'Invoke-EnsureQgisResources', 'Assert-NoEnvDebtRegression'
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
foreach ($k in 'PALEO_LOCAL_DEPS', 'PALEO_QGIS_PREFIX', 'PALEO_QTPDF_PREFIX', 'PALEO_PYTHON', 'QGIS_PREFIX_PATH',
               'CMAKE_PREFIX_PATH', 'QT_ADDITIONAL_PACKAGES_PREFIX_PATH', 'QSCINTILLA_PREFIX_PATH', 'PATH', 'GDAL_DATA', 'PROJ_LIB') {
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

  # 2) localdeps：PALEO_PYTHON 指向 deps 根 python.exe；用户已设值不覆盖。
  $deps = Join-Path $tmp 'deps'; $qgis = Join-Path $tmp 'qgis'
  foreach ($p in (Join-Path $deps 'Library/bin/Qt6Core.dll'), (Join-Path $qgis 'lib/qgis_core.lib'), (Join-Path $deps 'python.exe')) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $p) | Out-Null
    Set-Content $p -Value 'x'
  }
  $env:PALEO_LOCAL_DEPS = $deps; $env:PALEO_QGIS_PREFIX = $qgis; $env:PALEO_QTPDF_PREFIX = (Join-Path $tmp 'qtpdf')
  $env:PALEO_PYTHON = $null
  Check ((Enter-LocalDepsEnvironment) -eq $true) 'localdeps fixture not detected'
  Check ($env:PALEO_PYTHON -eq (Join-Path $deps 'python.exe')) ("PALEO_PYTHON -> '{0}'" -f $env:PALEO_PYTHON)
  Check ($env:QGIS_PREFIX_PATH -eq $qgis) 'QGIS_PREFIX_PATH not set to localdeps prefix'
  $env:PALEO_PYTHON = 'C:/custom/python.exe'
  Enter-LocalDepsEnvironment | Out-Null
  Check ($env:PALEO_PYTHON -eq 'C:/custom/python.exe') 'user PALEO_PYTHON overwritten'

  # 3) ensure-resources：srs.db 已在 → 零动作（不调用 python）；-Auto 且无前缀 → 静默返回。
  New-Item -ItemType Directory -Force (Join-Path $qgis 'resources') | Out-Null
  Set-Content (Join-Path $qgis 'resources/srs.db') -Value 'x'
  $env:QGIS_PREFIX_PATH = $qgis
  $env:PALEO_PYTHON = (Join-Path $tmp 'no-such-python.exe')   # 若被调用即抛
  $Root = Split-Path -Parent $Script
  $ok = $true; try { Invoke-EnsureQgisResources -Auto; Invoke-EnsureQgisResources } catch { $ok = $false }
  Check $ok 'ensure-resources invoked python although srs.db exists'
  # 解释器不存在 + srs.db 缺：-Auto 只告警不抛（test/selfcheck 不被打断），手动则抛。
  Remove-Item (Join-Path $qgis 'resources/srs.db') -Force
  $ok = $true; try { Invoke-EnsureQgisResources -Auto 3>$null } catch { $ok = $false }
  Check $ok 'ensure-resources -Auto with missing python must only warn'
  $threw = $false; try { Invoke-EnsureQgisResources 3>$null } catch { $threw = $true }
  Check $threw 'ensure-resources (manual) with missing python must throw'
  $env:QGIS_PREFIX_PATH = $null
  $ok = $true; try { Invoke-EnsureQgisResources -Auto } catch { $ok = $false }
  Check $ok 'ensure-resources -Auto without prefix must be silent'
  $threw = $false; try { Invoke-EnsureQgisResources } catch { $threw = $true }
  Check $threw 'ensure-resources (manual) without prefix must throw'
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
