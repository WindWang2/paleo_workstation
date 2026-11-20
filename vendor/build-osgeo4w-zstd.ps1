param(
  [Parameter(Mandatory=$true)][string]$Prefix,
  [switch]$PrepareOnly
)

$ErrorActionPreference = 'Stop'
$pin = (Get-Content -LiteralPath "$PSScriptRoot/manifest.json" -Raw | ConvertFrom-Json).deps.'zstd-source'
$stamp = Join-Path $Prefix '.paleo-zstd-source.sha256'

$archive = Join-Path $PSScriptRoot "cache/tarballs/zstd-$($pin.version).tar.gz"
function Assert-Archive([string]$Path) {
  if ((Get-Item -LiteralPath $Path).Length -ne $pin.size_bytes -or
      (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pin.sha256) {
    throw "zstd source archive size or hash mismatch: $Path"
  }
}
if (!(Test-Path -LiteralPath $archive)) {
  if ($env:PALEO_VENDOR_OFFLINE -eq '1') { throw "Missing offline zstd source archive: $archive" }
  New-Item -ItemType Directory -Path (Split-Path -Parent $archive) -Force | Out-Null
  $partial = "$archive.$([Guid]::NewGuid()).download"
  try {
    Invoke-WebRequest -Uri $pin.url -OutFile $partial -TimeoutSec 90
    Assert-Archive $partial
    Move-Item -LiteralPath $partial -Destination $archive
  } finally {
    if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial }
  }
}
Assert-Archive $archive
if ($PrepareOnly) { return }
if ((Test-Path -LiteralPath $stamp) -and
    (Get-Content -LiteralPath $stamp -Raw).Trim() -eq $pin.sha256 -and
    (Test-Path -LiteralPath (Join-Path $Prefix 'include/zstd.h')) -and
    (Test-Path -LiteralPath (Join-Path $Prefix 'lib/zstd_static.lib')) -and
    (Test-Path -LiteralPath (Join-Path $Prefix 'lib/cmake/zstd/zstdConfig.cmake'))) {
  return
}

$buildRoot = Join-Path $PSScriptRoot "build/osgeo4w-zstd-$($pin.version)"
$source = Join-Path $buildRoot "zstd-$($pin.version)"
$build = Join-Path $buildRoot 'build'
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
cmake -E chdir $buildRoot cmake -E tar xf $archive
if ($LASTEXITCODE) { throw 'Could not extract zstd source' }
# The Qt DLLs use the pinned OSGeo4W zstd runtime. Install the development
# files and static library for Paleo without overwriting that runtime DLL.
$flags = @('-DZSTD_BUILD_PROGRAMS=OFF', '-DZSTD_BUILD_SHARED=OFF', '-DZSTD_BUILD_TESTS=OFF')
cmake -S "$source/build/cmake" -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release @flags
if ($LASTEXITCODE) { throw 'Could not configure zstd' }
$jobs = 8
if ($env:PALEO_JOBS -match '^[1-8]$') { $jobs = [int]$env:PALEO_JOBS }
cmake --build $build --parallel $jobs
if ($LASTEXITCODE) { throw 'Could not build zstd' }
cmake --install $build --prefix $Prefix
if ($LASTEXITCODE) { throw 'Could not install zstd' }
$notice = Join-Path $Prefix 'share/doc/paleo-zstd'
New-Item -ItemType Directory -Path $notice -Force | Out-Null
Copy-Item -LiteralPath "$source/LICENSE" -Destination $notice
[IO.File]::WriteAllText($stamp, $pin.sha256 + [Environment]::NewLine)
