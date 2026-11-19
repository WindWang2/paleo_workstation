# Windows portable packaging uses the same dependency checks as the Python entry.
param(
  [string]$OutDir,
  [string]$Executable,
  [string]$OfficeEditor,
  [switch]$SkipPython,
  [switch]$Zip
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = if ($Executable) {
  $path = (Resolve-Path -LiteralPath $Executable).Path
  if ((Split-Path $path -Leaf) -ne 'paleo.exe') { throw '-Executable must name paleo.exe' }
  Split-Path $path -Parent
} else {
  Join-Path $repo 'build'
}
$arguments = @((Join-Path $PSScriptRoot 'package_portable.py'), '--build-dir', $build, '--verify')
if ($OutDir) { $arguments += @('--out', $OutDir) }
if ($OfficeEditor) { $arguments += @('--office-editor', $OfficeEditor) }
if ($SkipPython) { $arguments += '--skip-python' }
if ($Zip) { $arguments += '--zip' }
& python @arguments
exit $LASTEXITCODE
