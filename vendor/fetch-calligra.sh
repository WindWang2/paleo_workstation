#!/usr/bin/env bash
# Source-built, pinned offline Office renderer. Qt6/KF6 must use the same ABI
# as Paleo. Prefer PALEO_KF6_PREFIX; system development packages are a fallback.
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
task_prefix="$task_root/vendor/calligra"
task_build="$task_root/vendor/build/calligra"
task_cache="$task_root/vendor/cache/calligra"
task_jobs=${PALEO_BUILD_JOBS:-8}
[[ "$task_jobs" =~ ^[1-8]$ ]] || { echo 'PALEO_BUILD_JOBS must be 1..8' >&2; exit 1; }
mkdir -p "$task_cache" "$task_build" "$task_prefix"
python3 - "$task_root" <<'PY'
import hashlib, json, pathlib, subprocess, sys, tarfile
root = pathlib.Path(sys.argv[1])
dep = json.loads((root / 'vendor/manifest.json').read_text())['deps']['calligra']
for pin in [dep, *dep['build_deps']]:
    archive = root / 'vendor/cache/calligra' / pin['url'].rsplit('/', 1)[1]
    if not archive.exists():
        subprocess.run(['curl', '-fL', '--retry', '3', '-o', str(archive), pin['url']], check=True)
    if hashlib.file_digest(archive.open('rb'), 'sha256').hexdigest() != pin['sha256']:
        raise SystemExit(f'Checksum mismatch: {archive}; remove this archive and retry')
    directory = pin.get('directory', pin.get('name', 'calligra') + '-' + pin['version'])
    target = root / 'vendor/build/calligra' / directory
    if not target.exists():
        with tarfile.open(archive) as source:
            source.extractall(root / 'vendor/build/calligra', filter='data')
source = root / 'vendor/build/calligra' / ('calligra-' + dep['version']) / 'CMakeLists.txt'
bridge = '\ninclude("${PALEO_WORKSTATION_SOURCE}/cmake/CalligraRenderer.cmake")\n'
if bridge.strip() not in source.read_text():
    source.write_text(source.read_text() + bridge)
PY
task_source="$task_build/calligra-26.08.2"
for task_patch in "$task_root"/vendor/patches/calligra-26.08.2-*.patch; do
  if patch -R --dry-run --batch -p1 -d "$task_source" < "$task_patch" >/dev/null 2>&1; then
    continue
  fi
  patch --batch --forward -p1 -d "$task_source" < "$task_patch"
done
task_cmake_prefix="$task_prefix${PALEO_KF6_PREFIX:+;$PALEO_KF6_PREFIX}"
task_launcher=()
if command -v ccache >/dev/null; then task_launcher=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache); fi
task_common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$task_prefix"
  -DCMAKE_PREFIX_PATH="$task_cmake_prefix" -DBUILD_TESTING=OFF
  '-DCMAKE_INSTALL_RPATH=$ORIGIN;$ORIGIN/../lib;$ORIGIN/../../..' "${task_launcher[@]}")
cmake -S "$task_build/extra-cmake-modules-6.30.0" -B "$task_build/ecm" "${task_common[@]}"
cmake --install "$task_build/ecm"
cmake -S "$task_build/eigen-5.0.0" -B "$task_build/eigen" "${task_common[@]}" -DEIGEN_BUILD_TESTING=OFF -DEIGEN_BUILD_DOC=OFF
cmake --install "$task_build/eigen"
cmake -S "$task_build/kdiagram-3.0.1" -B "$task_build/kdiagram" "${task_common[@]}" -DBUILD_EXAMPLES=OFF
cmake --build "$task_build/kdiagram" -j"$task_jobs"
cmake --install "$task_build/kdiagram"
task_products='PART_WORDS PART_SHEETS PART_STAGE FILTER_DOC_TO_ODT FILTER_DOCX_TO_ODT FILTER_XLS_TO_SHEETS FILTER_XLSX_TO_ODS FILTER_PPT_TO_ODP FILTER_PPTX_TO_ODP PLUGIN_PATHSHAPES PLUGIN_PICTURESHAPE PLUGIN_VECTORSHAPE PLUGIN_COLORENGINES PLUGIN_CHARTSHAPE PLUGIN_FORMULASHAPE'
cmake -S "$task_build/calligra-26.08.2" -B "$task_build/build" "${task_common[@]}" \
  -DPALEO_WORKSTATION_SOURCE="$task_root" -DBUILD_DOC=OFF -DBUILD_VC=OFF -DPACKAGERS_BUILD=OFF \
  -DBoost_INCLUDE_DIR="$task_build/boost_1_90_0" -DPRODUCTSET="$task_products" \
  -DKDE_INSTALL_LIBDIR=lib -DKDE_INSTALL_PLUGINDIR=lib/plugins
cmake --build "$task_build/build" -j"$task_jobs"
cmake --install "$task_build/build"
mkdir -p "$task_prefix/share/licenses/calligra"
cp "$task_build/calligra-26.08.2"/COPYING* "$task_prefix/share/licenses/calligra/"
python3 - "$task_build" "$task_prefix" <<'PY'
import pathlib, shutil, sys
source, prefix = map(pathlib.Path, sys.argv[1:])
for name in ['kdiagram-3.0.1', 'boost_1_90_0', 'eigen-5.0.0']:
    target = prefix / 'share/licenses' / name
    target.mkdir(parents=True, exist_ok=True)
    for pattern in ['COPYING*', 'LICENSE*']:
        for notice in (source / name).glob(pattern):
            if notice.is_file():
                shutil.copy2(notice, target / notice.name)
PY
echo "Office renderer ready: $task_prefix/bin/paleo_office_renderer"
