# paleo_dev_ps1_selftest.ps1 — paleo-dev.ps1 的跨平台单元自检（方向 81）。
# 不执行 paleo-dev.ps1 本身（会进 VS/vendor 流程）：先整文件语法解析，再按
# AST 抽出被测函数在本进程定义，喂合成夹具断言。Windows PowerShell 5.1 与
# pwsh 7（含 Linux）都可跑；ctest 在找到 pwsh/powershell 时注册
# paleo_dev_ps1_selftest（Windows CI 必跑）。
#   -Mutate：把 Read-InstalledDbMap 换回 #297 的旧实现（头行/文件名当版本），
#            断言夹具抓到它——自检本身的突变证据。
param([string]$Script, [switch]$Mutate)
$ErrorActionPreference = 'Stop'
if (-not $Script) { $Script = Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'paleo-dev.ps1' }

$tokens = $null; $errs = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Script, [ref]$tokens, [ref]$errs)
if ($errs.Count) {
  $errs | ForEach-Object { Write-Host ("PARSE ERROR: {0} @ line {1}" -f $_.Message, $_.Extent.StartLineNumber) }
  exit 1
}
$want = 'Read-InstalledDbMap', 'Compare-PinnedClosure', 'Enter-LocalDepsEnvironment', 'Invoke-EnsureQgisResources'
$fns = $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true)
foreach ($f in $fns) { if ($want -contains $f.Name) { Invoke-Expression $f.Extent.Text } }
foreach ($n in $want) { if (-not (Get-Command $n -CommandType Function -ErrorAction SilentlyContinue)) { throw "function $n not found in $Script" } }
if ($Mutate) {
  # #297 原实现：不跳头行、不剥 name-/.tar.* —— 夹具必须判它失败。
  function Read-InstalledDbMap([string]$path) {
    if (-not (Test-Path $path)) { return $null }
    $map = @{}
    foreach ($line in (Get-Content $path -Encoding utf8)) {
      $f = $line -split '\s+'
      if ($f.Count -ge 2 -and $f[0]) { $map[$f[0]] = $f[1] }
    }
    if ($map.Count -eq 0) { return $null }
    return $map
  }
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
  # 1) installed.db：头行跳过、文件名剥 name- 与 .tar.*（#297 回归）。
  $db = Join-Path $tmp 'installed.db'
  Set-Content $db -Encoding utf8 -Value @('INSTALLED.DB 2', 'gsl gsl-2.7.101-1.tar.bz2 0',
    'arrow-cpp arrow-cpp-25.0.1-1.tar.bz2 0', 'py py-3.12.7-1.tar.xz 0', '')
  $m = Read-InstalledDbMap $db
  Check ($null -ne $m) 'installed.db map is null'
  if ($m) {
    Check (-not $m.ContainsKey('INSTALLED.DB')) 'header line parsed as package'
    Check ($m.Count -eq 3) ("map count {0} != 3" -f $m.Count)
    Check ($m['gsl'] -eq '2.7.101-1') ("gsl -> '{0}'" -f $m['gsl'])
    Check ($m['arrow-cpp'] -eq '25.0.1-1') ("arrow-cpp -> '{0}'" -f $m['arrow-cpp'])
    Check ($m['py'] -eq '3.12.7-1') ("py -> '{0}'" -f $m['py'])
    $closure = [pscustomobject]@{ 'gsl' = '2.7.101-1'; 'arrow-cpp' = '25.0.1-1'; 'py' = '3.12.7-1' }
    Check (@(Compare-PinnedClosure $m $closure).Count -eq 0) 'pinned closure reported drift on identical set'
    $drift = [pscustomobject]@{ 'gsl' = '2.7.1-1'; 'arrow-cpp' = '25.0.1-1'; 'py' = '3.12.7-1' }
    Check (@(Compare-PinnedClosure $m $drift).Count -eq 1) 'version drift not caught'
  }
  Check ($null -eq (Read-InstalledDbMap (Join-Path $tmp 'nope.db'))) 'missing installed.db must map to $null'

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
  if ($failures.Count -eq 0) { Write-Host 'MUTATION SURVIVED: old Read-InstalledDbMap passed the fixtures'; exit 1 }
  Write-Host ("mutation caught ({0} failures, e.g. {1}) — PASS" -f $failures.Count, $failures[0])
  exit 0
}
if ($failures.Count) { $failures | ForEach-Object { Write-Host "FAIL $_" }; exit 1 }
Write-Host ("paleo-dev.ps1 selftest: PASS ({0} functions parsed)" -f $fns.Count)
exit 0
