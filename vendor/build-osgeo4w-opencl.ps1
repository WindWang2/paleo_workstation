param([Parameter(Mandatory=$true)][string]$Prefix)

$ErrorActionPreference = 'Stop'
function Assert-OpenClArchive([string]$Path, $Pin) {
  if ((Get-Item -LiteralPath $Path).Length -ne $Pin.size -or
      (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Pin.sha256) {
    throw "OpenCL source archive size or hash mismatch: $Path"
  }
}

$lock = Join-Path $PSScriptRoot 'opencl-source.lock.json'
$pin = Get-Content -LiteralPath $lock -Raw | ConvertFrom-Json
$identity = (Get-FileHash -LiteralPath $lock -Algorithm SHA256).Hash.ToLowerInvariant()
$stamp = Join-Path $Prefix '.paleo-opencl-source.sha256'
$library = Join-Path $Prefix 'bin/OpenCL.dll'
if ((Test-Path -LiteralPath $library) -and (Test-Path -LiteralPath $stamp) -and
    (Get-Content -LiteralPath $stamp -Raw).Trim() -eq $identity) {
  return
}

$cache = Join-Path $PSScriptRoot 'cache/tarballs'
$buildRoot = Join-Path $PSScriptRoot "build/osgeo4w-opencl-$($pin.version)"
New-Item -ItemType Directory -Path $cache,$buildRoot -Force | Out-Null
foreach ($package in $pin.archives.PSObject.Properties) {
  $name = $package.Name
  $file = Join-Path $cache "$name-v$($pin.version).tar.gz"
  if (!(Test-Path -LiteralPath $file)) {
    if ($env:PALEO_VENDOR_OFFLINE -eq '1') { throw "Missing offline OpenCL input: $file" }
    $download = "$file.$([Guid]::NewGuid()).download"
    $url = "https://codeload.github.com/KhronosGroup/$name/tar.gz/refs/tags/v$($pin.version)"
    try {
      Invoke-WebRequest -Uri $url -OutFile $download -TimeoutSec 90
      Assert-OpenClArchive $download $package.Value
      Move-Item -LiteralPath $download -Destination $file
    } finally {
      if (Test-Path -LiteralPath $download) { Remove-Item -LiteralPath $download }
    }
  }
  Assert-OpenClArchive $file $package.Value
  cmake -E chdir $buildRoot cmake -E tar xf $file
  if ($LASTEXITCODE) { throw "Could not unpack $name" }
}

$headers = Join-Path $buildRoot "OpenCL-Headers-$($pin.version)"
$source = Join-Path $buildRoot "OpenCL-ICD-Loader-$($pin.version)"
$build = Join-Path $buildRoot 'build'
$flags = @("-DOPENCL_ICD_LOADER_HEADERS_DIR=$headers", '-DOPENCL_ICD_LOADER_BUILD_SHARED_LIBS=ON',
           '-DBUILD_TESTING=OFF', '-DENABLE_OPENCL_LAYERS=OFF')
cmake -S $source -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release @flags
if ($LASTEXITCODE) { throw 'Could not configure the OpenCL loader' }
$jobs = 8
if ($env:PALEO_JOBS -match '^[1-8]$') { $jobs = [int]$env:PALEO_JOBS }
cmake --build $build --target OpenCL --parallel $jobs
if ($LASTEXITCODE) { throw 'Could not build the OpenCL loader' }
Copy-Item -LiteralPath (Join-Path $build 'OpenCL.dll') -Destination $library
$notices = Join-Path $Prefix 'share/doc/paleo-opencl'
New-Item -ItemType Directory -Path $notices -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination (Join-Path $notices 'loader-LICENSE')
Copy-Item -LiteralPath (Join-Path $headers 'LICENSE') -Destination (Join-Path $notices 'headers-LICENSE')
[IO.File]::WriteAllText($stamp, $identity + [Environment]::NewLine)
