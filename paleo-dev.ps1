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

function Preflight {
  Write-Host "== preflight =="
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
    # TODO(ET1-windows): osgeo4w-setup.exe -q -k -A -s <mirror> -P qgis-devel,qgis-devel-deps
    # then copy closure into vendor/qgis + verify SHA512 pins from vendor/manifest.json.
    Write-Host "OSGeo4W download step is stubbed pending ET1 — manifest pins already recorded."
  }
  'build' {
    cmake -S $Root -B $Build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    ninja -C $Build
  }
  'test' {
    $env:QT_QPA_PLATFORM = 'offscreen'
    ctest --test-dir $Build --output-on-failure
  }
  'selfcheck' {
    $env:QT_QPA_PLATFORM = 'offscreen'
    & (Join-Path $Build 'paleo_selfcheck.exe')
  }
  'clean-vendor' {
    if (-not $Arg) { throw "clean-vendor needs a dep name (e.g. onnxruntime)" }
    $target = Join-Path $Vendor $Arg
    if (Test-Path $target) { Remove-Item -Recurse -Force $target; Write-Host "removed $target" }
  }
  default { throw "unknown verb '$Verb' — bootstrap|build|test|selfcheck|clean-vendor" }
}
