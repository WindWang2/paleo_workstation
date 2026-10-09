#!/usr/bin/env bash
# Install the pinned ranuts/document static editor (OnlyOffice WASM, no document
# server). The tree is AGPL-3.0 and stays gitignored. Do not strip the
# ONLYOFFICE logo from the built page.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dest="$root/vendor/ranuts-document"
source_dir="$root/vendor/build/ranuts-document/src"
# v0.0.6 includes the embedded-WebView Editor.bin fix.
pin_repo="https://github.com/ranuts/document.git"
pin_tag="v0.0.6"
pin_sha="82cb9f68617383c1b3a00865a5880069cbdf2d92"
jobs=${PALEO_BUILD_JOBS:-8}
[[ "$jobs" =~ ^[1-8]$ ]] || { echo 'PALEO_BUILD_JOBS must be 1..8' >&2; exit 1; }
command -v git >/dev/null
if ! command -v pnpm >/dev/null 2>&1; then
  command -v corepack >/dev/null || { echo 'pnpm or corepack is required' >&2; exit 1; }
  corepack prepare pnpm@11.4.0 --activate
  shim="$root/vendor/build/ranuts-document/pnpm-shim"
  mkdir -p "$shim"
  printf '%s\n' '#!/bin/sh' 'exec corepack pnpm "$@"' > "$shim/pnpm"
  chmod +x "$shim/pnpm"
  export PATH="$shim:$PATH"
fi
pnpm_bin=(pnpm)

mkdir -p "$root/vendor/build/ranuts-document"
if [[ ! -d "$source_dir/.git" ]]; then
  git clone --depth 1 --branch "$pin_tag" "$pin_repo" "$source_dir"
fi
actual=$(git -C "$source_dir" rev-parse HEAD)
if [[ "$actual" != "$pin_sha" ]]; then
  git -C "$source_dir" fetch --depth 1 origin "refs/tags/$pin_tag"
  git -C "$source_dir" checkout --detach "$pin_sha"
fi
git -C "$source_dir" rev-parse HEAD | grep -qx "$pin_sha"
"${pnpm_bin[@]}" -C "$source_dir" install --frozen-lockfile
"${pnpm_bin[@]}" -C "$source_dir" run build
rm -rf "$dest"
mkdir -p "$dest"
cp -a "$source_dir/dist/." "$dest/"
test -f "$dest/index.html"
echo "ranuts/document $pin_sha installed at $dest"
